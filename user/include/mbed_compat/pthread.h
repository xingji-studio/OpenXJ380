#pragma once

#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef int pthread_cond_t;
typedef int pthread_condattr_t;
typedef int pthread_t;
typedef int clockid_t;

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

#ifdef __cplusplus
}
#endif
