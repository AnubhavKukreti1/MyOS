/**
 * @file error.h
 * @brief Kernel error numbers. Functions return 0 on success or -E_xxx.
 * Values match Linux errno where one exists.
 */
#ifndef MYOS_KERNEL_ERROR_H
#define MYOS_KERNEL_ERROR_H

#include "kernel.h"

#define E_PERM       1
#define E_NOENT      2
#define E_IO         5
#define E_BADF       9
#define E_AGAIN     11
#define E_NOMEM     12
#define E_FAULT     14
#define E_BUSY      16
#define E_EXIST     17
#define E_NODEV     19
#define E_INVAL     22
#define E_NOSPC     28
#define E_RANGE     34
#define E_NOSYS     38
#define E_PROTO     71
#define E_OVERFLOW  75
#define E_NOTSUP    95
#define E_ADDR      99     /* buffer not reachable by the device's DMA mask */
#define E_TIMEDOUT 110
#define E_TIMEOUT  110     /* Alias for E_TIMEDOUT to handle both spellings */

/** @brief Human-readable name for an error code (sign ignored). */
static inline const char *kerr_str(int32_t err) {
    switch (err < 0 ? -err : err) {
    case 0:          return "ok";
    case E_PERM:     return "permission denied";
    case E_NOENT:    return "not found";
    case E_IO:       return "I/O error";
    case E_AGAIN:    return "try again";
    case E_NOMEM:    return "out of memory";
    case E_FAULT:    return "bad address";
    case E_BUSY:     return "busy";
    case E_EXIST:    return "already exists";
    case E_NODEV:    return "no such device";
    case E_INVAL:    return "invalid argument";
    case E_NOSPC:    return "no space";
    case E_RANGE:    return "out of range";
    case E_NOTSUP:   return "not supported";
    case E_ADDR:     return "address not DMA-reachable";
    case E_TIMEDOUT: return "timed out";
    default:         return "error";
    }
}

#endif