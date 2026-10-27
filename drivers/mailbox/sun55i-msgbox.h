/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SUN55I_MSGBOX_H_
#define _SUN55I_MSGBOX_H_

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#define SUN55I_PROC_ARM			0
#define SUN55I_PROC_DSP			1
#define SUN55I_PROC_CPUS		2
#define SUN55I_PROC_RV			3

#define SUN55I_MAX_PROCESSORS		4
#define SUN55I_CHANS_PER_PROC		4
#define SUN55I_NUM_ROUTES		(SUN55I_MAX_PROCESSORS - 1)
#define SUN55I_NUM_CHANS		(SUN55I_NUM_ROUTES * SUN55I_CHANS_PER_PROC)
#define SUN55I_FIFO_MAX			8

#define SUNXI_MSGBOX_OFFSET(n)			(0x100 * (n))
#define SUNXI_MSGBOX_READ_IRQ_ENABLE(n)		(0x020 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_READ_IRQ_STATUS(n)		(0x024 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_ENABLE(n)	(0x030 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_STATUS(n)	(0x034 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_MSG_STATUS(n, p)		(0x060 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_FIFO(n, p)		(0x070 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_WRITE_IRQ_THRESHOLD(n, p)	(0x080 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))

#define RD_IRQ_EN_BIT(p)			BIT_U32((p) * 2)
#define RD_IRQ_PEND_BIT(p)			BIT_U32((p) * 2)
#define WR_IRQ_EN_BIT(p)			BIT_U32((p) * 2 + 1)
#define WR_IRQ_PEND_BIT(p)			BIT_U32((p) * 2 + 1)
/* Write IRQ threshold field value 0: raised while at least one slot is free */
#define WR_IRQ_THR_ONE_FREE			0
#define MSG_NUM_MASK				GENMASK(3, 0)

struct sun55i_route {
	u8 remote_id;
	u8 remote_n;
};

struct sun55i_msgbox;

/* One per remote bank whose interrupt line carries our TX-done (write) IRQ */
struct sun55i_tx_irq {
	struct sun55i_msgbox *mbox;
	int remote_id;
	int irq;
	const char *name;
	/* Started channels on this bank; the IRQ is requested while > 0 */
	unsigned int users;
};

struct sun55i_msgbox {
	struct mbox_controller controller;
	void __iomem *regs[SUN55I_MAX_PROCESSORS];
	/* Protects concurrent MMIO register access */
	spinlock_t lock;
	/* Host bank interrupt: messages received by this processor */
	int irq;
	/* "allwinner,tx-done-irq": TX done by the write IRQ, else polled */
	bool tx_done_irq;
	/* Indexed by bank; the host bank's entry is unused */
	struct sun55i_tx_irq tx_irqs[SUN55I_MAX_PROCESSORS];
	/* Channels whose startup() holds their bank's TX-done line */
	unsigned long tx_irq_held;
	/* Serialises tx_irqs[].users and the matching request_irq()/free_irq() */
	struct mutex tx_irq_lock;
	/* Timer mode: channels whose message waits for room in a full FIFO */
	unsigned long tx_held;
	u32 tx_held_msg[SUN55I_NUM_CHANS];
};

#if IS_ENABLED(CONFIG_KUNIT)
extern const struct mbox_chan_ops sun55i_msgbox_chan_ops;

u32 sun55i_readl(void __iomem *addr);
void sun55i_writel(u32 val, void __iomem *addr);
void sun55i_chan_to_route(int chan_idx, int *local_n, int *p,
			  int *remote_id, int *remote_n);
irqreturn_t sun55i_msgbox_irq(int irq, void *dev_id);
irqreturn_t sun55i_msgbox_tx_irq(int irq, void *dev_id);
#endif

#endif /* _SUN55I_MSGBOX_H_ */
