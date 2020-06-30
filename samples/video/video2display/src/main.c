/*
 * Copyright (c) 2019 Linaro Limited
 * Copyright (c) 2020 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* This file is based on samples/video/capture/src/main.c */

#include <zephyr.h>
#include <device.h>

#include <drivers/video.h>
#include <drivers/display.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(main);

#define VIDEO_DEV_SW "VIDEO_SW_GENERATOR"

#if DT_NODE_HAS_STATUS(DT_INST(0, sitronix_st7789v), okay)
#define DISPLAY_DEV_NAME DT_LABEL(DT_INST(0, sitronix_st7789v))
#endif

#if DT_NODE_HAS_STATUS(DT_INST(0, fsl_imx6sx_lcdif), okay)
#define DISPLAY_DEV_NAME DT_LABEL(DT_INST(0, fsl_imx6sx_lcdif))
#endif

void main(void)
{
	struct device *display_dev;
	struct display_capabilities capabilities;
	struct video_buffer *buffers[1], *vbuf;
	struct display_buffer_descriptor buf_desc;
	struct video_format fmt;
	struct video_caps caps;
	struct device *video;
	size_t bsize;
	int i = 0;

	/* Default to software video pattern generator */
	video = device_get_binding(VIDEO_DEV_SW);
	if (video == NULL) {
		LOG_ERR("Video device %s not found", VIDEO_DEV_SW);
		return;
	}

	display_dev = device_get_binding(DISPLAY_DEV_NAME);

	if (display_dev == NULL) {
		LOG_ERR("Display device %s not found", DISPLAY_DEV_NAME);
		return;
	}

	display_get_capabilities(display_dev, &capabilities);
	switch (capabilities.current_pixel_format) {
	case PIXEL_FORMAT_BGR_565:
		LOG_INF("BGR_565, x %u, y %u",
			capabilities.x_resolution, capabilities.y_resolution);
		break;
	case PIXEL_FORMAT_RGB_565:
		LOG_INF("RGB_565, x %u, y %u",
			capabilities.x_resolution, capabilities.y_resolution);
		break;
	default:
		LOG_ERR("Unsupported pixel format.");
		return;
	}


	/* Get video device capabilities */
	if (video_get_caps(video, VIDEO_EP_OUT, &caps)) {
		LOG_ERR("Unable to retrieve video capabilities");
		return;
	}

	LOG_INF("Capabilities:");
	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];
		/* fourcc to string */
		LOG_INF("  %c%c%c%c width [%u; %u; %u] height [%u; %u; %u]",
		       (char)fcap->pixelformat,
		       (char)(fcap->pixelformat >> 8),
		       (char)(fcap->pixelformat >> 16),
		       (char)(fcap->pixelformat >> 24),
		       fcap->width_min, fcap->width_max, fcap->width_step,
		       fcap->height_min, fcap->height_max, fcap->height_step);
		i++;
	}

	/* Get/set video format */
	if (video_get_format(video, VIDEO_EP_OUT, &fmt)) {
		LOG_ERR("Unable to retrieve video format");
		return;
	}

	fmt.width = capabilities.x_resolution;
	fmt.height = capabilities.y_resolution;
	fmt.pitch = capabilities.x_resolution * 2;
	if (video_set_format(video, VIDEO_EP_OUT, &fmt)) {
		LOG_ERR("Unable to set video format");
		return;
	}

	/* Size to allocate for each buffer */
	bsize = fmt.pitch * fmt.height;
	/* Setup buffer descriptor for the display controller */
	buf_desc.buf_size = bsize;
	buf_desc.pitch = capabilities.x_resolution;
	buf_desc.width = capabilities.x_resolution;
	buf_desc.height = capabilities.y_resolution;
	LOG_INF("buf_desc: size %u", bsize);

	/* Alloc video buffers and enqueue for capture */
	for (i = 0; i < ARRAY_SIZE(buffers); i++) {
		buffers[i] = video_buffer_alloc(bsize);
		if (buffers[i] == NULL) {
			LOG_ERR("Unable to alloc video buffer");
			return;
		}

		video_enqueue(video, VIDEO_EP_OUT, buffers[i]);
	}

	/* Start video capture */
	if (video_stream_start(video)) {
		LOG_ERR("Unable to start capture (interface)");
		return;
	}

	LOG_INF("Capture started");
	display_blanking_off(display_dev);

	while (1) {
		int err;

		err = video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
		if (err) {
			LOG_ERR("Unable to dequeue video buf");
			return;
		}

		k_sleep(K_MSEC(100));

		err = video_enqueue(video, VIDEO_EP_OUT, vbuf);
		if (err) {
			LOG_ERR("Unable to requeue video buf");
			return;
		}

		LOG_DBG("LCD: %p, size %u, used %u",
			vbuf->buffer, vbuf->size, vbuf->bytesused);

		display_write(display_dev, 0, 0, &buf_desc, vbuf->buffer);
	}
}
