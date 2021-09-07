/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_udc_driver.h"

void test_main(void)
{
	ztest_test_suite(udc_driver_test,
			 ztest_unit_test(test_udc_device_get),
			 ztest_unit_test(test_udc_not_initialized),
			 ztest_unit_test(test_udc_initialized),
			 ztest_unit_test(test_udc_enabled),
			 ztest_unit_test(test_udc_ep_bulk),
			 ztest_unit_test(test_udc_ep_int),
			 ztest_unit_test(test_udc_ep_iso)
			 );
	ztest_run_test_suite(udc_driver_test);
}
