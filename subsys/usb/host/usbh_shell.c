/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbh.h>

#include "usbh_internal.h"
#include "usbh_ch9.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usbh_shell, CONFIG_USBH_LOG_LEVEL);

#define FOOBAZ_VREQ_OUT		0x5b
#define FOOBAZ_VREQ_IN		0x5c

const static struct shell *ctx_shell;
static const struct device *uhc_dev;

static void print_dev_desc(const struct shell *sh,
			   const struct usb_device_descriptor *const desc)
{
	shell_print(sh, "bLength\t\t\t%u", desc->bLength);
	shell_print(sh, "bDescriptorType\t\t%u", desc->bDescriptorType);
	shell_print(sh, "bcdUSB\t\t\t%x", desc->bcdUSB);
	shell_print(sh, "bDeviceClass\t\t%u", desc->bDeviceClass);
	shell_print(sh, "bDeviceSubClass\t\t%u", desc->bDeviceSubClass);
	shell_print(sh, "bDeviceProtocol\t\t%u", desc->bDeviceProtocol);
	shell_print(sh, "bMaxPacketSize0\t\t%u", desc->bMaxPacketSize0);
	shell_print(sh, "idVendor\t\t%x", desc->idVendor);
	shell_print(sh, "idProduct\t\t%x", desc->idProduct);
	shell_print(sh, "bcdDevice\t\t%x", desc->bcdDevice);
	shell_print(sh, "iManufacturer\t\t%u", desc->iManufacturer);
	shell_print(sh, "iProduct\t\t%u", desc->iProduct);
	shell_print(sh, "iSerial\t\t\t%u", desc->iSerialNumber);
	shell_print(sh, "bNumConfigurations\t%u", desc->bNumConfigurations);
}

static int bazfoo_request(struct usbh_contex *const ctx,
			  struct uhc_transfer *const xfer,
			  int err)
{
	const struct device *dev = ctx->dev;

	shell_info(ctx_shell, "host: transfer finished %p, err %d", xfer, err);

	while (!k_fifo_is_empty(&xfer->done)) {
		struct net_buf *buf;

		buf = net_buf_get(&xfer->done, K_NO_WAIT);
		if (buf) {
			if (xfer->ep == USB_CONTROL_EP_IN &&
			    buf->len == sizeof(struct usb_device_descriptor)) {
				print_dev_desc(ctx_shell, (void *)buf->data);
			} else {
				shell_hexdump(ctx_shell, buf->data, buf->len);
			}

			uhc_xfer_buf_free(dev, buf);
		}
	}

	return uhc_xfer_free(dev, xfer);
}

static int bazfoo_connected(struct usbh_contex *const uhs_ctx)
{
	shell_info(ctx_shell, "host: USB device connected");

	return 0;
}

static int bazfoo_removed(struct usbh_contex *const uhs_ctx)
{
	shell_info(ctx_shell, "host: USB device removed");

	return 0;
}

static int bazfoo_rwup(struct usbh_contex *const uhs_ctx)
{
	shell_info(ctx_shell, "host: Bus remote wakeup event");

	return 0;
}

static int bazfoo_suspended(struct usbh_contex *const uhs_ctx)
{
	shell_info(ctx_shell, "host: Bus suspended");

	return 0;
}

static int bazfoo_resumed(struct usbh_contex *const uhs_ctx)
{
	shell_info(ctx_shell, "host: Bus resumed");

	return 0;
}

USBH_DEFINE_CLASS(bazfoo) = {
	.request = bazfoo_request,
	.connected = bazfoo_connected,
	.removed = bazfoo_removed,
	.rwup = bazfoo_rwup,
	.suspended = bazfoo_suspended,
	.resumed = bazfoo_resumed,
};

static uint8_t vreq_test_buf[1024] = { 0x7b, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0xd4, 0xff, 0x0f, 0x7d };

static int cmd_bulk(const struct shell *sh, size_t argc, char **argv)
{
	struct uhc_transfer *xfer;
	struct net_buf *buf;
	uint8_t addr;
	uint8_t ep;
	size_t len;

	addr = strtol(argv[1], NULL, 10);
	ep = strtol(argv[2], NULL, 16);
	len = MIN(sizeof(vreq_test_buf), strtol(argv[3], NULL, 10));

	xfer = uhc_xfer_alloc(uhc_dev, addr, ep, 0, 512, 10, NULL);
	if (!xfer) {
		return -ENOMEM;
	}

	buf = uhc_xfer_buf_alloc(uhc_dev, xfer, len);
	if (!buf) {
		return -ENOMEM;
	}

	if (USB_EP_DIR_IS_OUT(ep)) {
		net_buf_add_mem(buf, vreq_test_buf, len);
	}

	return uhc_ep_enqueue(uhc_dev, xfer);
}

static int cmd_vendor_in(const struct shell *sh,
			 size_t argc, char **argv)
{
	const uint8_t bmRequestType = (USB_REQTYPE_DIR_TO_HOST << 7) |
				      (USB_REQTYPE_TYPE_VENDOR << 5);
	const uint8_t bRequest = FOOBAZ_VREQ_IN;
	const uint16_t wValue = 0x0000;
	uint16_t wLength;
	uint8_t addr;

	addr = strtol(argv[1], NULL, 10);
	wLength = MIN(sizeof(vreq_test_buf), strtol(argv[2], NULL, 10));

	return usbh_req_setup(uhc_dev, addr,
			      bmRequestType, bRequest, wValue, 0, wLength,
			      NULL);
}

static int cmd_vendor_out(const struct shell *sh,
			  size_t argc, char **argv)
{
	const uint8_t bmRequestType = (USB_REQTYPE_DIR_TO_DEVICE << 7) |
				      (USB_REQTYPE_TYPE_VENDOR << 5);
	const uint8_t bRequest = FOOBAZ_VREQ_OUT;
	const uint16_t wValue = 0x0000;
	uint16_t wLength;
	uint8_t addr;

	addr = strtol(argv[1], NULL, 10);
	wLength = MIN(sizeof(vreq_test_buf), strtol(argv[2], NULL, 10));

	for (int i = 0; i < wLength; i++) {
		vreq_test_buf[i] = i;
	}

	return usbh_req_setup(uhc_dev, addr,
			      bmRequestType, bRequest, wValue, 0, wLength,
			      vreq_test_buf);
}

static int cmd_desc_device(const struct shell *sh,
			   size_t argc, char **argv)
{
	uint8_t addr;
	int err;

	addr = strtol(argv[1], NULL, 10);

	err = usbh_req_desc_dev(uhc_dev, addr);
	if (err) {
		shell_print(sh, "host: Failed to request device descriptor");
	}

	return err;
}

static int cmd_desc_config(const struct shell *sh,
			   size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t cfg;
	int err;

	addr = strtol(argv[1], NULL, 10);
	cfg = strtol(argv[2], NULL, 10);

	/* TODO: cfg is ignored, add to usbh_req_desc_cfg */
	err = usbh_req_desc_cfg(uhc_dev, addr, cfg, 128);
	if (err) {
		shell_print(sh, "host: Failed to request configuration descriptor");
	}

	return err;
}

static int cmd_desc_string(const struct shell *sh,
			   size_t argc, char **argv)
{
	const uint8_t type = USB_DESC_STRING;
	uint8_t addr;
	uint8_t id;
	uint8_t idx;
	int err;

	addr = strtol(argv[1], NULL, 10);
	id = strtol(argv[2], NULL, 10);
	idx = strtol(argv[3], NULL, 10);

	err = usbh_req_desc(uhc_dev, addr, type, idx, id, 128);
	if (err) {
		shell_print(sh, "host: Failed to request configuration descriptor");
	}

	return err;
}

static int cmd_feature_set_halt(const struct shell *sh,
				size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t ep;
	int err;

	addr = strtol(argv[1], NULL, 10);
	ep = strtol(argv[2], NULL, 16);

	/* TODO: add usbh_req_set_sfs_halt(uhc_dev, 0); */
	err = usbh_req_set_sfs_rwup(uhc_dev, addr);
	if (err) {
		shell_error(sh, "host: Failed to set halt feature");
	} else {
		shell_print(sh, "host: Device 0x%02x, ep 0x%02x halt feature set",
			    addr, ep);
	}

	return err;
}

static int cmd_feature_clear_rwup(const struct shell *sh,
				  size_t argc, char **argv)
{
	uint8_t addr;
	int err;

	addr = strtol(argv[1], NULL, 10);

	err = usbh_req_clear_sfs_rwup(uhc_dev, addr);
	if (err) {
		shell_error(sh, "host: Failed to clear rwup feature");
	} else {
		shell_print(sh, "host: Device 0x%02x, rwup feature cleared", addr);
	}

	return err;
}

static int cmd_feature_set_rwup(const struct shell *sh,
				size_t argc, char **argv)
{
	uint8_t addr;
	int err;

	addr = strtol(argv[1], NULL, 10);

	err = usbh_req_set_sfs_rwup(uhc_dev, addr);
	if (err) {
		shell_error(sh, "host: Failed to set rwup feature");
	} else {
		shell_print(sh, "host: Device 0x%02x, rwup feature set", addr);
	}

	return err;
}

static int cmd_feature_set_ppwr(const struct shell *sh,
				size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t port;
	int err;

	addr = strtol(argv[1], NULL, 10);
	port = strtol(argv[2], NULL, 10);

	err = usbh_req_set_hcfs_ppwr(uhc_dev, addr, port);
	if (err) {
		shell_error(sh, "host: Failed to set ppwr feature");
	} else {
		shell_print(sh, "host: Device 0x%02x, port %d, ppwr feature set", addr, port);
	}

	return err;
}

static int cmd_feature_set_prst(const struct shell *sh,
				size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t port;
	int err;

	addr = strtol(argv[1], NULL, 10);
	port = strtol(argv[2], NULL, 10);

	err = usbh_req_set_hcfs_prst(uhc_dev, addr, port);
	if (err) {
		shell_error(sh, "host: Failed to set prst feature");
	} else {
		shell_print(sh, "host: Device 0x%02x, port %d, prst feature set", addr, port);
	}

	return err;
}

static int cmd_feature_set_pstn_ctrls(const struct shell *sh,
				      size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t iface;
	uint8_t value;
	int err;

	addr = strtol(argv[1], NULL, 10);
	iface = strtol(argv[2], NULL, 10);
	value = strtol(argv[3], NULL, 10);

	err = usbh_req_set_pstn_ctrls(uhc_dev, addr, iface, value);
	if (err) {
		shell_error(sh, "host: Failed to set pstn ctrls");
	} else {
		shell_print(sh, "host: Device 0x%02x, ctrls set %d", addr, value);
	}

	return err;
}

static int cmd_device_config(const struct shell *sh,
			     size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t cfg;
	int err;

	addr = strtol(argv[1], NULL, 10);
	cfg = strtol(argv[2], NULL, 10);

	err = usbh_req_set_cfg(uhc_dev, addr, cfg);
	if (err) {
		shell_error(sh, "host: Failed to set configuration");
	} else {
		shell_print(sh, "host: Device 0x%02x, new configuration %u",
			    addr, cfg);
	}

	return err;
}

static int cmd_device_interface(const struct shell *sh,
				size_t argc, char **argv)
{
	uint8_t addr;
	uint8_t iface;
	uint8_t alt;
	int err;

	addr = strtol(argv[1], NULL, 10);
	iface = strtol(argv[2], NULL, 10);
	alt = strtol(argv[3], NULL, 10);

	err = usbh_req_set_alt(uhc_dev, addr, iface, alt);
	if (err) {
		shell_error(sh, "host: Failed to set interface alternate");
	} else {
		shell_print(sh, "host: Device 0x%02x, new %u alternate %u",
			    addr, iface, alt);
	}

	return err;
}

static int cmd_device_address(const struct shell *sh,
			      size_t argc, char **argv)
{
	uint8_t addr;
	int err;

	addr = strtol(argv[1], NULL, 10);

	err = usbh_req_set_address(uhc_dev, 0, addr);
	if (err) {
		shell_error(sh, "host: Failed to set address");
	} else {
		shell_print(sh, "host: New device address is 0x%02x", addr);
	}

	return err;
}

static int cmd_bus_suspend(const struct shell *sh,
			   size_t argc, char **argv)
{
	int err;

	err = uhc_bus_suspend(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to perform bus suspend %d", err);
	} else {
		shell_print(sh, "host: USB bus suspended");
	}

	return err;
}

static int cmd_bus_resume(const struct shell *sh,
			  size_t argc, char **argv)
{
	int err;

	err = uhc_bus_resume(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to perform bus resume %d", err);
	} else {
		shell_print(sh, "host: USB bus resumed");
	}

	err = uhc_sof_enable(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to start SoF generator %d", err);
	}

	return err;
}

static int cmd_bus_reset(const struct shell *sh,
			 size_t argc, char **argv)
{
	int err;

	err = uhc_bus_reset(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to perform bus reset %d", err);
	} else {
		shell_print(sh, "host: USB bus reseted");
	}

	err = uhc_sof_enable(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to start SoF generator %d", err);
	}

	return err;
}

static int cmd_usbh_init(const struct shell *sh,
			 size_t argc, char **argv)
{
	int err;

	ctx_shell = sh;

	uhc_dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0));
	if (!device_is_ready(uhc_dev)) {
		shell_error(sh, "host: USB host controller is not ready");
	}

	err = usbh_init(uhc_dev);
	if (err == -EALREADY) {
		shell_error(sh, "host: USB host already initialized");
	} else if (err) {
		shell_error(sh, "host: Failed to initialize %d", err);
	} else {
		shell_print(sh, "host: USB host initialized");
	}

	return err;
}

static int cmd_usbh_enable(const struct shell *sh,
			   size_t argc, char **argv)
{
	int err;

	err = usbh_enable(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to enable USB host support");
	} else {
		shell_print(sh, "host: USB host enabled");
	}

	return err;
}

static int cmd_usbh_disable(const struct shell *sh,
			    size_t argc, char **argv)
{
	int err;

	err = usbh_disable(uhc_dev);
	if (err) {
		shell_error(sh, "host: Failed to disable USB host support");
	} else {
		shell_print(sh, "host: USB host disabled");
	}

	return err;
}

static int cmd_usbd_test(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct uhc_transfer *xfer;
	struct net_buf *buf;
	const uint8_t addr = 2;
	uint8_t ep = 1;
	size_t len = 14;

	len = MIN(sizeof(vreq_test_buf), len);

	xfer = uhc_xfer_alloc(uhc_dev, addr, ep, 0, 512, 10, NULL);
	if (!xfer) {
		return -ENOMEM;
	}

	buf = uhc_xfer_buf_alloc(uhc_dev, xfer, len);
	if (!buf) {
		return -ENOMEM;
	}

	if (USB_EP_DIR_IS_OUT(ep)) {
		net_buf_add_mem(buf, vreq_test_buf, len);
	}

	err = uhc_ep_enqueue(uhc_dev, xfer);
	if (err) {
		return err;
	}

	ep = 129;
	len = 62;

	xfer = uhc_xfer_alloc(uhc_dev, addr, ep, 0, 512, 10, NULL);
	if (!xfer) {
		return -ENOMEM;
	}

	buf = uhc_xfer_buf_alloc(uhc_dev, xfer, len);
	if (!buf) {
		return -ENOMEM;
	}

	if (USB_EP_DIR_IS_OUT(ep)) {
		net_buf_add_mem(buf, vreq_test_buf, len);
	}

	err = uhc_ep_enqueue(uhc_dev, xfer);
	if (err) {
		return err;
	}

	return err;
}

static int cmd_usbd_magic(const struct shell *sh,
			  size_t argc, char **argv)
{
	int err;
	const uint8_t hubConfig = 1;
	const uint8_t hubAddress = 1;
	const uint8_t agPort = 1;
	const uint8_t agIface = 0;
	const uint8_t agConfig = 1;
	const uint8_t agAddress = 2;
	const uint8_t agIfaceVal = 3;

	err = cmd_usbh_init(sh, argc, argv);
	if (err) {
		return err;
	}

	err = cmd_usbh_enable(sh, argc, argv);
	if (err) {
		return err;
	}

	err = cmd_bus_resume(sh, argc, argv);
	if (err) {
		return err;
	}

	err = usbh_req_set_address(uhc_dev, 0, hubAddress);
	if (err) {
		shell_error(sh, "host: Failed to set address");
		return err;
	} else {
		shell_print(sh, "host: New device address is 0x%02x", 1);
	}

	err = usbh_req_set_cfg(uhc_dev, hubAddress, hubConfig);
	if (err) {
		shell_error(sh, "host: Failed to set configuration");
		return err;
	} else {
		shell_print(sh, "host: Device 0x%02x, new configuration %u",
			    hubAddress, hubConfig);
	}

	err = usbh_req_set_hcfs_ppwr(uhc_dev, hubAddress, agPort);
	if (err) {
		shell_error(sh, "host: Failed to set ppwr feature");
		return err;
	} else {
		shell_print(sh, "host: Device 0x%02x, port %d, ppwr feature set", hubAddress, agPort);
	}

	// without this the port wouldn't reset?
	k_usleep(400000);

	err = usbh_req_set_hcfs_prst(uhc_dev, hubAddress, agPort);
	if (err) {
		shell_error(sh, "host: Failed to set prst feature");
		return err;
	} else {
		shell_print(sh, "host: Device 0x%02x, port %d, prst feature set", hubAddress, agPort);
	}

	// without this the port wouldn't reset?
	k_usleep(400000);

	err = usbh_req_set_address(uhc_dev, 0, agAddress);
	if (err) {
		shell_error(sh, "host: Failed to set address");
		return err;
	} else {
		shell_print(sh, "host: New device address is 0x%02x", agAddress);
	}

	err = usbh_req_set_cfg(uhc_dev, agAddress, agConfig);
	if (err) {
		shell_error(sh, "host: Failed to set configuration");
		return err;
	} else {
		shell_print(sh, "host: Device 0x%02x, new configuration %u",
			    agAddress, agConfig);
	}

	err = usbh_req_set_pstn_ctrls(uhc_dev, agAddress, agIface, agIfaceVal);
	if (err) {
		shell_error(sh, "host: Failed to set pstn ctrls");
		return err;
	} else {
		shell_print(sh, "host: Device 0x%02x, ctrls set %d", agAddress, agIfaceVal);
	}

	return cmd_usbd_test(sh, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(desc_cmds,
	SHELL_CMD_ARG(device, NULL, "<address>",
		      cmd_desc_device, 2, 0),
	SHELL_CMD_ARG(configuration, NULL, "<address> <index>",
		      cmd_desc_config, 3, 0),
	SHELL_CMD_ARG(string, NULL, "<address> <id> <index>",
		      cmd_desc_string, 4, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(feature_set_cmds,
	SHELL_CMD_ARG(rwup, NULL, "<address>",
		      cmd_feature_set_rwup, 2, 0),
	SHELL_CMD_ARG(ppwr, NULL, "<address> <port>",
		      cmd_feature_set_ppwr, 3, 0),
	SHELL_CMD_ARG(prst, NULL, "<address> <port>",
		      cmd_feature_set_prst, 3, 0),
	SHELL_CMD_ARG(halt, NULL, "<address> <endpoint>",
		      cmd_feature_set_halt, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(feature_clear_cmds,
	SHELL_CMD_ARG(rwup, NULL, "<address>",
		      cmd_feature_clear_rwup, 2, 0),
	SHELL_CMD_ARG(halt, NULL, "<address> <endpoint>",
		      cmd_feature_set_halt, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(device_cmds,
	SHELL_CMD_ARG(address, NULL, "<address>",
		      cmd_device_address, 2, 0),
	SHELL_CMD_ARG(config, NULL, "<address> <config>",
		      cmd_device_config, 3, 0),
	SHELL_CMD_ARG(interface, NULL, "<address> <interface> <alternate>",
		      cmd_device_interface, 4, 0),
	SHELL_CMD_ARG(descriptor, &desc_cmds, "descriptor request",
		      NULL, 1, 0),
	SHELL_CMD_ARG(feature-set, &feature_set_cmds, "feature selector",
		      NULL, 1, 0),
	SHELL_CMD_ARG(feature-clear, &feature_clear_cmds, "feature selector",
		      NULL, 1, 0),
	SHELL_CMD_ARG(vendor_in, NULL, "<address> <length>",
		      cmd_vendor_in, 3, 0),
	SHELL_CMD_ARG(vendor_out, NULL, "<address> <length>",
		      cmd_vendor_out, 3, 0),
	SHELL_CMD_ARG(bulk, NULL, "<address> <endpoint> <length>",
		      cmd_bulk, 4, 0),
	SHELL_CMD_ARG(pstn_ctrls, NULL, "<address> <interface> <value>",
		      cmd_feature_set_pstn_ctrls, 4, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(bus_cmds,
	SHELL_CMD_ARG(suspend, NULL, "[nono]",
		      cmd_bus_suspend, 1, 0),
	SHELL_CMD_ARG(resume, NULL, "[nono]",
		      cmd_bus_resume, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "[nono]",
		      cmd_bus_reset, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_usbh_cmds,
	SHELL_CMD_ARG(init, NULL, "[none]",
		      cmd_usbh_init, 1, 0),
	SHELL_CMD_ARG(enable, NULL, "[none]",
		      cmd_usbh_enable, 1, 0),
	SHELL_CMD_ARG(disable, NULL, "[none]",
		      cmd_usbh_disable, 1, 0),
	SHELL_CMD_ARG(bus, &bus_cmds, "bus commands",
		      NULL, 1, 0),
	SHELL_CMD_ARG(device, &device_cmds, "device commands",
		      NULL, 1, 0),
	SHELL_CMD_ARG(magic, NULL, "[none]",
		      cmd_usbd_magic, 1, 0),
	SHELL_CMD_ARG(test, NULL, "[none]",
		      cmd_usbd_test, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(usbh, &sub_usbh_cmds, "USBH commands", NULL);
