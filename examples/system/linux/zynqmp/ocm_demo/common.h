/*
 * Copyright (c) 2019, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#include <sys/types.h>
#include <metal/irq.h>
#include <metal/atomic.h>
#include <metal/cpu.h>
#include <stdio.h>

#define BUS_NAME        "platform"
#define SHM_DEV_NAME    "fffc0000.ocm"
#define LPRINTF(format, ...) \
  printf("MASTER> " format, ##__VA_ARGS__)

#define LPERROR(format, ...) LPRINTF("ERROR: " format, ##__VA_ARGS__)

/**
 * @brief shmem_demo() - Show use of shared memory with Libmetal.
 *        For NUM_TIMES times, send message to RPU and notify RPU by writing to
 *        share mem that RPU is polling. Once detected, RPU will then similarly
 *        write message and notify APU and the APU will then verify the
 *        response. If the message does not match expected response, record
 *        error. Afterwards, report test result and clean up.
 *        Notes:
 *        * The RPU will repeatedly wait for shared mem. from APU until APU
 *          notifies remote by changing the KEEP_GOING value in shared memory.
 *
 * @return - return 0 on success, otherwise return error number indicating
 *           type of error
 */
int shmem_demo();

/**
 * @brief wait_for_notified() - Loop until notified bit in channel is set.
 *
 * @param[in] notified - pointer to the notified variable
 */
static inline void  wait_for_notified(atomic_flag *notified)
{
	unsigned int flags;

	do {

		flags = metal_irq_save_disable();
		if (!atomic_flag_test_and_set(notified)) {
			metal_irq_restore_enable(flags);
			break;
		}
		metal_cpu_yield();
		metal_irq_restore_enable(flags);
	} while(1);
}

/**
 * @brief dump_buffer() - print hex value of each byte in the buffer
 *
 * @param[in] buf - pointer to the buffer
 * @param[in] len - len of the buffer
 */
static inline void dump_buffer(void *buf, unsigned int len)
{
	unsigned int i;
	unsigned char *tmp = (unsigned char *)buf;

	for (i = 0; i < len; i++) {
		printf(" %02x", *(tmp++));
		if (!(i % 20))
			printf("\n");
	}
	printf("\n");
}

/**
 * @brief print_demo() - print demo string
 *
 * @param[in] name - demo name
 */
static inline void print_demo(char *name)
{
	LPRINTF("****** libmetal demo: %s ******\n", name);
}

#endif /* __COMMON_H__ */
