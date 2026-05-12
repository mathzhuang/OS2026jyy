#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#define LENGTH(arr) (sizeof(arr) / sizeof(arr[0]))

enum {
    T_FREE = 0, // This slot is not used yet.
    T_LIVE,     // This thread is running.
    T_DEAD,     // This thread has terminated.
};

struct thread {
    int id;  // Thread number: 1, 2, ...
    int status;  // Thread status: FREE/LIVE/DEAD
    pthread_t thread;  // Thread struct
    void *entry;  // Entry point (any function pointer)
};

void spawn(void *fn);
void join();

__attribute__((weak)) struct thread threads_[4096];
__attribute__((weak)) int n_;

// This is the entry for a created POSIX thread. It "wraps"
// the function call of entry() to be compatible to the
// pthread library's requirements: a thread takes a void *
// pointer as argument, and returns a pointer.
static void *wrapper_(void *arg) {
    struct thread *t = (struct thread *)arg;
    void (*fn)(void *) = (void (*)(void *))t->entry;
    fn((void *)(long)t->id);
    return NULL;
}

// Create a thread that calls function fn.
// Spawn in a spawned thread is **undefined behavior**!
__attribute__((weak))
void spawn(void *fn) {
    assert(n_ < LENGTH(threads_));

    // Yes, we have resource leak here!
    threads_[n_] = (struct thread) {
        .id = n_ + 1,
        .status = T_LIVE,
        .entry = fn,
    };
    pthread_create(
        &(threads_[n_].thread),  // a pthread_t
        NULL,  // options; all to default
        wrapper_,  // the wrapper function
        &threads_[n_] // the argument to the wrapper
    );
    n_++;
}

// Wait until all threads return.
__attribute__((weak))
void join() {
    for (int i = 0; i < n_; i++) {
        struct thread *t = &threads_[i];
        if (t->status == T_LIVE) {
            pthread_join(t->thread, NULL);
            t->status = T_DEAD;
        }
    }
}

__attribute__((constructor))
static void startup() {
    atexit(join);
}
