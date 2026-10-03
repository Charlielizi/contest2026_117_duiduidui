/****************************************************************************
 * Cross-process ownership guard for the shared Wi-Fi radio
 ****************************************************************************/

#include <nuttx/config.h>

#include "infra/wifi_radio_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>

#define WIFI_RADIO_SEM_NAME "gemini_wifi_radio"

static pthread_once_t g_radio_once = PTHREAD_ONCE_INIT;
static sem_t *g_radio_sem = SEM_FAILED;
static int g_radio_open_error;

static void wifi_radio_sem_init(void)
{
    g_radio_sem = sem_open(WIFI_RADIO_SEM_NAME, O_CREAT, 0600, 1);
    if (g_radio_sem == SEM_FAILED)
        g_radio_open_error = errno;
}

static sem_t *wifi_radio_sem(void)
{
    pthread_once(&g_radio_once, wifi_radio_sem_init);
    return g_radio_sem;
}

int wifi_radio_guard_acquire(void)
{
    sem_t *sem = wifi_radio_sem();

    if (sem == SEM_FAILED)
        return -(g_radio_open_error ? g_radio_open_error : ENODEV);
    while (sem_wait(sem) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    return 0;
}

int wifi_radio_guard_try_acquire(void)
{
    sem_t *sem = wifi_radio_sem();

    if (sem == SEM_FAILED)
        return -(g_radio_open_error ? g_radio_open_error : ENODEV);
    if (sem_trywait(sem) < 0)
        return -errno;
    return 0;
}

void wifi_radio_guard_release(void)
{
    sem_t *sem = wifi_radio_sem();

    if (sem != SEM_FAILED)
        sem_post(sem);
}
