/*
 * Copyright 2022 The Chromium OS Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief USBC VBUS device APIs
 *
 * This file contains the USBC VBUS device APIs.
 * All USBC VBUS measurment and control device drivers should
 * implement the APIs described in this file.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_USBC_VBUS_H_
#define ZEPHYR_INCLUDE_DRIVERS_USBC_VBUS_H_

/**
 * @brief USBC VBUS API
 * @defgroup usbc_vbus_api USBC VBUS API
 * @ingroup io_interfaces
 * @{
 */

#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/usbc/usbc_tc.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vbus_driver_api {
	bool (*vbus_check_level)(const struct device *dev, enum tc_vbus_level level);
	int (*vbus_measure)(const struct device *dev, int *vbus_meas);
	int (*vbus_discharge)(const struct device *dev, bool enable);
	int (*vbus_auto_discharge_disconnect)(const struct device *dev, bool enable);
};

/**
 * @brief Checks if VBUS is at a particular level
 *
 * @param dev    Runtime device structure
 * @param level  The level voltage to check against
 *
 * @retval true if VBUS is at the level voltage
 * @retval false if VBUS is not at that level voltage
 */
static inline bool vbus_check_level(const struct device *dev, enum tc_vbus_level level)
{
	const struct vbus_driver_api *api = (const struct vbus_driver_api *)dev->api;

	__ASSERT(api->vbus_check_level != NULL, "Callback pointer should not be NULL");

	return api->vbus_check_level(dev, level);
}

/**
 * @brief Reads and returns VBUS measured in mV
 *
 * @param dev        Runtime device structure
 * @param meas       pointer where the measured VBUS voltage is stored
 *
 * @retval 0 on success
 * @retval -EIO on failure
 */
static inline int vbus_measure(const struct device *dev, int *meas)
{
	const struct vbus_driver_api *api = (const struct vbus_driver_api *)dev->api;

	__ASSERT(api->vbus_measure != NULL, "Callback pointer should not be NULL");

	return api->vbus_measure(dev, meas);
}

/**
 * @brief Discharge VBUS
 *
 * @param dev        Runtime device structure
 * @param enable     Discharge VBUS when true
 *
 * @retval 0 on success
 * @retval -EIO on failure
 * @retval -ENOSYS if not implemented
 */
static inline int vbus_discharge(const struct device *dev, bool enable)
{
	const struct vbus_driver_api *api = (const struct vbus_driver_api *)dev->api;

	if (api->vbus_discharge == NULL) {
		return -ENOSYS;
	}

	return api->vbus_discharge(dev, enable);
}

/**
 * @brief Automatically discharge TypeC VBUS on Source / Sink disconnect
 *       and power role swap
 *
 * @param dev     Runtime device structure
 * @param enable  Automatically discharges VBUS on disconnect or
 *                power role swap when true
 *
 * @retval 0 on success
 * @retval -EIO on failure
 * @retval -ENOSYS if not implemented
 */
static inline int vbus_auto_discharge_disconnect(const struct device *dev, bool enable)
{
	const struct vbus_driver_api *api = (const struct vbus_driver_api *)dev->api;

	if (api->vbus_auto_discharge_disconnect == NULL) {
		return -ENOSYS;
	}

	return api->vbus_auto_discharge_disconnect(dev, enable);
}

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_USBC_VBUS_H_ */
