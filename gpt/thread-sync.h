#include <semaphore.h>
#include <pthread.h>

// Spinlock
typedef int spinlock_t;
#define SPIN_INIT() 0

#define FREE 0
#define LOCKED 1

void spin_lock(spinlock_t *lk) {
    while (1) {
        int status = __atomic_exchange_n(lk, LOCKED, __ATOMIC_ACQ_REL);
        if (status == FREE) {
            break;
        }
    }
}
void spin_unlock(spinlock_t *lk) {
    __atomic_exchange_n(lk, FREE, __ATOMIC_ACQ_REL);
}

// Mutex
typedef pthread_mutex_t mutex_t;
#define MUTEX_INIT() PTHREAD_MUTEX_INITIALIZER
#define mutex_init(mutex) pthread_mutex_init(mutex, NULL)
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock

// Conditional Variable
typedef pthread_cond_t cond_t;
#define COND_INIT() PTHREAD_COND_INITIALIZER
#define cond_init(cv) pthread_cond_init(cv, NULL)
#define cond_wait pthread_cond_wait
#define cond_broadcast pthread_cond_broadcast
#define cond_signal pthread_cond_signal

// Semaphore
#define P sem_wait
#define V sem_post
#define SEM_INIT(sem, val) sem_init(sem, 0, val)