/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <cmsis_os2.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>

uint32_t tick;
osStatus_t status_val;

typedef struct {
	osVersion_t os_info;
	char info[100];
} versionInfo;

void get_version_check(const void *param)
{
	char infobuf[100];
	osVersion_t osv;
	osStatus_t status;
	versionInfo *version_i = (versionInfo *)param;

	status = osKernelGetInfo(&osv, infobuf, sizeof(infobuf));
	if (status == osOK) {
		version_i->os_info.api = osv.api;
		version_i->os_info.kernel = osv.kernel;
		strcpy(version_i->info, infobuf);
	}
}

void lock_unlock_check(const void *arg)
{
	int32_t state_before_lock, state_after_lock, current_state;

	ARG_UNUSED(arg);

	state_before_lock = osKernelLock();
	if (k_is_in_isr()) {
		zassert_true(state_before_lock == osErrorISR, NULL);
	}

	state_after_lock = osKernelUnlock();
	if (k_is_in_isr()) {
		zassert_true(state_after_lock == osErrorISR, NULL);
	} else {
		zassert_true(state_before_lock == !state_after_lock, NULL);
	}
	current_state = osKernelRestoreLock(state_before_lock);
	if (k_is_in_isr()) {
		zassert_true(current_state == osErrorISR, NULL);
	} else {
		zassert_equal(current_state, state_before_lock, NULL);
	}
}

ZTEST(cmsis_kernel, test_kernel_apis)
{
	versionInfo version = {
		.os_info = {
			.api = 0xfefefefe,
			.kernel = 0xfdfdfdfd,
		},
		.info = "local function call info is uninitialized"
	};
	versionInfo version_irq = {
		.os_info = {
			.api = 0xfcfcfcfc,
			.kernel = 0xfbfbfbfb,
		},
		.info = "irq_offload function call info is uninitialized"
	};

	get_version_check(&version);
	irq_offload(get_version_check, (const void *)&version_irq);

	/* Check if the version value retrieved in ISR and thread is same */
	zassert_equal(strcmp(version.info, version_irq.info), 0, NULL);
	zassert_equal(version.os_info.api, version_irq.os_info.api, NULL);
	zassert_equal(version.os_info.kernel, version_irq.os_info.kernel, NULL);

	lock_unlock_check(NULL);

	irq_offload(lock_unlock_check, NULL);
}

void delay_until(const void *param)
{
	ARG_UNUSED(param);

	tick = osKernelGetTickCount();
	tick += 50U;

	status_val = osDelayUntil(tick);
}

ZTEST(cmsis_kernel, test_delay)
{
	delay_until(NULL);
	zassert_true(tick <= osKernelGetTickCount(), NULL);
	zassert_equal(status_val, osOK, NULL);

	irq_offload(delay_until, NULL);
	zassert_equal(status_val, osErrorISR, NULL);
}
ZTEST_SUITE(cmsis_kernel, NULL, NULL, NULL, NULL, NULL);
