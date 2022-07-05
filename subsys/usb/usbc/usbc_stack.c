/*
 * Copyright (c) 2022 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT usbc_port

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/smf.h>
#include <zephyr/usbc/usbc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "usbc_stack.h"

LOG_MODULE_REGISTER(usbc_stack);

static const struct usbc_subsystem_api usbc_api;
static int usbc_subsys_init(const struct device *dev);

static ALWAYS_INLINE void usbc_handler(void *port_dev)
{
	const struct device *dev = (const struct device *)port_dev;
	struct usbc_port_data *port = dev->data;
	struct request_value *req;
	int32_t request;

	req = k_fifo_get(&port->request_fifo, K_NO_WAIT);
	request = (req != NULL) ? req->val : REQUEST_NOP;
	pe_run(dev, request);
	prl_run(dev);
	tc_run(dev, request);

	if (request == PRIV_PORT_REQUEST_SUSPEND) {
		k_thread_suspend(port->port_thread);
	}

	k_msleep(CONFIG_USBC_STATE_MACHINE_CYCLE_TIME);
}

#define USBC_SUBSYS_INIT(inst)							\
	K_THREAD_STACK_DEFINE(my_stack_area_##inst,				\
			      CONFIG_USBC_STACK_SIZE);				\
										\
	static struct tc_sm_t tc_##inst;					\
	static struct policy_engine pe_##inst;					\
	static struct protocol_layer_rx_t prl_rx_##inst;			\
	static struct protocol_layer_tx_t prl_tx_##inst;			\
	static struct protocol_hard_reset_t prl_hr_##inst;			\
										\
	static void run_usbc_##inst(void *port_dev,				\
				    void *unused1,				\
				    void *unused2)				\
	{									\
		while (1) {							\
			usbc_handler(port_dev);					\
		}								\
	}									\
										\
	static void create_thread_##inst(const struct device *dev)		\
	{									\
		struct usbc_port_data *port = dev->data;			\
										\
		port->port_thread = k_thread_create(&port->thread_data,		\
				my_stack_area_##inst,				\
				K_THREAD_STACK_SIZEOF(my_stack_area_##inst),	\
				run_usbc_##inst,				\
				(void *)dev, 0, 0,				\
				CONFIG_USBC_THREAD_PRIORITY,			\
				K_ESSENTIAL,					\
				K_NO_WAIT);					\
		k_thread_suspend(port->port_thread);				\
	}									\
										\
	static struct usbc_port_data usbc_port_data_##inst = {			\
		.tc = &tc_##inst,						\
		.pe = &pe_##inst,						\
		.prl_rx = &prl_rx_##inst,					\
		.prl_tx = &prl_tx_##inst,					\
		.prl_hr = &prl_hr_##inst,					\
		.tcpc = DEVICE_DT_GET(DT_INST_PROP(inst, tcpc)),		\
		.vbus = DEVICE_DT_GET(DT_INST_PROP(inst, vbus)),		\
	};									\
										\
	static const struct usbc_port_config usbc_port_config_##inst = {	\
		.create_thread = create_thread_##inst,				\
	};									\
										\
	DEVICE_DT_INST_DEFINE(inst,						\
			      &usbc_subsys_init,				\
			      NULL,						\
			      &usbc_port_data_##inst,				\
			      &usbc_port_config_##inst,				\
			      APPLICATION,					\
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,		\
			      &usbc_api);

DT_INST_FOREACH_STATUS_OKAY(USBC_SUBSYS_INIT)

/**
 * @brief Called by the Device Policy Manager to start the USBC Subsystem
 */
static int start(const struct device *dev)
{
	struct usbc_port_data *port = dev->data;

	/* Add private start request to fifo */
	port->request.val = PRIV_PORT_REQUEST_START;
	k_fifo_put(&port->request_fifo, &port->request);

	/* Start the port thread */
	k_thread_resume(port->port_thread);

	return 0;
}

/**
 * @brief Called by the Device Policy Manager to suspend the USBC Subsystem
 */
static int suspend(const struct device *dev)
{
	struct usbc_port_data *port = dev->data;

	/* Add private suspend request to fifo */
	port->request.val = PRIV_PORT_REQUEST_SUSPEND;
	k_fifo_put(&port->request_fifo, &port->request);

	return 0;
}

/**
 * @brief Called by the Device Policy Manager to make a request of the USBC Subsystem
 */
static int request(const struct device *dev, enum policy_request_t req)
{
	struct usbc_port_data *port = dev->data;

	/* Add public request to fifo */
	port->request.val = req;
	k_fifo_put(&port->request_fifo, &port->request);

	return 0;
}

/**
 * @brief Sets the Device Policy Manager's data
 */
static void set_dpm_data(const struct device *dev, void *dpm_data)
{
	struct usbc_port_data *port = dev->data;

	port->dpm_data = dpm_data;
}

/**
 * @brief Gets the Device Policy Manager's data
 */
static void *get_dpm_data(const struct device *dev)
{
	struct usbc_port_data *port = dev->data;

	return port->dpm_data;
}

/**
 * @brief Set the callback that gets the Sink Capabilities from the Device Policy Manager
 */
static void set_policy_cb_get_snk_cap(const struct device *dev,
				      policy_cb_get_snk_cap_t policy_cb_get_snk_cap)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_get_snk_cap = policy_cb_get_snk_cap;
}

/**
 * @brief Set the callback that sends the received Source Capabilities to the Device Policy Manager
 */
static void set_policy_cb_set_src_cap(const struct device *dev,
				      policy_cb_set_src_cap_t policy_cb_set_src_cap)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_set_src_cap = policy_cb_set_src_cap;
}

/**
 * @brief Set the callback for the Device Policy Manager policy check
 */
static void set_policy_cb_check(const struct device *dev, policy_cb_check_t policy_cb_check)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_check = policy_cb_check;
}

/**
 * @brief Set the callback for the Device Policy Manager policy change notify
 */
static void set_policy_cb_notify(const struct device *dev, policy_cb_notify_t policy_cb_notify)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_notify = policy_cb_notify;
}

/**
 * @brief Set the callback for the Device Policy Manager policy change notify
 */
static void set_policy_cb_wait_notify(const struct device *dev,
				      policy_cb_wait_notify_t policy_cb_wait_notify)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_wait_notify = policy_cb_wait_notify;
}

/**
 * @brief Set the callback for requesting the data object (RDO)
 */
static void set_policy_cb_get_request_data_object(const struct device *dev,
			policy_cb_get_request_data_object_t policy_cb_get_request_data_object)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_get_request_data_object = policy_cb_get_request_data_object;
}

/**
 * @brief Set the callback for checking if Sink Power Supply is at default level
 */
static void set_policy_cb_is_snk_at_default_level(const struct device *dev,
			policy_cb_is_snk_at_default_level_t policy_cb_is_snk_at_default_level)
{
	struct usbc_port_data *port = dev->data;

	port->policy_cb_is_snk_at_default_level = policy_cb_is_snk_at_default_level;
}

/**
 * @brief Initialize the USBC Subsystem
 */
static int usbc_subsys_init(const struct device *dev)
{
	struct usbc_port_data *port = dev->data;
	const struct usbc_port_config *const config = dev->config;
	const struct device *tcpc = port->tcpc;

	/* Make sure TCPC is ready */
	if (!device_is_ready(tcpc)) {
		LOG_ERR("TCPC NOT READY");
		return -ENODEV;
	}

	/* Initialize the state machines */
	tc_subsys_init(dev);
	pe_subsys_init(dev);
	prl_subsys_init(dev);

	/* Initialize the request fifo */
	k_fifo_init(&port->request_fifo);

	/* Create the thread for this port */
	config->create_thread(dev);
	return 0;
}

static const struct usbc_subsystem_api usbc_api = {
	.start = start,
	.suspend = suspend,
	.request = request,
	.set_dpm_data = set_dpm_data,
	.get_dpm_data = get_dpm_data,
	.set_policy_cb_check = set_policy_cb_check,
	.set_policy_cb_get_snk_cap = set_policy_cb_get_snk_cap,
	.set_policy_cb_set_src_cap = set_policy_cb_set_src_cap,
	.set_policy_cb_notify = set_policy_cb_notify,
	.set_policy_cb_wait_notify = set_policy_cb_wait_notify,
	.set_policy_cb_get_request_data_object = set_policy_cb_get_request_data_object,
	.set_policy_cb_is_snk_at_default_level = set_policy_cb_is_snk_at_default_level,
};
