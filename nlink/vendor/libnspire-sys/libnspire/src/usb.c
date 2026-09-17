/*
    This file is part of libnspire.

    libnspire is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "usb.h"
#include "error.h"

#include <string.h>

#ifndef __ANDROID__
#include <libusb.h>
#endif

#ifdef __ANDROID__
#include <errno.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#endif

#define NSP_DEFAULT_CONFIG 1
#define NSP_DEFAULT_IFACE 0
#define NSP_TIMEOUT 10000

#ifdef __ANDROID__
static int android_fd = -1;
static unsigned char android_ep_in;
static unsigned char android_ep_out;

int nspire_android_setup(int fd, unsigned char ep_in, unsigned char ep_out) {
	android_fd = fd;
	android_ep_in = ep_in;
	android_ep_out = ep_out;
	return NSPIRE_ERR_SUCCESS;
}

int usb_init(void) {
	return NSPIRE_ERR_SUCCESS;
}

void usb_finish(void) {
}

int usb_get_device(usb_device_t *handle, libusb_device_handle *dev) {
	(void)dev;
	if (android_fd < 0)
		return -NSPIRE_ERR_NODEVICE;
	handle->dev = NULL;
	handle->ep_in = android_ep_in;
	handle->ep_out = android_ep_out;
	return NSPIRE_ERR_SUCCESS;
}

void usb_free_device(usb_device_t *handle) {
	(void)handle;
}

int usb_bulk(usb_device_t *handle, unsigned char ep, void *ptr, int len,
	     int *transferred, unsigned int timeout) {
	struct usbdevfs_bulktransfer b;
	int r;
	(void)handle;
	memset(&b, 0, sizeof(b));
	b.ep = ep;
	b.len = (unsigned int)len;
	b.timeout = timeout;
	b.data = ptr;
	r = ioctl(android_fd, USBDEVFS_BULK, &b);
	if (r < 0) {
		if (errno == ETIMEDOUT)
			return -NSPIRE_ERR_TIMEOUT;
		if (errno == ENODEV || errno == ESHUTDOWN)
			return -NSPIRE_ERR_NODEVICE;
		return -NSPIRE_ERR_LIBUSB;
	}
	if (transferred)
		*transferred = r;
	return 0;
}

#else

static libusb_context *usb_ctx = NULL;

int nspire_android_setup(int fd, unsigned char ep_in, unsigned char ep_out) {
	(void)fd;
	(void)ep_in;
	(void)ep_out;
	return -NSPIRE_ERR_INVALID;
}

int usb_init(void) {
	if (usb_ctx)
		return NSPIRE_ERR_SUCCESS;
	if (libusb_init(&usb_ctx))
		return -NSPIRE_ERR_LIBUSB;
	return NSPIRE_ERR_SUCCESS;
}

void usb_finish(void) {
	libusb_exit(usb_ctx);
}

int usb_get_device(usb_device_t *handle, libusb_device_handle *dev) {
	int i;
	struct libusb_config_descriptor *config;
	const struct libusb_interface_descriptor *iface;

	if (libusb_set_configuration(dev, NSP_DEFAULT_CONFIG))
		goto error_close;
	if (libusb_reset_device(dev))
		goto error_close;
	if (libusb_claim_interface(dev, NSP_DEFAULT_IFACE))
		goto error_close;

	if (libusb_get_active_config_descriptor(libusb_get_device(dev), &config))
		goto error_close;
	if (config->bNumInterfaces < NSP_DEFAULT_IFACE)
		goto error_free_desc;
	iface = config->interface[NSP_DEFAULT_IFACE].altsetting;

	handle->ep_in = 0;
	handle->ep_out = 0;
	for (i = 0; i < iface->bNumEndpoints; i++) {
		unsigned char ep = iface->endpoint[i].bEndpointAddress;
		if (ep & LIBUSB_ENDPOINT_IN) {
			if (!handle->ep_in)
				handle->ep_in = ep;
		} else {
			if (!handle->ep_out)
				handle->ep_out = ep;
		}
	}
	libusb_free_config_descriptor(config);

	if (!handle->ep_in || !handle->ep_out)
		goto error_close;

	handle->dev = dev;
	return NSPIRE_ERR_SUCCESS;
error_free_desc:
	libusb_free_config_descriptor(config);
error_close:
	return -NSPIRE_ERR_NODEVICE;
}

void usb_free_device(usb_device_t *handle) {
	libusb_release_interface(handle->dev, NSP_DEFAULT_IFACE);
}

int usb_bulk(usb_device_t *handle, unsigned char ep, void *ptr, int len,
	     int *transferred, unsigned int timeout) {
	int ret = libusb_bulk_transfer(handle->dev, ep, ptr, len, transferred, timeout);
	switch (ret) {
	case 0:
		return 0;
	case LIBUSB_ERROR_NO_DEVICE:
		return -NSPIRE_ERR_NODEVICE;
	case LIBUSB_ERROR_TIMEOUT:
		return -NSPIRE_ERR_TIMEOUT;
	default:
		return -NSPIRE_ERR_LIBUSB;
	}
}

#endif

static int usb_xfer(usb_device_t *handle, unsigned char ep, void *ptr, int len) {
	int transferred = 0;
	int ret = usb_bulk(handle, ep, ptr, len, &transferred, NSP_TIMEOUT);
	if (ret == 0)
		return (len - transferred);
	return ret;
}

int usb_write(usb_device_t *handle, void *ptr, int len) {
	return usb_xfer(handle, handle->ep_out, ptr, len);
}

int usb_read(usb_device_t *handle, void *ptr, int len) {
	return usb_xfer(handle, handle->ep_in, ptr, len);
}
