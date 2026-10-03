/* Fault injection is confined to this test's isolated root and fd paths. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int fault_write, fault_sync, fault_rollback;
static int matches(int fd, const char *name)
{
    char proc[64], path[512];
    ssize_t n;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    n = readlink(proc, path, sizeof(path) - 1);
    if (n < 0) return 0;
    path[n] = 0;
    return strstr(path, name) != NULL;
}
static ssize_t injected_write(int fd, const void *buf, size_t size)
{
    if (fault_write && matches(fd, "/events.jsonl")) {
        if (fault_write == 1) { fault_write = 2; return write(fd, buf, size > 7 ? 7 : size); }
        fault_write = 0; errno = ENOSPC; return -1;
    }
    return write(fd, buf, size);
}
static int injected_sync(int fd)
{
    if ((fault_sync || fault_rollback) && matches(fd, "/events.jsonl")) {
        fault_sync = 0; errno = EIO; return -1;
    }
    return fsync(fd);
}
#define write injected_write
#define fsync injected_sync
#include "../src/labtwin/labtwin_service.c"
#undef write
#undef fsync

int main(void)
{
    char out[16384], path[256];
    labtwin_experiment_t before, recovered;
    struct stat st;
    off_t event_size;
    mkdir_checked(LABTWIN_ROOT);
    mkdir_checked(LABTWIN_EXPERIMENTS);
    assert(labtwin_experiment_create_json(
        "{\"experiment_id\":\"TX1\",\"name\":\"original\",\"steps\":[\"original step\"]}",
        out, sizeof(out), "test") == 0);
    before = *find_experiment("TX1");
    assert(labtwin_experiment_update_json(
        "{\"experiment_id\":\"TX1\",\"name\":\"rejected\",\"steps\":[\"\"],\"if_event_seq\":1}",
        out, sizeof(out), "test") != 0);
    assert(memcmp(&before, find_experiment("TX1"), sizeof(before)) == 0);
    snprintf(path, sizeof(path), "%s/TX1/events.jsonl", LABTWIN_EXPERIMENTS);
    assert(stat(path, &st) == 0); event_size = st.st_size;
    fault_write = 1;
    assert(labtwin_experiment_transition_json(
        "{\"experiment_id\":\"TX1\",\"action\":\"start\"}", out, sizeof(out), "test") != 0);
    assert(memcmp(&before, find_experiment("TX1"), sizeof(before)) == 0);
    assert(stat(path, &st) == 0 && st.st_size == event_size);
    fault_sync = 1;
    assert(labtwin_experiment_transition_json(
        "{\"experiment_id\":\"TX1\",\"action\":\"start\"}", out, sizeof(out), "test") != 0);
    assert(memcmp(&before, find_experiment("TX1"), sizeof(before)) == 0);
    /* A durable event is authoritative even when snapshot rename fails. */
    snprintf(path, sizeof(path), "%s/TX1/snapshot.json", LABTWIN_EXPERIMENTS);
    assert(unlink(path) == 0 && mkdir(path, 0700) == 0);
    assert(labtwin_experiment_transition_json(
        "{\"experiment_id\":\"TX1\",\"action\":\"start\"}", out, sizeof(out), "test") == 0);
    assert(strstr(out, "\"snapshot_pending\":true"));
    assert(find_experiment("TX1")->state == LABTWIN_STATE_RUNNING);
    assert(find_experiment("TX1")->last_event_seq == 2);
    assert(load_one("TX1", &recovered) == 0);
    assert(recovered.state == LABTWIN_STATE_RUNNING && recovered.last_event_seq == 2);
    /* Missing first snapshot must not lose a newly created task either. */
    snprintf(path, sizeof(path), "%s/TX2", LABTWIN_EXPERIMENTS); mkdir_checked(path);
    snprintf(path, sizeof(path), "%s/TX2/snapshot.json", LABTWIN_EXPERIMENTS); mkdir_checked(path);
    assert(labtwin_experiment_create_json(
        "{\"experiment_id\":\"TX2\",\"name\":\"new\",\"steps\":[\"step\"]}",
        out, sizeof(out), "test") == 0);
    assert(load_one("TX2", &recovered) == 0 && recovered.last_event_seq == 1);
    fault_rollback = 1;
    assert(labtwin_experiment_transition_json(
        "{\"experiment_id\":\"TX1\",\"action\":\"pause\"}", out, sizeof(out), "test") != 0);
    assert(find_experiment("TX1")->recovery_error);
    puts("labtwin_transaction_host_test: PASS");
    return 0;
}
