// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner A733 / T527 Hardware Image Signal Processor (ISP 602) Driver
 *
 * Copyright (c) 2026 Tim Michals <tcmichals@gmail.com>
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-vmalloc.h>

#include "sun60i_isp.h"
#include "sun60i_isp_reg.h"
#include "sun60i_isp_tbl.h"

#define SUN60I_ISP_NAME "sun60i-isp"

static const struct sun60i_isp_variant sun60i_a733_isp_var = {
	.name = "sun60i-a733-isp",
	.has_override = false,
};

static const struct sun60i_isp_variant sun55i_a523_isp_var = {
	.name = "sun55i-a523-isp",
	.has_override = true,
	.override_words = {
		0x120c1212, 0x002400c0, 0x24120c12, 0x000c00c0,
	},
};

static void sun60i_isp_apply_params(void *load,
				    const struct sun60i_isp_params_cfg *cfg,
				    bool *gamma_updated)
{
	u32 val;

	if (cfg->version != SUN60I_ISP_PARAM_API_V1)
		return;

	if (cfg->module_en_update) {
		val = readl((u8 *)load + ISP_LOAD_MODULE_BYPASS1);
		if (cfg->module_en_update & SUN60I_ISP_MOD_BLC) {
			if (cfg->module_ens & SUN60I_ISP_MOD_BLC)
				val |= ISP_MOD1_BLC_EN;
			else
				val &= ~ISP_MOD1_BLC_EN;
		}
		if (cfg->module_en_update & SUN60I_ISP_MOD_WB) {
			if (cfg->module_ens & SUN60I_ISP_MOD_WB)
				val |= ISP_MOD1_WB_EN;
			else
				val &= ~ISP_MOD1_WB_EN;
		}
		if (cfg->module_en_update & SUN60I_ISP_MOD_CCM) {
			if (cfg->module_ens & SUN60I_ISP_MOD_CCM)
				val |= ISP_MOD1_CCM_EN;
			else
				val &= ~ISP_MOD1_CCM_EN;
		}
		if (cfg->module_en_update & SUN60I_ISP_MOD_GAMMA) {
			if (cfg->module_ens & SUN60I_ISP_MOD_GAMMA)
				val |= ISP_MOD1_GAMMA_EN;
			else
				val &= ~ISP_MOD1_GAMMA_EN;
		}
		writel(val, (u8 *)load + ISP_LOAD_MODULE_BYPASS1);
	}

	if (cfg->module_cfg_update & SUN60I_ISP_MOD_BLC) {
		val = readl((u8 *)load + ISP_LOAD_BLC_OFFSET0) & 0xe000e000;
		val |= ((u32)(cfg->blc.gr & 0x1fff) << 16) | (cfg->blc.r & 0x1fff);
		writel(val, (u8 *)load + ISP_LOAD_BLC_OFFSET0);

		val = readl((u8 *)load + ISP_LOAD_BLC_OFFSET1) & 0xe000e000;
		val |= ((u32)(cfg->blc.b & 0x1fff) << 16) | (cfg->blc.gb & 0x1fff);
		writel(val, (u8 *)load + ISP_LOAD_BLC_OFFSET1);
	}

	if (cfg->module_cfg_update & SUN60I_ISP_MOD_WB) {
		val = readl((u8 *)load + ISP_LOAD_WB_GAIN0) & 0xf000f000;
		val |= ((u32)(cfg->wb.gr & 0xfff) << 16) | (cfg->wb.r & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_WB_GAIN0);

		val = readl((u8 *)load + ISP_LOAD_WB_GAIN1) & 0xf000f000;
		val |= ((u32)(cfg->wb.b & 0xfff) << 16) | (cfg->wb.gb & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_WB_GAIN1);
	}

	if (cfg->module_cfg_update & SUN60I_ISP_MOD_CCM) {
		const s16 *c = cfg->ccm.coeff;
		const s16 *o = cfg->ccm.offset;

		val = readl((u8 *)load + ISP_LOAD_CCM_M0) & 0xf000f000;
		val |= ((u32)(c[1] & 0xfff) << 16) | (c[0] & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_M0);

		val = readl((u8 *)load + ISP_LOAD_CCM_M1) & 0xf000f000;
		val |= ((u32)(c[3] & 0xfff) << 16) | (c[2] & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_M1);

		val = readl((u8 *)load + ISP_LOAD_CCM_M2) & 0xf000f000;
		val |= ((u32)(c[5] & 0xfff) << 16) | (c[4] & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_M2);

		val = readl((u8 *)load + ISP_LOAD_CCM_M3) & 0xf000f000;
		val |= ((u32)(c[7] & 0xfff) << 16) | (c[6] & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_M3);

		val = readl((u8 *)load + ISP_LOAD_CCM_M4) & 0xe000f000;
		val |= ((u32)(o[0] & 0x1fff) << 16) | (c[8] & 0xfff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_M4);

		val = readl((u8 *)load + ISP_LOAD_CCM_OFF) & 0xe000e000;
		val |= ((u32)(o[2] & 0x1fff) << 16) | (o[1] & 0x1fff);
		writel(val, (u8 *)load + ISP_LOAD_CCM_OFF);
	}

	if (cfg->module_cfg_update & SUN60I_ISP_MOD_GAMMA) {
		u32 *gamma = (u32 *)((u8 *)load + ISP_LOAD_GAMMA_OFFSET);
		u32 i;

		for (i = 0; i < ISP_GAMMA_TBL_LEN; i++) {
			u32 r = min_t(u32, cfg->gamma.lut_r[i], 1023);
			u32 g = min_t(u32, cfg->gamma.lut_g[i], 1023);
			u32 b = min_t(u32, cfg->gamma.lut_b[i], 1023);

			gamma[i] = r | (g << 10) | (b << 20);
		}
		*gamma_updated = true;
	}
}

/* Must be called with isp->lock held */
static void sun60i_isp_stage_next_params(struct sun60i_isp_dev *isp)
{
	struct sun60i_isp_meta_buf *mbuf;
	struct sun60i_isp_params_cfg *cfg;
	bool gamma_updated = false;
	int next;

	if (isp->params_pending || list_empty(&isp->params.buf_list))
		return;

	mbuf = list_first_entry(&isp->params.buf_list,
				struct sun60i_isp_meta_buf, list);
	list_del(&mbuf->list);

	cfg = vb2_plane_vaddr(&mbuf->vb.vb2_buf, 0);
	if (cfg && vb2_get_plane_payload(&mbuf->vb.vb2_buf, 0) >= sizeof(*cfg)) {
		next = isp->active_load_idx ^ 1;
		memcpy(isp->load_buf[next], isp->load_buf[isp->active_load_idx],
		       ISP_LOAD_DRAM_SIZE);
		sun60i_isp_apply_params(isp->load_buf[next], cfg, &gamma_updated);
		isp->params_pending = true;
		isp->gamma_pending = gamma_updated;
	}

	mbuf->vb.sequence = isp->frame_count;
	mbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&mbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static irqreturn_t sun60i_isp_irq_handler(int irq, void *dev_id)
{
	struct sun60i_isp_dev *isp = dev_id;
	u32 status;

	status = readl(isp->regs + ISP_INT_STATUS_REG);
	if (!status)
		return IRQ_NONE;

	writel(status, isp->regs + ISP_INT_STATUS_REG);

	spin_lock(&isp->lock);

	if (status & ISP_IRQ_PARA_SAVE) {
		isp->frame_count++;

		if (!list_empty(&isp->stats.buf_list)) {
			struct sun60i_isp_meta_buf *sbuf;
			struct sun60i_isp_stat_buffer *dst;

			sbuf = list_first_entry(&isp->stats.buf_list,
						struct sun60i_isp_meta_buf, list);
			list_del(&sbuf->list);

			dst = vb2_plane_vaddr(&sbuf->vb.vb2_buf, 0);
			if (dst) {
				dst->version = SUN60I_ISP_STAT_API_V1;
				dst->frame_id = (u32)isp->frame_count;
				memcpy(dst->ae,
				       (u8 *)isp->save_buf + ISP_SAVE_STAT_AE_OFS,
				       SUN60I_ISP_STAT_AE_SIZE);
				memcpy(dst->awb,
				       (u8 *)isp->save_buf + ISP_SAVE_STAT_AWB_OFS,
				       SUN60I_ISP_STAT_AWB_SIZE);
				memcpy(dst->hist0,
				       (u8 *)isp->save_buf + ISP_SAVE_STAT_HIST0_OFS,
				       SUN60I_ISP_STAT_HIST0_SIZE);
				vb2_set_plane_payload(&sbuf->vb.vb2_buf, 0,
						      sizeof(*dst));
			}
			sbuf->vb.sequence = isp->frame_count - 1;
			sbuf->vb.vb2_buf.timestamp = ktime_get_ns();
			vb2_buffer_done(&sbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
	}

	if (status & ISP_IRQ_PARA_LOAD) {
		u32 val = readl(isp->regs + ISP_AHB_CFG0_REG);

		sun60i_isp_stage_next_params(isp);
		if (isp->params_pending) {
			int next = isp->active_load_idx ^ 1;

			writel(isp->load_dma[next] >> 2,
			       isp->regs + ISP_LOAD_ADDR_REG);
			isp->active_load_idx = next;
			isp->params_pending = false;
			if (isp->gamma_pending) {
				val |= ISP_AHB_GAMMA_UPDATE;
				isp->gamma_pending = false;
			}
		}
		val |= ISP_AHB_PARA_READY;
		writel(val, isp->regs + ISP_AHB_CFG0_REG);
	}

	spin_unlock(&isp->lock);

	if (unlikely(status & ISP_IRQ_ERRORS)) {
		dev_warn_ratelimited(isp->dev,
				     "ISP IRQ error status: 0x%08x (inter0: 0x%08x, inter1: 0x%08x, inter2: 0x%08x)\n",
				     status,
				     readl(isp->regs + ISP_INTER_STATUS0_REG),
				     readl(isp->regs + ISP_INTER_STATUS1_REG),
				     readl(isp->regs + ISP_INTER_STATUS2_REG));
	}

	return IRQ_HANDLED;
}

static void sun60i_isp_init_load_buffer(struct sun60i_isp_dev *isp,
					u32 width, u32 height)
{
	u32 *load = isp->load_buf[0];
	u32 val;

	/* 1. Copy default register values */
	memcpy(load, isp602_default_reg, ISP_LOAD_REG_SIZE);
	memset((u8 *)load + ISP_LOAD_REG_SIZE, 0,
	       ISP_LOAD_DRAM_SIZE - ISP_LOAD_REG_SIZE);

	/* 2. Apply SoC variant table overrides if present */
	if (isp->variant && isp->variant->has_override) {
		memcpy(&load[92], isp->variant->override_words,
		       sizeof(isp->variant->override_words));
	}

	/* 3. Input format GBRG (OV5647 raw pattern) */
	val = readl((u8 *)load + ISP_LOAD_GLOBAL_CFG0);
	val = (val & ~0x7) | ISP_GBRG;
	writel(val, (u8 *)load + ISP_LOAD_GLOBAL_CFG0);

	/* Save-load buffer random seed format */
	if (isp->save_load_buf) {
		u32 *save_load = isp->save_load_buf;
		val = readl((u8 *)save_load + ISP_SAVE_LOAD_RANDOM_SEED);
		val = (val & ~(0x7 << 28)) | (ISP_GBRG << 28);
		writel(val, (u8 *)save_load + ISP_SAVE_LOAD_RANDOM_SEED);
	}

	/* 4. Frame dimensions */
	writel((height << 16) | width, (u8 *)load + ISP_LOAD_INPUT_SIZE);
	writel((height << 16) | width, (u8 *)load + ISP_LOAD_VALID_SIZE);
	writel(0, (u8 *)load + ISP_LOAD_VALID_START);

	/* 5. Speed mode 3, blank cycle 5 */
	val = readl((u8 *)load + ISP_LOAD_GLOBAL_CFG1);
	val = (val & ~((0x3 << 4) | (0x7f << 8))) | (3 << 4) | (5 << 8);
	writel(val, (u8 *)load + ISP_LOAD_GLOBAL_CFG1);

	/* 6. Channel 0: 10-bit input (bits 8:4 = 10) -> 12-bit internal (bits 20:16 = 12) */
	writel(0x000c00a0, (u8 *)load + ISP_LOAD_CH0_EXPAND_CFG0);

	/* 7. Clear D3D enable and enable Gamma + AE/AWB/HIST statistics */
	val = readl((u8 *)load + ISP_LOAD_MODULE_BYPASS1);
	val &= ~BIT(5);
	val |= ISP_MOD1_GAMMA_EN | ISP_MOD1_AE_EN;
	writel(val, (u8 *)load + ISP_LOAD_MODULE_BYPASS1);

	/* 7b. Populate 10-bit packed RGB Gamma LUT (gamma ~2.0 via int_sqrt) */
	{
		u32 *gamma = (u32 *)((u8 *)load + ISP_LOAD_GAMMA_OFFSET);
		u32 i;

		for (i = 0; i < ISP_GAMMA_TBL_LEN; i++) {
			u32 v = min_t(u32, int_sqrt(i * 1023), 1023);

			gamma[i] = v | (v << 10) | (v << 20);
		}
	}

	/* 8. Clone into second load buffer for ping-pong safety */
	memcpy(isp->load_buf[1], load, ISP_LOAD_DRAM_SIZE);
}

static int sun60i_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sun60i_isp_dev *isp = to_sun60i_isp(sd);
	unsigned long flags;
	u32 val;
	int ret;

	if (enable) {
		ret = pm_runtime_resume_and_get(isp->dev);
		if (ret < 0)
			return ret;

		ret = clk_prepare_enable(isp->clk_bus);
		if (ret)
			goto err_pm;

		ret = clk_prepare_enable(isp->clk_ram);
		if (ret)
			goto err_clk_bus;

		ret = clk_prepare_enable(isp->clk_mod);
		if (ret)
			goto err_clk_ram;

		if (isp->rst) {
			ret = reset_control_deassert(isp->rst);
			if (ret)
				goto err_clk_mod;
		}

		spin_lock_irqsave(&isp->lock, flags);

		sun60i_isp_init_load_buffer(isp, isp->format.width,
					    isp->format.height);
		isp->active_load_idx = 0;
		isp->params_pending = false;
		isp->gamma_pending = false;

		/* Apply any pre-queued params buffer before frame 0 */
		if (!list_empty(&isp->params.buf_list)) {
			struct sun60i_isp_meta_buf *mbuf;
			struct sun60i_isp_params_cfg *cfg;
			bool dummy = false;

			mbuf = list_first_entry(&isp->params.buf_list,
						struct sun60i_isp_meta_buf, list);
			list_del(&mbuf->list);
			cfg = vb2_plane_vaddr(&mbuf->vb.vb2_buf, 0);
			if (cfg && vb2_get_plane_payload(&mbuf->vb.vb2_buf, 0) >= sizeof(*cfg))
				sun60i_isp_apply_params(isp->load_buf[0], cfg, &dummy);
			mbuf->vb.sequence = 0;
			mbuf->vb.vb2_buf.timestamp = ktime_get_ns();
			vb2_buffer_done(&mbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}

		/* Configure base physical addresses (>> 2 shift required by hardware) */
		writel(isp->save_dma >> 2, isp->regs + ISP_SAVE_ADDR_REG);
		writel(isp->load_dma[0] >> 2, isp->regs + ISP_LOAD_ADDR_REG);
		writel(isp->save_load_dma >> 2, isp->regs + ISP_SAVE_LOAD_ADDR_REG);

		/* Clear pending IRQs and unmask operational and error interrupts */
		writel(0xffffffff, isp->regs + ISP_INT_STATUS_REG);
		writel(ISP_IRQ_ENABLE_MASK, isp->regs + ISP_INT_BYPASS_REG);

		/* Start ISP Top engine in online streaming mode */
		writel(ISP_TOP_EN | ISP_TOP_CAP_EN, isp->regs + ISP_TOP_CFG0_REG);

		/* Arm Gamma LUT and trigger capture start */
		val = ISP_AHB_GAMMA_UPDATE | ISP_AHB_PARA_READY | ISP_AHB_CAP_EN;
		writel(val, isp->regs + ISP_AHB_CFG0_REG);

		isp->streaming = true;
		isp->frame_count = 0;

		spin_unlock_irqrestore(&isp->lock, flags);
		return 0;

err_clk_mod:
		clk_disable_unprepare(isp->clk_mod);
err_clk_ram:
		clk_disable_unprepare(isp->clk_ram);
err_clk_bus:
		clk_disable_unprepare(isp->clk_bus);
err_pm:
		pm_runtime_put(isp->dev);
		return ret;
	}

	spin_lock_irqsave(&isp->lock, flags);
	writel(0, isp->regs + ISP_AHB_CFG0_REG);
	writel(0, isp->regs + ISP_INT_BYPASS_REG);
	writel(0xffffffff, isp->regs + ISP_INT_STATUS_REG);
	writel(0, isp->regs + ISP_TOP_CFG0_REG);
	isp->streaming = false;
	spin_unlock_irqrestore(&isp->lock, flags);

	if (isp->rst)
		reset_control_assert(isp->rst);

	clk_disable_unprepare(isp->clk_mod);
	clk_disable_unprepare(isp->clk_ram);
	clk_disable_unprepare(isp->clk_bus);
	pm_runtime_put(isp->dev);

	return 0;
}

static int sun60i_isp_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad == SUN60I_ISP_PAD_SINK) {
		switch (code->index) {
		case 0:
			code->code = MEDIA_BUS_FMT_SGBRG10_1X10;
			break;
		case 1:
			code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
			break;
		case 2:
			code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
			break;
		case 3:
			code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
			break;
		default:
			return -EINVAL;
		}
	} else if (code->pad == SUN60I_ISP_PAD_SOURCE) {
		switch (code->index) {
		case 0:
			code->code = MEDIA_BUS_FMT_UYVY8_1X16;
			break;
		case 1:
			code->code = MEDIA_BUS_FMT_YUYV8_1_5X8;
			break;
		default:
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static int sun60i_isp_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct sun60i_isp_dev *isp = to_sun60i_isp(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt;
		try_fmt = v4l2_subdev_state_get_format(state, fmt->pad);
		if (!try_fmt)
			return -EINVAL;
		fmt->format = *try_fmt;
	} else {
		fmt->format = isp->format;
		if (fmt->pad == SUN60I_ISP_PAD_SOURCE)
			fmt->format.code = MEDIA_BUS_FMT_UYVY8_1X16;
	}

	return 0;
}

static int sun60i_isp_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct sun60i_isp_dev *isp = to_sun60i_isp(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt;
		try_fmt = v4l2_subdev_state_get_format(state, fmt->pad);
		if (!try_fmt)
			return -EINVAL;
		*try_fmt = fmt->format;
	} else {
		if (fmt->pad == SUN60I_ISP_PAD_SINK) {
			isp->format = fmt->format;
		} else {
			fmt->format = isp->format;
			fmt->format.code = MEDIA_BUS_FMT_UYVY8_1X16;
		}
	}

	return 0;
}

static const struct v4l2_subdev_video_ops sun60i_isp_video_ops = {
	.s_stream = sun60i_isp_s_stream,
};

static const struct v4l2_subdev_pad_ops sun60i_isp_pad_ops = {
	.enum_mbus_code = sun60i_isp_enum_mbus_code,
	.get_fmt = sun60i_isp_get_fmt,
	.set_fmt = sun60i_isp_set_fmt,
};

static const struct v4l2_subdev_ops sun60i_isp_subdev_ops = {
	.video = &sun60i_isp_video_ops,
	.pad = &sun60i_isp_pad_ops,
};

/* --- Metadata Video Nodes: Parameters (META_OUTPUT) & Statistics (META_CAPTURE) --- */

static int sun60i_isp_meta_querycap(struct file *file, void *priv,
				    struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, SUN60I_ISP_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 SUN60I_ISP_NAME);
	return 0;
}

static int sun60i_isp_params_enum_fmt(struct file *file, void *priv,
				      struct v4l2_fmtdesc *f)
{
	if (f->index > 0 || f->type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;
	f->pixelformat = V4L2_META_FMT_SUN60I_ISP_PARAMS;
	return 0;
}

static int sun60i_isp_params_g_fmt(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;
	f->fmt.meta.dataformat = V4L2_META_FMT_SUN60I_ISP_PARAMS;
	f->fmt.meta.buffersize = sizeof(struct sun60i_isp_params_cfg);
	return 0;
}

static int sun60i_isp_stats_enum_fmt(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	if (f->index > 0 || f->type != V4L2_BUF_TYPE_META_CAPTURE)
		return -EINVAL;
	f->pixelformat = V4L2_META_FMT_SUN60I_ISP_STAT;
	return 0;
}

static int sun60i_isp_stats_g_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_META_CAPTURE)
		return -EINVAL;
	f->fmt.meta.dataformat = V4L2_META_FMT_SUN60I_ISP_STAT;
	f->fmt.meta.buffersize = sizeof(struct sun60i_isp_stat_buffer);
	return 0;
}

static const struct v4l2_ioctl_ops sun60i_isp_params_ioctl_ops = {
	.vidioc_querycap = sun60i_isp_meta_querycap,
	.vidioc_enum_fmt_meta_out = sun60i_isp_params_enum_fmt,
	.vidioc_g_fmt_meta_out = sun60i_isp_params_g_fmt,
	.vidioc_s_fmt_meta_out = sun60i_isp_params_g_fmt,
	.vidioc_try_fmt_meta_out = sun60i_isp_params_g_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_ioctl_ops sun60i_isp_stats_ioctl_ops = {
	.vidioc_querycap = sun60i_isp_meta_querycap,
	.vidioc_enum_fmt_meta_cap = sun60i_isp_stats_enum_fmt,
	.vidioc_g_fmt_meta_cap = sun60i_isp_stats_g_fmt,
	.vidioc_s_fmt_meta_cap = sun60i_isp_stats_g_fmt,
	.vidioc_try_fmt_meta_cap = sun60i_isp_stats_g_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations sun60i_isp_meta_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int sun60i_isp_meta_queue_setup(struct vb2_queue *q,
				       unsigned int *num_buffers,
				       unsigned int *num_planes,
				       unsigned int sizes[],
				       struct device *alloc_devs[])
{
	unsigned int size = (q->type == V4L2_BUF_TYPE_META_OUTPUT) ?
			    sizeof(struct sun60i_isp_params_cfg) :
			    sizeof(struct sun60i_isp_stat_buffer);

	if (*num_planes)
		return sizes[0] < size ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = size;
	return 0;
}

static int sun60i_isp_meta_buf_prepare(struct vb2_buffer *vb)
{
	unsigned int size = (vb->vb2_queue->type == V4L2_BUF_TYPE_META_OUTPUT) ?
			    sizeof(struct sun60i_isp_params_cfg) :
			    sizeof(struct sun60i_isp_stat_buffer);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;

	if (vb->vb2_queue->type == V4L2_BUF_TYPE_META_CAPTURE)
		vb2_set_plane_payload(vb, 0, size);

	return 0;
}

static void sun60i_isp_params_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sun60i_isp_meta_buf *mbuf =
		container_of(vbuf, struct sun60i_isp_meta_buf, vb);
	struct sun60i_isp_dev *isp = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&isp->lock, flags);
	list_add_tail(&mbuf->list, &isp->params.buf_list);
	spin_unlock_irqrestore(&isp->lock, flags);
}

static void sun60i_isp_stats_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sun60i_isp_meta_buf *mbuf =
		container_of(vbuf, struct sun60i_isp_meta_buf, vb);
	struct sun60i_isp_dev *isp = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&isp->lock, flags);
	list_add_tail(&mbuf->list, &isp->stats.buf_list);
	spin_unlock_irqrestore(&isp->lock, flags);
}

static void sun60i_isp_meta_stop_streaming(struct vb2_queue *q)
{
	struct sun60i_isp_dev *isp = vb2_get_drv_priv(q);
	struct list_head *head = (q->type == V4L2_BUF_TYPE_META_OUTPUT) ?
				 &isp->params.buf_list : &isp->stats.buf_list;
	struct sun60i_isp_meta_buf *mbuf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&isp->lock, flags);
	list_for_each_entry_safe(mbuf, tmp, head, list) {
		list_del(&mbuf->list);
		vb2_buffer_done(&mbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&isp->lock, flags);
}

static const struct vb2_ops sun60i_isp_params_vb2_ops = {
	.queue_setup = sun60i_isp_meta_queue_setup,
	.buf_prepare = sun60i_isp_meta_buf_prepare,
	.buf_queue = sun60i_isp_params_buf_queue,
	.stop_streaming = sun60i_isp_meta_stop_streaming,
};

static const struct vb2_ops sun60i_isp_stats_vb2_ops = {
	.queue_setup = sun60i_isp_meta_queue_setup,
	.buf_prepare = sun60i_isp_meta_buf_prepare,
	.buf_queue = sun60i_isp_stats_buf_queue,
	.stop_streaming = sun60i_isp_meta_stop_streaming,
};

static int sun60i_isp_register_meta_node(struct sun60i_isp_dev *isp,
					 struct sun60i_isp_meta_node *node,
					 bool is_params)
{
	struct video_device *vdev = &node->vdev;
	struct vb2_queue *q = &node->queue;
	int ret;

	q->type = is_params ? V4L2_BUF_TYPE_META_OUTPUT :
			      V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = isp;
	q->ops = is_params ? &sun60i_isp_params_vb2_ops :
			     &sun60i_isp_stats_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct sun60i_isp_meta_buf);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->lock;

	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	node->pad.flags = is_params ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		return ret;

	strscpy(vdev->name, is_params ? "sun60i-isp-params" : "sun60i-isp-stats",
		sizeof(vdev->name));
	vdev->vfl_type = VFL_TYPE_VIDEO;
	vdev->vfl_dir = is_params ? VFL_DIR_TX : VFL_DIR_RX;
	vdev->device_caps = (is_params ? V4L2_CAP_META_OUTPUT :
					 V4L2_CAP_META_CAPTURE) |
			    V4L2_CAP_STREAMING;
	vdev->release = video_device_release_empty;
	vdev->fops = &sun60i_isp_meta_fops;
	vdev->ioctl_ops = is_params ? &sun60i_isp_params_ioctl_ops :
				      &sun60i_isp_stats_ioctl_ops;
	vdev->v4l2_dev = isp->subdev.v4l2_dev;
	vdev->queue = q;
	vdev->lock = &node->lock;
	video_set_drvdata(vdev, isp);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		media_entity_cleanup(&vdev->entity);
		return ret;
	}

	if (is_params)
		ret = media_create_pad_link(&vdev->entity, 0,
					    &isp->subdev.entity,
					    SUN60I_ISP_PAD_PARAMS,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
	else
		ret = media_create_pad_link(&isp->subdev.entity,
					    SUN60I_ISP_PAD_STATS,
					    &vdev->entity, 0,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		video_unregister_device(vdev);
		media_entity_cleanup(&vdev->entity);
		return ret;
	}

	node->registered = true;
	return 0;
}

static int sun60i_isp_subdev_registered(struct v4l2_subdev *sd)
{
	struct sun60i_isp_dev *isp = to_sun60i_isp(sd);
	int ret;

	ret = sun60i_isp_register_meta_node(isp, &isp->params, true);
	if (ret)
		return ret;

	ret = sun60i_isp_register_meta_node(isp, &isp->stats, false);
	if (ret) {
		video_unregister_device(&isp->params.vdev);
		media_entity_cleanup(&isp->params.vdev.entity);
		isp->params.registered = false;
		return ret;
	}

	return 0;
}

static void sun60i_isp_subdev_unregistered(struct v4l2_subdev *sd)
{
	struct sun60i_isp_dev *isp = to_sun60i_isp(sd);

	if (isp->stats.registered) {
		video_unregister_device(&isp->stats.vdev);
		media_entity_cleanup(&isp->stats.vdev.entity);
		isp->stats.registered = false;
	}
	if (isp->params.registered) {
		video_unregister_device(&isp->params.vdev);
		media_entity_cleanup(&isp->params.vdev.entity);
		isp->params.registered = false;
	}
}

static const struct v4l2_subdev_internal_ops sun60i_isp_internal_ops = {
	.registered = sun60i_isp_subdev_registered,
	.unregistered = sun60i_isp_subdev_unregistered,
};

static int sun60i_isp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun60i_isp_dev *isp;
	int ret;

	isp = devm_kzalloc(dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->dev = dev;
	spin_lock_init(&isp->lock);
	mutex_init(&isp->params.lock);
	mutex_init(&isp->stats.lock);
	INIT_LIST_HEAD(&isp->params.buf_list);
	INIT_LIST_HEAD(&isp->stats.buf_list);

	isp->variant = of_device_get_match_data(dev);

	isp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(isp->regs))
		return PTR_ERR(isp->regs);

	isp->clk_bus = devm_clk_get_optional(dev, "bus");
	if (IS_ERR(isp->clk_bus))
		return dev_err_probe(dev, PTR_ERR(isp->clk_bus),
				     "failed to get bus clock\n");

	isp->clk_mod = devm_clk_get(dev, "mod");
	if (IS_ERR(isp->clk_mod))
		return dev_err_probe(dev, PTR_ERR(isp->clk_mod),
				      "failed to get mod clock\n");

	isp->clk_ram = devm_clk_get(dev, "ram");
	if (IS_ERR(isp->clk_ram))
		return dev_err_probe(dev, PTR_ERR(isp->clk_ram),
				      "failed to get ram clock\n");

	isp->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(isp->rst))
		return PTR_ERR(isp->rst);

	isp->irq = platform_get_irq(pdev, 0);
	if (isp->irq < 0)
		return isp->irq;

	ret = devm_request_irq(dev, isp->irq, sun60i_isp_irq_handler,
			       IRQF_SHARED, dev_name(dev), isp);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	/* Restrict DMA mask to 32 bits for physical address reachability */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set 32-bit DMA mask\n");

	/* Allocate coherent parameter buffers */
	isp->load_buf[0] = dma_alloc_coherent(dev, ISP_LOAD_DRAM_SIZE,
					      &isp->load_dma[0], GFP_KERNEL);
	isp->load_buf[1] = dma_alloc_coherent(dev, ISP_LOAD_DRAM_SIZE,
					      &isp->load_dma[1], GFP_KERNEL);
	isp->save_buf = dma_alloc_coherent(dev, ISP_SAVE_DRAM_SIZE,
					    &isp->save_dma, GFP_KERNEL);
	isp->save_load_buf = dma_alloc_coherent(dev, ISP_SAVE_LOAD_DRAM_SIZE,
						&isp->save_load_dma, GFP_KERNEL);

	if (!isp->load_buf[0] || !isp->load_buf[1] ||
	    !isp->save_buf || !isp->save_load_buf) {
		ret = -ENOMEM;
		goto err_free_dma;
	}

	platform_set_drvdata(pdev, isp);
	pm_runtime_enable(dev);

	/* Probe silicon hardware version */
	ret = pm_runtime_resume_and_get(dev);
	if (!ret) {
		clk_prepare_enable(isp->clk_bus);
		clk_prepare_enable(isp->clk_ram);
		clk_prepare_enable(isp->clk_mod);
		if (isp->rst)
			reset_control_deassert(isp->rst);

		writel(ISP_TOP_VER_RD_EN, isp->regs + ISP_TOP_CFG0_REG);
		isp->isp_ver = readl(isp->regs + ISP_VER_CFG_REG);
		writel(0, isp->regs + ISP_TOP_CFG0_REG);

		if (isp->rst)
			reset_control_assert(isp->rst);
		clk_disable_unprepare(isp->clk_mod);
		clk_disable_unprepare(isp->clk_ram);
		clk_disable_unprepare(isp->clk_bus);
		pm_runtime_put(dev);

		dev_info(dev, "Allwinner Hardware ISP version: 0x%08x (rev %x.%x)\n",
			 isp->isp_ver,
			 (isp->isp_ver >> 12) & 0xfff,
			 isp->isp_ver & 0xfff);
	}

	/* Initialize V4L2 Subdevice */
	v4l2_subdev_init(&isp->subdev, &sun60i_isp_subdev_ops);
	isp->subdev.internal_ops = &sun60i_isp_internal_ops;
	isp->subdev.owner = THIS_MODULE;
	isp->subdev.dev = dev;
	strscpy(isp->subdev.name, SUN60I_ISP_NAME, sizeof(isp->subdev.name));
	isp->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	isp->pads[SUN60I_ISP_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	isp->pads[SUN60I_ISP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	isp->pads[SUN60I_ISP_PAD_PARAMS].flags = MEDIA_PAD_FL_SINK;
	isp->pads[SUN60I_ISP_PAD_STATS].flags = MEDIA_PAD_FL_SOURCE;
	isp->subdev.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;

	ret = media_entity_pads_init(&isp->subdev.entity, SUN60I_ISP_NUM_PADS,
				     isp->pads);
	if (ret)
		goto err_pm_disable;

	/* Default 1080p format */
	isp->format.width = 1920;
	isp->format.height = 1080;
	isp->format.code = MEDIA_BUS_FMT_SGBRG10_1X10;
	isp->format.field = V4L2_FIELD_NONE;

	ret = v4l2_async_register_subdev(&isp->subdev);
	if (ret) {
		dev_err(dev, "failed to register v4l2 async subdev: %d\n", ret);
		goto err_entity_cleanup;
	}

	return 0;

err_entity_cleanup:
	media_entity_cleanup(&isp->subdev.entity);
err_pm_disable:
	pm_runtime_disable(dev);
err_free_dma:
	if (isp->load_buf[0])
		dma_free_coherent(dev, ISP_LOAD_DRAM_SIZE, isp->load_buf[0],
				  isp->load_dma[0]);
	if (isp->load_buf[1])
		dma_free_coherent(dev, ISP_LOAD_DRAM_SIZE, isp->load_buf[1],
				  isp->load_dma[1]);
	if (isp->save_buf)
		dma_free_coherent(dev, ISP_SAVE_DRAM_SIZE, isp->save_buf,
				  isp->save_dma);
	if (isp->save_load_buf)
		dma_free_coherent(dev, ISP_SAVE_LOAD_DRAM_SIZE,
				  isp->save_load_buf, isp->save_load_dma);
	return ret;
}

static void sun60i_isp_remove(struct platform_device *pdev)
{
	struct sun60i_isp_dev *isp = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	v4l2_async_unregister_subdev(&isp->subdev);
	media_entity_cleanup(&isp->subdev.entity);
	pm_runtime_disable(dev);

	dma_free_coherent(dev, ISP_LOAD_DRAM_SIZE, isp->load_buf[0],
			  isp->load_dma[0]);
	dma_free_coherent(dev, ISP_LOAD_DRAM_SIZE, isp->load_buf[1],
			  isp->load_dma[1]);
	dma_free_coherent(dev, ISP_SAVE_DRAM_SIZE, isp->save_buf,
			  isp->save_dma);
	dma_free_coherent(dev, ISP_SAVE_LOAD_DRAM_SIZE, isp->save_load_buf,
			  isp->save_load_dma);
}

static const struct of_device_id sun60i_isp_of_match[] = {
	{
		.compatible = "allwinner,sun60i-a733-isp",
		.data = &sun60i_a733_isp_var,
	},
	{
		.compatible = "allwinner,sun55i-a523-isp",
		.data = &sun55i_a523_isp_var,
	},
	{},
};
MODULE_DEVICE_TABLE(of, sun60i_isp_of_match);

static struct platform_driver sun60i_isp_driver = {
	.probe = sun60i_isp_probe,
	.remove = sun60i_isp_remove,
	.driver = {
		.name = SUN60I_ISP_NAME,
		.of_match_table = sun60i_isp_of_match,
	},
};
module_platform_driver(sun60i_isp_driver);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("Allwinner A733 / T527 Hardware ISP 602 Driver");
MODULE_LICENSE("GPL");
