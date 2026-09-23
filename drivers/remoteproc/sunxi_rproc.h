/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SUNXI_RPROC_H_
#define _SUNXI_RPROC_H_

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

/* XuanTie core-local view of dedicated SRAM Spaces (E906/E907) */
#define E907_SRAM_C_DA			0x00020000
#define E907_SRAM_SPACE0_DA		0x3ff80000
#define E907_SRAM_SPACE0_DA_ALT		0x3ffc0000
#define E907_SRAM_SPACE1_DA		0x40000000

struct sunxi_rproc_cfg {
	const char *name;
};

struct sunxi_rproc {
	struct rproc *rproc;
	struct device *dev;
	const struct sunxi_rproc_cfg *cfg;

	/* CCU Clocks & Resets */
	struct clk *clk_parent;
	struct clk *clk_bus;
	struct clk *clk_core;
	struct clk *clk_sram;
	struct clk *clk_msgbox;
	struct reset_control *rst_cfg;
	struct reset_control *rst_core;
	struct reset_control *rst_sram;
	struct reset_control *rst_msgbox;

	/* Hardware Memory Windows (Dedicated SRAM, Switchable SRAM, Remap) */
	void __iomem *cfg_va;
	phys_addr_t cfg_phys;

	void __iomem *remap_va;
	phys_addr_t remap_phys;

	void __iomem *r_sram_va;
	phys_addr_t r_sram_phys;
	size_t r_sram_size;

	void __iomem *r_sram1_va;
	phys_addr_t r_sram1_phys;
	size_t r_sram1_size;

	void *dram_va;
	phys_addr_t dram_phys;
	size_t dram_size;

	/* Trace / DDR Reserved Memory Window */
	void *trace_va;
	phys_addr_t trace_phys;
	size_t trace_size;

	/* Reserved Memory & Mailbox State */
	bool has_reserved_mem;
	int crash_irq;
	struct mbox_client cl;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct work_struct vq_work;
	u32 kick_msg;
};

extern const struct rproc_ops sunxi_rproc_ops;

int sunxi_rproc_prepare(struct rproc *rproc);
int sunxi_rproc_unprepare(struct rproc *rproc);
int sunxi_rproc_start(struct rproc *rproc);
int sunxi_rproc_stop(struct rproc *rproc);
void sunxi_rproc_kick(struct rproc *rproc, int vqid);
void *sunxi_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);

#endif /* _SUNXI_RPROC_H_ */
