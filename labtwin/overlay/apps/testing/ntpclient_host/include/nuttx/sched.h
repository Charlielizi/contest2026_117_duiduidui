#pragma once
static inline void sched_lock(void) {}
static inline void sched_unlock(void) {}
int task_create(const char *, int, int, int (*)(int, char **), char **);
