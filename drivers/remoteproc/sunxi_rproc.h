/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SUNXI_RPROC_H_
#define _SUNXI_RPROC_H_

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/pm_domain.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

/*
 * XuanTie E906 view of its two SRAM spaces (Allwinner sun55iw3 vendor DT
 * "memory-mappings"). DRAM is seen 1:1 from 0x40040000 up, and 0x20000 is
 * the HiFi4 DSP RAM (also 1:1); neither goes through this table.
 */
#define E906_SRAM_SPACE0_DA		0x3ffc0000UL
#define E906_SRAM_SPACE1_DA		0x40000000UL

/* Allwinner A523/A527/T527 System Bus (Host Physical) Addresses & Window Sizes */
#define SUN55I_SRAM_SPACE0_SYS		0x07280000UL
#define SUN55I_SRAM_SPACE0_SIZE		0x00040000UL /* 256 KB */
#define SUN55I_SRAM_SPACE1_SYS		0x072c0000UL
#define SUN55I_SRAM_SPACE1_SIZE		0x00040000UL /* 256 KB */

/* Address Translation Table flags */
#define ATT_IOMEM			BIT(30)

struct sunxi_rproc_att {
	u64 da;
	u64 sa;
	size_t size;
	int flags;
};

/* XuanTie CFG Block Register Offsets */
#define E906_STA_ADD_REG		0x0204

/* Remap Control Register (offset 0x364 in PRCM_R_CCU / MCU_CCU) */
/* Bit 0: 0 = local RAM for MCU; 1 = share for system */
#define SUNXI_REMAP_MCU_RAM_BIT		BIT(0)
/* Bit 1: 0 = SRAMA3_2 not shared; 1 = share for MCU_SYS */
#define SUNXI_REMAP_SRAMA3_2_BIT	BIT(1)

#define SUNXI_RPROC_MAX_RMEM		3
#define SUNXI_RPROC_CRASH_IRQ_ARMED	0

struct sunxi_rproc_rmem {
	const char *name;
	void *va;
	phys_addr_t phys;
	size_t size;
	bool is_iomem;
};

struct sunxi_rproc_cfg {
	const char *name;
	const struct sunxi_rproc_att *att;
	size_t att_size;
	u32 boot_reg_offset;
};

struct sunxi_rproc {
	struct rproc *rproc;
	struct device *dev;
	const struct sunxi_rproc_cfg *cfg;

	/* CCU Clocks & Resets */
	struct clk *clk_bus;
	struct clk *clk_core;
	struct clk *clk_sram;
	struct reset_control *rst_cfg;
	struct reset_control *rst_core;
	struct reset_control *rst_sram;
	struct reset_control *rst_debug;

	/* Hardware Memory Windows (Dedicated SRAM, Switchable SRAM, Remap) */
	void __iomem *cfg_va;

	void __iomem *remap_va;

	void __iomem *r_sram_va;
	phys_addr_t r_sram_phys;
	size_t r_sram_size;

	void __iomem *r_sram1_va;
	phys_addr_t r_sram1_phys;
	size_t r_sram1_size;

	/* Reserved Memory & Mailbox State */
	struct sunxi_rproc_rmem rmem[SUNXI_RPROC_MAX_RMEM];
	unsigned int num_rmem;
	unsigned long flags;
	bool has_reserved_mem;
	int crash_irq;
	struct mbox_client cl;
	struct mbox_chan *mbox_chan;
	struct workqueue_struct *workqueue;
	struct work_struct vq_work;
	struct dev_pm_domain_list *pd_list;
};

#if IS_ENABLED(CONFIG_KUNIT)
extern const struct sunxi_rproc_cfg sun55i_riscv_cfg;
extern const struct rproc_ops sunxi_rproc_ops;

int sunxi_rproc_prepare(struct rproc *rproc);
int sunxi_rproc_unprepare(struct rproc *rproc);
int sunxi_rproc_start(struct rproc *rproc);
void sunxi_rproc_kick(struct rproc *rproc, int vqid);
void sunxi_rproc_mb_rx_callback(struct mbox_client *cl, void *data);
void *sunxi_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);
#endif

#endif /* _SUNXI_RPROC_H_ */
