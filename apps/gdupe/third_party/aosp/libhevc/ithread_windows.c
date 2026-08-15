/******************************************************************************
 *
 * Copyright (C) 2012 Ittiam Systems Pvt Ltd, Bangalore
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/
/*
 * Modified for gdupe on 2026-08-15.
 * Windows thread abstraction for AOSP libhevc using Win32 primitives so the
 * decoder can be statically built with MSVC without pthreads.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <limits.h>

#include "ihevc_typedefs.h"
#include "ithread.h"

typedef void *(*ithread_start_fn)(void *);

typedef struct
{
    HANDLE handle;
    ithread_start_fn start;
    void *argument;
    void *result;
} ithread_win_handle_t;

static unsigned __stdcall ithread_win_thunk(void *opaque)
{
    ithread_win_handle_t *h = (ithread_win_handle_t *)opaque;
    h->result = h->start(h->argument);
    return 0;
}

UWORD32 ithread_get_handle_size(void)
{
    return (UWORD32)sizeof(ithread_win_handle_t);
}

UWORD32 ithread_get_mutex_lock_size(void)
{
    return (UWORD32)sizeof(CRITICAL_SECTION);
}

WORD32 ithread_create(void *thread_handle, void *attribute, void *strt, void *argument)
{
    ithread_win_handle_t *h = (ithread_win_handle_t *)thread_handle;
    uintptr_t raw;
    (void)attribute;

    h->start = (ithread_start_fn)strt;
    h->argument = argument;
    h->result = NULL;
    h->handle = NULL;

    raw = _beginthreadex(NULL, 0, ithread_win_thunk, h, 0, NULL);
    if(raw == 0)
        return -1;

    h->handle = (HANDLE)raw;
    return 0;
}

WORD32 ithread_join(void *thread_handle, void **val_ptr)
{
    ithread_win_handle_t *h = (ithread_win_handle_t *)thread_handle;
    DWORD wait_result;

    if(h->handle == NULL)
        return -1;

    wait_result = WaitForSingleObject(h->handle, INFINITE);
    if(wait_result != WAIT_OBJECT_0)
        return -1;

    if(val_ptr != NULL)
        *val_ptr = h->result;

    CloseHandle(h->handle);
    h->handle = NULL;
    return 0;
}

void ithread_exit(void *val_ptr)
{
    _endthreadex((unsigned)(uintptr_t)val_ptr);
}

WORD32 ithread_get_mutex_struct_size(void)
{
    return (WORD32)sizeof(CRITICAL_SECTION);
}

WORD32 ithread_mutex_init(void *mutex)
{
    InitializeCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_destroy(void *mutex)
{
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_lock(void *mutex)
{
    EnterCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_unlock(void *mutex)
{
    LeaveCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

void ithread_yield(void)
{
    if(!SwitchToThread())
        Sleep(0);
}

void ithread_sleep(UWORD32 seconds)
{
    Sleep((DWORD)(seconds * 1000u));
}

void ithread_msleep(UWORD32 milliseconds)
{
    Sleep((DWORD)milliseconds;
}

void ithread_usleep(UWORD32 microseconds)
{
    if(microseconds == 0)
    {
        ithread_yield();
        return;
    }
    Sleep((DWORD)((microseconds + 999u) / 1000u));
}

UWORD32 ithread_get_sem_struct_size(void)
{
    return (UWORD32)sizeof(HANDLE);
}

WORD32 ithread_sem_init(void *sem, WORD32 pshared, UWORD32 value)
{
    HANDLE h;
    (void)pshared;
    h = CreateSemaphoreW(NULL, (LONG)value, LONG_MAX, NULL);
    if(h == NULL)
        return -1;
    *(HANDLE *)sem = h;
    return 0;
}

WORD32 ithread_sem_post(void *sem)
{
    return ReleaseSemaphore(*(HANDLE *)sem, 1, NULL) ? 0 : -1;
}

WORD32 ithread_sem_wait(void *sem)
{
    return WaitForSingleObject(*(HANDLE *)sem, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

WORD32 ithread_sem_destroy(void *sem)
{
    HANDLE h = *(HANDLE *)sem;
    if(h != NULL)
    {
        CloseHandle(h);
        *(HANDLE *)sem = NULL;
    }
    return 0;
}

WORD32 ithread_set_affinity(WORD32 core_id)
{
    DWORD_PTR mask;
    if(core_id < 0 || core_id >= (WORD32)(sizeof(DWORD_PTR) * 8))
        return -1;
    mask = ((DWORD_PTR)1) << core_id;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0 ? core_id : -1;
}

WORD32 ithread_get_cond_struct_size(void)
{
    return (WORD32)sizeof(CONDITION_VARIABLE);
}

WORD32 ithread_cond_init(void *cond)
{
    InitializeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}

WORD32 ithread_cond_destroy(void *cond)
{
    (void)cond;
    return 0;
}

WORD32 ithread_cond_wait(void *cond, void *mutex)
{
    return SleepConditionVariableCS((CONDITION_VARIABLE *)cond,
                                    (CRITICAL_SECTION *)mutex,
                                    INFINITE)
               ? 0
               : -1;
}

WORD32 ithread_cond_signal(void *cond)
{
    WakeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}
