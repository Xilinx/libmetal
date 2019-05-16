/*
 * Copyright (c) 2019, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <unistd.h>
#include <metal/io.h>
#include <metal/device.h>
#include "common.h"
#include "sys_init.h"

/**
 * @brief main function of the demo application.
 *        Here are the steps for the main function:
 *        * initialize libmetal environment
 *        * Run the IPI with shared memory demo.
 *        * Run the shared memory demo.
 *        * Run the atomic across shared memory demo.
 *        * Run the ipi latency demo.
 *        * Run the shared memory latency demo.
 *        * Run the shared memory throughput demo.
 *        * Cleanup libmetal environment
 *        Report if any of the above tasks failed.
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

	ret = shmem_demo();
	if (ret) {
		LPERROR("shared memory demo failed.\n");
		goto out;
	}
out:
	sys_cleanup();

	return ret;
}
