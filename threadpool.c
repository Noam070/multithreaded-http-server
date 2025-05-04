//NOAM

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "threadpool.h"

threadpool* create_threadpool(int num_threads_in_pool, int max_queue_size) {
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL || max_queue_size <= 0 || max_queue_size > MAXW_IN_QUEUE) {
        fprintf(stderr, "Invalid threadpool parameters\n");
        return NULL;
    }

    threadpool* pool = (threadpool*)malloc(sizeof(threadpool));
    if (!pool) {
        perror("malloc");
        return NULL;
    }

    pool->num_threads = num_threads_in_pool;
    pool->qsize = 0;
    pool->max_qsize = max_queue_size;
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads_in_pool);
    pool->qhead = NULL;
    pool->qtail = NULL;
    pool->shutdown = 0;
    pool->dont_accept = 0;

    if (pthread_mutex_init(&(pool->qlock), NULL) != 0 ||
        pthread_cond_init(&(pool->q_not_empty), NULL) != 0 ||
        pthread_cond_init(&(pool->q_empty), NULL) != 0 ||
        pthread_cond_init(&(pool->q_not_full), NULL) != 0) {
        perror("mutex or cond init");
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, do_work, (void*)pool) != 0) {
            perror("pthread_create");
            destroy_threadpool(pool);
            return NULL;
        }
    }

    return pool;
}

void dispatch(threadpool* pool, dispatch_fn dispatch_to_here, void* arg) {
    if (!pool || pool->dont_accept) {
        return;
    }

    work_t* work = (work_t*)malloc(sizeof(work_t));
    if (!work) {
        perror("malloc");
        return;
    }

    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    pthread_mutex_lock(&(pool->qlock));

    while (pool->qsize >= pool->max_qsize && !pool->shutdown) {
        pthread_cond_wait(&(pool->q_not_full), &(pool->qlock));
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&(pool->qlock));
        free(work);
        return;
    }

    if (pool->qtail) {
        pool->qtail->next = work;
    }
    else {
        pool->qhead = work;
    }
    pool->qtail = work;
    pool->qsize++;

    pthread_cond_signal(&(pool->q_not_empty));
    pthread_mutex_unlock(&(pool->qlock));
}

void* do_work(void* p) {
    threadpool* pool = (threadpool*)p;

    while (1) {
        pthread_mutex_lock(&(pool->qlock));

        while (pool->qsize == 0 && !pool->shutdown) {
            pthread_cond_wait(&(pool->q_not_empty), &(pool->qlock));
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->qlock));
            pthread_exit(NULL);
        }

        work_t* work = pool->qhead;
        if (work) {
            pool->qhead = work->next;
            if (!pool->qhead) {
                pool->qtail = NULL;
            }
            pool->qsize--;

            if (pool->qsize == 0) {
                pthread_cond_signal(&(pool->q_empty));
            }
        }

        pthread_cond_signal(&(pool->q_not_full));
        pthread_mutex_unlock(&(pool->qlock));

        if (work) {
            work->routine(work->arg);
            free(work);
        }
    }

    pthread_exit(NULL);
}

void destroy_threadpool(threadpool* pool) {
    if (!pool) return;

    pthread_mutex_lock(&(pool->qlock));
    pool->dont_accept = 1;

    while (pool->qsize > 0) {
        pthread_cond_wait(&(pool->q_empty), &(pool->qlock));
    }

    pool->shutdown = 1;
    pthread_cond_broadcast(&(pool->q_not_empty));
    pthread_mutex_unlock(&(pool->qlock));

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);

    work_t* current;
    while (pool->qhead) {
        current = pool->qhead;
        pool->qhead = pool->qhead->next;
        free(current);
    }

    pthread_mutex_destroy(&(pool->qlock));
    pthread_cond_destroy(&(pool->q_not_empty));
    pthread_cond_destroy(&(pool->q_empty));
    pthread_cond_destroy(&(pool->q_not_full));
    free(pool);
}
