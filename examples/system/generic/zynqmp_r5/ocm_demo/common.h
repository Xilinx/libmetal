 /*
 * Copyright (c) 2019, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <metal/atomic.h>
#include <metal/alloc.h>
#include <metal/irq.h>
#include <errno.h>
#include <metal/sys.h>
#include <metal/cpu.h>
#include <metal/io.h>
#include <metal/device.h>
#include <sys/types.h>
#include <xil_printf.h>
#include "sys_init.h"

/* Devices names */
#define BUS_NAME        "generic"
#define SHM_DEV_NAME    "0xfffc0000.ocm"

#define LPRINTF(format, ...) \
  xil_printf("\r\nSERVER> " format, ##__VA_ARGS__)

#define LPERROR(format, ...) xil_printf("ERROR: " format, ##__VA_ARGS__)

extern struct metal_device *shm_dev; /* SHM metal device */

/**
 * @brief master_shmem_demo() - Show use of shared memory with Libmetal.
 *        Until KEEP_GOING signal is stopped, keep looping.
 *        In the loop, read message from remote, add one to message and
 *        then respond. After the loop, cleanup resources.
 *
 * @return - return 0 on success, otherwise return error number indicating
 *           type of error
 */
int master_shmem_demo();

/**
 * @brief slave_shmem_demo() - Show use of shared memory with Libmetal.
 *        Until KEEP_GOING signal is stopped, keep looping.
 *        In the loop, send message to remote, validate and
 *        then respond. After the loop, cleanup resources.
 *
 * @return - return 0 on success, otherwise return error number indicating
 *           type of error
 */
int slave_shmem_demo();

/**
 * @brief print_demo() - print demo string
 *
 * @param[in] name - demo name
 */
static inline void print_demo(char *name)
{
	LPRINTF("====== libmetal demo: %s ======\n", name);
}

#endif /* __COMMON_H__ */

