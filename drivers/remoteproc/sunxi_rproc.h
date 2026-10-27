/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SUNXI_RPROC_H_
#define _SUNXI_RPROC_H_

#include <linux/bits.h>
#include <linux/irqreturn.h>
#include <linux/mailbox_client.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct clk;
struct device;
struct firmware;
struct reset_control;
struct rproc;
struct rproc_ops;

/*
 * XuanTie E906 view of its two SRAM spaces (Allwinner sun55iw3 vendor DT
 * "memory-mappings"). DRAM is seen 1:1 from 0x40040000 up, and 0x20000 is
 * the HiFi4 DSP RAM (also 1:1); neither goes through this table.
 */
#define SUN55I_E906_SRAM_SPACE0_DA	0x3ffc0000UL
#define SUN55I_E906_SRAM_SPACE1_DA	0x40000000UL

/* Host physical addresses and sizes of the two SRAM spaces */
#define SUN55I_SRAM_SPACE0_SYS		0x07280000UL
#define SUN55I_SRAM_SPACE0_SIZE		0x00040000UL /* 256 KiB */
#define SUN55I_SRAM_SPACE1_SYS		0x072c0000UL
#define SUN55I_SRAM_SPACE1_SIZE		0x00040000UL /* 256 KiB */

/* Address translation table entry flags */
#define SUNXI_RPROC_ATT_IOMEM		BIT(30)

struct sunxi_rproc_att {
	u64 da;
	u64 sa;
	size_t size;
	u32 flags;
};

/* Configuration block: boot address register */
#define SUN55I_E906_STA_ADD_REG		0x0204

/* Remap register (R-PRCM 0x07010364), shared with the HiFi4 DSP: bit 1 is ours */
#define SUNXI_REMAP_SRAMA3_2_BIT	BIT(1)	/* SRAM A3_2 (space 1) to the MCU */

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
	/* Core views come first: a host address maps to the first entry covering it */
	const struct sunxi_rproc_att *att;
	size_t att_size;
	u32 boot_reg_offset;
};

struct sunxi_rproc {
	struct rproc *rproc;
	struct device *dev;
	const struct sunxi_rproc_cfg *cfg;

	/* Clocks and resets */
	struct clk *clk_bus;
	struct clk *clk_core;
	struct clk *clk_sram;
	struct reset_control *rst_cfg;
	struct reset_control *rst_core;
	struct reset_control *rst_sram;
	struct reset_control *rst_debug;

	/* Configuration block, remap register and the two SRAM spaces */
	void __iomem *cfg_va;

	void __iomem *remap_va;

	void __iomem *r_sram_va;
	phys_addr_t r_sram_phys;
	size_t r_sram_size;

	void __iomem *r_sram1_va;
	phys_addr_t r_sram1_phys;
	size_t r_sram1_size;

	/* memory-region entries, the crash interrupt and the mailbox */
	struct sunxi_rproc_rmem rmem[SUNXI_RPROC_MAX_RMEM];
	unsigned int num_rmem;
	unsigned long flags;
	int crash_irq;
	struct mbox_client cl;
	struct mbox_chan *mbox_chan;
	struct workqueue_struct *workqueue;
	struct work_struct vq_work;
};

#if IS_ENABLED(CONFIG_KUNIT)
extern const struct sunxi_rproc_cfg sun55i_riscv_cfg;
extern const struct rproc_ops sunxi_rproc_ops;

u32 sunxi_rproc_readl(void __iomem *addr);
void sunxi_rproc_writel(u32 val, void __iomem *addr);
void sunxi_rproc_memset_io(void __iomem *addr, int val, size_t len);
int sunxi_rproc_vq_notify(int id, void *ptr, void *data);
void sunxi_rproc_vq_work(struct work_struct *work);
irqreturn_t sunxi_rproc_crash_handler(int irq, void *data);
int sunxi_rproc_prepare(struct rproc *rproc);
int sunxi_rproc_unprepare(struct rproc *rproc);
int sunxi_rproc_start(struct rproc *rproc);
int sunxi_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw);
void sunxi_rproc_kick(struct rproc *rproc, int vqid);
void sunxi_rproc_mb_rx_callback(struct mbox_client *cl, void *data);
void *sunxi_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);
#endif

#endif /* _SUNXI_RPROC_H_ */
