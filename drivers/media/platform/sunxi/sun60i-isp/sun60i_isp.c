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
#include <media/v4l2-subdev.h>

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

static irqreturn_t sun60i_isp_irq_handler(int irq, void *dev_id)
{
	struct sun60i_isp_dev *isp = dev_id;
	u32 status;

	status = readl(isp->regs + ISP_INT_STATUS_REG);
	if (!status)
		return IRQ_NONE;

	writel(status, isp->regs + ISP_INT_STATUS_REG);

	if (status & ISP_IRQ_PARA_SAVE)
		isp->frame_count++;

	if (status & ISP_IRQ_PARA_LOAD) {
		/* Re-arm parameter ready for online continuous streaming */
		u32 val = readl(isp->regs + ISP_AHB_CFG0_REG);
		val |= ISP_AHB_PARA_READY;
		writel(val, isp->regs + ISP_AHB_CFG0_REG);
	}

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

	/* 6. Channel 0 10-bit raw */
	writel(10, (u8 *)load + ISP_LOAD_CH0_EXPAND_CFG0);

	/* 7. Clear D3D enable in module bypass registers */
	val = readl((u8 *)load + ISP_LOAD_MODULE_BYPASS1);
	val &= ~BIT(5);
	writel(val, (u8 *)load + ISP_LOAD_MODULE_BYPASS1);

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

		ret = clk_prepare_enable(isp->clk_ram);
		if (ret)
			goto err_pm;

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

		/* Configure base physical addresses (>> 2 shift required by hardware) */
		writel(isp->save_dma >> 2, isp->regs + ISP_SAVE_ADDR_REG);
		writel(isp->load_dma[0] >> 2, isp->regs + ISP_LOAD_ADDR_REG);
		writel(isp->save_load_dma >> 2, isp->regs + ISP_SAVE_LOAD_ADDR_REG);

		/* Clear pending IRQs and unmask operational and error interrupts */
		writel(0xffffffff, isp->regs + ISP_INT_STATUS_REG);
		writel(ISP_IRQ_ENABLE_MASK, isp->regs + ISP_INT_BYPASS_REG);

		/* Start ISP Top engine in online streaming mode */
		writel(ISP_TOP_EN | ISP_TOP_CAP_EN, isp->regs + ISP_TOP_CFG0_REG);

		/* Arm tables and trigger capture start */
		val = ISP_AHB_GAMMA_UPDATE | ISP_AHB_CEM_UPDATE |
		      ISP_AHB_PARA_READY | ISP_AHB_CAP_EN;
		writel(val, isp->regs + ISP_AHB_CFG0_REG);

		isp->streaming = true;
		isp->frame_count = 0;

		spin_unlock_irqrestore(&isp->lock, flags);
		return 0;

err_clk_mod:
		clk_disable_unprepare(isp->clk_mod);
err_clk_ram:
		clk_disable_unprepare(isp->clk_ram);
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

	isp->variant = of_device_get_match_data(dev);

	isp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(isp->regs))
		return PTR_ERR(isp->regs);

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
		pm_runtime_put(dev);

		dev_info(dev, "Allwinner Hardware ISP version: 0x%08x (rev %x.%x)\n",
			 isp->isp_ver,
			 (isp->isp_ver >> 12) & 0xfff,
			 isp->isp_ver & 0xfff);
	}

	/* Initialize V4L2 Subdevice */
	v4l2_subdev_init(&isp->subdev, &sun60i_isp_subdev_ops);
	isp->subdev.owner = THIS_MODULE;
	isp->subdev.dev = dev;
	strscpy(isp->subdev.name, SUN60I_ISP_NAME, sizeof(isp->subdev.name));
	isp->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	isp->pads[SUN60I_ISP_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	isp->pads[SUN60I_ISP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

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
