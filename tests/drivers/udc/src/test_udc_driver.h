/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_TEST_UDC_DRIVER_H
#define ZEPHYR_TEST_UDC_DRIVER_H

#include <ztest.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/usb/usb_ch9.h>

void test_udc_device_get(void);
void test_udc_not_initialized(void);
void test_udc_initialized(void);
void test_udc_enabled(void);
void test_udc_ep_bulk(void);
void test_udc_ep_int(void);
void test_udc_ep_iso(void);

#endif /* ZEPHYR_TEST_UDC_DRIVER_H */
