// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sun55i 4-Port Hardware Message Box Driver
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 * Based on vendor sunxi-msgbox driver by Allwinner Technology Co., Ltd.
 */

#include <kunit/visibility.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include "sun55i-msgbox.h"

/*
 * Hardware routing table for Cortex-A55 host (local_id = 0):
 *   local_n = 0 -> CPUS (remote_id = SUN55I_PROC_CPUS, remote_n = 0) -> Channels 0..3
 *   local_n = 1 -> DSP  (remote_id = SUN55I_PROC_DSP,  remote_n = 0) -> Channels 4..7
 *   local_n = 2 -> RV   (remote_id = SUN55I_PROC_RV,   remote_n = 2) -> Channels 8..11
 */
VISIBLE_IF_KUNIT
const struct sun55i_route sun55i_msgbox_arm_routes[SUN55I_NUM_ROUTES] = {
	[0] = { .remote_id = SUN55I_PROC_CPUS, .remote_n = 0 },
	[1] = { .remote_id = SUN55I_PROC_DSP,  .remote_n = 0 },
	[2] = { .remote_id = SUN55I_PROC_RV,   .remote_n = 2 },
};
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_arm_routes);

static inline struct sun55i_msgbox *to_sun55i_msgbox(struct mbox_chan *chan)
{
	return chan->con_priv;
}

VISIBLE_IF_KUNIT
void sun55i_chan_to_route(int chan_idx, int *local_n, int *p,
			  int *remote_id, int *remote_n)
{
	if (chan_idx < 0 || chan_idx >= SUN55I_NUM_CHANS) {
		*local_n = 0;
		*p = 0;
		*remote_id = 0;
		*remote_n = 0;
		return;
	}
	*local_n = chan_idx / SUN55I_CHANS_PER_PROC;
	*p = chan_idx % SUN55I_CHANS_PER_PROC;
	*remote_id = sun55i_msgbox_arm_routes[*local_n].remote_id;
	*remote_n = sun55i_msgbox_arm_routes[*local_n].remote_n;
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_chan_to_route);

VISIBLE_IF_KUNIT
irqreturn_t sun55i_msgbox_irq(int irq, void *dev_id)
{
	struct sun55i_msgbox *mbox = dev_id;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	int i, local_n, p, chan_idx;

	/*
	 * startup()/shutdown() change the IRQ enable bits under the same lock,
	 * so a channel cannot be torn down while its FIFO is being drained.
	 */
	spin_lock_irqsave(&mbox->lock, flags);

	for (local_n = 0; local_n < SUN55I_NUM_ROUTES; local_n++) {
		void __iomem *local_base = mbox->regs[0];
		u32 en, stat, pending;

		en = readl(local_base + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
		stat = readl(local_base + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
		pending = en & stat;

		if (!pending)
			continue;

		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			if (!(pending & RD_IRQ_PEND_BIT(p)))
				continue;

			chan_idx = local_n * SUN55I_CHANS_PER_PROC + p;

			/*
			 * Clear pending status BEFORE draining the FIFO to seal
			 * the TOCTOU race: any new message arriving while draining
			 * will re-assert the hardware pending bit, guaranteeing
			 * a subsequent interrupt and preventing blackholed IPC data.
			 */
			writel(RD_IRQ_PEND_BIT(p),
			       local_base + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));

			/* Cap drain at FIFO_MAX to prevent CPU lockup from a runaway remote */
			for (i = 0; i < SUN55I_FIFO_MAX; i++) {
				u32 msg;

				if (!(readl(local_base +
					    SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK))
					break;
				msg = readl(local_base + SUNXI_MSGBOX_MSG_FIFO(local_n, p));
				mbox_chan_received_data(&mbox->controller.chans[chan_idx], &msg);
			}

			ret = IRQ_HANDLED;
		}
	}

	spin_unlock_irqrestore(&mbox->lock, flags);

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_irq);

static int sun55i_msgbox_send_data(struct mbox_chan *chan, void *data)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 msg = 0;

	if (data)
		memcpy(&msg, data, sizeof(msg));

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	writel(msg, mbox->regs[remote_id] + SUNXI_MSGBOX_MSG_FIFO(remote_n, p));
	return 0;
}

static int sun55i_msgbox_startup(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int i, local_n, p, remote_id, remote_n;
	unsigned long flags;
	u32 val;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/*
	 * Clear pending status BEFORE flushing, same order as the ISR: a message
	 * landing after the clear re-asserts the pending bit instead of being
	 * left unread with no interrupt.
	 */
	writel(RD_IRQ_PEND_BIT(p),
	       mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));

	/* Flush any stale receive data (bounded to FIFO_MAX) */
	for (i = 0; i < SUN55I_FIFO_MAX; i++) {
		if (!(readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK))
			break;
		readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_FIFO(local_n, p));
	}

	/* Enable receive IRQ */
	spin_lock_irqsave(&mbox->lock, flags);
	val = readl(mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	val |= RD_IRQ_EN_BIT(p);
	writel(val, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	spin_unlock_irqrestore(&mbox->lock, flags);

	return 0;
}

static void sun55i_msgbox_shutdown(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int i, local_n, p, remote_id, remote_n;
	unsigned long flags;
	u32 val;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/* Disable receive IRQ */
	spin_lock_irqsave(&mbox->lock, flags);
	val = readl(mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	val &= ~RD_IRQ_EN_BIT(p);
	writel(val, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	spin_unlock_irqrestore(&mbox->lock, flags);

	/* Clear pending status and flush (bounded to FIFO_MAX) */
	writel(RD_IRQ_PEND_BIT(p),
	       mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
	for (i = 0; i < SUN55I_FIFO_MAX; i++) {
		if (!(readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK))
			break;
		readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_FIFO(local_n, p));
	}
}

static bool sun55i_msgbox_last_tx_done(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 count;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/*
	 * The hardware allows snooping on the remote user's FIFO status.
	 * Consider the last message acknowledged only once the remote has
	 * drained the FIFO; free space alone does not mean it was consumed.
	 */
	count = readl(mbox->regs[remote_id] + SUNXI_MSGBOX_MSG_STATUS(remote_n, p)) & MSG_NUM_MASK;
	return count == 0;
}

static bool sun55i_msgbox_peek_data(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 count;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	count = readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK;
	return count > 0;
}

VISIBLE_IF_KUNIT
const struct mbox_chan_ops sun55i_msgbox_chan_ops = {
	.send_data    = sun55i_msgbox_send_data,
	.startup      = sun55i_msgbox_startup,
	.shutdown     = sun55i_msgbox_shutdown,
	.last_tx_done = sun55i_msgbox_last_tx_done,
	.peek_data    = sun55i_msgbox_peek_data,
};
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_chan_ops);

static void sun55i_msgbox_assert_reset(void *data)
{
	reset_control_assert(data);
}

static int sun55i_msgbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	struct reset_control *rst;
	struct mbox_chan *chans;
	struct sun55i_msgbox *mbox;
	int i, ret, irq, local_n;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	spin_lock_init(&mbox->lock);

	chans = devm_kcalloc(dev, SUN55I_NUM_CHANS, sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	for (i = 0; i < SUN55I_NUM_CHANS; i++)
		chans[i].con_priv = mbox;

	for (i = 0; i < SUN55I_MAX_PROCESSORS; i++) {
		mbox->regs[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(mbox->regs[i]))
			return dev_err_probe(dev, PTR_ERR(mbox->regs[i]),
					     "failed to map resource %d\n", i);
	}

	/*
	 * Only the host bank's clock and reset belong to Linux. The remote
	 * banks are clocked and reset from their own processors' domains.
	 */
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable clock\n");

	rst = devm_reset_control_get_shared(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get reset\n");

	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert reset\n");

	ret = devm_add_action_or_reset(dev, sun55i_msgbox_assert_reset, rst);
	if (ret)
		return ret;

	/* Disable all read IRQs and clear status */
	for (local_n = 0; local_n < SUN55I_NUM_ROUTES; local_n++) {
		writel(0, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
		writel(U32_MAX, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
	}

	/*
	 * Only the ARM port interrupt belongs to this processor; the others
	 * signal reads pending in the remote processors' own banks.
	 */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, sun55i_msgbox_irq, 0, dev_name(dev), mbox);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	mbox->controller.dev           = dev;
	mbox->controller.ops           = &sun55i_msgbox_chan_ops;
	mbox->controller.chans         = chans;
	mbox->controller.num_chans     = SUN55I_NUM_CHANS;
	mbox->controller.txdone_irq    = false;
	mbox->controller.txdone_poll   = true;
	mbox->controller.txpoll_period = 1;

	ret = devm_mbox_controller_register(dev, &mbox->controller);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register controller\n");

	return 0;
}

static const struct of_device_id sun55i_msgbox_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-msgbox" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun55i_msgbox_of_match);

static struct platform_driver sun55i_msgbox_driver = {
	.driver = {
		.name = "sun55i-msgbox",
		.of_match_table = sun55i_msgbox_of_match,
	},
	.probe = sun55i_msgbox_probe,
};
module_platform_driver(sun55i_msgbox_driver);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("Allwinner sun55i 4-Port Message Box Driver");
MODULE_LICENSE("GPL");
