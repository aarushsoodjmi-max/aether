#define _GNU_SOURCE
#include "t.h"
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/syscall.h>
#define NT 4
#define ITER 20000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter; static int ready;
static __thread int tls_val; static pthread_barrier_t bar; static sem_t sem;
static pid_t tids[NT];
static void *worker(void *a) {
    long id = (long)a; tls_val = (int)id + 100; tids[id] = (pid_t)syscall(SYS_gettid);
    pthread_barrier_wait(&bar);
    for (int i = 0; i < ITER; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    CHECK(tls_val == (int)id + 100);
    sem_post(&sem);
    return (void *)(id * 2);
}
static void *waiter(void *a) { (void)a; pthread_mutex_lock(&mu); while (!ready) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu); return NULL; }
int main(void) {
    pthread_t t[NT]; CHECK(pthread_barrier_init(&bar, NULL, NT) == 0); CHECK(sem_init(&sem, 0, 0) == 0);
    for (long i = 0; i < NT; i++) CHECK(pthread_create(&t[i], NULL, worker, (void *)i) == 0);
    for (long i = 0; i < NT; i++) { void *r; CHECK(pthread_join(t[i], &r) == 0); CHECK((long)r == i * 2); }
    CHECK(counter == (long)NT * ITER); tls_val = 5; CHECK(tls_val == 5);
    for (int i = 0; i < NT; i++) CHECK(tids[i] != 0 && tids[i] != getpid());
    int v; CHECK(sem_getvalue(&sem, &v) == 0 && v == NT);
    pthread_t w; CHECK(pthread_create(&w, NULL, waiter, NULL) == 0); usleep(50000);
    pthread_mutex_lock(&mu); ready = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); CHECK(pthread_join(w, NULL) == 0);
    pthread_attr_t at; CHECK(pthread_attr_init(&at) == 0 && pthread_attr_setstacksize(&at, 1 << 20) == 0);
    pthread_t d; CHECK(pthread_create(&d, &at, waiter, NULL) == 0); CHECK(pthread_join(d, NULL) == 0);
    DONE("threads");
}
