/*
 * Copyright 2022 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT vbus_voltage_divider_adc

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(vbus_voltage_divider_adc, CONFIG_USBC_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/usbc/usbc_pd.h>
#include <zephyr/drivers/usbc/usbc_vbus.h>
#include <soc.h>
#include <stddef.h>

#include "vbus_voltage_divider_adc_priv.h"

/**
 * @brief Reads and returns VBUS measured in mV
 *
 * @retval 0 on success
 * @retval -EIO on failure
 */
static int adc_vbus_measure(const struct device *dev, int *meas)
{
	const struct vbus_config *const config = dev->config;
	struct vbus_data *data = dev->data;
	int value;
	int ret;

	__ASSERT(meas != NULL, "ADC VBUS meas must not be NULL");

	ret = adc_read(config->adc_channel.dev, &data->sequence);
	if (ret != 0) {
		LOG_INF("ADC reading failed with error %d.", ret);
		return ret;
	}

	value = data->sample;
	ret = adc_raw_to_millivolts_dt(&config->adc_channel, &value);
	if (ret != 0) {
		LOG_INF("Scaling ADC failed with error %d.", ret);
		return ret;
	}

	/* VBUS is scaled down though a voltage divider */
	*meas = (value * 1000) / ((config->output_ohm * 1000) / config->full_ohm);

	return 0;
}

/**
 * @brief Checks if VBUS is at a particular level
 *
 * @retval true if VBUS is at the level voltage, else false
 */
static bool adc_vbus_check_level(const struct device *dev, enum tc_vbus_level level)
{
	int meas;
	int ret;

	ret = adc_vbus_measure(dev, &meas);
	if (ret) {
		return false;
	}

	switch (level) {
	case TC_VBUS_SAFE0V:
		return (meas < PD_V_SAFE_0V_MAX_MV);
	case TC_VBUS_PRESENT:
		return (meas >= PD_V_SAFE_5V_MIN_MV);
	case TC_VBUS_REMOVED:
		return (meas < TC_V_SINK_DISCONNECT_MAX_MV);
	}

	return false;
}

/**
 * @brief Initializes the TCPC
 *
 * @retval 0 on success
 * @retval -EIO on failure
 */
static int adc_vbus_init(const struct device *dev)
{
	const struct vbus_config *const config = dev->config;
	struct vbus_data *data = dev->data;
	int ret;

	data->sequence.buffer = &data->sample;
	data->sequence.buffer_size = sizeof(data->sample);

	ret = adc_channel_setup_dt(&config->adc_channel);
	if (ret < 0) {
		LOG_INF("Could not setup channel (%d)\n", ret);
		return -EIO;
	}

	ret = adc_sequence_init_dt(&config->adc_channel, &data->sequence);
	if (ret < 0) {
		LOG_INF("Could not init sequence (%d)\n", ret);
		return -EIO;
	}

	return 0;
}

static const struct vbus_driver_api driver_api = {
	.vbus_measure = adc_vbus_measure,
	.vbus_check_level = adc_vbus_check_level
};

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0,
	     "No compatible USBC VBUS Measurement instance found");

#define DRIVER_INIT(inst)						\
	static struct vbus_data drv_data_##inst;			\
	static const struct vbus_config drv_config_##inst = {		\
		.output_ohm = DT_INST_PROP(inst, output_ohms),		\
		.full_ohm = DT_INST_PROP(inst, full_ohms),		\
		.adc_channel = ADC_DT_SPEC_INST_GET(inst)		\
	};								\
	DEVICE_DT_INST_DEFINE(inst,					\
			      &adc_vbus_init,				\
			      NULL,					\
			      &drv_data_##inst,				\
			      &drv_config_##inst,			\
			      POST_KERNEL,				\
			      CONFIG_USBC_INIT_PRIORITY,		\
			      &driver_api);

DT_INST_FOREACH_STATUS_OKAY(DRIVER_INIT)
