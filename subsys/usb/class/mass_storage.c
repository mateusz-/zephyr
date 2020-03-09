/*
 * The Mass Storage protocol state machine in this file is based on mbed's
 * implementation. We augment it by adding Zephyr's USB transport and Storage
 * APIs.
 *
 * Copyright (c) 2010-2011 mbed.org, MIT License
 * Copyright (c) 2016 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


/**
 * @file
 * @brief Mass Storage device class driver
 *
 * Driver for USB Mass Storage device class driver
 */

#include <init.h>
#include <errno.h>
#include <string.h>
#include <sys/byteorder.h>
#include <sys/__assert.h>
#include <disk/disk_access.h>
#include <usb/class/usb_msc.h>
#include <usb/usb_device.h>
#include <usb/usb_common.h>
#include <usb_descriptor.h>

#include "msc_scsi.h"

#define LOG_LEVEL CONFIG_USB_MASS_STORAGE_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(usb_msc);

/* max USB packet size */
#define MAX_PACKET	CONFIG_MASS_STORAGE_BULK_EP_MPS

#define BLOCK_SIZE	512
#define DISK_THREAD_STACK_SZ	512
#define DISK_THREAD_PRIO	-5

#define THREAD_OP_READ_QUEUED		1
#define THREAD_OP_WRITE_QUEUED		3
#define THREAD_OP_WRITE_DONE		4

#define MASS_STORAGE_IN_EP_ADDR		0x82
#define MASS_STORAGE_OUT_EP_ADDR	0x01

struct usb_mass_config {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_out_ep;
} __packed;

USBD_CLASS_DESCR_DEFINE(primary, 0) struct usb_mass_config mass_cfg = {
	/* Interface descriptor */
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_INTERFACE_DESC,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = MASS_STORAGE_CLASS,
		.bInterfaceSubClass = SCSI_TRANSPARENT_SUBCLASS,
		.bInterfaceProtocol = BULK_ONLY_PROTOCOL,
		.iInterface = 0,
	},
	/* First Endpoint IN */
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_ENDPOINT_DESC,
		.bEndpointAddress = MASS_STORAGE_IN_EP_ADDR,
		.bmAttributes = USB_DC_EP_BULK,
		.wMaxPacketSize =
			sys_cpu_to_le16(CONFIG_MASS_STORAGE_BULK_EP_MPS),
		.bInterval = 0x00,
	},
	/* Second Endpoint OUT */
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_ENDPOINT_DESC,
		.bEndpointAddress = MASS_STORAGE_OUT_EP_ADDR,
		.bmAttributes = USB_DC_EP_BULK,
		.wMaxPacketSize =
			sys_cpu_to_le16(CONFIG_MASS_STORAGE_BULK_EP_MPS),
		.bInterval = 0x00,
	},
};

static volatile int thread_op;
static K_THREAD_STACK_DEFINE(mass_thread_stack, DISK_THREAD_STACK_SZ);
static struct k_thread mass_thread_data;
static struct k_sem disk_wait_sem;
static volatile u32_t defered_wr_sz;

/*
 * Keep block buffer larger than BLOCK_SIZE for the case
 * the dCBWDataTransferLength is multiple of the BLOCK_SIZE and
 * the length of the transferred data is not aligned to the BLOCK_SIZE.
 */
static u8_t page[BLOCK_SIZE + CONFIG_MASS_STORAGE_BULK_EP_MPS];

/* Initialized during mass_storage_init() */
static u32_t memory_size;
static u32_t block_count;
static const char *disk_pdrv = CONFIG_MASS_STORAGE_DISK_NAME;

#define MSD_OUT_EP_IDX			0
#define MSD_IN_EP_IDX			1

static void mass_storage_bulk_out(u8_t ep,
				  enum usb_dc_ep_cb_status_code ep_status);
static void mass_storage_bulk_in(u8_t ep,
				 enum usb_dc_ep_cb_status_code ep_status);

/* Describe EndPoints configuration */
static struct usb_ep_cfg_data mass_ep_data[] = {
	{
		.ep_cb	= mass_storage_bulk_out,
		.ep_addr = MASS_STORAGE_OUT_EP_ADDR
	},
	{
		.ep_cb = mass_storage_bulk_in,
		.ep_addr = MASS_STORAGE_IN_EP_ADDR
	}
};

/* CSW Status */
enum Status {
	CSW_PASSED,
	CSW_FAILED,
	CSW_ERROR,
};

/* MSC Bulk-only Stage */
enum Stage {
	MSC_READ_CBW,     /* wait a CBW */
	MSC_ERROR,        /* error */
	MSC_PROCESS_CBW,  /* process a CBW request */
	MSC_SEND_CSW,     /* send a CSW */
	MSC_WAIT_CSW      /* wait that a CSW has been effectively sent */
};

/* state of the bulk-only state machine */
static enum Stage stage;

/*current CBW*/
static struct CBW cbw;

/*CSW which will be sent*/
static struct CSW csw;

/*addr where will be read or written data*/
static u32_t addr;

/*length of a reading or writing*/
static u32_t length;

static u8_t max_lun_count;

/*memory OK (after a memoryVerify)*/
static bool memOK;

static void msd_state_machine_reset(void)
{
	stage = MSC_READ_CBW;
}

static void msd_init(void)
{
	(void)memset((void *)&cbw, 0, sizeof(struct CBW));
	(void)memset((void *)&csw, 0, sizeof(struct CSW));
	(void)memset(page, 0, sizeof(page));
	addr = 0U;
	length = 0U;
}

static void sendCSW(void)
{
	csw.Signature = CSW_Signature;
	if (usb_write(mass_ep_data[MSD_IN_EP_IDX].ep_addr, (u8_t *)&csw,
		      sizeof(struct CSW), NULL) != 0) {
		LOG_ERR("usb write failure");
	}
	stage = MSC_WAIT_CSW;
}

static bool write(u8_t *buf, u16_t size)
{
	if (size >= cbw.DataLength) {
		size = cbw.DataLength;
	}

	/* updating the State Machine , so that we send CSW when this
	 * transfer is complete, ie when we get a bulk in callback
	 */
	stage = MSC_SEND_CSW;

	if (usb_write(mass_ep_data[MSD_IN_EP_IDX].ep_addr, buf, size, NULL)) {
		LOG_ERR("USB write failed");
		return false;
	}

	csw.DataResidue -= size;
	csw.Status = CSW_PASSED;
	return true;
}

/**
 * @brief Handler called for Class requests not handled by the USB stack.
 *
 * @param pSetup    Information about the request to execute.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
static int mass_storage_class_handle_req(struct usb_setup_packet *pSetup,
					 s32_t *len, u8_t **data)
{
	if (sys_le16_to_cpu(pSetup->wIndex) != mass_cfg.if0.bInterfaceNumber ||
	    sys_le16_to_cpu(pSetup->wValue) != 0) {
		LOG_WRN("Invalid setup parameters");
		return -EINVAL;
	}

	switch (pSetup->bRequest) {
	case MSC_REQUEST_RESET:
		LOG_DBG("MSC_REQUEST_RESET");

		if (sys_le16_to_cpu(pSetup->wLength)) {
			LOG_WRN("Invalid length");
			return -EINVAL;
		}

		msd_state_machine_reset();
		break;

	case MSC_REQUEST_GET_MAX_LUN:
		LOG_DBG("MSC_REQUEST_GET_MAX_LUN");

		if (sys_le16_to_cpu(pSetup->wLength) != 1) {
			LOG_WRN("Invalid length");
			return -EINVAL;
		}

		max_lun_count = 0U;
		*data = (u8_t *)(&max_lun_count);
		*len = 1;
		break;

	default:
		LOG_WRN("Unknown request 0x%x, value 0x%x",
			pSetup->bRequest, pSetup->wValue);
		return -EINVAL;
	}

	return 0;
}

static void test_unit_ready_cmd(void)
{
	if (cbw.DataLength != 0U) {
		if ((cbw.Flags & 0x80) != 0U) {
			LOG_WRN("Stall IN endpoint");
			usb_ep_set_stall(mass_ep_data[MSD_IN_EP_IDX].ep_addr);
		} else {
			LOG_WRN("Stall OUT endpoint");
			usb_ep_set_stall(mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
		}
	}

	csw.Status = CSW_PASSED;
	sendCSW();
}

static struct fixed_format_sense_data sense_data = {
	.code = SDRC_CURRENT_ERRORS,
	.sense_key = SK_ILLEGAL_REQUEST,
	.as_length = sizeof(struct additional_sense_data),
	.asd = {
		.asc_ascq = {0x30, 0x01},
	},
};

static void update_sense_data_ascq(u16_t ascq)
{
	sys_put_be16(ascq, sense_data.asd.asc_ascq);
}

static bool req_sense_cmd(void)
{
	struct cdb6 *cmd = (struct cdb6 *)&cbw.CB[0];

	return write((u8_t *)&sense_data,
		     MIN(cdb6_get_length(cmd), sizeof(sense_data)));
}

static struct dabc_inquiry_data inq_data = {
	.type = DIRECT_ACCESS_BLOCK_DEVICE,
	.qualifier = 0,
	.rmb = 1,	/* Medium is not removable */
	.version = 0,
	.rdf = 2,
	.length = sizeof(struct dabc_inquiry_data) - 5,
	.sccs = 0,
	.t10_vid = {'Z', 'E', 'P', 'H', 'Y', 'R', ' ', ' '},
	.product_id = {'Z', 'E', 'P', 'H', 'Y', 'R', ' ', 'U', 'S', 'B', ' ',
		       'D', 'I', 'S', 'K', ' '},
	.product_rev = {'0', '.', '0', '1'},
};

static bool inquiry_cmd(void)
{
	struct cdb_inquiry *cmd = (struct cdb_inquiry *)&cbw.CB[0];

	if (cmd->evpd) {
		update_sense_data_ascq(ASCQ_INVALID_FIELD_IN_CDB);
		csw.Status = CSW_FAILED;
		sendCSW();
		return false;
	}

	return write((u8_t *)&inq_data,
		     MIN(sys_get_be16(&cmd->length[0]), sizeof(inq_data)));
}

static struct mode_parameter6 sense6_param = {
	.hdr = {
		.data_length = sizeof(struct mode_parameter6) - 1,
		.medium_type = DIRECT_ACCESS_BLOCK_DEVICE,
		.wp = 0,
		.bd_length = 0,
	},
};

static bool mode_sense6_cmd(void)
{
	struct cdb6 *cmd = (struct cdb6 *)&cbw.CB[0];

	return write((u8_t *)&sense6_param,
		     MIN(cdb6_get_length(cmd), sizeof(sense6_param)));
}

static bool read_format_capacities_cmd(void)
{
	struct capacity_descriptor capacity;
	struct cdb10 *cmd = (struct cdb10 *)&cbw.CB[0];

	memset(&capacity, 0, sizeof(struct capacity_descriptor));
	capacity.clh.length = sizeof(capacity.ccd);
	capacity.ccd.type = DESCRIPTOR_TYPE_FORMATTED_MEDIA;
	sys_put_be32(block_count, capacity.ccd.numof_blocks);
	sys_put_be24(BLOCK_SIZE, capacity.ccd.block_length);

	LOG_ERR("rfcc %u", MIN(cdb10_get_length(cmd), sizeof(capacity)));

	return write((u8_t *)&capacity,
		     MIN(cdb10_get_length(cmd), sizeof(capacity)));
}

static bool read_capacity_cmd(void)
{
	u8_t capacity[8];

	/* Last logical block */
	sys_put_be32(block_count - 1, &capacity[0]);
	/* Block length in bytes */
	sys_put_be32(BLOCK_SIZE, &capacity[4]);

	return write(capacity, sizeof(capacity));
}

static void thread_memory_read_done(void)
{
	u32_t n;

	n = (length > MAX_PACKET) ? MAX_PACKET : length;
	if ((addr + n) > memory_size) {
		n = memory_size - addr;
		stage = MSC_ERROR;
	}

	if (usb_write(mass_ep_data[MSD_IN_EP_IDX].ep_addr,
		&page[addr % BLOCK_SIZE], n, NULL) != 0) {
		LOG_ERR("Failed to write EP 0x%x",
			mass_ep_data[MSD_IN_EP_IDX].ep_addr);
	}
	addr += n;
	length -= n;

	csw.DataResidue -= n;

	if (!length || (stage != MSC_PROCESS_CBW)) {
		csw.Status = (stage == MSC_PROCESS_CBW) ?
			CSW_PASSED : CSW_FAILED;
		stage = (stage == MSC_PROCESS_CBW) ? MSC_SEND_CSW : stage;
	}
}


static void memoryRead(void)
{
	u32_t n;

	n = (length > MAX_PACKET) ? MAX_PACKET : length;
	if ((addr + n) > memory_size) {
		n = memory_size - addr;
		stage = MSC_ERROR;
	}

	/* we read an entire block */
	if (!(addr % BLOCK_SIZE)) {
		thread_op = THREAD_OP_READ_QUEUED;
		LOG_DBG("Signal thread for %d", (addr/BLOCK_SIZE));
		k_sem_give(&disk_wait_sem);
		return;
	}
	usb_write(mass_ep_data[MSD_IN_EP_IDX].ep_addr,
		  &page[addr % BLOCK_SIZE], n, NULL);
	addr += n;
	length -= n;

	csw.DataResidue -= n;

	if (!length || (stage != MSC_PROCESS_CBW)) {
		csw.Status = (stage == MSC_PROCESS_CBW) ?
			CSW_PASSED : CSW_FAILED;
		stage = (stage == MSC_PROCESS_CBW) ? MSC_SEND_CSW : stage;
	}
}

static bool check_cbw_data_length(void)
{
	if (!cbw.DataLength) {
		LOG_WRN("Zero length in CBW");
		csw.Status = CSW_FAILED;
		sendCSW();
		return false;
	}

	return true;
}

static bool infoTransfer(void)
{
	u32_t n;

	if (!check_cbw_data_length()) {
		return false;
	}

	/* Logical Block Address of First Block */
	n = sys_get_be32(&cbw.CB[2]);

	LOG_DBG("LBA (block) : 0x%x ", n);
	if ((n * BLOCK_SIZE) >= memory_size) {
		LOG_ERR("LBA out of range");
		update_sense_data_ascq(ASCQ_CANNOT_RM_UNKNOWN_FORMAT);
		csw.Status = CSW_FAILED;
		sendCSW();
		return false;
	}

	addr = n * BLOCK_SIZE;

	/* Number of Blocks to transfer */
	switch (cbw.CB[0]) {
	case READ10:
	case WRITE10:
	case VERIFY10:
		n = sys_get_be16(&cbw.CB[7]);
		break;

	case READ12:
	case WRITE12:
		n = sys_get_be32(&cbw.CB[6]);
		break;
	}

	LOG_DBG("Size (block) : 0x%x ", n);
	length = n * BLOCK_SIZE;

	if (cbw.DataLength != length) {
		if ((cbw.Flags & 0x80) != 0U) {
			LOG_WRN("Stall IN endpoint");
			usb_ep_set_stall(mass_ep_data[MSD_IN_EP_IDX].ep_addr);
		} else {
			LOG_WRN("Stall OUT endpoint");
			usb_ep_set_stall(mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
		}

		update_sense_data_ascq(ASCQ_CANNOT_RM_UNKNOWN_FORMAT);
		csw.Status = CSW_FAILED;
		sendCSW();
		return false;
	}

	return true;
}

static void fail(void)
{
	if (cbw.DataLength) {
		/* Stall data stage */
		usb_ep_set_stall(mass_ep_data[MSD_IN_EP_IDX].ep_addr);
	}

	csw.Status = CSW_FAILED;
	sendCSW();
}

static void CBWDecode(u8_t *buf, u16_t size)
{
	if (size != sizeof(cbw)) {
		LOG_ERR("size != sizeof(cbw)");
		return;
	}

	memcpy((u8_t *)&cbw, buf, size);
	if (cbw.Signature != CBW_Signature) {
		LOG_ERR("CBW Signature Mismatch");
		return;
	}

	csw.Tag = cbw.Tag;
	csw.DataResidue = cbw.DataLength;

	if ((cbw.CBLength <  1) || (cbw.CBLength > 16) || (cbw.LUN != 0U)) {
		LOG_WRN("cbw.CBLength %d", cbw.CBLength);
		update_sense_data_ascq(ASCQ_CANNOT_RM_UNKNOWN_FORMAT);
		fail();
	} else {
		switch (cbw.CB[0]) {
		case TEST_UNIT_READY:
			LOG_DBG(">> TUR");
			test_unit_ready_cmd();
			break;
		case REQUEST_SENSE:
			LOG_DBG("opcode: REQUEST SENSE");
			if (check_cbw_data_length()) {
				req_sense_cmd();
			}
			break;
		case INQUIRY:
			LOG_DBG("opcode: INQUIRY");
			if (check_cbw_data_length()) {
				inquiry_cmd();
			}
			break;
		case MODE_SENSE6:
			LOG_DBG("opcode: MODE SENSE 6");
			if (check_cbw_data_length()) {
				mode_sense6_cmd();
			}
			break;
		case READ_FORMAT_CAPACITIES:
			LOG_INF("opcode: READ FORMAT CAPACITIES");
			if (check_cbw_data_length()) {
				read_format_capacities_cmd();
			}
			break;
		case READ_CAPACITY:
			LOG_DBG("opcode: READ CAPACITY 10");
			if (check_cbw_data_length()) {
				read_capacity_cmd();
			}
			break;
		case READ10:
		case READ12:
			LOG_DBG(">> READ");
			if (infoTransfer()) {
				if ((cbw.Flags & 0x80)) {
					stage = MSC_PROCESS_CBW;
					memoryRead();
				} else {
					usb_ep_set_stall(
					  mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
					LOG_WRN("Stall OUT endpoint");
					csw.Status = CSW_ERROR;
					sendCSW();
				}
			}
			break;
		case WRITE10:
		case WRITE12:
			LOG_DBG(">> WRITE");
			if (infoTransfer()) {
				if (!(cbw.Flags & 0x80)) {
					stage = MSC_PROCESS_CBW;
				} else {
					usb_ep_set_stall(
					  mass_ep_data[MSD_IN_EP_IDX].ep_addr);
					LOG_WRN("Stall IN endpoint");
					csw.Status = CSW_ERROR;
					sendCSW();
				}
			}
			break;
		case VERIFY10:
			LOG_DBG(">> VERIFY10");
			if (!(cbw.CB[1] & 0x02)) {
				csw.Status = CSW_PASSED;
				sendCSW();
				break;
			}
			if (infoTransfer()) {
				if (!(cbw.Flags & 0x80)) {
					stage = MSC_PROCESS_CBW;
					memOK = true;
				} else {
					usb_ep_set_stall(
					  mass_ep_data[MSD_IN_EP_IDX].ep_addr);
					LOG_WRN("Stall IN endpoint");
					csw.Status = CSW_ERROR;
					sendCSW();
				}
			}
			break;
		case MEDIA_REMOVAL:
			LOG_DBG(">> MEDIA_REMOVAL");
			csw.Status = CSW_PASSED;
			sendCSW();
			break;
		default:
			LOG_WRN("Unsupported opcode 0x%02x", cbw.CB[0]);
			update_sense_data_ascq(ASCQ_INVALID_CMD_OPCODE);
			fail();
			break;
		}
	}

}

static void memoryVerify(u8_t *buf, u16_t size)
{
	u32_t n;

	if ((addr + size) > memory_size) {
		size = memory_size - addr;
		stage = MSC_ERROR;
		usb_ep_set_stall(mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
		LOG_WRN("Stall OUT endpoint");
	}

	/* beginning of a new block -> load a whole block in RAM */
	if (!(addr % BLOCK_SIZE)) {
		LOG_DBG("Disk READ sector %d", addr/BLOCK_SIZE);
		if (disk_access_read(disk_pdrv, page, addr/BLOCK_SIZE, 1)) {
			LOG_ERR("---- Disk Read Error %d", addr/BLOCK_SIZE);
		}
	}

	/* info are in RAM -> no need to re-read memory */
	for (n = 0U; n < size; n++) {
		if (page[addr%BLOCK_SIZE + n] != buf[n]) {
			LOG_DBG("Mismatch sector %d offset %d",
				addr/BLOCK_SIZE, n);
			memOK = false;
			break;
		}
	}

	addr += size;
	length -= size;
	csw.DataResidue -= size;

	if (!length || (stage != MSC_PROCESS_CBW)) {
		csw.Status = (memOK && (stage == MSC_PROCESS_CBW)) ?
						CSW_PASSED : CSW_FAILED;
		sendCSW();
	}
}

static void memoryWrite(u8_t *buf, u16_t size)
{
	if ((addr + size) > memory_size) {
		size = memory_size - addr;
		stage = MSC_ERROR;
		usb_ep_set_stall(mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
		LOG_WRN("Stall OUT endpoint");
	}

	/* we fill an array in RAM of 1 block before writing it in memory */
	for (int i = 0; i < size; i++) {
		page[addr % BLOCK_SIZE + i] = buf[i];
	}

	/* if the array is filled, write it in memory */
	if ((addr % BLOCK_SIZE) + size >= BLOCK_SIZE) {
		if (!(disk_access_status(disk_pdrv) &
					DISK_STATUS_WR_PROTECT)) {
			LOG_DBG("Disk WRITE Qd %d", (addr/BLOCK_SIZE));
			thread_op = THREAD_OP_WRITE_QUEUED;  /* write_queued */
			defered_wr_sz = size;
			k_sem_give(&disk_wait_sem);
			return;
		}
	}

	addr += size;
	length -= size;
	csw.DataResidue -= size;

	if ((!length) || (stage != MSC_PROCESS_CBW)) {
		csw.Status = (stage == MSC_ERROR) ? CSW_FAILED : CSW_PASSED;
		sendCSW();
	}
}


static void mass_storage_bulk_out(u8_t ep,
		enum usb_dc_ep_cb_status_code ep_status)
{
	u32_t bytes_read = 0U;
	u8_t bo_buf[CONFIG_MASS_STORAGE_BULK_EP_MPS];

	ARG_UNUSED(ep_status);

	usb_ep_read_wait(ep, bo_buf, CONFIG_MASS_STORAGE_BULK_EP_MPS,
			 &bytes_read);

	switch (stage) {
	/*the device has to decode the CBW received*/
	case MSC_READ_CBW:
		LOG_DBG("> BO - MSC_READ_CBW");
		CBWDecode(bo_buf, bytes_read);
		break;

	/*the device has to receive data from the host*/
	case MSC_PROCESS_CBW:
		switch (cbw.CB[0]) {
		case WRITE10:
		case WRITE12:
			/* LOG_DBG("> BO - PROC_CBW WR");*/
			memoryWrite(bo_buf, bytes_read);
			break;
		case VERIFY10:
			LOG_DBG("> BO - PROC_CBW VER");
			memoryVerify(bo_buf, bytes_read);
			break;
		default:
			LOG_ERR("> BO - PROC_CBW default <<ERROR!!!>>");
			break;
		}
		break;

	/*an error has occurred: stall endpoint and send CSW*/
	default:
		LOG_WRN("Stall OUT endpoint, stage: %d", stage);
		update_sense_data_ascq(ASCQ_CANNOT_RM_UNKNOWN_FORMAT);
		usb_ep_set_stall(ep);
		csw.Status = CSW_ERROR;
		sendCSW();
		break;
	}

	if (thread_op != THREAD_OP_WRITE_QUEUED) {
		usb_ep_read_continue(ep);
	} else {
		LOG_DBG("> BO not clearing NAKs yet");
	}

}

static void thread_memory_write_done(void)
{
	u32_t size = defered_wr_sz;
	size_t overflowed_len = (addr + size) % CONFIG_MASS_STORAGE_BULK_EP_MPS;

	if (overflowed_len) {
		memcpy(page, &page[BLOCK_SIZE], overflowed_len);
	}

	addr += size;
	length -= size;
	csw.DataResidue -= size;


	if ((!length) || (stage != MSC_PROCESS_CBW)) {
		csw.Status = (stage == MSC_ERROR) ? CSW_FAILED : CSW_PASSED;
		sendCSW();
	}

	thread_op = THREAD_OP_WRITE_DONE;

	usb_ep_read_continue(mass_ep_data[MSD_OUT_EP_IDX].ep_addr);
}

/**
 * @brief EP Bulk IN handler, used to send data to the Host
 *
 * @param ep        Endpoint address.
 * @param ep_status Endpoint status code.
 *
 * @return  N/A.
 */
static void mass_storage_bulk_in(u8_t ep,
				 enum usb_dc_ep_cb_status_code ep_status)
{
	ARG_UNUSED(ep_status);
	ARG_UNUSED(ep);

	switch (stage) {
	/*the device has to send data to the host*/
	case MSC_PROCESS_CBW:
		switch (cbw.CB[0]) {
		case READ10:
		case READ12:
			/* LOG_DBG("< BI - PROC_CBW  READ"); */
			memoryRead();
			break;
		default:
			LOG_ERR("< BI-PROC_CBW default <<ERROR!!>>");
			break;
		}
		break;

	/*the device has to send a CSW*/
	case MSC_SEND_CSW:
		LOG_DBG("< BI - MSC_SEND_CSW");
		sendCSW();
		break;

	/*the host has received the CSW -> we wait a CBW*/
	case MSC_WAIT_CSW:
		LOG_DBG("< BI - MSC_WAIT_CSW");
		stage = MSC_READ_CBW;
		break;

	/*an error has occurred*/
	default:
		LOG_WRN("Stall IN endpoint, stage: %d", stage);
		usb_ep_set_stall(mass_ep_data[MSD_IN_EP_IDX].ep_addr);
		sendCSW();
		break;
	}
}



/**
 * @brief Callback used to know the USB connection status
 *
 * @param status USB device status code.
 *
 * @return  N/A.
 */
static void mass_storage_status_cb(struct usb_cfg_data *cfg,
				   enum usb_dc_status_code status,
				   const u8_t *param)
{
	ARG_UNUSED(param);
	ARG_UNUSED(cfg);

	/* Check the USB status and do needed action if required */
	switch (status) {
	case USB_DC_ERROR:
		LOG_DBG("USB device error");
		break;
	case USB_DC_RESET:
		LOG_DBG("USB device reset detected");
		msd_state_machine_reset();
		msd_init();
		break;
	case USB_DC_CONNECTED:
		LOG_DBG("USB device connected");
		break;
	case USB_DC_CONFIGURED:
		LOG_DBG("USB device configured");
		break;
	case USB_DC_DISCONNECTED:
		LOG_DBG("USB device disconnected");
		break;
	case USB_DC_SUSPEND:
		LOG_DBG("USB device supended");
		break;
	case USB_DC_RESUME:
		LOG_DBG("USB device resumed");
		break;
	case USB_DC_INTERFACE:
		LOG_DBG("USB interface selected");
		break;
	case USB_DC_SOF:
		break;
	case USB_DC_UNKNOWN:
	default:
		LOG_DBG("USB unknown state");
		break;
	}
}

static void mass_interface_config(struct usb_desc_header *head,
				  u8_t bInterfaceNumber)
{
	ARG_UNUSED(head);

	mass_cfg.if0.bInterfaceNumber = bInterfaceNumber;
}

/* Configuration of the Mass Storage Device send to the USB Driver */
USBD_CFG_DATA_DEFINE(primary, msd) struct usb_cfg_data mass_storage_config = {
	.usb_device_description = NULL,
	.interface_config = mass_interface_config,
	.interface_descriptor = &mass_cfg.if0,
	.cb_usb_status = mass_storage_status_cb,
	.interface = {
		.class_handler = mass_storage_class_handle_req,
		.custom_handler = NULL,
	},
	.num_endpoints = ARRAY_SIZE(mass_ep_data),
	.endpoint = mass_ep_data
};

static void mass_thread_main(int arg1, int unused)
{
	ARG_UNUSED(unused);
	ARG_UNUSED(arg1);

	while (1) {
		k_sem_take(&disk_wait_sem, K_FOREVER);
		LOG_DBG("sem %d", thread_op);

		switch (thread_op) {
		case THREAD_OP_READ_QUEUED:
			if (disk_access_read(disk_pdrv,
						page, (addr/BLOCK_SIZE), 1)) {
				LOG_ERR("!! Disk Read Error %d !",
					addr/BLOCK_SIZE);
			}

			thread_memory_read_done();
			break;
		case THREAD_OP_WRITE_QUEUED:
			if (disk_access_write(disk_pdrv,
						page, (addr/BLOCK_SIZE), 1)) {
				LOG_ERR("!!!!! Disk Write Error %d !!!!!",
					addr/BLOCK_SIZE);
			}
			thread_memory_write_done();
			break;
		default:
			LOG_ERR("XXXXXX thread_op  %d ! XXXXX", thread_op);
		}
	}
}

/**
 * @brief Initialize USB mass storage setup
 *
 * This routine is called to reset the USB device controller chip to a
 * quiescent state. Also it initializes the backing storage and initializes
 * the mass storage protocol state.
 *
 * @param dev device struct.
 *
 * @return negative errno code on fatal failure, 0 otherwise
 */
static int mass_storage_init(struct device *dev)
{
	u32_t block_size = 0U;

	ARG_UNUSED(dev);

	if (disk_access_init(disk_pdrv) != 0) {
		LOG_ERR("Storage init ERROR !!!! - Aborting USB init");
		return 0;
	}

	if (disk_access_ioctl(disk_pdrv,
				DISK_IOCTL_GET_SECTOR_COUNT, &block_count)) {
		LOG_ERR("Unable to get sector count - Aborting USB init");
		return 0;
	}

	if (disk_access_ioctl(disk_pdrv,
				DISK_IOCTL_GET_SECTOR_SIZE, &block_size)) {
		LOG_ERR("Unable to get sector size - Aborting USB init");
		return 0;
	}

	if (block_size != BLOCK_SIZE) {
		LOG_ERR("Block Size reported by the storage side is "
			"different from Mass Storgae Class page Buffer - "
			"Aborting");
		return 0;
	}


	LOG_INF("Sect Count %d", block_count);
	memory_size = block_count * BLOCK_SIZE;
	LOG_INF("Memory Size %d", memory_size);

	msd_state_machine_reset();
	msd_init();

	k_sem_init(&disk_wait_sem, 0, 1);

	/* Start a thread to offload disk ops */
	k_thread_create(&mass_thread_data, mass_thread_stack,
			DISK_THREAD_STACK_SZ,
			(k_thread_entry_t)mass_thread_main, NULL, NULL, NULL,
			DISK_THREAD_PRIO, 0, K_NO_WAIT);

	return 0;
}

SYS_INIT(mass_storage_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
