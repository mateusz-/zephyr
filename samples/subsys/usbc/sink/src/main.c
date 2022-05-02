/*
 * Copyright (c) 2022 The Chromium OS Authors.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/zephyr.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usbc/usbc.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define PORT1_NODE DT_NODELABEL(port1)

/* usbc.rst port data object start */
/**
 * @brief A structure that encapsulates Port data.
 */
static struct port1_data_t {
	/** Sink Capabilities */
	uint32_t snk_caps[1];
	/** Number of Sink Capabilities */
	int snk_cap_cnt;
	/** Source Capabilities */
	uint32_t src_caps[PDO_MAX_DATA_OBJECTS];
	/** Number of Source Capabilities */
	int src_cap_cnt;
	/* Power Supply Ready flag */
	atomic_t ps_ready;
} port1_data;
/* usbc.rst port data object end */

/**
 * @brief Builds a Request Data Object (RDO) with the following properties:
 *		- Maximum operating current 100mA
 *		- Operating current is 100mA
 *		- Unchunked Extended Messages Not Supported
 *		- No USB Suspend
 *		- Not USB Communications Capable
 *		- No capability mismatch
 *		- Does not Giveback
 *		- Select object position 1 (5V Power Data Object (PDO))
 */
static uint32_t build_request_data_object(struct port1_data_t *dpm_data)
{
	union pd_rdo rdo;

	/* Maximum operating current 100mA (GIVEBACK = 0) */
	rdo.fixed.min_or_max_operating_current = PD_CONVERT_MA_TO_FIXED_PDO_CURRENT(100);
	/* Operating current 100mA */
	rdo.fixed.operating_current = PD_CONVERT_MA_TO_FIXED_PDO_CURRENT(100);
	/* Unchunked Extended Messages Not Supported */
	rdo.fixed.unchunked_ext_msg_supported = 0;
	/* No USB Suspend */
	rdo.fixed.no_usb_suspend = 1;
	/* Not USB Communications Capable */
	rdo.fixed.usb_comm_capable = 0;
	/* No capability mismatch */
	rdo.fixed.cap_mismatch = 0;
	/* Don't giveback */
	rdo.fixed.giveback = 0;
	/* Object position 1 (5V PDO) */
	rdo.fixed.object_pos = 1;

	return rdo.raw_value;
}

/**
 * @brief Display a single Power Delivery Object (PDO)
 */
static void display_pdo(int idx, uint32_t pdo_value)
{
	union pd_fixed_supply_pdo_source pdo;

	/* Default to fixed supply pdo source until type is detected */
	pdo.raw_value = pdo_value;

	LOG_INF("PDO %d:", idx);
	switch (pdo.type) {
	case PDO_FIXED: {
		LOG_INF("\tType:              FIXED");
		LOG_INF("\tCurrent:           %d",
		       PD_CONVERT_FIXED_PDO_CURRENT_TO_MA(pdo.max_current));
		LOG_INF("\tVoltage:           %d",
		       PD_CONVERT_FIXED_PDO_VOLTAGE_TO_MV(pdo.voltage));
		LOG_INF("\tPeak Current:      %d", pdo.peak_current);
		LOG_INF("\tUchunked Support:  %d",
		       pdo.unchunked_ext_msg_supported);
		LOG_INF("\tDual Role Data:    %d",
		       pdo.dual_role_data);
		LOG_INF("\tUSB Comms:         %d",
		       pdo.usb_comms_capable);
		LOG_INF("\tUnconstrained Pwr: %d",
		       pdo.unconstrained_power);
		LOG_INF("\tUSB Suspend:       %d",
		       pdo.usb_suspend_supported);
		LOG_INF("\tDual Role Power:   %d",
		       pdo.dual_role_power);
	}
	break;
	case PDO_BATTERY: {
		union pd_battery_supply_pdo_source pdo;

		pdo.raw_value = pdo_value;
		LOG_INF("\tType:              BATTERY");
		LOG_INF("\tMin Voltage: %d",
			PD_CONVERT_BATTERY_PDO_VOLTAGE_TO_MV(pdo.min_voltage));
		LOG_INF("\tMax Voltage: %d",
			PD_CONVERT_BATTERY_PDO_VOLTAGE_TO_MV(pdo.max_voltage));
		LOG_INF("\tMax Power:   %d",
			PD_CONVERT_BATTERY_PDO_POWER_TO_MW(pdo.max_power));
	}
	break;
	case PDO_VARIABLE: {
		union pd_variable_supply_pdo_source pdo;

		pdo.raw_value = pdo_value;
		LOG_INF("\tType:        VARIABLE");
		LOG_INF("\tMin Voltage: %d",
			PD_CONVERT_VARIABLE_PDO_VOLTAGE_TO_MV(pdo.min_voltage));
		LOG_INF("\tMax Voltage: %d",
			PD_CONVERT_VARIABLE_PDO_VOLTAGE_TO_MV(pdo.max_voltage));
		LOG_INF("\tMax Current: %d",
			PD_CONVERT_VARIABLE_PDO_CURRENT_TO_MA(pdo.max_current));
	}
	break;
	case PDO_AUGMENTED: {
		union pd_augmented_supply_pdo_source pdo;

		pdo.raw_value = pdo_value;
		LOG_INF("\tType:              AUGMENTED");
		LOG_INF("\tMin Voltage:       %d",
			PD_CONVERT_AUGMENTED_PDO_VOLTAGE_TO_MV(pdo.min_voltage));
		LOG_INF("\tMax Voltage:       %d",
			PD_CONVERT_AUGMENTED_PDO_VOLTAGE_TO_MV(pdo.max_voltage));
		LOG_INF("\tMax Current:       %d",
			PD_CONVERT_AUGMENTED_PDO_CURRENT_TO_MA(pdo.max_current));
		LOG_INF("\tPPS Power Limited: %d", pdo.pps_power_limited);
	}
	break;
	}
}

/**
 * @brief Display Source Capabilities
 */
static void display_source_caps(const struct device *dev)
{
	struct port1_data_t *dpm_data = usbc_get_dpm_data(dev);

	LOG_INF("Source Caps:");
	for (int i = 0; i < dpm_data->src_cap_cnt; i++) {
		display_pdo(i, dpm_data->src_caps[i]);
		k_msleep(50);
	}
}

/* usbc.rst callbacks start */
/**
 * @brief Port Policy Callback to Get Sink Capabilities
 */
static int usbc_port1_policy_cb_get_snk_cap(const struct device *dev,
					    uint32_t **pdos, int *num_pdos)
{
	struct port1_data_t *dpm_data = usbc_get_dpm_data(dev);

	*pdos = dpm_data->snk_caps;
	num_pdos = &dpm_data->snk_cap_cnt;

	return 0;
}

/**
 * @brief Port Policy Callback for Set Source Capabilities
 */
static void usbc_port1_policy_cb_set_src_cap(const struct device *dev, uint32_t *pdos, int num_pdos)
{
	struct port1_data_t *dpm_data;
	int i;

	dpm_data = usbc_get_dpm_data(dev);

	if (num_pdos > PDO_MAX_DATA_OBJECTS) {
		num_pdos = PDO_MAX_DATA_OBJECTS;
	}

	for (i = 0; i < num_pdos; i++) {
		dpm_data->src_caps[i] = *(pdos + i);
	}

	dpm_data->src_cap_cnt = num_pdos;
}

/**
 * @brief Port Policy Callback to get Request Data Object (RDO)
 */
static uint32_t usbc_port1_policy_cb_get_request_data_object(const struct device *dev)
{
	struct port1_data_t *dpm_data = usbc_get_dpm_data(dev);

	return build_request_data_object(dpm_data);
}
/* usbc.rst callbacks end */

/* usbc.rst notify start */
/**
 * @brief Port Policy Callback to notify the application of an event that occurred on this port
 */
static void usbc_port1_notify(const struct device *dev, enum policy_notify_t policy_notify)
{
	struct port1_data_t *dpm_data = usbc_get_dpm_data(dev);

	switch (policy_notify) {
	case PROTOCOL_ERROR:
		break;
	case MSG_DISCARDED:
		break;
	case MSG_ACCEPT_RECEIVED:
		break;
	case MSG_REJECTED_RECEIVED:
		break;
	case MSG_NOT_SUPPORTED_RECEIVED:
		break;
	case TRANSITION_PS:
		atomic_set_bit(&dpm_data->ps_ready, 0);
		break;
	case PD_CONNECTED:
		break;
	case NOT_PD_CONNECTED:
		break;
	case POWER_CHANGE_0A0:
		LOG_INF("PWR 0A\n");
		break;
	case POWER_CHANGE_DEF:
		LOG_INF("PWR DEF\n");
		break;
	case POWER_CHANGE_1A5:
		LOG_INF("PWR 1A5\n");
		break;
	case POWER_CHANGE_3A0:
		LOG_INF("PWR 3A0\n");
		break;
	case DATA_ROLE_IS_UFP:
		break;
	case DATA_ROLE_IS_DFP:
		break;
	case PORT_PARTNER_NOT_RESPONSIVE:
		LOG_INF("Port Partner not PD Capable\n");
		break;
	case SNK_TRANSITION_TO_DEFAULT:
		break;
	case HARD_RESET_RECEIVED:
		break;
	}
}
/* usbc.rst notify end */

/* usbc.rst check start */
/**
 * @brief Port Policy Callback to check if the USBC subsystem should take an action
 */
bool usbc_port1_policy_check(const struct device *dev, enum policy_check_t policy_check)
{
	switch (policy_check) {
	case CHECK_POWER_ROLE_SWAP:
		/* Reject power role swaps */
		return false;
	case CHECK_DATA_ROLE_SWAP_TO_DFP:
		/* Reject data role swap to DFP */
		return false;
	case CHECK_DATA_ROLE_SWAP_TO_UFP:
		/* Accept data role swap to UFP */
		return true;
	case CHECK_SNK_AT_DEFAULT_LEVEL:
		/* This device is always at the default power level */
		return true;
	default:
		/* Reject all other policy checks */
		return false;

	}
}
/* usbc.rst check end */

/* usbc.rst init sink caps start */
/**
 * @brief Initialize this port's Sink Capabilities
 */
static void init_port1_snk_caps(void)
{
	union pd_fixed_supply_pdo_sink fixed_supply_pdo_sink;

	fixed_supply_pdo_sink.operational_current = PD_CONVERT_MA_TO_FIXED_PDO_CURRENT(100);
	fixed_supply_pdo_sink.voltage = PD_CONVERT_MV_TO_FIXED_PDO_VOLTAGE(5000);
	fixed_supply_pdo_sink.frs_required = FRS_NOT_SUPPORTED;
	fixed_supply_pdo_sink.dual_role_data = false;
	fixed_supply_pdo_sink.usb_comms_capable = false;
	fixed_supply_pdo_sink.unconstrained_power = false;
	fixed_supply_pdo_sink.higher_capability = false;
	fixed_supply_pdo_sink.dual_role_power = false;
	fixed_supply_pdo_sink.type = PDO_FIXED;

	port1_data.snk_caps[0] = fixed_supply_pdo_sink.raw_value;
	port1_data.snk_cap_cnt = 1;
}
/* usbc.rst init sink caps end */

void main(void)
{
	const struct device *usbc_port1;

	/* Get the device for this port */
	usbc_port1 = DEVICE_DT_GET(PORT1_NODE);
	if (!device_is_ready(usbc_port1)) {
		LOG_ERR("PORT1 device not ready\n");
		return;
	}

	/* Initialize the Sink Capabilities */
	init_port1_snk_caps();

	/* usbc.rst register start */
	/* Register USBC Callbacks */

	/* Register Policy Check callback */
	usbc_set_policy_cb_check(usbc_port1, usbc_port1_policy_check);
	/* Register Policy Notify callback */
	usbc_set_policy_cb_notify(usbc_port1, usbc_port1_notify);
	/* Register Policy Get Sink Capabilities callback */
	usbc_set_policy_cb_get_snk_cap(usbc_port1, usbc_port1_policy_cb_get_snk_cap);
	/* Register Policy Set Source Capabilities callback */
	usbc_set_policy_cb_set_src_cap(usbc_port1, usbc_port1_policy_cb_set_src_cap);
	/* Register Policy Get Request Data Object callback */
	usbc_set_policy_cb_get_request_data_object(usbc_port1,
						   usbc_port1_policy_cb_get_request_data_object);
	/* usbc.rst register end */

	/* usbc.rst user data start */
	/* Set Application port data object. This object is passed to the policy callbacks */
	port1_data.ps_ready = ATOMIC_INIT(0);
	usbc_set_dpm_data(usbc_port1, &port1_data);
	/* usbc.rst user data end */

	/* usbc.rst usbc start */
	/* Start the USBC Subsystem */
	usbc_start(usbc_port1);
	/* usbc.rst usbc end */

	while (1) {
		/* Perform Application Specific functions */
		if (atomic_test_and_clear_bit(&port1_data.ps_ready, 0)) {
			/* Display the Source Capabilities */
			display_source_caps(usbc_port1);
		}

		/* Arbitrary delay */
		k_msleep(1000);
	}
}
