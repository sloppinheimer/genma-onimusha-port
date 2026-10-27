// Linux: what a HANDLE points at (compat_*.c). One struct for every kind, as few as there are.
#pragma once
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>

enum { H_THREAD = 1, H_EVENT, H_MUTEX, H_TIMER, H_FILE, H_DIR, H_FIND };
struct compat_handle {
    int kind;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled, manual;                 // events (and a finished thread: signaled)
    DWORD owner, count;                   // mutexes
    int suspended;                        // threads
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    DWORD code, tid;
    pthread_t thread;
    uint64_t due;                         // waitable timers: due time, CLOCK_MONOTONIC ns
    int fd;                               // files and folders
    char *path;                           // their Linux path (UTF-8); none for the null device
    ACCESS_MASK access;                   // files: the rights NtCreateFile was asked for
    DIR *dir;                             // folders being listed (NtQueryDirectoryFile, FindFirstFileW)
    char pattern[260];                    // FindFirstFileW's pattern (the name part)
    int append;
};
struct compat_handle *compat_new_handle(int kind);
DWORD compat_wait(struct compat_handle *h, DWORD ms);
void compat_set_errno_error(void);
BOOL compat_release_mutex(HANDLE h);
int compat_match(const char *pattern, const char *name);   // * and ?, case-insensitive
ssize_t compat_read_full(int fd, void *buf, size_t n);         // until n bytes, end of file or an error
ssize_t compat_write_full(int fd, const void *buf, size_t n);
