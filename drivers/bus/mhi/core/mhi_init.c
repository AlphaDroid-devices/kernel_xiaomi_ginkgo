/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/mhi.h>
#include "mhi_internal.h"

const char * const mhi_ee_str[MHI_EE_MAX] = {
	[MHI_EE_PBL] = "PBL",
	[MHI_EE_SBL] = "SBL",
	[MHI_EE_AMSS] = "AMSS",
	[MHI_EE_BHIE] = "BHIE",
	[MHI_EE_RDDM] = "RDDM",
	[MHI_EE_PTHRU] = "PASS THRU",
	[MHI_EE_EDL] = "EDL",
	[MHI_EE_DISABLE_TRANSITION] = "DISABLE",
};

const char * const mhi_state_tran_str[MHI_ST_TRANSITION_MAX] = {
	[MHI_ST_TRANSITION_PBL] = "PBL",
	[MHI_ST_TRANSITION_READY] = "READY",
	[MHI_ST_TRANSITION_SBL] = "SBL",
	[MHI_ST_TRANSITION_AMSS] = "AMSS",
	[MHI_ST_TRANSITION_BHIE] = "BHIE",
};

const char * const mhi_state_str[MHI_STATE_MAX] = {
	[MHI_STATE_RESET] = "RESET",
	[MHI_STATE_READY] = "READY",
	[MHI_STATE_M0] = "M0",
	[MHI_STATE_M1] = "M1",
	[MHI_STATE_M2] = "M2",
	[MHI_STATE_M3] = "M3",
	[MHI_STATE_BHI] = "BHI",
	[MHI_STATE_SYS_ERR] = "SYS_ERR",
};

static const char * const mhi_pm_state_str[] = {
	"DISABLE",
	"POR",
	"M0",
	"M1",
	"M1->M2",
	"M2",
	"M?->M3",
	"M3",
	"M3->M0",
	"FW DL Error",
	"SYS_ERR Detect",
	"SYS_ERR Process",
	"SHUTDOWN Process",
	"LD or Error Fatal Detect",
};

struct mhi_bus mhi_bus;

const char *to_mhi_pm_state_str(enum MHI_PM_STATE state)
{
	int index = find_last_bit((unsigned long *)&state, 32);

	if (index >= ARRAY_SIZE(mhi_pm_state_str))
		return "Invalid State";

	return mhi_pm_state_str[index];
}

/* MHI protocol require transfer ring to be aligned to ring length */
static int mhi_alloc_aligned_ring(struct mhi_controller *mhi_cntrl,
				  struct mhi_ring *ring,
				  u64 len)
{
	ring->alloc_size = len + (len - 1);
	ring->pre_aligned = mhi_alloc_coherent(mhi_cntrl, ring->alloc_size,
					       &ring->dma_handle, GFP_KERNEL);
	if (!ring->pre_aligned)
		return -ENOMEM;

	ring->iommu_base = (ring->dma_handle + (len - 1)) & ~(len - 1);
	ring->base = ring->pre_aligned + (ring->iommu_base - ring->dma_handle);
	return 0;
}

void mhi_deinit_free_irq(struct mhi_controller *mhi_cntrl)
{
	int i;
	struct mhi_event *mhi_event = mhi_cntrl->mhi_event;

	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		free_irq(mhi_cntrl->irq[mhi_event->msi], mhi_event);
	}

	free_irq(mhi_cntrl->irq[0], mhi_cntrl);
}

int mhi_init_irq_setup(struct mhi_controller *mhi_cntrl)
{
	int i;
	int ret;
	struct mhi_event *mhi_event = mhi_cntrl->mhi_event;

	/* for BHI INTVEC msi */
	ret = request_threaded_irq(mhi_cntrl->irq[0], mhi_intvec_handlr,
				   mhi_intvec_threaded_handlr, IRQF_ONESHOT,
				   "mhi", mhi_cntrl);
	if (ret)
		return ret;

	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		ret = request_irq(mhi_cntrl->irq[mhi_event->msi],
				  mhi_msi_handlr, IRQF_SHARED, "mhi",
				  mhi_event);
		if (ret) {
			MHI_ERR("Error requesting irq:%d for ev:%d\n",
				mhi_cntrl->irq[mhi_event->msi], i);
			goto error_request;
		}
	}

	return 0;

error_request:
	for (--i, --mhi_event; i >= 0; i--, mhi_event--) {
		if (mhi_event->offload_ev)
			continue;

		free_irq(mhi_cntrl->irq[mhi_event->msi], mhi_event);
	}
	free_irq(mhi_cntrl->irq[0], mhi_cntrl);

	return ret;
}

void mhi_deinit_dev_ctxt(struct mhi_controller *mhi_cntrl)
{
	int i;
	struct mhi_ctxt *mhi_ctxt = mhi_cntrl->mhi_ctxt;
	struct mhi_cmd *mhi_cmd;
	struct mhi_event *mhi_event;
	struct mhi_ring *ring;

	mhi_cmd = mhi_cntrl->mhi_cmd;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++) {
		ring = &mhi_cmd->ring;
		mhi_free_coherent(mhi_cntrl, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
		ring->base = NULL;
		ring->iommu_base = 0;
	}

	mhi_free_coherent(mhi_cntrl,
			  sizeof(*mhi_ctxt->cmd_ctxt) * NR_OF_CMD_RINGS,
			  mhi_ctxt->cmd_ctxt, mhi_ctxt->cmd_ctxt_addr);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		ring = &mhi_event->ring;
		mhi_free_coherent(mhi_cntrl, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
		ring->base = NULL;
		ring->iommu_base = 0;
	}

	mhi_free_coherent(mhi_cntrl, sizeof(*mhi_ctxt->er_ctxt) *
			  mhi_cntrl->total_ev_rings, mhi_ctxt->er_ctxt,
			  mhi_ctxt->er_ctxt_addr);

	mhi_free_coherent(mhi_cntrl, sizeof(*mhi_ctxt->chan_ctxt) *
			  mhi_cntrl->max_chan, mhi_ctxt->chan_ctxt,
			  mhi_ctxt->chan_ctxt_addr);

	kfree(mhi_ctxt);
	mhi_cntrl->mhi_ctxt = NULL;
}

static int mhi_init_debugfs_mhi_states_open(struct inode *inode,
					    struct file *fp)
{
	return single_open(fp, mhi_debugfs_mhi_states_show, inode->i_private);
}

static int mhi_init_debugfs_mhi_event_open(struct inode *inode, struct file *fp)
{
	return single_open(fp, mhi_debugfs_mhi_event_show, inode->i_private);
}

static int mhi_init_debugfs_mhi_chan_open(struct inode *inode, struct file *fp)
{
	return single_open(fp, mhi_debugfs_mhi_chan_show, inode->i_private);
}

static const struct file_operations debugfs_state_ops = {
	.open = mhi_init_debugfs_mhi_states_open,
	.release = single_release,
	.read = seq_read,
};

static const struct file_operations debugfs_ev_ops = {
	.open = mhi_init_debugfs_mhi_event_open,
	.release = single_release,
	.read = seq_read,
};

static const struct file_operations debugfs_chan_ops = {
	.open = mhi_init_debugfs_mhi_chan_open,
	.release = single_release,
	.read = seq_read,
};

DEFINE_SIMPLE_ATTRIBUTE(debugfs_trigger_reset_fops, NULL,
			mhi_debugfs_trigger_reset, "%llu\n");

void mhi_init_debugfs(struct mhi_controller *mhi_cntrl)
{
	struct dentry *dentry;
	char node[32];

	if (!mhi_cntrl->parent)
		return;

	snprintf(node, sizeof(node), "%04x_%02u:%02u.%02u",
		 mhi_cntrl->dev_id, mhi_cntrl->domain, mhi_cntrl->bus,
		 mhi_cntrl->slot);

	dentry = debugfs_create_dir(node, mhi_cntrl->parent);
	if (IS_ERR_OR_NULL(dentry))
		return;

	debugfs_create_file("states", 0444, dentry, mhi_cntrl,
			    &debugfs_state_ops);
	debugfs_create_file("events", 0444, dentry, mhi_cntrl,
			    &debugfs_ev_ops);
	debugfs_create_file("chan", 0444, dentry, mhi_cntrl, &debugfs_chan_ops);
	debugfs_create_file("reset", 0444, dentry, mhi_cntrl,
			    &debugfs_trigger_reset_fops);
	mhi_cntrl->dentry = dentry;
}

void mhi_deinit_debugfs(struct mhi_controller *mhi_cntrl)
{
	debugfs_remove_recursive(mhi_cntrl->dentry);
	mhi_cntrl->dentry = NULL;
}

int mhi_init_dev_ctxt(struct mhi_controller *mhi_cntrl)
{
	struct mhi_ctxt *mhi_ctxt;
	struct mhi_chan_ctxt *chan_ctxt;
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_cmd_ctxt *cmd_ctxt;
	struct mhi_chan *mhi_chan;
	struct mhi_event *mhi_event;
	struct mhi_cmd *mhi_cmd;
	int ret = -ENOMEM, i;

	atomic_set(&mhi_cntrl->dev_wake, 0);
	atomic_set(&mhi_cntrl->alloc_size, 0);

	mhi_ctxt = kzalloc(sizeof(*mhi_ctxt), GFP_KERNEL);
	if (!mhi_ctxt)
		return -ENOMEM;

	/* setup channel ctxt */
	mhi_ctxt->chan_ctxt = mhi_alloc_coherent(mhi_cntrl,
			sizeof(*mhi_ctxt->chan_ctxt) * mhi_cntrl->max_chan,
			&mhi_ctxt->chan_ctxt_addr, GFP_KERNEL);
	if (!mhi_ctxt->chan_ctxt)
		goto error_alloc_chan_ctxt;

	mhi_chan = mhi_cntrl->mhi_chan;
	chan_ctxt = mhi_ctxt->chan_ctxt;
	for (i = 0; i < mhi_cntrl->max_chan; i++, chan_ctxt++, mhi_chan++) {
		/* If it's offload channel skip this step */
		if (mhi_chan->offload_ch)
			continue;

		chan_ctxt->chstate = MHI_CH_STATE_DISABLED;
		chan_ctxt->brstmode = mhi_chan->db_cfg.brstmode;
		chan_ctxt->pollcfg = mhi_chan->db_cfg.pollcfg;
		chan_ctxt->chtype = mhi_chan->dir;
		chan_ctxt->erindex = mhi_chan->er_index;

		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
		mhi_chan->tre_ring.db_addr = &chan_ctxt->wp;
	}

	/* setup event context */
	mhi_ctxt->er_ctxt = mhi_alloc_coherent(mhi_cntrl,
			sizeof(*mhi_ctxt->er_ctxt) * mhi_cntrl->total_ev_rings,
			&mhi_ctxt->er_ctxt_addr, GFP_KERNEL);
	if (!mhi_ctxt->er_ctxt)
		goto error_alloc_er_ctxt;

	er_ctxt = mhi_ctxt->er_ctxt;
	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, er_ctxt++,
		     mhi_event++) {
		struct mhi_ring *ring = &mhi_event->ring;

		/* it's a satellite ev, we do not touch it */
		if (mhi_event->offload_ev)
			continue;

		er_ctxt->intmodc = 0;
		er_ctxt->intmodt = mhi_event->intmod;
		er_ctxt->ertype = MHI_ER_TYPE_VALID;
		er_ctxt->msivec = mhi_event->msi;
		mhi_event->db_cfg.db_mode = true;

		ring->el_size = sizeof(struct __packed mhi_tre);
		ring->len = ring->el_size * ring->elements;
		ret = mhi_alloc_aligned_ring(mhi_cntrl, ring, ring->len);
		if (ret)
			goto error_alloc_er;

		ring->rp = ring->wp = ring->base;
		er_ctxt->rbase = ring->iommu_base;
		er_ctxt->rp = er_ctxt->wp = er_ctxt->rbase;
		er_ctxt->rlen = ring->len;
		ring->ctxt_wp = &er_ctxt->wp;
	}

	/* setup cmd context */
	mhi_ctxt->cmd_ctxt = mhi_alloc_coherent(mhi_cntrl,
				sizeof(*mhi_ctxt->cmd_ctxt) * NR_OF_CMD_RINGS,
				&mhi_ctxt->cmd_ctxt_addr, GFP_KERNEL);
	if (!mhi_ctxt->cmd_ctxt)
		goto error_alloc_er;

	mhi_cmd = mhi_cntrl->mhi_cmd;
	cmd_ctxt = mhi_ctxt->cmd_ctxt;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++, cmd_ctxt++) {
		struct mhi_ring *ring = &mhi_cmd->ring;

		ring->el_size = sizeof(struct __packed mhi_tre);
		ring->elements = CMD_EL_PER_RING;
		ring->len = ring->el_size * ring->elements;
		ret = mhi_alloc_aligned_ring(mhi_cntrl, ring, ring->len);
		if (ret)
			goto error_alloc_cmd;

		ring->rp = ring->wp = ring->base;
		cmd_ctxt->rbase = ring->iommu_base;
		cmd_ctxt->rp = cmd_ctxt->wp = cmd_ctxt->rbase;
		cmd_ctxt->rlen = ring->len;
		ring->ctxt_wp = &cmd_ctxt->wp;
	}

	mhi_cntrl->mhi_ctxt = mhi_ctxt;

	return 0;

error_alloc_cmd:
	for (--i, --mhi_cmd; i >= 0; i--, mhi_cmd--) {
		struct mhi_ring *ring = &mhi_cmd->ring;

		mhi_free_coherent(mhi_cntrl, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
	}
	mhi_free_coherent(mhi_cntrl,
			  sizeof(*mhi_ctxt->cmd_ctxt) * NR_OF_CMD_RINGS,
			  mhi_ctxt->cmd_ctxt, mhi_ctxt->cmd_ctxt_addr);
	i = mhi_cntrl->total_ev_rings;
	mhi_event = mhi_cntrl->mhi_event + i;

error_alloc_er:
	for (--i, --mhi_event; i >= 0; i--, mhi_event--) {
		struct mhi_ring *ring = &mhi_event->ring;

		if (mhi_event->offload_ev)
			continue;

		mhi_free_coherent(mhi_cntrl, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
	}
	mhi_free_coherent(mhi_cntrl, sizeof(*mhi_ctxt->er_ctxt) *
			  mhi_cntrl->total_ev_rings, mhi_ctxt->er_ctxt,
			  mhi_ctxt->er_ctxt_addr);

error_alloc_er_ctxt:
	mhi_free_coherent(mhi_cntrl, sizeof(*mhi_ctxt->chan_ctxt) *
			  mhi_cntrl->max_chan, mhi_ctxt->chan_ctxt,
			  mhi_ctxt->chan_ctxt_addr);

error_alloc_chan_ctxt:
	kfree(mhi_ctxt);

	return ret;
}

int mhi_init_mmio(struct mhi_controller *mhi_cntrl)
{
	u32 val;
	int i, ret;
	struct mhi_chan *mhi_chan;
	struct mhi_event *mhi_event;
	void __iomem *base = mhi_cntrl->regs;
	struct {
		u32 offset;
		u32 mask;
		u32 shift;
		u32 val;
	} reg_info[] = {
		{
			CCABAP_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->mhi_ctxt->chan_ctxt_addr),
		},
		{
			CCABAP_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->mhi_ctxt->chan_ctxt_addr),
		},
		{
			ECABAP_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->mhi_ctxt->er_ctxt_addr),
		},
		{
			ECABAP_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->mhi_ctxt->er_ctxt_addr),
		},
		{
			CRCBAP_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->mhi_ctxt->cmd_ctxt_addr),
		},
		{
			CRCBAP_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->mhi_ctxt->cmd_ctxt_addr),
		},
		{
			MHICFG, MHICFG_NER_MASK, MHICFG_NER_SHIFT,
			mhi_cntrl->total_ev_rings,
		},
		{
			MHICFG, MHICFG_NHWER_MASK, MHICFG_NHWER_SHIFT,
			mhi_cntrl->hw_ev_rings,
		},
		{
			MHICTRLBASE_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHICTRLBASE_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHIDATABASE_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHIDATABASE_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHICTRLLIMIT_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHICTRLLIMIT_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHIDATALIMIT_HIGHER, U32_MAX, 0,
			upper_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHIDATALIMIT_LOWER, U32_MAX, 0,
			lower_32_bits(mhi_cntrl->iova_stop),
		},
		{ 0, 0, 0 }
	};

	MHI_LOG("Initializing MMIO\n");

	/* set up DB register for all the chan rings */
	ret = mhi_read_reg_field(mhi_cntrl, base, CHDBOFF, CHDBOFF_CHDBOFF_MASK,
				 CHDBOFF_CHDBOFF_SHIFT, &val);
	if (ret)
		return -EIO;

	MHI_LOG("CHDBOFF:0x%x\n", val);

	/* setup wake db */
	mhi_cntrl->wake_db = base + val + (8 * MHI_DEV_WAKE_DB);
	mhi_write_reg(mhi_cntrl, mhi_cntrl->wake_db, 4, 0);
	mhi_write_reg(mhi_cntrl, mhi_cntrl->wake_db, 0, 0);
	mhi_cntrl->wake_set = false;

	/* setup channel db addresses */
	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, val += 8, mhi_chan++)
		mhi_chan->tre_ring.db_addr = base + val;

	/* setup event ring db addresses */
	ret = mhi_read_reg_field(mhi_cntrl, base, ERDBOFF, ERDBOFF_ERDBOFF_MASK,
				 ERDBOFF_ERDBOFF_SHIFT, &val);
	if (ret)
		return -EIO;

	MHI_LOG("ERDBOFF:0x%x\n", val);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, val += 8, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		mhi_event->ring.db_addr = base + val;
	}

	/* set up DB register for primary CMD rings */
	mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING].ring.db_addr = base + CRDB_LOWER;

	MHI_LOG("Programming all MMIO values.\n");
	for (i = 0; reg_info[i].offset; i++)
		mhi_write_reg_field(mhi_cntrl, base, reg_info[i].offset,
				    reg_info[i].mask, reg_info[i].shift,
				    reg_info[i].val);

	return 0;
}

void mhi_deinit_chan_ctxt(struct mhi_controller *mhi_cntrl,
			  struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring;
	struct mhi_ring *tre_ring;
	struct mhi_chan_ctxt *chan_ctxt;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];

	mhi_free_coherent(mhi_cntrl, tre_ring->alloc_size,
			  tre_ring->pre_aligned, tre_ring->dma_handle);
	kfree(buf_ring->base);

	buf_ring->base = tre_ring->base = NULL;
	chan_ctxt->rbase = 0;
}

int mhi_init_chan_ctxt(struct mhi_controller *mhi_cntrl,
		       struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring;
	struct mhi_ring *tre_ring;
	struct mhi_chan_ctxt *chan_ctxt;
	int ret;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	tre_ring->el_size = sizeof(struct __packed mhi_tre);
	tre_ring->len = tre_ring->el_size * tre_ring->elements;
	chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
	ret = mhi_alloc_aligned_ring(mhi_cntrl, tre_ring, tre_ring->len);
	if (ret)
		return -ENOMEM;

	buf_ring->el_size = sizeof(struct mhi_buf_info);
	buf_ring->len = buf_ring->el_size * buf_ring->elements;
	buf_ring->base = kzalloc(buf_ring->len, GFP_KERNEL);

	if (!buf_ring->base) {
		mhi_free_coherent(mhi_cntrl, tre_ring->alloc_size,
				  tre_ring->pre_aligned, tre_ring->dma_handle);
		return -ENOMEM;
	}

	chan_ctxt->chstate = MHI_CH_STATE_ENABLED;
	chan_ctxt->rbase = tre_ring->iommu_base;
	chan_ctxt->rp = chan_ctxt->wp = chan_ctxt->rbase;
	chan_ctxt->rlen = tre_ring->len;
	tre_ring->ctxt_wp = &chan_ctxt->wp;

	tre_ring->rp = tre_ring->wp = tre_ring->base;
	buf_ring->rp = buf_ring->wp = buf_ring->base;
	mhi_chan->db_cfg.db_mode = 1;

	/* update to all cores */
	smp_wmb();

	return 0;
}

int mhi_device_configure(struct mhi_device *mhi_dev,
			 enum dma_data_direction dir,
			 struct mhi_buf *cfg_tbl,
			 int elements)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_chan_ctxt *ch_ctxt;
	int er_index, chan;

	mhi_chan = (dir == DMA_TO_DEVICE) ? mhi_dev->ul_chan : mhi_dev->dl_chan;
	er_index = mhi_chan->er_index;
	chan = mhi_chan->chan;

	for (; elements > 0; elements--, cfg_tbl++) {
		/* update event context array */
		if (!strcmp(cfg_tbl->name, "ECA")) {
			er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[er_index];
			if (sizeof(*er_ctxt) != cfg_tbl->len) {
				MHI_ERR(
					"Invalid ECA size, expected:%zu actual%zu\n",
					sizeof(*er_ctxt), cfg_tbl->len);
				return -EINVAL;
			}
			memcpy((void *)er_ctxt, cfg_tbl->buf, sizeof(*er_ctxt));
			continue;
		}

		/* update channel context array */
		if (!strcmp(cfg_tbl->name, "CCA")) {
			ch_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[chan];
			if (cfg_tbl->len != sizeof(*ch_ctxt)) {
				MHI_ERR(
					"Invalid CCA size, expected:%zu actual:%zu\n",
					sizeof(*ch_ctxt), cfg_tbl->len);
				return -EINVAL;
			}
			memcpy((void *)ch_ctxt, cfg_tbl->buf, sizeof(*ch_ctxt));
			continue;
		}

		return -EINVAL;
	}

	return 0;
}

#if defined(CONFIG_OF)
static int of_parse_ev_cfg(struct mhi_controller *mhi_cntrl,
			   struct device_node *of_node)
{
	struct {
		u32 ev_cfg[MHI_EV_CFG_MAX];
	} *ev_cfg;
	int num, i, ret;
	struct mhi_event *mhi_event;
	u32 bit_cfg;

	num = of_property_count_elems_of_size(of_node, "mhi,ev-cfg",
					      sizeof(*ev_cfg));
	if (num <= 0)
		return -EINVAL;

	ev_cfg = kcalloc(num, sizeof(*ev_cfg), GFP_KERNEL);
	if (!ev_cfg)
		return -ENOMEM;

	mhi_cntrl->total_ev_rings = num;
	mhi_cntrl->mhi_event = kcalloc(num, sizeof(*mhi_cntrl->mhi_event),
				       GFP_KERNEL);
	if (!mhi_cntrl->mhi_event) {
		kfree(ev_cfg);
		return -ENOMEM;
	}

	ret = of_property_read_u32_array(of_node, "mhi,ev-cfg", (u32 *)ev_cfg,
					 num * sizeof(*ev_cfg) / sizeof(u32));
	if (ret)
		goto error_ev_cfg;

	/* populate ev ring */
	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		mhi_event->er_index = i;
		mhi_event->ring.elements =
			ev_cfg[i].ev_cfg[MHI_EV_CFG_ELEMENTS];
		mhi_event->intmod = ev_cfg[i].ev_cfg[MHI_EV_CFG_INTMOD];
		mhi_event->msi = ev_cfg[i].ev_cfg[MHI_EV_CFG_MSI];
		mhi_event->chan = ev_cfg[i].ev_cfg[MHI_EV_CFG_CHAN];
		if (mhi_event->chan >= mhi_cntrl->max_chan)
			goto error_ev_cfg;

		/* this event ring has a dedicated channel */
		if (mhi_event->chan)
			mhi_event->mhi_chan =
				&mhi_cntrl->mhi_chan[mhi_event->chan];

		mhi_event->priority = ev_cfg[i].ev_cfg[MHI_EV_CFG_PRIORITY];
		mhi_event->db_cfg.brstmode =
			ev_cfg[i].ev_cfg[MHI_EV_CFG_BRSTMODE];
		if (MHI_INVALID_BRSTMODE(mhi_event->db_cfg.brstmode))
			goto error_ev_cfg;

		mhi_event->db_cfg.process_db =
			(mhi_event->db_cfg.brstmode == MHI_BRSTMODE_ENABLE) ?
			mhi_db_brstmode : mhi_db_brstmode_disable;

		bit_cfg = ev_cfg[i].ev_cfg[MHI_EV_CFG_BITCFG];
		if (bit_cfg & MHI_EV_CFG_BIT_HW_EV) {
			mhi_event->hw_ring = true;
			mhi_cntrl->hw_ev_rings++;
		} else
			mhi_cntrl->sw_ev_rings++;

		mhi_event->cl_manage = !!(bit_cfg & MHI_EV_CFG_BIT_CL_MANAGE);
		mhi_event->offload_ev = !!(bit_cfg & MHI_EV_CFG_BIT_OFFLOAD_EV);
		mhi_event->ctrl_ev = !!(bit_cfg & MHI_EV_CFG_BIT_CTRL_EV);
	}

	kfree(ev_cfg);

	/* we need msi for each event ring + additional one for BHI */
	mhi_cntrl->msi_required = mhi_cntrl->total_ev_rings + 1;

	return 0;

 error_ev_cfg:
	kfree(ev_cfg);
	kfree(mhi_cntrl->mhi_event);
	return -EINVAL;
}
static int of_parse_ch_cfg(struct mhi_controller *mhi_cntrl,
			   struct device_node *of_node)
{
	int num, i, ret;
	struct {
		u32 chan_cfg[MHI_CH_CFG_MAX];
	} *chan_cfg;

	ret = of_property_read_u32(of_node, "mhi,max-channels",
				   &mhi_cntrl->max_chan);
	if (ret)
		return ret;
	num = of_property_count_elems_of_size(of_node, "mhi,chan-cfg",
					      sizeof(*chan_cfg));
	if (num <= 0 || num >= mhi_cntrl->max_chan)
		return -EINVAL;
	if (of_property_count_strings(of_node, "mhi,chan-names") != num)
		return -EINVAL;

	mhi_cntrl->mhi_chan = kcalloc(mhi_cntrl->max_chan,
				      sizeof(*mhi_cntrl->mhi_chan), GFP_KERNEL);
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;
	chan_cfg = kcalloc(num, sizeof(*chan_cfg), GFP_KERNEL);
	if (!chan_cfg) {
		kfree(mhi_cntrl->mhi_chan);
		return -ENOMEM;
	}

	ret = of_property_read_u32_array(of_node, "mhi,chan-cfg",
					 (u32 *)chan_cfg,
					 num * sizeof(*chan_cfg) / sizeof(u32));
	if (ret)
		goto error_chan_cfg;

	INIT_LIST_HEAD(&mhi_cntrl->lpm_chans);

	/* populate channel configurations */
	for (i = 0; i < num; i++) {
		struct mhi_chan *mhi_chan;
		int chan = chan_cfg[i].chan_cfg[MHI_CH_CFG_CHAN_ID];
		u32 bit_cfg = chan_cfg[i].chan_cfg[MHI_CH_CFG_BITCFG];

		if (chan >= mhi_cntrl->max_chan)
			goto error_chan_cfg;

		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		mhi_chan->chan = chan;
		mhi_chan->buf_ring.elements =
			chan_cfg[i].chan_cfg[MHI_CH_CFG_ELEMENTS];
		mhi_chan->tre_ring.elements = mhi_chan->buf_ring.elements;
		mhi_chan->er_index = chan_cfg[i].chan_cfg[MHI_CH_CFG_ER_INDEX];
		mhi_chan->dir = chan_cfg[i].chan_cfg[MHI_CH_CFG_DIRECTION];

		mhi_chan->db_cfg.pollcfg =
			chan_cfg[i].chan_cfg[MHI_CH_CFG_POLLCFG];
		mhi_chan->ee = chan_cfg[i].chan_cfg[MHI_CH_CFG_EE];
		if (mhi_chan->ee >= MHI_EE_MAX_SUPPORTED)
			goto error_chan_cfg;

		mhi_chan->xfer_type =
			chan_cfg[i].chan_cfg[MHI_CH_CFG_XFER_TYPE];

		switch (mhi_chan->xfer_type) {
		case MHI_XFER_BUFFER:
			mhi_chan->gen_tre = mhi_gen_tre;
			mhi_chan->queue_xfer = mhi_queue_buf;
			break;
		case MHI_XFER_SKB:
			mhi_chan->queue_xfer = mhi_queue_skb;
			break;
		case MHI_XFER_SCLIST:
			mhi_chan->gen_tre = mhi_gen_tre;
			mhi_chan->queue_xfer = mhi_queue_sclist;
			break;
		case MHI_XFER_NOP:
			mhi_chan->queue_xfer = mhi_queue_nop;
			break;
		default:
			goto error_chan_cfg;
		}

		mhi_chan->lpm_notify = !!(bit_cfg & MHI_CH_CFG_BIT_LPM_NOTIFY);
		mhi_chan->offload_ch = !!(bit_cfg & MHI_CH_CFG_BIT_OFFLOAD_CH);
		mhi_chan->db_cfg.reset_req =
			!!(bit_cfg & MHI_CH_CFG_BIT_DBMODE_RESET_CH);
		mhi_chan->pre_alloc = !!(bit_cfg & MHI_CH_CFG_BIT_PRE_ALLOC);

		if (mhi_chan->pre_alloc &&
		    (mhi_chan->dir != DMA_FROM_DEVICE ||
		     mhi_chan->xfer_type != MHI_XFER_BUFFER))
			goto error_chan_cfg;

		/* if mhi host allocate the buffers then client cannot queue */
		if (mhi_chan->pre_alloc)
			mhi_chan->queue_xfer = mhi_queue_nop;

		ret = of_property_read_string_index(of_node, "mhi,chan-names",
						    i, &mhi_chan->name);
		if (ret)
			goto error_chan_cfg;

		if (!mhi_chan->offload_ch) {
			mhi_chan->db_cfg.brstmode =
				chan_cfg[i].chan_cfg[MHI_CH_CFG_BRSTMODE];
			if (MHI_INVALID_BRSTMODE(mhi_chan->db_cfg.brstmode))
				goto error_chan_cfg;

			mhi_chan->db_cfg.process_db =
				(mhi_chan->db_cfg.brstmode ==
				 MHI_BRSTMODE_ENABLE) ?
				mhi_db_brstmode : mhi_db_brstmode_disable;
		}
		mhi_chan->configured = true;

		if (mhi_chan->lpm_notify)
			list_add_tail(&mhi_chan->node, &mhi_cntrl->lpm_chans);
	}

	kfree(chan_cfg);

	return 0;

error_chan_cfg:
	kfree(mhi_cntrl->mhi_chan);
	kfree(chan_cfg);

	return -EINVAL;
}

static int of_parse_dt(struct mhi_controller *mhi_cntrl,
		       struct device_node *of_node)
{
	int ret;

	/* parse firmware image info (optional parameters) */
	of_property_read_string(of_node, "mhi,fw-name", &mhi_cntrl->fw_image);
	of_property_read_string(of_node, "mhi,edl-name", &mhi_cntrl->fw_image);
	mhi_cntrl->fbc_download = of_property_read_bool(of_node, "mhi,dl-fbc");
	of_property_read_u32(of_node, "mhi,sbl-size",
			     (u32 *)&mhi_cntrl->sbl_size);
	of_property_read_u32(of_node, "mhi,seg-len",
			     (u32 *)&mhi_cntrl->seg_len);

	/* parse MHI channel configuration */
	ret = of_parse_ch_cfg(mhi_cntrl, of_node);
	if (ret)
		return ret;

	/* parse MHI event configuration */
	ret = of_parse_ev_cfg(mhi_cntrl, of_node);
	if (ret)
		goto error_ev_cfg;

	ret = of_property_read_u32(of_node, "mhi,timeout",
				   &mhi_cntrl->timeout_ms);
	if (ret)
		mhi_cntrl->timeout_ms = MHI_TIMEOUT_MS;

	return 0;

 error_ev_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}
#else
static int of_parse_dt(struct mhi_controller *mhi_cntrl,
		       struct device_node *of_node)
{
	return -EINVAL;
}
#endif

int of_register_mhi_controller(struct mhi_controller *mhi_cntrl)
{
	int ret;
	int i;
	struct mhi_event *mhi_event;
	struct mhi_chan *mhi_chan;
	struct mhi_cmd *mhi_cmd;

	if (!mhi_cntrl->of_node)
		return -EINVAL;

	if (!mhi_cntrl->runtime_get || !mhi_cntrl->runtime_put)
		return -EINVAL;

	if (!mhi_cntrl->status_cb || !mhi_cntrl->link_status)
		return -EINVAL;

	ret = of_parse_dt(mhi_cntrl, mhi_cntrl->of_node);
	if (ret)
		return -EINVAL;

	mhi_cntrl->mhi_cmd = kcalloc(NR_OF_CMD_RINGS,
				     sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd)
		goto error_alloc_cmd;

	INIT_LIST_HEAD(&mhi_cntrl->transition_list);
	mutex_init(&mhi_cntrl->pm_mutex);
	rwlock_init(&mhi_cntrl->pm_lock);
	spin_lock_init(&mhi_cntrl->transition_lock);
	spin_lock_init(&mhi_cntrl->wlock);
	INIT_WORK(&mhi_cntrl->st_worker, mhi_pm_st_worker);
	INIT_WORK(&mhi_cntrl->fw_worker, mhi_fw_load_worker);
	INIT_WORK(&mhi_cntrl->m1_worker, mhi_pm_m1_worker);
	INIT_WORK(&mhi_cntrl->syserr_worker, mhi_pm_sys_err_worker);
	init_waitqueue_head(&mhi_cntrl->state_event);

	mhi_cmd = mhi_cntrl->mhi_cmd;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++)
		spin_lock_init(&mhi_cmd->lock);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		mhi_event->mhi_cntrl = mhi_cntrl;
		spin_lock_init(&mhi_event->lock);
		if (mhi_event->ctrl_ev)
			tasklet_init(&mhi_event->task, mhi_ctrl_ev_task,
				     (ulong)mhi_event);
		else
			tasklet_init(&mhi_event->task, mhi_ev_task,
				     (ulong)mhi_event);
	}

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		mutex_init(&mhi_chan->mutex);
		init_completion(&mhi_chan->completion);
		rwlock_init(&mhi_chan->lock);
	}

	mhi_cntrl->parent = mhi_bus.dentry;
	mhi_cntrl->klog_lvl = MHI_MSG_LVL_ERROR;

	/* add to list */
	mutex_lock(&mhi_bus.lock);
	list_add_tail(&mhi_cntrl->node, &mhi_bus.controller_list);
	mutex_unlock(&mhi_bus.lock);

	return 0;

error_alloc_cmd:
	kfree(mhi_cntrl->mhi_chan);
	kfree(mhi_cntrl->mhi_event);

	return -ENOMEM;
};
EXPORT_SYMBOL(of_register_mhi_controller);

void mhi_unregister_mhi_controller(struct mhi_controller *mhi_cntrl)
{
	kfree(mhi_cntrl->mhi_cmd);
	kfree(mhi_cntrl->mhi_event);
	kfree(mhi_cntrl->mhi_chan);

	mutex_lock(&mhi_bus.lock);
	list_del(&mhi_cntrl->node);
	mutex_unlock(&mhi_bus.lock);
}

/* set ptr to control private data */
static inline void mhi_controller_set_devdata(struct mhi_controller *mhi_cntrl,
					 void *priv)
{
	mhi_cntrl->priv_data = priv;
}


/* allocate mhi controller to register */
struct mhi_controller *mhi_alloc_controller(size_t size)
{
	struct mhi_controller *mhi_cntrl;

	mhi_cntrl = kzalloc(size + sizeof(*mhi_cntrl), GFP_KERNEL);

	if (mhi_cntrl && size)
		mhi_controller_set_devdata(mhi_cntrl, mhi_cntrl + 1);

	return mhi_cntrl;
}
EXPORT_SYMBOL(mhi_alloc_controller);

int mhi_prepare_for_power_up(struct mhi_controller *mhi_cntrl)
{
	int ret;

	mutex_lock(&mhi_cntrl->pm_mutex);

	ret = mhi_init_dev_ctxt(mhi_cntrl);
	if (ret) {
		MHI_ERR("Error with init dev_ctxt\n");
		goto error_dev_ctxt;
	}

	ret = mhi_init_irq_setup(mhi_cntrl);
	if (ret) {
		MHI_ERR("Error setting up irq\n");
		goto error_setup_irq;
	}

	/*
	 * allocate rddm table if specified, this table is for debug purpose
	 * so we'll ignore erros
	 */
	if (mhi_cntrl->rddm_size)
		mhi_alloc_bhie_table(mhi_cntrl, &mhi_cntrl->rddm_image,
				     mhi_cntrl->rddm_size);

	mhi_cntrl->pre_init = true;

	mutex_unlock(&mhi_cntrl->pm_mutex);

	return 0;

error_setup_irq:
	mhi_deinit_dev_ctxt(mhi_cntrl);

error_dev_ctxt:
	mutex_unlock(&mhi_cntrl->pm_mutex);

	return ret;
}
EXPORT_SYMBOL(mhi_prepare_for_power_up);

void mhi_unprepare_after_power_down(struct mhi_controller *mhi_cntrl)
{
	if (mhi_cntrl->fbc_image) {
		mhi_free_bhie_table(mhi_cntrl, mhi_cntrl->fbc_image);
		mhi_cntrl->fbc_image = NULL;
	}

	if (mhi_cntrl->rddm_image) {
		mhi_free_bhie_table(mhi_cntrl, mhi_cntrl->rddm_image);
		mhi_cntrl->rddm_image = NULL;
	}

	mhi_deinit_free_irq(mhi_cntrl);
	mhi_deinit_dev_ctxt(mhi_cntrl);
	mhi_cntrl->pre_init = false;
}

/* match dev to drv */
static int mhi_match(struct device *dev, struct device_driver *drv)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_driver *mhi_drv = to_mhi_driver(drv);
	const struct mhi_device_id *id;

	for (id = mhi_drv->id_table; id->chan; id++)
		if (!strcmp(mhi_dev->chan_name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

static void mhi_release_device(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);

	kfree(mhi_dev);
}

struct bus_type mhi_bus_type = {
	.name = "mhi",
	.dev_name = "mhi",
	.match = mhi_match,
};

static int mhi_driver_probe(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct device_driver *drv = dev->driver;
	struct mhi_driver *mhi_drv = to_mhi_driver(drv);
	struct mhi_event *mhi_event;
	struct mhi_chan *ul_chan = mhi_dev->ul_chan;
	struct mhi_chan *dl_chan = mhi_dev->dl_chan;
	bool offload_ch = ((ul_chan && ul_chan->offload_ch) ||
			   (dl_chan && dl_chan->offload_ch));

	/* all offload channels require status_cb to be defined */
	if (offload_ch) {
		if (!mhi_dev->status_cb)
			return -EINVAL;
		mhi_dev->status_cb = mhi_drv->status_cb;
	}

	if (ul_chan && !offload_ch) {
		if (!mhi_drv->ul_xfer_cb)
			return -EINVAL;
		ul_chan->xfer_cb = mhi_drv->ul_xfer_cb;
	}

	if (dl_chan && !offload_ch) {
		if (!mhi_drv->dl_xfer_cb)
			return -EINVAL;
		dl_chan->xfer_cb = mhi_drv->dl_xfer_cb;
		mhi_event = &mhi_cntrl->mhi_event[dl_chan->er_index];

		/*
		 * if this channal event ring manage by client, then
		 * status_cb must be defined so we can send the async
		 * cb whenever there are pending data
		 */
		if (mhi_event->cl_manage && !mhi_drv->status_cb)
			return -EINVAL;
		mhi_dev->status_cb = mhi_drv->status_cb;
	}

	return mhi_drv->probe(mhi_dev, mhi_dev->id);
}

static int mhi_driver_remove(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_driver *mhi_drv = to_mhi_driver(dev->driver);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;
	enum MHI_CH_STATE ch_state[] = {
		MHI_CH_STATE_DISABLED,
		MHI_CH_STATE_DISABLED
	};
	int dir;

	MHI_LOG("Removing device for chan:%s\n", mhi_dev->chan_name);

	/* reset both channels */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		/* wake all threads waiting for completion */
		write_lock_irq(&mhi_chan->lock);
		mhi_chan->ccs = MHI_EV_CC_INVALID;
		complete_all(&mhi_chan->completion);
		write_unlock_irq(&mhi_chan->lock);

		/* move channel state to disable, no more processing */
		mutex_lock(&mhi_chan->mutex);
		write_lock_irq(&mhi_chan->lock);
		ch_state[dir] = mhi_chan->ch_state;
		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
		write_unlock_irq(&mhi_chan->lock);

		/* reset the channel */
		if (!mhi_chan->offload_ch)
			mhi_reset_chan(mhi_cntrl, mhi_chan);
	}

	/* destroy the device */
	mhi_drv->remove(mhi_dev);

	/* de_init channel if it was enabled */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		if (ch_state[dir] == MHI_CH_STATE_ENABLED &&
		    !mhi_chan->offload_ch)
			mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);

		mutex_unlock(&mhi_chan->mutex);
	}

	/* relinquish any pending votes */
	read_lock_bh(&mhi_cntrl->pm_lock);
	while (atomic_read(&mhi_dev->dev_wake))
		mhi_device_put(mhi_dev);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return 0;
}

int mhi_driver_register(struct mhi_driver *mhi_drv)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	driver->bus = &mhi_bus_type;
	driver->probe = mhi_driver_probe;
	driver->remove = mhi_driver_remove;
	return driver_register(driver);
}
EXPORT_SYMBOL(mhi_driver_register);

void mhi_driver_unregister(struct mhi_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL(mhi_driver_unregister);

struct mhi_device *mhi_alloc_device(struct mhi_controller *mhi_cntrl)
{
	struct mhi_device *mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	struct device *dev;

	if (!mhi_dev)
		return NULL;

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_bus_type;
	dev->release = mhi_release_device;
	dev->parent = mhi_cntrl->dev;
	mhi_dev->mhi_cntrl = mhi_cntrl;
	mhi_dev->dev_id = mhi_cntrl->dev_id;
	mhi_dev->domain = mhi_cntrl->domain;
	mhi_dev->bus = mhi_cntrl->bus;
	mhi_dev->slot = mhi_cntrl->slot;
	mhi_dev->mtu = MHI_MAX_MTU;
	atomic_set(&mhi_dev->dev_wake, 0);

	return mhi_dev;
}

static int __init mhi_init(void)
{
	struct dentry *dentry;
	int ret;

	mutex_init(&mhi_bus.lock);
	INIT_LIST_HEAD(&mhi_bus.controller_list);
	dentry = debugfs_create_dir("mhi", NULL);
	if (!IS_ERR_OR_NULL(dentry))
		mhi_bus.dentry = dentry;

	ret = bus_register(&mhi_bus_type);

	if (!ret)
		mhi_dtr_init();
	return ret;
}
postcore_initcall(mhi_init);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("MHI_CORE");
MODULE_DESCRIPTION("MHI Host Interface");
