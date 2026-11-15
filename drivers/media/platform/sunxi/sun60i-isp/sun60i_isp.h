/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Allwinner A733 / T527 Hardware ISP 602 Driver
 *
 * Copyright (c) 2026 Tim Michals <tcmichals@gmail.com>
 */

#ifndef _SUN60I_ISP_H_
#define _SUN60I_ISP_H_

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <media/media-entity.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#define SUN60I_ISP_PAD_SINK	0
#define SUN60I_ISP_PAD_SOURCE	1
#define SUN60I_ISP_NUM_PADS	2

struct sun60i_isp_variant {
	const char *name;
	u32 override_words[4];
	bool has_override;
};

struct sun60i_isp_dev {
	struct device			*dev;
	void __iomem			*regs;
	struct clk			*clk_mod;
	struct clk			*clk_ram;
	struct reset_control		*rst;
	int				irq;

	const struct sun60i_isp_variant	*variant;

	struct v4l2_subdev		subdev;
	struct media_pad		pads[SUN60I_ISP_NUM_PADS];
	struct v4l2_mbus_framefmt	format;

	/* Coherent DMA parameter buffers */
	void				*load_buf[2];
	dma_addr_t			load_dma[2];
	void				*save_buf;
	dma_addr_t			save_dma;
	void				*save_load_buf;
	dma_addr_t			save_load_dma;
	int				active_load_idx;

	u32				isp_ver;
	u64				frame_count;
	bool				streaming;

	spinlock_t			lock;
};

static inline struct sun60i_isp_dev *to_sun60i_isp(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sun60i_isp_dev, subdev);
}

#endif /* _SUN60I_ISP_H_ */
