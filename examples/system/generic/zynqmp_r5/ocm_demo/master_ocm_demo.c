/*
 * Copyright (c) 2019, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

 /***************************************************************************
  * master_ocm_demo.c
  *
  * This application shows how to use OCM with libmetal API for 
  * communication between processors.
  *
  * This app does the following:
  * 1.  Initialize the platform hardware such as UART, GIC.
  * 2.  Register shared memory descriptor device and shared memory
  *     device with libmetal in the intialization.
  * 3.  In the main application it does the following,
  *     * open the registered libmetal devices: shared memory
  *       descriptor device and shared memory device.
  *     * Map the shared memory descriptor as non-cached memory.
  *     * Map the shared memory as non-cached memory. If you do not map the
  *       shared memory as non-cached memory, make sure you flush the cache,
  *       before you notify the remote.
  */

#include <unistd.h>
#include <metal/atomic.h>
#include <metal/io.h>
#include <metal/device.h>
#include <metal/irq.h>
#include "common.h"

/**
 * @brief    main function of the demo application.
 *           Here are the steps for the main function:
 *           * Setup libmetal resources
 *           * Run the shared memory demo.
 *           * Cleanup libmetal resources
 *           Report if any of the above demos failed.
 * @return   0 - succeeded, non-zero for failures.
 */
int main(void)
{
	int ret;

	ret = sys_init();

	if (ret) {
		LPERROR("Failed to initialize system.\n");
		return ret;
	}

	ret = master_shmem_demo();
	if (ret){
		LPERROR("shared memory demo failed.\n");
		return ret;
	}

	sys_cleanup();
	return ret;
}

