/* Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _USB_BAM_H_
#define _USB_BAM_H_

#include <linux/msm-sps.h>
#include <linux/usb/ch9.h>

#define MAX_BAMS	NUM_CTRL	/* Bam per USB controllers */

/* Supported USB controllers*/
enum usb_ctrl {
	USB_CTRL_UNUSED = 0,
	NUM_CTRL,
};

enum peer_bam {
	QDSS_P_BAM = 0,
	MAX_PEER_BAMS,
};

enum usb_bam_pipe_dir {
	USB_TO_PEER_PERIPHERAL,
	PEER_PERIPHERAL_TO_USB,
};

enum usb_pipe_mem_type {
	SPS_PIPE_MEM = 0,	/* Default, SPS dedicated pipe memory */
	SYSTEM_MEM,		/* System RAM, requires allocation */
	OCI_MEM,		/* Shared memory among peripherals */
};

enum usb_bam_event_type {
	USB_BAM_EVENT_WAKEUP_PIPE = 0,	/* Wake a pipe */
	USB_BAM_EVENT_WAKEUP,		/* Wake a bam (first pipe waked) */
	USB_BAM_EVENT_INACTIVITY,	/* Inactivity on all pipes */
};

enum usb_bam_pipe_type {
	USB_BAM_PIPE_BAM2BAM = 0,	/* Connection is BAM2BAM (default) */
	USB_BAM_PIPE_SYS2BAM,		/* Connection is SYS2BAM or BAM2SYS
					 * depending on usb_bam_pipe_dir
					 */
	USB_BAM_MAX_PIPE_TYPES,
};

/*
 * struct usb_bam_event_info: suspend/resume event information.
 * @type: usb bam event type.
 * @event: holds event data.
 * @callback: suspend/resume callback.
 * @param: port num (for suspend) or NULL (for resume).
 * @event_w: holds work queue parameters.
 */
struct usb_bam_event_info {
	enum usb_bam_event_type type;
	struct sps_register_event event;
	int (*callback)(void *);
	void *param;
	struct work_struct event_w;
};

/*
 * struct usb_bam_pipe_connect: pipe connection information
 * between USB/HSIC BAM and another BAM. USB/HSIC BAM can be
 * either src BAM or dst BAM
 * @name: pipe description.
 * @mem_type: type of memory used for BAM FIFOs
 * @src_phy_addr: src bam physical address.
 * @src_pipe_index: src bam pipe index.
 * @dst_phy_addr: dst bam physical address.
 * @dst_pipe_index: dst bam pipe index.
 * @data_fifo_base_offset: data fifo offset.
 * @data_fifo_size: data fifo size.
 * @desc_fifo_base_offset: descriptor fifo offset.
 * @desc_fifo_size: descriptor fifo size.
 * @data_mem_buf: data fifo buffer.
 * @desc_mem_buf: descriptor fifo buffer.
 * @event: event for wakeup.
 * @enabled: true if pipe is enabled.
 * @suspended: true if pipe is suspended.
 * @cons_stopped: true is pipe has consumer requests stopped.
 * @prod_stopped: true if pipe has producer requests stopped.
 * @priv: private data to return upon activity_notify
 *	or inactivity_notify callbacks.
 * @activity_notify: callback to invoke on activity on one of the in pipes.
 * @inactivity_notify: callback to invoke on inactivity on all pipes.
 * @start: callback to invoke to enqueue transfers on a pipe.
 * @stop: callback to invoke on dequeue transfers on a pipe.
 * @start_stop_param: param for the start/stop callbacks.
 */
struct usb_bam_pipe_connect {
	const char *name;
	u32 pipe_num;
	enum usb_pipe_mem_type mem_type;
	enum usb_bam_pipe_dir dir;
	enum usb_ctrl bam_type;
	enum peer_bam peer_bam;
	enum usb_bam_pipe_type pipe_type;
	u32 src_phy_addr;
	u32 src_pipe_index;
	u32 dst_phy_addr;
	u32 dst_pipe_index;
	u32 data_fifo_base_offset;
	u32 data_fifo_size;
	u32 desc_fifo_base_offset;
	u32 desc_fifo_size;
	struct sps_mem_buffer data_mem_buf;
	struct sps_mem_buffer desc_mem_buf;
	struct usb_bam_event_info event;
	bool enabled;
	bool suspended;
	bool cons_stopped;
	bool prod_stopped;
	void *priv;
	int (*activity_notify)(void *priv);
	int (*inactivity_notify)(void *priv);
	void (*start)(void *, enum usb_bam_pipe_dir);
	void (*stop)(void *, enum usb_bam_pipe_dir);
	void *start_stop_param;
	bool reset_pipe_after_lpm;
};

/**
 * struct msm_usb_bam_data: pipe connection information
 * between USB/HSIC BAM and another BAM. USB/HSIC BAM can be
 * either src BAM or dst BAM
 * @usb_bam_num_pipes: max number of pipes to use.
 * @active_conn_num: number of active pipe connections.
 * @usb_bam_fifo_baseaddr: base address for bam pipe's data and descriptor
 *                         fifos. This can be on chip memory (ocimem) or usb
 *                         private memory.
 * @reset_on_connect: BAM must be reset before its first pipe connect
 * @reset_on_disconnect: BAM must be reset after its last pipe disconnect
 * @disable_clk_gating: Disable clock gating
 * @override_threshold: Override the default threshold value for Read/Write
 *                         event generation by the BAM towards another BAM.
 * @max_mbps_highspeed: Maximum Mbits per seconds that the USB core
 *		can work at in bam2bam mode when connected to HS host.
 * @max_mbps_superspeed: Maximum Mbits per seconds that the USB core
 *		can work at in bam2bam mode when connected to SS host.
 */
struct msm_usb_bam_data {
	u8 max_connections;
	int usb_bam_num_pipes;
	phys_addr_t usb_bam_fifo_baseaddr;
	bool reset_on_connect;
	bool reset_on_disconnect;
	bool disable_clk_gating;
	u32 override_threshold;
	u32 max_mbps_highspeed;
	u32 max_mbps_superspeed;
	enum usb_ctrl bam_type;
};

#if  IS_ENABLED(CONFIG_USB_BAM)
/**
 * Connect USB-to-Peripheral SPS connection.
 *
 * This function returns the allocated pipe number.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Connection index.
 *
 * @bam_pipe_idx - allocated pipe index.
 *
 * @return 0 on success, negative value on error
 *
 */
int usb_bam_connect(enum usb_ctrl bam_type, int idx, u32 *bam_pipe_idx);

/**
 * Register a wakeup callback from peer BAM.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Connection index.
 *
 * @callback - the callback function
 *
 * @return 0 on success, negative value on error
 */
int usb_bam_register_wake_cb(enum usb_ctrl bam_type, u8 idx,
	int (*callback)(void *), void *param);

/**
 * Register callbacks for start/stop of transfers.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Connection index
 *
 * @start - the callback function that will be called in USB
 *				driver to start transfers
 * @stop - the callback function that will be called in USB
 *				driver to stop transfers
 *
 * @param - context that the caller can supply
 *
 * @return 0 on success, negative value on error
 */
int usb_bam_register_start_stop_cbs(enum usb_ctrl bam_type,
	u8 idx,
	void (*start)(void *, enum usb_bam_pipe_dir),
	void (*stop)(void *, enum usb_bam_pipe_dir),
	void *param);

/**
 * Disconnect USB-to-Periperal SPS connection.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Connection index.
 *
 * @return 0 on success, negative value on error
 */
int usb_bam_disconnect_pipe(enum usb_ctrl bam_type, u8 idx);

/**
 * Returns usb bam connection parameters.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Connection index.
 *
 * @usb_bam_pipe_idx - Usb bam pipe index.
 *
 * @desc_fifo - Descriptor fifo parameters.
 *
 * @data_fifo - Data fifo parameters.
 *
 * @return pipe index on success, negative value on error.
 */
int get_bam2bam_connection_info(enum usb_ctrl bam_type, u8 idx,
	u32 *usb_bam_pipe_idx, struct sps_mem_buffer *desc_fifo,
	struct sps_mem_buffer *data_fifo, enum usb_pipe_mem_type *mem_type);

/**
 * Returns usb bam connection parameters for qdss pipe.
 * @usb_bam_handle - Usb bam handle.
 * @usb_bam_pipe_idx - Usb bam pipe index.
 * @peer_pipe_idx - Peer pipe index.
 * @desc_fifo - Descriptor fifo parameters.
 * @data_fifo - Data fifo parameters.
 * @return pipe index on success, negative value on error.
 */
int get_qdss_bam_connection_info(
	unsigned long *usb_bam_handle, u32 *usb_bam_pipe_idx,
	u32 *peer_pipe_idx, struct sps_mem_buffer *desc_fifo,
	struct sps_mem_buffer *data_fifo, enum usb_pipe_mem_type *mem_type);

/*
 * Indicates if the client of the USB BAM is ready to start
 * sending/receiving transfers.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @client - Usb pipe peer (a2, ipa, qdss...)
 *
 * @dir - In (from peer to usb) or out (from usb to peer)
 *
 * @num - Pipe number.
 *
 * @return 0 on success, negative value on error
 */
int usb_bam_get_connection_idx(enum usb_ctrl bam_type, enum peer_bam client,
	enum usb_bam_pipe_dir dir, u32 num);

/*
 * return the usb controller bam type used for the supplied connection index
 *
 * @core_name - Core name (ssusb/hsusb/hsic).
 *
 * @return usb control bam type
 */
enum usb_ctrl usb_bam_get_bam_type(const char *core_name);

/*
 * Indicates the type of connection the USB side of the connection is.
 *
 * @bam_type - USB BAM type - dwc3/CI/hsic
 *
 * @idx - Pipe number.
 *
 * @type - Type of connection
 *
 * @return 0 on success, negative value on error
 */
int usb_bam_get_pipe_type(enum usb_ctrl bam_type,
			  u8 idx, enum usb_bam_pipe_type *type);

/* Allocates memory for data fifo and descriptor fifos. */
int usb_bam_alloc_fifos(enum usb_ctrl cur_bam, u8 idx);

/* Frees memory for data fifo and descriptor fifos. */
int usb_bam_free_fifos(enum usb_ctrl cur_bam, u8 idx);

#else
static inline int usb_bam_connect(enum usb_ctrl bam, u8 idx, u32 *bam_pipe_idx)
{
	return -ENODEV;
}

static inline int usb_bam_register_wake_cb(enum usb_ctrl bam_type, u8 idx,
	int (*callback)(void *), void *param)
{
	return -ENODEV;
}

static inline int usb_bam_register_start_stop_cbs(enum usb_ctrl bam, u8 idx,
	void (*start)(void *, enum usb_bam_pipe_dir),
	void (*stop)(void *, enum usb_bam_pipe_dir),
	void *param)
{
	return -ENODEV;
}

static inline int usb_bam_disconnect_pipe(enum usb_ctrl bam_type, u8 idx)
{
	return -ENODEV;
}

static inline int get_bam2bam_connection_info(enum usb_ctrl bam_type, u8 idx,
	u32 *usb_bam_pipe_idx, struct sps_mem_buffer *desc_fifo,
	struct sps_mem_buffer *data_fifo, enum usb_pipe_mem_type *mem_type)
{
	return -ENODEV;
}

static inline int get_qdss_bam_connection_info(
	unsigned long *usb_bam_handle, u32 *usb_bam_pipe_idx,
	u32 *peer_pipe_idx, struct sps_mem_buffer *desc_fifo,
	struct sps_mem_buffer *data_fifo, enum usb_pipe_mem_type *mem_type)
{
	return -ENODEV;
}

static inline int usb_bam_get_connection_idx(enum usb_ctrl bam_type,
		enum peer_bam client, enum usb_bam_pipe_dir dir, u32 num)
{
	return -ENODEV;
}

static inline enum usb_ctrl usb_bam_get_bam_type(const char *core_nam)
{
	return -ENODEV;
}

static inline int usb_bam_get_pipe_type(enum usb_ctrl bam_type, u8 idx,
					enum usb_bam_pipe_type *type)
{
	return -ENODEV;
}

static inline int usb_bam_alloc_fifos(enum usb_ctrl cur_bam, u8 idx)
{
	return false;
}

static inline int usb_bam_free_fifos(enum usb_ctrl cur_bam, u8 idx)
{
	return false;
}
#endif

/* CONFIG_PM */
#ifdef CONFIG_PM
static inline int get_pm_runtime_counter(struct device *dev)
{
	return atomic_read(&dev->power.usage_count);
}
#else
/* !CONFIG_PM */
static inline int get_pm_runtime_counter(struct device *dev)
{ return -EOPNOTSUPP; }
#endif
#endif				/* _USB_BAM_H_ */
