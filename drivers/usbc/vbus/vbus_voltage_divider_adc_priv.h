/*
 * Copyright (c) 2022  The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_VBUS_VOLTAGE_DIVIDER_ADC_PRIV_H_
#define ZEPHYR_DRIVERS_VBUS_VOLTAGE_DIVIDER_ADC_PRIV_H_

#include <zephyr/drivers/adc.h>

/**
 * @brief Driver config
 */
struct vbus_config {
	uint32_t output_ohm;
	uint32_t full_ohm;
	struct adc_dt_spec adc_channel;
};

/**
 * @brief Driver data
 */
struct vbus_data {
	int sample;
	struct adc_sequence sequence;
};

#endif /* ZEPHYR_DRIVERS_VBUS_VOLTAGE_DIVIDER_ADC_PRIV_H_ */
