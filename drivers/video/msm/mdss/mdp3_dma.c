/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/bitops.h>
#include <linux/iopoll.h>

#include "mdp3.h"
#include "mdp3_dma.h"
#include "mdp3_hwio.h"

#include "mdss_timeout.h"

#ifdef CONFIG_FB_MSM_MDSS_MDP3_KCAL_CTRL
#include "mdp3_ctrl.h"
#endif

#define DMA_STOP_POLL_SLEEP_US 1000
#define DMA_STOP_POLL_TIMEOUT_US 32000
#define DMA_HISTO_RESET_TIMEOUT_MS 40
#define DMA_LUT_CONFIG_MASK 0xfffffbe8
#define DMA_CCS_CONFIG_MASK 0xfffffc17
#define HIST_WAIT_TIMEOUT(frame) ((75 * HZ * (frame)) / 1000)

static int mdp3_wait4vsync(struct mdp3_dma *dma, bool killable,
	unsigned long timeout)
{
	int rc;
	int timeouts = 0;
	static int timeout_occurred;
	int prev_vsync_cnt = dma->vsync_cnt;
	unsigned long tout = timeout ? timeout : KOFF_TIMEOUT;

	do {
		if (killable) {
			rc = wait_for_completion_killable_timeout(
				&dma->vsync_comp, tout);
		} else {
			rc = wait_for_completion_timeout(
				&dma->vsync_comp, tout);
		}

		if (rc == -ERESTARTSYS)
			return rc;

		if (rc == 0) {
			pr_err("%s: TIMEOUT (vsync_cnt: prev: %u cur: %u)\n",
				__func__, prev_vsync_cnt, dma->vsync_cnt);
			timeout_occurred = 1;
			if (timeouts == 0 && dma->vsync_cnt > 0)
				mdss_timeout_dump(__func__);
			timeouts++;
		} else {
			if (timeout_occurred)
				pr_info("%s: recovered from previous timeout\n",
					__func__);
			timeout_occurred = 0;
			break;
		}
	} while (timeout == 0);

	if (timeout == 0 && timeouts)
		pr_err("%s: wait of %u ms timed out %d times!\n", __func__,
			jiffies_to_msecs(tout), timeouts);

	return rc;
}

static void mdp3_vsync_intr_handler(int type, void *arg)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)arg;
	struct mdp3_notification vsync_client;
	unsigned int wait_for_next_vs;

	pr_debug("mdp3_vsync_intr_handler\n");
	spin_lock(&dma->dma_lock);
	vsync_client = dma->vsync_client;
	wait_for_next_vs = !dma->vsync_status;
	dma->vsync_cnt++;
	dma->vsync_status = 0;
	if (wait_for_next_vs)
		complete(&dma->vsync_comp);
	spin_unlock(&dma->dma_lock);
	if (vsync_client.handler) {
		vsync_client.handler(vsync_client.arg);
	} else {
		if (wait_for_next_vs)
			mdp3_irq_disable_nosync(type);
	}
}

static void mdp3_dma_done_intr_handler(int type, void *arg)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)arg;
	struct mdp3_notification dma_client;

	pr_debug("mdp3_dma_done_intr_handler\n");
	spin_lock(&dma->dma_lock);
	dma_client = dma->dma_notifier_client;
	complete(&dma->dma_comp);
	spin_unlock(&dma->dma_lock);
	mdp3_irq_disable_nosync(type);
	if (dma_client.handler)
		dma_client.handler(dma_client.arg);
}

static void mdp3_hist_done_intr_handler(int type, void *arg)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)arg;
	u32 isr, mask;

	isr = MDP3_REG_READ(MDP3_REG_DMA_P_HIST_INTR_STATUS);
	mask = MDP3_REG_READ(MDP3_REG_DMA_P_HIST_INTR_ENABLE);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_CLEAR, isr);

	isr &= mask;
	if (isr == 0)
		return;

	if (isr & MDP3_DMA_P_HIST_INTR_HIST_DONE_BIT) {
		spin_lock(&dma->histo_lock);
		dma->histo_state = MDP3_DMA_HISTO_STATE_READY;
		complete(&dma->histo_comp);
		spin_unlock(&dma->histo_lock);
	}
	if (isr & MDP3_DMA_P_HIST_INTR_RESET_DONE_BIT) {
		spin_lock(&dma->histo_lock);
		dma->histo_state = MDP3_DMA_HISTO_STATE_IDLE;
		complete(&dma->histo_comp);
		spin_unlock(&dma->histo_lock);
	}
}

void mdp3_dma_callback_enable(struct mdp3_dma *dma, int type)
{
	int irq_bit;

	pr_debug("mdp3_dma_callback_enable type=%d\n", type);

	if (dma->dma_sel == MDP3_DMA_P) {
		if (type & MDP3_DMA_CALLBACK_TYPE_HIST_RESET_DONE)
			mdp3_irq_enable(MDP3_INTR_DMA_P_HISTO);

		if (type & MDP3_DMA_CALLBACK_TYPE_HIST_DONE)
			mdp3_irq_enable(MDP3_INTR_DMA_P_HISTO);
	}

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC)
			mdp3_irq_enable(MDP3_INTR_LCDC_START_OF_FRAME);
	} else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC) {
			irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
			irq_bit += dma->dma_sel;
			mdp3_irq_enable(irq_bit);
		}

		if (type & MDP3_DMA_CALLBACK_TYPE_DMA_DONE) {
			irq_bit = MDP3_INTR_DMA_P_DONE;
			if (dma->dma_sel == MDP3_DMA_S)
				irq_bit = MDP3_INTR_DMA_S_DONE;
			mdp3_irq_enable(irq_bit);
		}
	} else {
		pr_err("mdp3_dma_callback_enable not supported interface\n");
	}
}

void mdp3_dma_callback_disable(struct mdp3_dma *dma, int type)
{
	int irq_bit;

	pr_debug("mdp3_dma_callback_disable type=%d\n", type);

	if (dma->dma_sel == MDP3_DMA_P) {
		if (type & MDP3_DMA_CALLBACK_TYPE_HIST_RESET_DONE)
			mdp3_irq_disable(MDP3_INTR_DMA_P_HISTO);

		if (type & MDP3_DMA_CALLBACK_TYPE_HIST_DONE)
			mdp3_irq_disable(MDP3_INTR_DMA_P_HISTO);
	}

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC)
			mdp3_irq_disable(MDP3_INTR_LCDC_START_OF_FRAME);
	} else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC) {
			irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
			irq_bit += dma->dma_sel;
			mdp3_irq_disable(irq_bit);
		}

		if (type & MDP3_DMA_CALLBACK_TYPE_DMA_DONE) {
			irq_bit = MDP3_INTR_DMA_P_DONE;
			if (dma->dma_sel == MDP3_DMA_S)
				irq_bit = MDP3_INTR_DMA_S_DONE;
			mdp3_irq_disable(irq_bit);
		}
	}
}

static int mdp3_dma_callback_setup(struct mdp3_dma *dma)
{
	int rc = 0;
	struct mdp3_intr_cb vsync_cb = {
		.cb = mdp3_vsync_intr_handler,
		.data = dma,
	};

	struct mdp3_intr_cb dma_cb = {
		.cb = mdp3_dma_done_intr_handler,
		.data = dma,
	};


	struct mdp3_intr_cb hist_cb = {
		.cb = mdp3_hist_done_intr_handler,
		.data = dma,
	};

	if (dma->dma_sel == MDP3_DMA_P)
		rc = mdp3_set_intr_callback(MDP3_INTR_DMA_P_HISTO, &hist_cb);

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC)
		rc |= mdp3_set_intr_callback(MDP3_INTR_LCDC_START_OF_FRAME,
					&vsync_cb);
	else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		int irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
		irq_bit += dma->dma_sel;
		rc |= mdp3_set_intr_callback(irq_bit, &vsync_cb);
		irq_bit = MDP3_INTR_DMA_P_DONE;
		if (dma->dma_sel == MDP3_DMA_S)
			irq_bit = MDP3_INTR_DMA_S_DONE;
		rc |= mdp3_set_intr_callback(irq_bit, &dma_cb);
	} else {
		pr_err("mdp3_dma_callback_setup not suppported interface\n");
		rc = -ENODEV;
	}

	return rc;
}

static void mdp3_dma_vsync_enable(struct mdp3_dma *dma,
				struct mdp3_notification *vsync_client)
{
	unsigned long flag;
	int updated = 0;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;

	pr_debug("mdp3_dma_vsync_enable\n");

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (vsync_client) {
		if (dma->vsync_client.handler != vsync_client->handler) {
			dma->vsync_client = *vsync_client;
			updated = 1;
		}
	} else {
		if (dma->vsync_client.handler) {
			dma->vsync_client.handler = NULL;
			dma->vsync_client.arg = NULL;
			updated = 1;
		}
	}
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	if (updated) {
		if (vsync_client && vsync_client->handler)
			mdp3_dma_callback_enable(dma, cb_type);
		else
			mdp3_dma_callback_disable(dma, cb_type);
	}
}

static void mdp3_dma_done_notifier(struct mdp3_dma *dma,
				struct mdp3_notification *dma_client)
{
	unsigned long flag;

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (dma_client) {
		dma->dma_notifier_client = *dma_client;
	} else {
		dma->dma_notifier_client.handler = NULL;
		dma->dma_notifier_client.arg = NULL;
	}
	spin_unlock_irqrestore(&dma->dma_lock, flag);
}

static void mdp3_dma_clk_auto_gating(struct mdp3_dma *dma, int enable)
{
	u32 cgc;
	int clock_bit = 10;

	clock_bit += dma->dma_sel;

	if (enable) {
		cgc = MDP3_REG_READ(MDP3_REG_CGC_EN);
		cgc |= BIT(clock_bit);
		MDP3_REG_WRITE(MDP3_REG_CGC_EN, cgc);

	} else {
		cgc = MDP3_REG_READ(MDP3_REG_CGC_EN);
		cgc &= ~BIT(clock_bit);
		MDP3_REG_WRITE(MDP3_REG_CGC_EN, cgc);
	}
}

static int mdp3_dma_sync_config(struct mdp3_dma *dma,
			struct mdp3_dma_source *source_config)
{
	u32 sync_config;
	int dma_sel = dma->dma_sel;

	pr_debug("mdp3_dma_sync_config\n");

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		int porch = source_config->vporch;
		int height = source_config->height;
		int vtotal = height + porch;
		sync_config = vtotal << 21;
		sync_config |= source_config->vsync_count;
		sync_config |= BIT(19);
		sync_config |= BIT(20);

		MDP3_REG_WRITE(MDP3_REG_SYNC_CONFIG_0 + dma_sel, sync_config);
		MDP3_REG_WRITE(MDP3_REG_VSYNC_SEL, 0x024);
		MDP3_REG_WRITE(MDP3_REG_PRIMARY_VSYNC_INIT_VAL + dma_sel,
				height);
		MDP3_REG_WRITE(MDP3_REG_PRIMARY_RD_PTR_IRQ, 0x5);
		MDP3_REG_WRITE(MDP3_REG_SYNC_THRESH_0 + dma_sel, (4 << 16 | 2));
		MDP3_REG_WRITE(MDP3_REG_PRIMARY_START_P0S + dma_sel, porch);
		MDP3_REG_WRITE(MDP3_REG_TEAR_CHECK_EN, 0x1);
	}
	return 0;
}

static int mdp3_dmap_config(struct mdp3_dma *dma,
			struct mdp3_dma_source *source_config,
			struct mdp3_dma_output_config *output_config)
{
	u32 dma_p_cfg_reg, dma_p_size, dma_p_out_xy;

	dma_p_cfg_reg = source_config->format << 25;
	if (output_config->dither_en)
		dma_p_cfg_reg |= BIT(24);
	dma_p_cfg_reg |= output_config->out_sel << 19;
	dma_p_cfg_reg |= output_config->bit_mask_polarity << 18;
	dma_p_cfg_reg |= output_config->color_components_flip << 14;
	dma_p_cfg_reg |= output_config->pack_pattern << 8;
	dma_p_cfg_reg |= output_config->pack_align << 7;
	dma_p_cfg_reg |= output_config->color_comp_out_bits;

	dma_p_size = source_config->width | (source_config->height << 16);
	dma_p_out_xy = source_config->x | (source_config->y << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_CONFIG, dma_p_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_SIZE, dma_p_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_ADDR, (u32)source_config->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_Y_STRIDE, source_config->stride);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_OUT_XY, dma_p_out_xy);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_FETCH_CFG, 0x40);

	dma->source_config = *source_config;
	dma->output_config = *output_config;
	mdp3_dma_sync_config(dma, source_config);

	mdp3_irq_enable(MDP3_INTR_LCDC_UNDERFLOW);
	mdp3_dma_callback_setup(dma);
	return 0;
}

static void mdp3_dmap_config_source(struct mdp3_dma *dma)
{
	struct mdp3_dma_source *source_config = &dma->source_config;
	u32 dma_p_cfg_reg, dma_p_size;

	dma_p_cfg_reg = MDP3_REG_READ(MDP3_REG_DMA_P_CONFIG);
	dma_p_cfg_reg &= ~MDP3_DMA_IBUF_FORMAT_MASK;
	dma_p_cfg_reg |= source_config->format << 25;
	dma_p_cfg_reg &= ~MDP3_DMA_PACK_PATTERN_MASK;
	dma_p_cfg_reg |= dma->output_config.pack_pattern << 8;

	dma_p_size = source_config->width | (source_config->height << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_CONFIG, dma_p_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_SIZE, dma_p_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_Y_STRIDE, source_config->stride);
}

static int mdp3_dmas_config(struct mdp3_dma *dma,
			struct mdp3_dma_source *source_config,
			struct mdp3_dma_output_config *output_config)
{
	u32 dma_s_cfg_reg, dma_s_size, dma_s_out_xy;

	dma_s_cfg_reg = source_config->format << 25;
	if (output_config->dither_en)
		dma_s_cfg_reg |= BIT(24);
	dma_s_cfg_reg |= output_config->out_sel << 19;
	dma_s_cfg_reg |= output_config->bit_mask_polarity << 18;
	dma_s_cfg_reg |= output_config->color_components_flip << 14;
	dma_s_cfg_reg |= output_config->pack_pattern << 8;
	dma_s_cfg_reg |= output_config->pack_align << 7;
	dma_s_cfg_reg |= output_config->color_comp_out_bits;

	dma_s_size = source_config->width | (source_config->height << 16);
	dma_s_out_xy = source_config->x | (source_config->y << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_S_CONFIG, dma_s_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_SIZE, dma_s_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_ADDR, (u32)source_config->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_Y_STRIDE, source_config->stride);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_OUT_XY, dma_s_out_xy);

	MDP3_REG_WRITE(MDP3_REG_SECONDARY_RD_PTR_IRQ, 0x10);

	dma->source_config = *source_config;
	dma->output_config = *output_config;
	mdp3_dma_sync_config(dma, source_config);

	mdp3_dma_callback_setup(dma);
	return 0;
}

static void mdp3_dmas_config_source(struct mdp3_dma *dma)
{
	struct mdp3_dma_source *source_config = &dma->source_config;
	u32 dma_s_cfg_reg, dma_s_size;

	dma_s_cfg_reg = MDP3_REG_READ(MDP3_REG_DMA_S_CONFIG);
	dma_s_cfg_reg &= ~MDP3_DMA_IBUF_FORMAT_MASK;
	dma_s_cfg_reg |= source_config->format << 25;

	dma_s_size = source_config->width | (source_config->height << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_S_CONFIG, dma_s_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_SIZE, dma_s_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_Y_STRIDE, source_config->stride);
}

static int mdp3_dmap_cursor_config(struct mdp3_dma *dma,
				struct mdp3_dma_cursor *cursor)
{
	u32 cursor_size, cursor_pos, blend_param, trans_mask;

	cursor_size = cursor->width | (cursor->height << 16);
	cursor_pos = cursor->x | (cursor->y << 16);
	trans_mask = 0;
	if (cursor->blend_config.mode == MDP3_DMA_CURSOR_BLEND_CONSTANT_ALPHA) {
		blend_param = cursor->blend_config.constant_alpha << 24;
	} else if (cursor->blend_config.mode ==
			MDP3_DMA_CURSOR_BLEND_COLOR_KEYING) {
		blend_param = cursor->blend_config.transparent_color;
		trans_mask = cursor->blend_config.transparency_mask;
	} else {
		blend_param = 0;
	}

	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_FORMAT, cursor->format);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_SIZE, cursor_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BUF_ADDR, (u32)cursor->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_POS, cursor_pos);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_CONFIG,
			cursor->blend_config.mode);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_PARAM, blend_param);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_TRANS_MASK, trans_mask);
	dma->cursor = *cursor;
	return 0;
}

static void mdp3_ccs_update(struct mdp3_dma *dma)
{
	u32 cc_config;
	int updated = 0;

	cc_config = MDP3_REG_READ(MDP3_REG_DMA_P_COLOR_CORRECT_CONFIG);

	if (dma->ccs_config.ccs_dirty) {
		cc_config &= DMA_CCS_CONFIG_MASK;
		if (dma->ccs_config.ccs_enable)
			cc_config |= BIT(3);
		else
			cc_config &= ~BIT(3);
		cc_config |= dma->ccs_config.ccs_sel << 5;
		cc_config |= dma->ccs_config.pre_bias_sel << 6;
		cc_config |= dma->ccs_config.post_bias_sel << 7;
		cc_config |= dma->ccs_config.pre_limit_sel << 8;
		cc_config |= dma->ccs_config.post_limit_sel << 9;
		dma->ccs_config.ccs_dirty = false;
		updated = 1;
	}

	if (dma->lut_config.lut_dirty) {
		cc_config &= DMA_LUT_CONFIG_MASK;
		cc_config |= dma->lut_config.lut_enable;
		cc_config |= dma->lut_config.lut_position << 4;
		cc_config |= dma->lut_config.lut_sel << 10;
		dma->lut_config.lut_dirty = false;
		updated = 1;
	}
	if (updated) {
		MDP3_REG_WRITE(MDP3_REG_DMA_P_COLOR_CORRECT_CONFIG, cc_config);

		/* Make sure ccs configuration update is done before continuing
		with the DMA transfer */
		wmb();
	}
}

static int mdp3_dmap_ccs_config(struct mdp3_dma *dma,
			struct mdp3_dma_color_correct_config *config,
			struct mdp3_dma_ccs *ccs)
{
	int i;
	u32 addr;

	if (!ccs)
		return -EINVAL;

	if (config->ccs_enable) {
		addr = MDP3_REG_DMA_P_CSC_MV1;
		if (config->ccs_sel)
			addr = MDP3_REG_DMA_P_CSC_MV2;
		for (i = 0; i < 9; i++) {
			MDP3_REG_WRITE(addr, ccs->mv[i]);
			addr += 4;
		}

		addr = MDP3_REG_DMA_P_CSC_PRE_BV1;
		if (config->pre_bias_sel)
			addr = MDP3_REG_DMA_P_CSC_PRE_BV2;
		for (i = 0; i < 3; i++) {
			MDP3_REG_WRITE(addr, ccs->pre_bv[i]);
			addr += 4;
		}

		addr = MDP3_REG_DMA_P_CSC_POST_BV1;
		if (config->post_bias_sel)
			addr = MDP3_REG_DMA_P_CSC_POST_BV2;
		for (i = 0; i < 3; i++) {
			MDP3_REG_WRITE(addr, ccs->post_bv[i]);
			addr += 4;
		}

		addr = MDP3_REG_DMA_P_CSC_PRE_LV1;
		if (config->pre_limit_sel)
			addr = MDP3_REG_DMA_P_CSC_PRE_LV2;
		for (i = 0; i < 6; i++) {
			MDP3_REG_WRITE(addr, ccs->pre_lv[i]);
			addr += 4;
		}

		addr = MDP3_REG_DMA_P_CSC_POST_LV1;
		if (config->post_limit_sel)
			addr = MDP3_REG_DMA_P_CSC_POST_LV2;
		for (i = 0; i < 6; i++) {
			MDP3_REG_WRITE(addr, ccs->post_lv[i]);
			addr += 4;
		}
	}
	dma->ccs_config = *config;

	if (dma->output_config.out_sel != MDP3_DMA_OUTPUT_SEL_DSI_CMD)
		mdp3_ccs_update(dma);

	return 0;
}

#ifdef CONFIG_FB_MSM_MDSS_MDP3_KCAL_CTRL
static unsigned int lcd_rgb_linear_lut[256] = {
       /* default linear qlut */
       0x00000000, 0x00010101, 0x00020202, 0x00030303,
       0x00040404, 0x00050505, 0x00060606, 0x00070707,
       0x00080808, 0x00090909, 0x000a0a0a, 0x000b0b0b,
       0x000c0c0c, 0x000d0d0d, 0x000e0e0e, 0x000f0f0f,
       0x00101010, 0x00111111, 0x00121212, 0x00131313,
       0x00141414, 0x00151515, 0x00161616, 0x00171717,
       0x00181818, 0x00191919, 0x001a1a1a, 0x001b1b1b,
       0x001c1c1c, 0x001d1d1d, 0x001e1e1e, 0x001f1f1f,
       0x00202020, 0x00212121, 0x00222222, 0x00232323,
       0x00242424, 0x00252525, 0x00262626, 0x00272727,
       0x00282828, 0x00292929, 0x002a2a2a, 0x002b2b2b,
       0x002c2c2c, 0x002d2d2d, 0x002e2e2e, 0x002f2f2f,
       0x00303030, 0x00313131, 0x00323232, 0x00333333,
       0x00343434, 0x00353535, 0x00363636, 0x00373737,
       0x00383838, 0x00393939, 0x003a3a3a, 0x003b3b3b,
       0x003c3c3c, 0x003d3d3d, 0x003e3e3e, 0x003f3f3f,
       0x00404040, 0x00414141, 0x00424242, 0x00434343,
       0x00444444, 0x00454545, 0x00464646, 0x00474747,
       0x00484848, 0x00494949, 0x004a4a4a, 0x004b4b4b,
       0x004c4c4c, 0x004d4d4d, 0x004e4e4e, 0x004f4f4f,
       0x00505050, 0x00515151, 0x00525252, 0x00535353,
       0x00545454, 0x00555555, 0x00565656, 0x00575757,
       0x00585858, 0x00595959, 0x005a5a5a, 0x005b5b5b,
       0x005c5c5c, 0x005d5d5d, 0x005e5e5e, 0x005f5f5f,
       0x00606060, 0x00616161, 0x00626262, 0x00636363,
       0x00646464, 0x00656565, 0x00666666, 0x00676767,
       0x00686868, 0x00696969, 0x006a6a6a, 0x006b6b6b,
       0x006c6c6c, 0x006d6d6d, 0x006e6e6e, 0x006f6f6f,
       0x00707070, 0x00717171, 0x00727272, 0x00737373,
       0x00747474, 0x00757575, 0x00767676, 0x00777777,
       0x00787878, 0x00797979, 0x007a7a7a, 0x007b7b7b,
       0x007c7c7c, 0x007d7d7d, 0x007e7e7e, 0x007f7f7f,
       0x00808080, 0x00818181, 0x00828282, 0x00838383,
       0x00848484, 0x00858585, 0x00868686, 0x00878787,
       0x00888888, 0x00898989, 0x008a8a8a, 0x008b8b8b,
       0x008c8c8c, 0x008d8d8d, 0x008e8e8e, 0x008f8f8f,
       0x00909090, 0x00919191, 0x00929292, 0x00939393,
       0x00949494, 0x00959595, 0x00969696, 0x00979797,
       0x00989898, 0x00999999, 0x009a9a9a, 0x009b9b9b,
       0x009c9c9c, 0x009d9d9d, 0x009e9e9e, 0x009f9f9f,
       0x00a0a0a0, 0x00a1a1a1, 0x00a2a2a2, 0x00a3a3a3,
       0x00a4a4a4, 0x00a5a5a5, 0x00a6a6a6, 0x00a7a7a7,
       0x00a8a8a8, 0x00a9a9a9, 0x00aaaaaa, 0x00ababab,
       0x00acacac, 0x00adadad, 0x00aeaeae, 0x00afafaf,
       0x00b0b0b0, 0x00b1b1b1, 0x00b2b2b2, 0x00b3b3b3,
       0x00b4b4b4, 0x00b5b5b5, 0x00b6b6b6, 0x00b7b7b7,
       0x00b8b8b8, 0x00b9b9b9, 0x00bababa, 0x00bbbbbb,
       0x00bcbcbc, 0x00bdbdbd, 0x00bebebe, 0x00bfbfbf,
       0x00c0c0c0, 0x00c1c1c1, 0x00c2c2c2, 0x00c3c3c3,
       0x00c4c4c4, 0x00c5c5c5, 0x00c6c6c6, 0x00c7c7c7,
       0x00c8c8c8, 0x00c9c9c9, 0x00cacaca, 0x00cbcbcb,
       0x00cccccc, 0x00cdcdcd, 0x00cecece, 0x00cfcfcf,
       0x00d0d0d0, 0x00d1d1d1, 0x00d2d2d2, 0x00d3d3d3,
       0x00d4d4d4, 0x00d5d5d5, 0x00d6d6d6, 0x00d7d7d7,
       0x00d8d8d8, 0x00d9d9d9, 0x00dadada, 0x00dbdbdb,
       0x00dcdcdc, 0x00dddddd, 0x00dedede, 0x00dfdfdf,
       0x00e0e0e0, 0x00e1e1e1, 0x00e2e2e2, 0x00e3e3e3,
       0x00e4e4e4, 0x00e5e5e5, 0x00e6e6e6, 0x00e7e7e7,
       0x00e8e8e8, 0x00e9e9e9, 0x00eaeaea, 0x00ebebeb,
       0x00ececec, 0x00ededed, 0x00eeeeee, 0x00efefef,
       0x00f0f0f0, 0x00f1f1f1, 0x00f2f2f2, 0x00f3f3f3,
       0x00f4f4f4, 0x00f5f5f5, 0x00f6f6f6, 0x00f7f7f7,
       0x00f8f8f8, 0x00f9f9f9, 0x00fafafa, 0x00fbfbfb,
       0x00fcfcfc, 0x00fdfdfd, 0x00fefefe, 0x00ffffff
};
#endif

static int mdp3_dmap_lut_config(struct mdp3_dma *dma,
			struct mdp3_dma_lut_config *config,
			struct mdp3_dma_lut *lut)
{
	u32 addr, color;
	int i;
#ifdef CONFIG_FB_MSM_MDSS_MDP3_KCAL_CTRL
	u16 r, g, b;
	uint32_t *internal_lut = lcd_rgb_linear_lut;
#endif

	if (config->lut_enable && lut) {
		addr = MDP3_REG_DMA_P_CSC_LUT1;
		if (config->lut_sel)
			addr = MDP3_REG_DMA_P_CSC_LUT2;

		for (i = 0; i < MDP_LUT_SIZE; i++) {
#ifdef CONFIG_FB_MSM_MDSS_MDP3_KCAL_CTRL
			r = lut2rgb(internal_lut[i], R_MASK, R_SHIFT);
			g = lut2rgb(internal_lut[i], G_MASK, G_SHIFT);
			b = lut2rgb(internal_lut[i], B_MASK, B_SHIFT);

			r = scaled_by_kcal(r, *(lut->color0_lut));
			g = scaled_by_kcal(g, *(lut->color1_lut));
			b = scaled_by_kcal(b, *(lut->color2_lut));

			color = r & 0xff;
			color |= (g & 0xff) << 8;
			color |= (b & 0xff) << 16;
#else
			color = lut->color0_lut[i] & 0xff;
			color |= (lut->color1_lut[i] & 0xff) << 8;
			color |= (lut->color2_lut[i] & 0xff) << 16;
#endif
			MDP3_REG_WRITE(addr, color);
			addr += 4;
		}
	}

	dma->lut_config = *config;

	if (dma->output_config.out_sel != MDP3_DMA_OUTPUT_SEL_DSI_CMD)
		mdp3_ccs_update(dma);

	return 0;
}

static int mdp3_dmap_histo_config(struct mdp3_dma *dma,
			struct mdp3_dma_histogram_config *histo_config)
{
	unsigned long flag;
	u32 histo_bit_mask = 0, histo_control = 0;
	u32 histo_isr_mask = MDP3_DMA_P_HIST_INTR_HIST_DONE_BIT |
			MDP3_DMA_P_HIST_INTR_RESET_DONE_BIT;

	spin_lock_irqsave(&dma->histo_lock, flag);

	if (histo_config->bit_mask_polarity)
		histo_bit_mask = BIT(31);
	histo_bit_mask |= histo_config->bit_mask;

	if (histo_config->auto_clear_en)
		histo_control = BIT(0);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_FRAME_CNT,
			histo_config->frame_count);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_BIT_MASK, histo_bit_mask);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_CONTROL, histo_control);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_ENABLE, histo_isr_mask);

	spin_unlock_irqrestore(&dma->histo_lock, flag);

	dma->histogram_config = *histo_config;
	return 0;
}

static int mdp3_dmap_update(struct mdp3_dma *dma, void *buf,
				struct mdp3_intf *intf)
{
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;
	int rc = 0;

	pr_debug("mdp3_dmap_update\n");

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type = MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		if (intf->active) {
			rc = wait_for_completion_timeout(&dma->dma_comp,
				KOFF_TIMEOUT);
			if (rc <= 0) {
				//WARN(1, "cmd kickoff timed out (%d)\n", rc);
				rc = -1;
			}
		}
	}
	if (dma->update_src_cfg) {
		if (dma->output_config.out_sel ==
				 MDP3_DMA_OUTPUT_SEL_DSI_VIDEO && intf->active)
			pr_err("configuring dma source while dma is active\n");
		dma->dma_config_source(dma);
		dma->update_src_cfg = false;
	}
	spin_lock_irqsave(&dma->dma_lock, flag);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_ADDR, (u32)buf);
	dma->source_config.buf = buf;
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		mdp3_ccs_update(dma);
		MDP3_REG_WRITE(MDP3_REG_DMA_P_START, 1);
	}

	if (!intf->active) {
		pr_debug("mdp3_dmap_update start interface\n");
		intf->start(intf);
	}

	mb();
	dma->vsync_status = MDP3_REG_READ(MDP3_REG_INTR_STATUS) &
		(1 << MDP3_INTR_LCDC_START_OF_FRAME);
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	pr_debug("mdp3_dmap_update wait for vsync_comp in\n");
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO) {
		rc = mdp3_wait4vsync(dma, false, KOFF_TIMEOUT);
		if (rc <= 0)
			rc = -1;
	}
	pr_debug("mdp3_dmap_update wait for vsync_comp out\n");
	return rc;
}

static int mdp3_dmas_update(struct mdp3_dma *dma, void *buf,
				struct mdp3_intf *intf)
{
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type = MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		if (intf->active)
			wait_for_completion_killable(&dma->dma_comp);
	}

	spin_lock_irqsave(&dma->dma_lock, flag);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_ADDR, (u32)buf);
	dma->source_config.buf = buf;
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD)
		MDP3_REG_WRITE(MDP3_REG_DMA_S_START, 1);

	if (!intf->active) {
		pr_debug("mdp3_dmap_update start interface\n");
		intf->start(intf);
	}

	wmb();
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO)
		mdp3_wait4vsync(dma, true, 0);
	return 0;
}

static int mdp3_dmap_cursor_update(struct mdp3_dma *dma, int x, int y)
{
	u32 cursor_pos;

	cursor_pos = x | (y << 16);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_POS, cursor_pos);
	dma->cursor.x = x;
	dma->cursor.y = y;
	return 0;
}

static int mdp3_dmap_histo_get(struct mdp3_dma *dma)
{
	int i, state, timeout, ret;
	u32 addr;
	unsigned long flag;

	spin_lock_irqsave(&dma->histo_lock, flag);
	state = dma->histo_state;
	spin_unlock_irqrestore(&dma->histo_lock, flag);

	if (state != MDP3_DMA_HISTO_STATE_START &&
		state != MDP3_DMA_HISTO_STATE_READY) {
		pr_err("mdp3_dmap_histo_get invalid state %d\n", state);
		return -EINVAL;
	}

	timeout = HIST_WAIT_TIMEOUT(dma->histogram_config.frame_count);
	ret = wait_for_completion_killable_timeout(&dma->histo_comp, timeout);

	if (ret == 0) {
		pr_debug("mdp3_dmap_histo_get time out\n");
		ret = -ETIMEDOUT;
	} else if (ret < 0) {
		pr_err("mdp3_dmap_histo_get interrupted\n");
	}

	if (ret < 0)
		return ret;

	if (dma->histo_state != MDP3_DMA_HISTO_STATE_READY) {
		pr_debug("mdp3_dmap_histo_get after dma shut down\n");
		return -EPERM;
	}

	addr = MDP3_REG_DMA_P_HIST_R_DATA;
	for (i = 0; i < MDP_HISTOGRAM_BIN_NUM; i++) {
		dma->histo_data.r_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	addr = MDP3_REG_DMA_P_HIST_G_DATA;
	for (i = 0; i < MDP_HISTOGRAM_BIN_NUM; i++) {
		dma->histo_data.g_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	addr = MDP3_REG_DMA_P_HIST_B_DATA;
	for (i = 0; i < MDP_HISTOGRAM_BIN_NUM; i++) {
		dma->histo_data.b_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	dma->histo_data.extra[0] =
			MDP3_REG_READ(MDP3_REG_DMA_P_HIST_EXTRA_INFO_0);
	dma->histo_data.extra[1] =
			MDP3_REG_READ(MDP3_REG_DMA_P_HIST_EXTRA_INFO_1);

	spin_lock_irqsave(&dma->histo_lock, flag);
	INIT_COMPLETION(dma->histo_comp);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_START, 1);
	wmb();
	dma->histo_state = MDP3_DMA_HISTO_STATE_START;
	spin_unlock_irqrestore(&dma->histo_lock, flag);

	return 0;
}

static int mdp3_dmap_histo_start(struct mdp3_dma *dma)
{
	unsigned long flag;

	if (dma->histo_state != MDP3_DMA_HISTO_STATE_IDLE)
		return -EINVAL;

	spin_lock_irqsave(&dma->histo_lock, flag);

	INIT_COMPLETION(dma->histo_comp);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_START, 1);
	wmb();
	dma->histo_state = MDP3_DMA_HISTO_STATE_START;

	spin_unlock_irqrestore(&dma->histo_lock, flag);

	mdp3_dma_callback_enable(dma, MDP3_DMA_CALLBACK_TYPE_HIST_DONE);
	return 0;

}

static int mdp3_dmap_histo_reset(struct mdp3_dma *dma)
{
	unsigned long flag;
	int ret;

	spin_lock_irqsave(&dma->histo_lock, flag);

	INIT_COMPLETION(dma->histo_comp);

	mdp3_dma_clk_auto_gating(dma, 0);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_ENABLE, BIT(0)|BIT(1));
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_RESET_SEQ_START, 1);
	wmb();
	dma->histo_state = MDP3_DMA_HISTO_STATE_RESET;

	spin_unlock_irqrestore(&dma->histo_lock, flag);

	mdp3_dma_callback_enable(dma, MDP3_DMA_CALLBACK_TYPE_HIST_RESET_DONE);
	ret = wait_for_completion_killable_timeout(&dma->histo_comp,
				msecs_to_jiffies(DMA_HISTO_RESET_TIMEOUT_MS));

	if (ret == 0) {
		pr_err("mdp3_dmap_histo_reset time out\n");
		ret = -ETIMEDOUT;
	} else if (ret < 0) {
		pr_err("mdp3_dmap_histo_reset interrupted\n");
	} else {
		ret = 0;
	}
	mdp3_dma_callback_disable(dma, MDP3_DMA_CALLBACK_TYPE_HIST_RESET_DONE);
	mdp3_dma_clk_auto_gating(dma, 1);

	return ret;
}

static int mdp3_dmap_histo_stop(struct mdp3_dma *dma)
{
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_HIST_RESET_DONE |
			MDP3_DMA_CALLBACK_TYPE_HIST_DONE;

	spin_lock_irqsave(&dma->histo_lock, flag);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_CANCEL_REQ, 1);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_ENABLE, 0);
	wmb();
	dma->histo_state = MDP3_DMA_HISTO_STATE_IDLE;
	complete(&dma->histo_comp);

	spin_unlock_irqrestore(&dma->histo_lock, flag);

	mdp3_dma_callback_disable(dma, cb_type);
	return 0;
}

static int mdp3_dmap_histo_op(struct mdp3_dma *dma, u32 op)
{
	int ret;

	switch (op) {
	case MDP3_DMA_HISTO_OP_START:
		ret = mdp3_dmap_histo_start(dma);
		break;
	case MDP3_DMA_HISTO_OP_STOP:
	case MDP3_DMA_HISTO_OP_CANCEL:
		ret = mdp3_dmap_histo_stop(dma);
		break;
	case MDP3_DMA_HISTO_OP_RESET:
		ret = mdp3_dmap_histo_reset(dma);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int mdp3_dma_start(struct mdp3_dma *dma, struct mdp3_intf *intf)
{
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;
	u32 dma_start_offset = MDP3_REG_DMA_P_START;

	if (dma->dma_sel == MDP3_DMA_P)
		dma_start_offset = MDP3_REG_DMA_P_START;
	else if (dma->dma_sel == MDP3_DMA_S)
		dma_start_offset = MDP3_REG_DMA_S_START;
	else
		return -EINVAL;

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type |= MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		MDP3_REG_WRITE(dma_start_offset, 1);
	}

	intf->start(intf);
	wmb();
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	pr_debug("mdp3_dma_start wait for vsync_comp in\n");
	mdp3_wait4vsync(dma, true, 0);
	pr_debug("mdp3_dma_start wait for vsync_comp out\n");
	return 0;
}

static int mdp3_dma_stop(struct mdp3_dma *dma, struct mdp3_intf *intf)
{
	int ret = 0;
	u32 status, display_status_bit;

	if (dma->dma_sel == MDP3_DMA_P)
		display_status_bit = BIT(6);
	else if (dma->dma_sel == MDP3_DMA_S)
		display_status_bit = BIT(7);
	else
		return -EINVAL;

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO)
		display_status_bit |= BIT(11);

	intf->stop(intf);
	ret = readl_poll_timeout((mdp3_res->mdp_base + MDP3_REG_DISPLAY_STATUS),
				status,
				((status & display_status_bit) == 0),
				DMA_STOP_POLL_SLEEP_US,
				DMA_STOP_POLL_TIMEOUT_US);

	mdp3_dma_callback_disable(dma, MDP3_DMA_CALLBACK_TYPE_VSYNC |
					MDP3_DMA_CALLBACK_TYPE_DMA_DONE);
	mdp3_irq_disable(MDP3_INTR_LCDC_UNDERFLOW);

	MDP3_REG_WRITE(MDP3_REG_INTR_ENABLE, 0);
	MDP3_REG_WRITE(MDP3_REG_INTR_CLEAR, 0xfffffff);

	init_completion(&dma->dma_comp);
	dma->vsync_client.handler = NULL;
	return ret;
}

void mdp3_dump_dma(void *data)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)data;
	u32 isr, mask;

	mdp3_clk_prepare();
	mdp3_clk_enable(1, 0);

	isr = MDP3_REG_READ(MDP3_REG_INTR_STATUS);
	mask = MDP3_REG_READ(MDP3_REG_INTR_ENABLE);
	MDSS_TIMEOUT_LOG("-------- MDP3 INTERRUPT DATA ---------\n");
	MDSS_TIMEOUT_LOG("MDP3_REG_INTR_STATUS: 0x%08X\n", isr);
	MDSS_TIMEOUT_LOG("MDP3_REG_INTR_ENABLE: 0x%08X\n", mask);
	MDSS_TIMEOUT_LOG("global irqs disabled: %d\n", irqs_disabled());
	MDSS_TIMEOUT_LOG("------ MDP3 INTERRUPT DATA DONE ------\n");

	if (dma) {
		MDSS_TIMEOUT_LOG("-------- MDP3 DMA DATA ---------\n");
		MDSS_TIMEOUT_LOG("vsync_cnt=%u\n", dma->vsync_cnt);
		MDSS_TIMEOUT_LOG("------ MDP3 DMA DATA DONE ------\n");
	}

	mdp3_clk_enable(0, 0);
	mdp3_clk_unprepare();
}

int mdp3_dma_init(struct mdp3_dma *dma)
{
	int ret = 0;

	pr_debug("mdp3_dma_init\n");
	switch (dma->dma_sel) {
	case MDP3_DMA_P:
		dma->dma_config = mdp3_dmap_config;
		dma->dma_config_source = mdp3_dmap_config_source;
		dma->config_cursor = mdp3_dmap_cursor_config;
		dma->config_ccs = mdp3_dmap_ccs_config;
		dma->config_histo = mdp3_dmap_histo_config;
		dma->config_lut = mdp3_dmap_lut_config;
		dma->update = mdp3_dmap_update;
		dma->update_cursor = mdp3_dmap_cursor_update;
		dma->get_histo = mdp3_dmap_histo_get;
		dma->histo_op = mdp3_dmap_histo_op;
		dma->vsync_enable = mdp3_dma_vsync_enable;
		dma->dma_done_notifier = mdp3_dma_done_notifier;
		dma->start = mdp3_dma_start;
		dma->stop = mdp3_dma_stop;
		break;
	case MDP3_DMA_S:
		dma->dma_config = mdp3_dmas_config;
		dma->dma_config_source = mdp3_dmas_config_source;
		dma->config_cursor = NULL;
		dma->config_ccs = NULL;
		dma->config_histo = NULL;
		dma->config_lut = NULL;
		dma->update = mdp3_dmas_update;
		dma->update_cursor = NULL;
		dma->get_histo = NULL;
		dma->histo_op = NULL;
		dma->vsync_enable = mdp3_dma_vsync_enable;
		dma->start = mdp3_dma_start;
		dma->stop = mdp3_dma_stop;
		break;
	case MDP3_DMA_E:
	default:
		ret = -ENODEV;
		break;
	}

	spin_lock_init(&dma->dma_lock);
	spin_lock_init(&dma->histo_lock);
	init_completion(&dma->vsync_comp);
	init_completion(&dma->dma_comp);
	init_completion(&dma->histo_comp);
	dma->vsync_client.handler = NULL;
	dma->vsync_client.arg = NULL;
	dma->histo_state = MDP3_DMA_HISTO_STATE_IDLE;
	dma->update_src_cfg = false;

	dma->vsync_cnt = 0;

	memset(&dma->cursor, 0, sizeof(dma->cursor));
	memset(&dma->ccs_config, 0, sizeof(dma->ccs_config));
	memset(&dma->histogram_config, 0, sizeof(dma->histogram_config));

	mdss_timeout_init(mdp3_dump_dma, dma);

	return ret;
}

int lcdc_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 temp;
	struct mdp3_video_intf_cfg *v = &cfg->video;
	temp = v->hsync_pulse_width | (v->hsync_period << 16);
	MDP3_REG_WRITE(MDP3_REG_LCDC_HSYNC_CTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_VSYNC_PERIOD, v->vsync_period);
	MDP3_REG_WRITE(MDP3_REG_LCDC_VSYNC_PULSE_WIDTH, v->vsync_pulse_width);
	temp = v->display_start_x | (v->display_end_x << 16);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_V_START, v->display_start_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_V_END, v->display_end_y);
	temp = v->active_start_x | (v->active_end_x);
	if (v->active_h_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_V_START, v->active_start_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_V_END, v->active_end_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_HSYNC_SKEW, v->hsync_skew);
	temp = 0;
	if (!v->hsync_polarity)
		temp = BIT(0);
	if (!v->vsync_polarity)
		temp = BIT(1);
	if (!v->de_polarity)
		temp = BIT(2);
	MDP3_REG_WRITE(MDP3_REG_LCDC_CTL_POLARITY, temp);

	return 0;
}

int lcdc_start(struct mdp3_intf *intf)
{
	MDP3_REG_WRITE(MDP3_REG_LCDC_EN, BIT(0));
	wmb();
	intf->active = true;
	return 0;
}

int lcdc_stop(struct mdp3_intf *intf)
{
	MDP3_REG_WRITE(MDP3_REG_LCDC_EN, 0);
	wmb();
	intf->active = false;
	return 0;
}

int dsi_video_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 temp;
	struct mdp3_video_intf_cfg *v = &cfg->video;

	pr_debug("dsi_video_config\n");

	temp = v->hsync_pulse_width | (v->hsync_period << 16);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_HSYNC_CTL, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_VSYNC_PERIOD, v->vsync_period);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_VSYNC_PULSE_WIDTH,
			v->vsync_pulse_width);
	temp = v->display_start_x | (v->display_end_x << 16);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_V_START, v->display_start_y);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_V_END, v->display_end_y);
	temp = v->active_start_x | (v->active_end_x << 16);
	if (v->active_h_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_HCTL, temp);

	temp = v->active_start_y;
	if (v->active_v_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_V_START, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_V_END, v->active_end_y);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_HSYNC_SKEW, v->hsync_skew);
	temp = 0;
	if (!v->hsync_polarity)
		temp |= BIT(0);
	if (!v->vsync_polarity)
		temp |= BIT(1);
	if (!v->de_polarity)
		temp |= BIT(2);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_CTL_POLARITY, temp);

	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_UNDERFLOW_CTL, 0x800000ff);
	return 0;
}

int dsi_video_start(struct mdp3_intf *intf)
{
	pr_debug("dsi_video_start\n");
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_EN, BIT(0));
	wmb();
	intf->active = true;
	return 0;
}

int dsi_video_stop(struct mdp3_intf *intf)
{
	pr_debug("dsi_video_stop\n");
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_EN, 0);
	wmb();
	intf->active = false;
	return 0;
}

int dsi_cmd_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 id_map = 0;
	u32 trigger_en = 0;

	if (cfg->dsi_cmd.primary_dsi_cmd_id)
		id_map = BIT(0);
	if (cfg->dsi_cmd.secondary_dsi_cmd_id)
		id_map = BIT(4);

	if (cfg->dsi_cmd.dsi_cmd_tg_intf_sel)
		trigger_en = BIT(4);

	MDP3_REG_WRITE(MDP3_REG_DSI_CMD_MODE_ID_MAP, id_map);
	MDP3_REG_WRITE(MDP3_REG_DSI_CMD_MODE_TRIGGER_EN, trigger_en);

	return 0;
}

int dsi_cmd_start(struct mdp3_intf *intf)
{
	intf->active = true;
	return 0;
}

int dsi_cmd_stop(struct mdp3_intf *intf)
{
	intf->active = false;
	return 0;
}

int mdp3_intf_init(struct mdp3_intf *intf)
{
	switch (intf->cfg.type) {
	case MDP3_DMA_OUTPUT_SEL_LCDC:
		intf->config = lcdc_config;
		intf->start = lcdc_start;
		intf->stop = lcdc_stop;
		break;
	case MDP3_DMA_OUTPUT_SEL_DSI_VIDEO:
		intf->config = dsi_video_config;
		intf->start = dsi_video_start;
		intf->stop = dsi_video_stop;
		break;
	case MDP3_DMA_OUTPUT_SEL_DSI_CMD:
		intf->config = dsi_cmd_config;
		intf->start = dsi_cmd_start;
		intf->stop = dsi_cmd_stop;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}
