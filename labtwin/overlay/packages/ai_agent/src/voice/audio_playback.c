/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* audio_playback.c - Streaming PCM playback with a direct-device fallback. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <errno.h>
#include <fcntl.h>
#include <media_player.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128
#define DIRECT_PCM_MQ_NAME "/tmp/ai_agent_tts"
#define DIRECT_PCM_WAIT_MIN_MS 3000U
#define DIRECT_PCM_WAIT_MAX_MS 15000U

static void* s_active_player;

struct direct_pcm {
    int fd;
    mqd_t mq;
    struct ap_buffer_s** buffers;
    unsigned char* busy;
    unsigned int buffer_count;
    int current_buffer;
    size_t current_used;
    int started;
};

struct audio_playback {
    void* player;
    struct direct_pcm direct;
    size_t total_written;
    int bytes_per_frame;
    unsigned int sample_rate;
    volatile int stopped;
};

static void direct_pcm_reset(struct direct_pcm* direct)
{
    memset(direct, 0, sizeof(*direct));
    direct->fd = -1;
    direct->mq = (mqd_t)-1;
    direct->current_buffer = -1;
}

static void direct_pcm_release(struct direct_pcm* direct)
{
    unsigned int i;

    if (direct->fd >= 0) {
        if (direct->started) {
            (void)ioctl(direct->fd, AUDIOIOC_STOP, 0);
        }

        if (direct->buffers) {
            for (i = 0; i < direct->buffer_count; i++) {
                if (direct->buffers[i]) {
                    struct audio_buf_desc_s desc;
                    memset(&desc, 0, sizeof(desc));
                    desc.u.buffer = direct->buffers[i];
                    (void)ioctl(direct->fd, AUDIOIOC_FREEBUFFER,
                                (unsigned long)(uintptr_t)&desc);
                }
            }
        }

        if (direct->mq != (mqd_t)-1) {
            (void)ioctl(direct->fd, AUDIOIOC_UNREGISTERMQ,
                        (unsigned long)direct->mq);
        }
        (void)ioctl(direct->fd, AUDIOIOC_RELEASE, 0);
        close(direct->fd);
    }

    if (direct->mq != (mqd_t)-1) {
        mq_close(direct->mq);
    }
    mq_unlink(DIRECT_PCM_MQ_NAME);
    free(direct->buffers);
    free(direct->busy);
    direct_pcm_reset(direct);
}

static int direct_pcm_open(struct audio_playback* pb, const char* dev_path,
                           unsigned int sample_rate, unsigned int channels,
                           unsigned int bits_per_sample)
{
    struct direct_pcm* direct = &pb->direct;
    struct audio_caps_desc_s caps;
    struct ap_buffer_info_s info;
    struct mq_attr attr;
    unsigned int i;
    int ret;

    direct_pcm_reset(direct);
    direct->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (direct->fd < 0) {
        syslog(LOG_ERR, "[%s] direct open %s failed: %d\n", TAG,
               dev_path, errno);
        return -errno;
    }

    if (ioctl(direct->fd, AUDIOIOC_RESERVE, 0) < 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] direct reserve failed: %d\n", TAG, errno);
        direct_pcm_release(direct);
        return ret;
    }

    memset(&caps, 0, sizeof(caps));
    caps.caps.ac_len = sizeof(struct audio_caps_s);
    caps.caps.ac_type = AUDIO_TYPE_OUTPUT;
    caps.caps.ac_channels = channels;
    caps.caps.ac_controls.hw[0] = sample_rate;
    caps.caps.ac_controls.b[2] = bits_per_sample;
    if (ioctl(direct->fd, AUDIOIOC_CONFIGURE,
              (unsigned long)(uintptr_t)&caps) < 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] direct configure failed: %d\n", TAG, errno);
        direct_pcm_release(direct);
        return ret;
    }

    memset(&info, 0, sizeof(info));
    if (ioctl(direct->fd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)(uintptr_t)&info) < 0 ||
        info.nbuffers == 0 || info.buffer_size == 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] direct buffer info failed: %d\n", TAG, errno);
        direct_pcm_release(direct);
        return ret ? ret : -EIO;
    }

    direct->buffer_count = info.nbuffers;
    direct->buffers = calloc(direct->buffer_count, sizeof(*direct->buffers));
    direct->busy = calloc(direct->buffer_count, sizeof(*direct->busy));
    if (!direct->buffers || !direct->busy) {
        direct_pcm_release(direct);
        return -ENOMEM;
    }

    mq_unlink(DIRECT_PCM_MQ_NAME);
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = direct->buffer_count + 4;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    direct->mq = mq_open(DIRECT_PCM_MQ_NAME, O_RDWR | O_CREAT | O_EXCL,
                         0600, &attr);
    if (direct->mq == (mqd_t)-1 ||
        ioctl(direct->fd, AUDIOIOC_REGISTERMQ,
              (unsigned long)direct->mq) < 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] direct message queue setup failed: %d\n",
               TAG, errno);
        direct_pcm_release(direct);
        return ret ? ret : -EIO;
    }

    for (i = 0; i < direct->buffer_count; i++) {
        struct audio_buf_desc_s desc;
        memset(&desc, 0, sizeof(desc));
        desc.numbytes = info.buffer_size;
        desc.u.pbuffer = &direct->buffers[i];
        if (ioctl(direct->fd, AUDIOIOC_ALLOCBUFFER,
                  (unsigned long)(uintptr_t)&desc) < 0 ||
            !direct->buffers[i]) {
            ret = -errno;
            syslog(LOG_ERR, "[%s] direct buffer allocation failed: %d\n",
                   TAG, errno);
            direct_pcm_release(direct);
            return ret ? ret : -ENOMEM;
        }
    }

    syslog(LOG_INFO, "[%s] direct PCM fallback opened %s (%uHz %uch %ubit, %u buffers)\n",
           TAG, dev_path, sample_rate, channels, bits_per_sample,
           direct->buffer_count);
    return 0;
}

static void direct_pcm_reclaim(struct direct_pcm* direct,
                               const struct audio_msg_s* msg)
{
    unsigned int i;

    if (msg->msg_id != AUDIO_MSG_DEQUEUE || !msg->u.ptr) {
        return;
    }

    for (i = 0; i < direct->buffer_count; i++) {
        if (direct->buffers[i] == msg->u.ptr) {
            direct->busy[i] = 0;
            return;
        }
    }
}

static int direct_pcm_take_buffer(struct direct_pcm* direct)
{
    struct timespec deadline;
    struct audio_msg_s msg;
    unsigned int priority;
    unsigned int i;
    ssize_t received;

    for (;;) {
        for (i = 0; i < direct->buffer_count; i++) {
            if (!direct->busy[i]) {
                direct->current_buffer = (int)i;
                direct->current_used = 0;
                return 0;
            }
        }

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;
        received = mq_timedreceive(direct->mq, (char*)&msg, sizeof(msg),
                                   &priority, &deadline);
        if (received != sizeof(msg)) {
            syslog(LOG_ERR, "[%s] direct PCM buffer wait failed: %d\n",
                   TAG, errno);
            return -ETIMEDOUT;
        }
        direct_pcm_reclaim(direct, &msg);
    }
}

static int direct_pcm_enqueue_current(struct audio_playback* pb, int final)
{
    struct direct_pcm* direct = &pb->direct;
    struct ap_buffer_s* apb;
    int index;
    int ret;

    if (direct->current_buffer < 0) {
        if (!final) {
            return 0;
        }
        ret = direct_pcm_take_buffer(direct);
        if (ret < 0) {
            return ret;
        }
    }

    if (direct->current_used == 0 && !final) {
        return 0;
    }

    index = direct->current_buffer;
    apb = direct->buffers[index];
    apb->nbytes = direct->current_used;
    apb->curbyte = 0;
    apb->flags &= ~AUDIO_APB_FINAL;
    if (final) {
        apb->flags |= AUDIO_APB_FINAL;
    }

    ret = ioctl(direct->fd, AUDIOIOC_ENQUEUEBUFFER,
                (unsigned long)(uintptr_t)&(struct audio_buf_desc_s){
                    .numbytes = apb->nbytes, .u.buffer = apb });
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] direct enqueue failed: %d\n", TAG, errno);
        return -errno;
    }

    direct->busy[index] = 1;
    direct->current_buffer = -1;
    direct->current_used = 0;
    if (!direct->started) {
        if (ioctl(direct->fd, AUDIOIOC_START, 0) < 0) {
            syslog(LOG_ERR, "[%s] direct start failed: %d\n", TAG, errno);
            return -errno;
        }
        direct->started = 1;
    }
    return 0;
}

static int direct_pcm_write(struct audio_playback* pb, const void* buf,
                            size_t len)
{
    struct direct_pcm* direct = &pb->direct;
    const unsigned char* source = buf;

    while (len > 0) {
        struct ap_buffer_s* apb;
        size_t room;
        size_t copied;
        int ret;

        if (direct->current_buffer < 0) {
            ret = direct_pcm_take_buffer(direct);
            if (ret < 0) {
                return ret;
            }
        }

        apb = direct->buffers[direct->current_buffer];
        room = apb->nmaxbytes - direct->current_used;
        copied = len < room ? len : room;
        memcpy(apb->samp + direct->current_used, source, copied);
        direct->current_used += copied;
        source += copied;
        len -= copied;

        if (direct->current_used == apb->nmaxbytes) {
            ret = direct_pcm_enqueue_current(pb, 0);
            if (ret < 0) {
                return ret;
            }
        }
    }

    return 0;
}

static void direct_pcm_wait_complete(struct audio_playback* pb)
{
    struct direct_pcm* direct = &pb->direct;
    struct timespec deadline;
    struct audio_msg_s msg;
    unsigned int priority;
    uint64_t playback_ms;
    ssize_t received;

    if (!direct->started || pb->bytes_per_frame <= 0 || pb->sample_rate == 0) {
        return;
    }

    playback_ms = ((uint64_t)pb->total_written * 1000U) /
                  ((uint64_t)pb->sample_rate * pb->bytes_per_frame);
    playback_ms += 1500U;
    if (playback_ms < DIRECT_PCM_WAIT_MIN_MS) {
        playback_ms = DIRECT_PCM_WAIT_MIN_MS;
    } else if (playback_ms > DIRECT_PCM_WAIT_MAX_MS) {
        playback_ms = DIRECT_PCM_WAIT_MAX_MS;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += playback_ms / 1000U;
    deadline.tv_nsec += (playback_ms % 1000U) * 1000000UL;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        received = mq_timedreceive(direct->mq, (char*)&msg, sizeof(msg),
                                   &priority, &deadline);
        if (received != sizeof(msg)) {
            syslog(LOG_WARNING, "[%s] direct PCM completion wait timed out: %d\n",
                   TAG, errno);
            return;
        }
        if (msg.msg_id == AUDIO_MSG_COMPLETE) {
            return;
        }
        direct_pcm_reclaim(direct, &msg);
    }
}

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    audio_playback_t* pb;

    if (!dev_path || !dev_path[0] || sample_rate == 0 || channels == 0 ||
        bits_per_sample == 0 || (bits_per_sample % 8) != 0) {
        return NULL;
    }

    if (s_active_player) {
        syslog(LOG_WARNING, "[%s] force closing stale player\n", TAG);
        media_player_stop(s_active_player);
        media_player_close(s_active_player, 0);
        s_active_player = NULL;
        usleep(100000);
    }

    void* player = media_player_open(MEDIA_STREAM_MUSIC);
    if (!player) {
        syslog(LOG_WARNING, "[%s] media_player_open failed; trying direct PCM\n",
               TAG);
        pb = calloc(1, sizeof(*pb));
        if (!pb) {
            return NULL;
        }

        pb->bytes_per_frame = (bits_per_sample / 8) * channels;
        pb->sample_rate = sample_rate;
        if (direct_pcm_open(pb, dev_path, sample_rate, channels,
                            bits_per_sample) < 0) {
            free(pb);
            return NULL;
        }
        return pb;
    }

    char opts[PB_OPTIONS_LEN];
    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    int ret = media_player_prepare(player, NULL, opts);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    ret = media_player_start(player);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    pb = calloc(1, sizeof(*pb));
    if (!pb) {
        media_player_close(player, 0);
        return NULL;
    }

    pb->player = player;
    pb->bytes_per_frame = (bits_per_sample / 8) * channels;
    pb->sample_rate = sample_rate;
    s_active_player = player;

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return pb;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    if (!pb->player) {
        int ret = direct_pcm_write(pb, buf, len);
        if (ret < 0) {
            return ret;
        }
        pb->total_written += len;
        return (int)len;
    }

    ssize_t n = media_player_write_data(pb->player, buf, len);
    if (n > 0) {
        pb->total_written += (size_t)n;
    }

    return (int)n;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        if (pb->player) {
            media_player_stop(pb->player);
        } else if (pb->direct.fd >= 0) {
            (void)ioctl(pb->direct.fd, AUDIOIOC_STOP, 0);
        }
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    if (pb->player) {
        syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
            TAG, pb->total_written);
        media_player_stop(pb->player);
        usleep(50 * 1000);
        media_player_close(pb->player, 0);
        s_active_player = NULL;
    } else if (pb->direct.fd >= 0) {
        if (!pb->stopped) {
            int ret = direct_pcm_enqueue_current(pb, 1);
            if (ret < 0) {
                syslog(LOG_ERR, "[%s] direct PCM final buffer failed: %d\n",
                       TAG, ret);
            } else {
                direct_pcm_wait_complete(pb);
            }
        }
        syslog(LOG_INFO, "[%s] closing direct PCM (%zu bytes written)\n",
               TAG, pb->total_written);
        direct_pcm_release(&pb->direct);
    }

    free(pb);
}
