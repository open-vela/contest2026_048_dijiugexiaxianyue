/**
 * @file lv_pthread.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_os_private.h"

#if LV_USE_OS == LV_OS_PTHREAD

#include "../misc/lv_log.h"
#include <pthread.h>
#include <sched.h>
#include <string.h>

#ifndef __linux__
    #include "../misc/lv_timer.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void * generic_callback(void * user_data);

#if defined(SCHED_PRIORITY_DEFAULT) && defined(SCHED_PRIORITY_MIN) && defined(SCHED_PRIORITY_MAX)
    static int _nuttx_prio_from_lv(lv_thread_prio_t prio);
#endif


/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_result_t lv_thread_init(lv_thread_t * thread, const char * const name,
                           lv_thread_prio_t prio, void (*callback)(void *),
                           size_t stack_size, void * user_data)
{
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);

#if defined(SCHED_PRIORITY_DEFAULT) && defined(SCHED_PRIORITY_MIN) && defined(SCHED_PRIORITY_MAX)
    /* NuttX: pthread_attr_init() uses PTHREAD_EXPLICIT_SCHED with the creator's
     * priority, so without this the render thread always inherits the UI
     * thread's priority (usually SCHED_PRIORITY_DEFAULT) and RR-shares the CPU
     * with it.  Honour the requested LVGL priority instead.  Note that on
     * NuttX a numerically higher priority value means a higher priority. */
    {
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = _nuttx_prio_from_lv(prio);
        ret = pthread_attr_setschedparam(&attr, &param);
        if(ret != 0) {
            LV_LOG_WARN("pthread_attr_setschedparam failed: %d", ret);
        }
    }
#else
    LV_UNUSED(prio);
#endif

    thread->callback = callback;
    thread->user_data = user_data;
    ret = pthread_create(&thread->thread, &attr, generic_callback, thread);
    pthread_attr_destroy(&attr);
#ifndef CONFIG_DISABLE_PTHREAD
    if(ret == 0 && name != NULL && name[0] != '\0') {
        pthread_setname_np(thread->thread, name);
    }
#endif
    if(ret != 0) {
        LV_LOG_WARN("pthread_create failed: %d", ret);
        return LV_RESULT_INVALID;
    }
    return LV_RESULT_OK;
}

lv_result_t lv_thread_delete(lv_thread_t * thread)
{
    int ret = pthread_join(thread->thread, NULL);
    if(ret != 0) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_init(lv_mutex_t * mutex)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int ret = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if(ret) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }
    else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_mutex_lock(lv_mutex_t * mutex)
{
    int ret = pthread_mutex_lock(mutex);
    if(ret) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }
    else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_mutex_lock_isr(lv_mutex_t * mutex)
{
    int ret = pthread_mutex_lock(mutex);
    if(ret) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }
    else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_mutex_unlock(lv_mutex_t * mutex)
{
    int ret = pthread_mutex_unlock(mutex);
    if(ret) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }
    else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_mutex_delete(lv_mutex_t * mutex)
{
    pthread_mutex_destroy(mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_init(lv_thread_sync_t * sync)
{
    pthread_mutex_init(&sync->mutex, 0);
    pthread_cond_init(&sync->cond, 0);
    sync->v = false;
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_wait(lv_thread_sync_t * sync)
{
    pthread_mutex_lock(&sync->mutex);
    while(!sync->v) {
        pthread_cond_wait(&sync->cond, &sync->mutex);
    }
    sync->v = false;
    pthread_mutex_unlock(&sync->mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_signal(lv_thread_sync_t * sync)
{
    pthread_mutex_lock(&sync->mutex);
    sync->v = true;
    pthread_cond_signal(&sync->cond);
    pthread_mutex_unlock(&sync->mutex);

    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_delete(lv_thread_sync_t * sync)
{
    pthread_mutex_destroy(&sync->mutex);
    pthread_cond_destroy(&sync->cond);
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_signal_isr(lv_thread_sync_t * sync)
{
    LV_UNUSED(sync);
    return LV_RESULT_INVALID;
}

#ifndef __linux__
uint32_t lv_os_get_idle_percent(void)
{
    return lv_timer_get_idle();
}
#endif

void lv_sleep_ms(uint32_t ms)
{
    /* Just call standard posix sleep function */
    usleep(ms * 1000);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void * generic_callback(void * user_data)
{
    lv_thread_t * thread = user_data;
    thread->callback(thread->user_data);
    return NULL;
}

#if defined(SCHED_PRIORITY_DEFAULT) && defined(SCHED_PRIORITY_MIN) && defined(SCHED_PRIORITY_MAX)
static int _nuttx_prio_from_lv(lv_thread_prio_t prio)
{
    int p;

    switch(prio) {
        case LV_THREAD_PRIO_LOWEST:
            p = SCHED_PRIORITY_DEFAULT - 20;
            break;
        case LV_THREAD_PRIO_LOW:
            p = SCHED_PRIORITY_DEFAULT - 10;
            break;
        case LV_THREAD_PRIO_MID:
            p = SCHED_PRIORITY_DEFAULT;
            break;
        case LV_THREAD_PRIO_HIGH:
            p = SCHED_PRIORITY_DEFAULT + 10;
            break;
        case LV_THREAD_PRIO_HIGHEST:
            p = SCHED_PRIORITY_DEFAULT + 20;
            break;
        default:
            p = SCHED_PRIORITY_DEFAULT;
            break;
    }

    if(p < SCHED_PRIORITY_MIN) {
        p = SCHED_PRIORITY_MIN;
    }
    if(p > SCHED_PRIORITY_MAX) {
        p = SCHED_PRIORITY_MAX;
    }

    return p;
}
#endif


#endif /*LV_USE_OS == LV_OS_PTHREAD*/
