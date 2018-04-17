/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
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

#ifndef _NPU_COMMON_H
#define _NPU_COMMON_H

/* -------------------------------------------------------------------------
 * Includes
 * -------------------------------------------------------------------------
 */
#include <asm/dma-iommu.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/msm_npu.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "npu_mgr.h"

/* -------------------------------------------------------------------------
 * Defines
 * -------------------------------------------------------------------------
 */
/* get npu info */
#define MSM_NPU_GET_INFO_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 1, compat_caddr_t)

/* map buf */
#define MSM_NPU_MAP_BUF_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 2, compat_caddr_t)

/* map buf */
#define MSM_NPU_UNMAP_BUF_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 3, compat_caddr_t)

/* load network */
#define MSM_NPU_LOAD_NETWORK_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 4, compat_caddr_t)

/* unload network */
#define MSM_NPU_UNLOAD_NETWORK_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 5, compat_caddr_t)

/* exec network */
#define MSM_NPU_EXEC_NETWORK_32 \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 6, compat_caddr_t)

#define NPU_MAX_MBOX_NUM	    2
#define NPU_MBOX_LOW_PRI	    0
#define NPU_MBOX_HIGH_PRI	    1

#define DEFAULT_REG_DUMP_NUM	64
#define ROW_BYTES 16
#define GROUP_BYTES 4

#define NUM_TOTAL_CLKS          19
#define NPU_MAX_REGULATOR_NUM	2
#define NPU_MAX_DT_NAME_LEN	    21
#define NPU_MAX_PWRLEVELS		7

/* -------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------
 */
struct npu_smmu_ctx {
	int domain;
	struct dma_iommu_mapping *mmu_mapping;
	struct reg_bus_client *reg_bus_clt;
	int32_t attach_cnt;
};

struct npu_ion_buf {
	int fd;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	dma_addr_t iova;
	uint32_t size;
	void *phys_addr;
	void *buf;
	struct list_head list;
};

struct npu_clk {
	struct clk *clk;
	char clk_name[NPU_MAX_DT_NAME_LEN];
};

struct npu_regulator {
	struct regulator *regulator;
	char regulator_name[NPU_MAX_DT_NAME_LEN];
};

struct npu_debugfs_ctx {
	struct dentry *root;
	uint32_t reg_off;
	uint32_t reg_cnt;
	char *buf;
	size_t buf_len;
	uint8_t *log_buf;
	struct mutex log_lock;
	uint32_t log_num_bytes_buffered;
	uint32_t log_read_index;
	uint32_t log_write_index;
	uint32_t log_buf_size;
	bool sys_cache_disable;
};

struct npu_mbox {
	struct mbox_client client;
	struct mbox_chan *chan;
	struct npu_device *npu_dev;
	uint32_t id;
};

/**
 * struct npul_pwrlevel - Struct holding different pwrlevel info obtained from
 * from dtsi file
 * @freq[]:              NPU frequency vote in Hz
 */
struct npu_pwrlevel {
	long clk_freq[NUM_TOTAL_CLKS];
};

/**
 * struct npu_pwrctrl - Power control settings for a NPU device
 * @pwr_vote_num - voting information for power enable
 * @pwrlevels - List of supported power levels
 * @active_pwrlevel - The currently active power level
 * @default_pwrlevel - device wake up power level
 * @max_pwrlevel - maximum allowable powerlevel per the user
 * @min_pwrlevel - minimum allowable powerlevel per the user
 * @num_pwrlevels - number of available power levels
 * @devbw - bw device
 */
struct npu_pwrctrl {
	int32_t pwr_vote_num;

	struct npu_pwrlevel pwrlevels[NPU_MAX_PWRLEVELS];
	uint32_t active_pwrlevel;
	uint32_t default_pwrlevel;
	uint32_t max_pwrlevel;
	uint32_t min_pwrlevel;
	uint32_t num_pwrlevels;

	struct device *devbw;
	uint32_t bwmon_enabled;
};

struct npu_device {
	struct mutex ctx_lock;

	struct platform_device *pdev;

	dev_t dev_num;
	struct cdev cdev;
	struct class *class;
	struct device *device;

	size_t reg_size;
	char __iomem *npu_base;
	uint32_t npu_phys;

	uint32_t core_clk_num;
	struct npu_clk core_clks[NUM_TOTAL_CLKS];

	uint32_t regulator_num;
	struct npu_regulator regulators[NPU_MAX_DT_NAME_LEN];

	uint32_t irq;

	struct npu_ion_buf mapped_buffers;

	struct device *cb_device;

	struct npu_host_ctx host_ctx;
	struct npu_smmu_ctx smmu_ctx;
	struct npu_debugfs_ctx debugfs_ctx;

	struct npu_mbox mbox[NPU_MAX_MBOX_NUM];

	struct thermal_cooling_device *tcdev;
	struct npu_pwrctrl pwrctrl;

	struct llcc_slice_desc *sys_cache;
};


/* -------------------------------------------------------------------------
 * Function Prototypes
 * -------------------------------------------------------------------------
 */
int npu_debugfs_init(struct npu_device *npu_dev);
void npu_debugfs_deinit(struct npu_device *npu_dev);

int npu_enable_core_power(struct npu_device *npu_dev);
void npu_disable_core_power(struct npu_device *npu_dev);
int npu_enable_post_pil_clocks(struct npu_device *npu_dev);

irqreturn_t npu_intr_hdler(int irq, void *ptr);

int npu_set_power_level(struct npu_device *npu_dev, uint32_t pwr_level);

int fw_init(struct npu_device *npu_dev);
void fw_deinit(struct npu_device *npu_dev);

#endif /* _NPU_COMMON_H */
