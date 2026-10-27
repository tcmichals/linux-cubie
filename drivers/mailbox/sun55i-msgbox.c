// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sun55i (A523) Message Box driver
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 * Based on vendor sunxi-msgbox driver by Allwinner Technology Co., Ltd.
 */

#include <kunit/static_stub.h>
#include <kunit/visibility.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/lockdep.h>
#include <linux/mailbox_controller.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "sun55i-msgbox.h"

/*
 * Hardware routing table for the Cortex-A55 host (bank SUN55I_PROC_ARM). The
 * name is the remote bank's entry in "interrupt-names".
 *   local_n = 0 -> CPUS (remote_n = 0) -> channels 0..3
 *   local_n = 1 -> DSP  (remote_n = 0) -> channels 4..7
 *   local_n = 2 -> RV   (remote_n = 2) -> channels 8..11
 */
static const struct sun55i_route sun55i_msgbox_routes[SUN55I_NUM_ROUTES] = {
	[0] = { .remote_id = SUN55I_PROC_CPUS, .remote_n = 0, .name = "cpus" },
	[1] = { .remote_id = SUN55I_PROC_DSP,  .remote_n = 0, .name = "dsp" },
	[2] = { .remote_id = SUN55I_PROC_RV,   .remote_n = 2, .name = "rv" },
};

static inline struct sun55i_msgbox *to_sun55i_msgbox(struct mbox_chan *chan)
{
	return chan->con_priv;
}

/* All register access goes through these, so KUnit can model the hardware */
VISIBLE_IF_KUNIT
u32 sun55i_msgbox_readl(void __iomem *addr)
{
	KUNIT_STATIC_STUB_REDIRECT(sun55i_msgbox_readl, addr);
	return readl(addr);
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_readl);

VISIBLE_IF_KUNIT
void sun55i_msgbox_writel(u32 val, void __iomem *addr)
{
	KUNIT_STATIC_STUB_REDIRECT(sun55i_msgbox_writel, val, addr);
	writel(val, addr);
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_writel);

/* @chan_idx comes from the core, which only passes its own channels */
VISIBLE_IF_KUNIT
void sun55i_msgbox_chan_to_route(int chan_idx, int *local_n, int *p,
				 int *remote_id, int *remote_n)
{
	*local_n = chan_idx / SUN55I_CHANS_PER_PROC;
	*p = chan_idx % SUN55I_CHANS_PER_PROC;
	*remote_id = sun55i_msgbox_routes[*local_n].remote_id;
	*remote_n = sun55i_msgbox_routes[*local_n].remote_n;
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_chan_to_route);

/*
 * Read what is queued for us on one channel (at most FIFO_MAX, so a runaway
 * remote cannot lock up the CPU) into @msgs, then clear its pending bit.
 * Returns the number of messages read.
 */
static int sun55i_msgbox_rx_drain(struct sun55i_msgbox *mbox, int local_n, int p,
				  u32 *msgs)
{
	void __iomem *base = mbox->regs[SUN55I_PROC_ARM];
	int count = 0;

	lockdep_assert_held(&mbox->lock);

	while (count < SUN55I_FIFO_MAX &&
	       (sun55i_msgbox_readl(base + SUN55I_MSGBOX_MSG_STATUS(local_n, p)) &
		SUN55I_MSGBOX_MSG_NUM_MASK))
		msgs[count++] = sun55i_msgbox_readl(base + SUN55I_MSGBOX_MSG_FIFO(local_n, p));

	/*
	 * The pending bit follows "FIFO not empty": the hardware ignores the
	 * clear while a message is still waiting. Clearing it before the drain
	 * left it set and raised a second interrupt for every message. A
	 * message that arrives after this clear sets it again.
	 */
	sun55i_msgbox_writel(SUN55I_MSGBOX_RD_IRQ_PEND_BIT(p),
			     base + SUN55I_MSGBOX_READ_IRQ_STATUS(local_n));

	return count;
}

VISIBLE_IF_KUNIT
irqreturn_t sun55i_msgbox_irq(int irq, void *dev_id)
{
	struct sun55i_msgbox *mbox = dev_id;
	void __iomem *base = mbox->regs[SUN55I_PROC_ARM];
	u32 msgs[SUN55I_CHANS_PER_PROC][SUN55I_FIFO_MAX];
	int count[SUN55I_CHANS_PER_PROC];
	irqreturn_t ret = IRQ_NONE;
	int local_n, p, i;

	for (local_n = 0; local_n < SUN55I_NUM_ROUTES; local_n++) {
		struct mbox_chan *chans =
			&mbox->controller.chans[local_n * SUN55I_CHANS_PER_PROC];
		unsigned long flags;
		u32 pending;

		spin_lock_irqsave(&mbox->lock, flags);
		pending = sun55i_msgbox_readl(base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n)) &
			  sun55i_msgbox_readl(base + SUN55I_MSGBOX_READ_IRQ_STATUS(local_n));
		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++)
			count[p] = pending & SUN55I_MSGBOX_RD_IRQ_PEND_BIT(p) ?
				   sun55i_msgbox_rx_drain(mbox, local_n, p, msgs[p]) : -1;
		spin_unlock_irqrestore(&mbox->lock, flags);

		/*
		 * Outside the lock: a client may send from its rx callback,
		 * and send_data() takes the lock. shutdown() waits for this
		 * handler before the channel goes away.
		 */
		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			if (count[p] < 0)
				continue;
			for (i = 0; i < count[p]; i++)
				mbox_chan_received_data(&chans[p], &msgs[p][i]);
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_irq);

static u32 sun55i_msgbox_tx_count(struct sun55i_msgbox *mbox, int remote_id,
				  int remote_n, int p)
{
	return sun55i_msgbox_readl(mbox->regs[remote_id] +
				   SUN55I_MSGBOX_MSG_STATUS(remote_n, p)) &
	       SUN55I_MSGBOX_MSG_NUM_MASK;
}

/*
 * The write IRQ of a channel lives in the receiver's bank and is raised while
 * at least one FIFO slot is free (level). In write IRQ mode it is enabled
 * after every send: it fires at once if the FIFO still has room, or when the
 * receiver reads a message, and the handler disables it again.
 *
 * The status bit is latched: it stays set after the FIFO fills up again until
 * it is cleared, and the clear only takes while no slot is free. Clear it
 * before enabling, so a stale bit does not fire at once; if a slot has been
 * freed meanwhile the clear is ignored and the IRQ fires, as it should.
 */
static void sun55i_msgbox_tx_irq_enable(struct sun55i_msgbox *mbox, int remote_id,
					int remote_n, int p, bool enable)
{
	void __iomem *base = mbox->regs[remote_id];
	void __iomem *reg = base + SUN55I_MSGBOX_WRITE_IRQ_ENABLE(remote_n);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&mbox->lock, flags);
	val = sun55i_msgbox_readl(reg);
	if (enable) {
		sun55i_msgbox_writel(SUN55I_MSGBOX_WR_IRQ_PEND_BIT(p),
				     base + SUN55I_MSGBOX_WRITE_IRQ_STATUS(remote_n));
		val |= SUN55I_MSGBOX_WR_IRQ_EN_BIT(p);
	} else {
		val &= ~SUN55I_MSGBOX_WR_IRQ_EN_BIT(p);
	}
	sun55i_msgbox_writel(val, reg);
	spin_unlock_irqrestore(&mbox->lock, flags);
}

VISIBLE_IF_KUNIT
irqreturn_t sun55i_msgbox_tx_irq(int irq, void *dev_id)
{
	struct sun55i_tx_irq *tx = dev_id;
	struct sun55i_msgbox *mbox = tx->mbox;
	const struct sun55i_route *route = &sun55i_msgbox_routes[tx->local_n];
	void __iomem *base = mbox->regs[route->remote_id];
	struct mbox_chan *chans = &mbox->controller.chans[tx->local_n * SUN55I_CHANS_PER_PROC];
	unsigned long flags, done = 0;
	u32 en, pend;
	int p;

	spin_lock_irqsave(&mbox->lock, flags);

	en = sun55i_msgbox_readl(base + SUN55I_MSGBOX_WRITE_IRQ_ENABLE(route->remote_n));
	pend = sun55i_msgbox_readl(base + SUN55I_MSGBOX_WRITE_IRQ_STATUS(route->remote_n));

	for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
		if (!(en & pend & SUN55I_MSGBOX_WR_IRQ_PEND_BIT(p)))
			continue;

		/* Level: mask it before acking, a slot is free now */
		en &= ~SUN55I_MSGBOX_WR_IRQ_EN_BIT(p);
		sun55i_msgbox_writel(en, base + SUN55I_MSGBOX_WRITE_IRQ_ENABLE(route->remote_n));
		sun55i_msgbox_writel(SUN55I_MSGBOX_WR_IRQ_PEND_BIT(p),
				     base + SUN55I_MSGBOX_WRITE_IRQ_STATUS(route->remote_n));
		__set_bit(p, &done);
	}

	spin_unlock_irqrestore(&mbox->lock, flags);

	/* Outside the lock: txdone submits the next message (send_data) */
	for_each_set_bit(p, &done, SUN55I_CHANS_PER_PROC)
		mbox_chan_txdone(&chans[p], 0);

	return done ? IRQ_HANDLED : IRQ_NONE;
}
EXPORT_SYMBOL_IF_KUNIT(sun55i_msgbox_tx_irq);

/*
 * A route's TX-done line is requested while at least one channel on it is
 * started, so lines of banks nobody talks to stay free.
 */
static int sun55i_msgbox_tx_irq_get(struct sun55i_msgbox *mbox, int local_n)
{
	struct sun55i_tx_irq *tx = &mbox->tx_irqs[local_n];
	int ret = 0;

	mutex_lock(&mbox->tx_irq_lock);
	if (!tx->users) {
		ret = request_irq(tx->irq, sun55i_msgbox_tx_irq, 0, tx->name, tx);
		if (ret)
			dev_err(mbox->controller.dev, "failed to request %s: %d\n",
				tx->name, ret);
	}
	if (!ret)
		tx->users++;
	mutex_unlock(&mbox->tx_irq_lock);

	return ret;
}

static void sun55i_msgbox_tx_irq_put(struct sun55i_msgbox *mbox, int local_n)
{
	struct sun55i_tx_irq *tx = &mbox->tx_irqs[local_n];

	mutex_lock(&mbox->tx_irq_lock);
	if (!--tx->users) {
		free_irq(tx->irq, tx);
	} else {
		/* The line stays in use: let a running handler finish with us */
		synchronize_irq(tx->irq);
	}
	mutex_unlock(&mbox->tx_irq_lock);
}

static int sun55i_msgbox_send_data(struct mbox_chan *chan, void *data)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	unsigned long flags;
	u32 msg = 0;
	bool full;

	if (data)
		memcpy(&msg, data, sizeof(msg));

	sun55i_msgbox_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/*
	 * Under the lock: in timer mode the poll timer may write a held
	 * message into the same FIFO from another CPU (last_tx_done()).
	 */
	spin_lock_irqsave(&mbox->lock, flags);

	/*
	 * The core calls send_data() only with no request active, so a message
	 * still held here is one it gave up on (a blocking send timed out):
	 * drop it rather than send it after this newer one.
	 */
	__clear_bit(n, &mbox->tx_held);

	/*
	 * Writing a full FIFO corrupts its count, so never do it. In write IRQ
	 * mode the IRQ armed below retries the message once a slot is free. In
	 * timer mode the core polls only a channel whose send succeeded, so
	 * hold the message and let last_tx_done() write it.
	 */
	full = sun55i_msgbox_tx_count(mbox, remote_id, remote_n, p) >= SUN55I_FIFO_MAX;
	if (!full) {
		sun55i_msgbox_writel(msg, mbox->regs[remote_id] +
				     SUN55I_MSGBOX_MSG_FIFO(remote_n, p));
	} else if (!mbox->tx_done_irq) {
		mbox->tx_held_msg[n] = msg;
		__set_bit(n, &mbox->tx_held);
	}

	spin_unlock_irqrestore(&mbox->lock, flags);

	/*
	 * A kick is a doorbell, the data is in shared memory: it is done once
	 * the FIFO accepted it and has room for the next one. The write IRQ
	 * reports exactly that, at once or when the receiver reads a message.
	 */
	if (mbox->tx_done_irq) {
		sun55i_msgbox_tx_irq_enable(mbox, remote_id, remote_n, p, true);
		return full ? -EBUSY : 0;
	}

	return 0;
}

static int sun55i_msgbox_startup(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	void __iomem *base = mbox->regs[SUN55I_PROC_ARM];
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 stale[SUN55I_FIFO_MAX];
	unsigned long flags;
	u32 val;

	sun55i_msgbox_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/*
	 * Get the TX-done line before the receive IRQ is enabled: on failure the
	 * core clears chan->cl, which a receive handler must not be using.
	 */
	if (mbox->tx_done_irq) {
		int ret;

		/* TX done: the write IRQ, raised while one slot is free */
		sun55i_msgbox_writel(SUN55I_MSGBOX_WR_IRQ_THR_ONE_FREE, mbox->regs[remote_id] +
				     SUN55I_MSGBOX_WRITE_IRQ_THRESHOLD(remote_n, p));

		ret = sun55i_msgbox_tx_irq_get(mbox, local_n);
		if (ret)
			return ret;
		set_bit(n, &mbox->tx_irq_held);
	}

	/* Drop stale receive data, then enable the receive IRQ */
	spin_lock_irqsave(&mbox->lock, flags);
	sun55i_msgbox_rx_drain(mbox, local_n, p, stale);
	val = sun55i_msgbox_readl(base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n));
	val |= SUN55I_MSGBOX_RD_IRQ_EN_BIT(p);
	sun55i_msgbox_writel(val, base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n));
	spin_unlock_irqrestore(&mbox->lock, flags);

	return 0;
}

static void sun55i_msgbox_shutdown(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	void __iomem *base = mbox->regs[SUN55I_PROC_ARM];
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 stale[SUN55I_FIFO_MAX];
	unsigned long flags;
	u32 val;

	sun55i_msgbox_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/* Disable the receive IRQ; a message held for a full FIFO is dropped */
	spin_lock_irqsave(&mbox->lock, flags);
	val = sun55i_msgbox_readl(base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n));
	val &= ~SUN55I_MSGBOX_RD_IRQ_EN_BIT(p);
	sun55i_msgbox_writel(val, base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n));
	__clear_bit(n, &mbox->tx_held);
	spin_unlock_irqrestore(&mbox->lock, flags);

	/* The handler delivers outside the lock: let it finish with this channel */
	synchronize_irq(mbox->irq);

	/* Release the line only if this channel's startup() got it */
	if (test_and_clear_bit(n, &mbox->tx_irq_held)) {
		sun55i_msgbox_tx_irq_enable(mbox, remote_id, remote_n, p, false);
		sun55i_msgbox_tx_irq_put(mbox, local_n);
		/*
		 * A TX-done handler that was already running submits the next
		 * message through send_data(), which arms the IRQ again. It has
		 * finished now (free_irq() or synchronize_irq() above), so
		 * disarm once more.
		 */
		sun55i_msgbox_tx_irq_enable(mbox, remote_id, remote_n, p, false);
	}

	/* Drop what arrived meanwhile */
	spin_lock_irqsave(&mbox->lock, flags);
	sun55i_msgbox_rx_drain(mbox, local_n, p, stale);
	spin_unlock_irqrestore(&mbox->lock, flags);
}

static bool sun55i_msgbox_last_tx_done(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	unsigned long flags;
	bool done;

	sun55i_msgbox_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/*
	 * Timer mode only. A kick is a doorbell, the data is in shared memory:
	 * it is done once the receiver's FIFO accepted it and has room for the
	 * next one; a full FIFO waits for the next poll.
	 */
	spin_lock_irqsave(&mbox->lock, flags);
	done = sun55i_msgbox_tx_count(mbox, remote_id, remote_n, p) < SUN55I_FIFO_MAX;

	/* send_data() found the FIFO full and held the message: write it now */
	if (done && __test_and_clear_bit(n, &mbox->tx_held)) {
		sun55i_msgbox_writel(mbox->tx_held_msg[n], mbox->regs[remote_id] +
				     SUN55I_MSGBOX_MSG_FIFO(remote_n, p));
		done = sun55i_msgbox_tx_count(mbox, remote_id, remote_n, p) < SUN55I_FIFO_MAX;
	}
	spin_unlock_irqrestore(&mbox->lock, flags);

	return done;
}

static bool sun55i_msgbox_peek_data(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;

	sun55i_msgbox_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	return sun55i_msgbox_readl(mbox->regs[SUN55I_PROC_ARM] +
				   SUN55I_MSGBOX_MSG_STATUS(local_n, p)) &
	       SUN55I_MSGBOX_MSG_NUM_MASK;
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

static int sun55i_msgbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clks;
	struct reset_control *rst;
	struct mbox_chan *chans;
	struct sun55i_msgbox *mbox;
	int i, ret, irq, local_n;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	spin_lock_init(&mbox->lock);
	ret = devm_mutex_init(dev, &mbox->tx_irq_lock);
	if (ret)
		return ret;

	chans = devm_kcalloc(dev, SUN55I_NUM_CHANS, sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	for (i = 0; i < SUN55I_NUM_CHANS; i++)
		chans[i].con_priv = mbox;

	/* The interrupt handler walks the channels: set them up before it */
	mbox->controller.dev = dev;
	mbox->controller.ops = &sun55i_msgbox_chan_ops;
	mbox->controller.chans = chans;
	mbox->controller.num_chans = SUN55I_NUM_CHANS;

	for (i = 0; i < SUN55I_MAX_PROCESSORS; i++) {
		mbox->regs[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(mbox->regs[i]))
			return dev_err_probe(dev, PTR_ERR(mbox->regs[i]),
					     "failed to map resource %d\n", i);
	}

	/*
	 * Every bank has its own bus clock and reset. The host writes messages
	 * into the remote banks and reads their FIFO status, so all of them
	 * must be clocked and out of reset; an unclocked bank drops writes and
	 * reads back as zero. A DT missing one would fail silently, so check.
	 */
	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");
	if (ret != SUN55I_MAX_PROCESSORS)
		return dev_err_probe(dev, -EINVAL, "need %d clocks, found %d\n",
				     SUN55I_MAX_PROCESSORS, ret);

	ret = reset_control_get_count(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to count resets\n");
	if (ret != SUN55I_MAX_PROCESSORS)
		return dev_err_probe(dev, -EINVAL, "need %d resets, found %d\n",
				     SUN55I_MAX_PROCESSORS, ret);

	rst = devm_reset_control_array_get_shared(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get resets\n");

	/*
	 * The DSP, CPUS and RISC-V banks also carry traffic between the other
	 * processors, which may already be running. Asserting the reset would
	 * wipe their FIFOs, so it is never asserted, on unbind or on a probe
	 * failure, as in sun6i-msgbox.
	 */
	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert reset\n");

	/*
	 * Disable all read IRQs and clear status, and disarm our write IRQs in
	 * the remote banks, which an earlier kernel or bind may have left on.
	 */
	for (local_n = 0; local_n < SUN55I_NUM_ROUTES; local_n++) {
		const struct sun55i_route *route = &sun55i_msgbox_routes[local_n];
		void __iomem *base = mbox->regs[SUN55I_PROC_ARM];
		void __iomem *wr_en = mbox->regs[route->remote_id] +
				      SUN55I_MSGBOX_WRITE_IRQ_ENABLE(route->remote_n);
		u32 val = sun55i_msgbox_readl(wr_en);
		int p;

		sun55i_msgbox_writel(0, base + SUN55I_MSGBOX_READ_IRQ_ENABLE(local_n));
		sun55i_msgbox_writel(U32_MAX, base + SUN55I_MSGBOX_READ_IRQ_STATUS(local_n));

		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++)
			val &= ~SUN55I_MSGBOX_WR_IRQ_EN_BIT(p);
		sun55i_msgbox_writel(val, wr_en);
	}

	/* The ARM bank interrupt signals messages received by this processor */
	irq = platform_get_irq_byname(pdev, "arm");
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, sun55i_msgbox_irq, 0, dev_name(dev), mbox);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");
	mbox->irq = irq;

	/*
	 * The write IRQ of a channel is raised in the receiver's bank, on that
	 * bank's own line to this processor. Look the lines up now so a bad DT
	 * fails the probe; each is requested only while a channel on its route
	 * is started.
	 */
	mbox->tx_done_irq = of_property_read_bool(dev->of_node, "allwinner,tx-done-irq");
	for (local_n = 0; mbox->tx_done_irq && local_n < SUN55I_NUM_ROUTES; local_n++) {
		const struct sun55i_route *route = &sun55i_msgbox_routes[local_n];
		struct sun55i_tx_irq *tx = &mbox->tx_irqs[local_n];

		tx->mbox = mbox;
		tx->local_n = local_n;
		tx->irq = platform_get_irq_byname(pdev, route->name);
		if (tx->irq < 0)
			return tx->irq;

		tx->name = devm_kasprintf(dev, GFP_KERNEL, "%s-%s", dev_name(dev), route->name);
		if (!tx->name)
			return -ENOMEM;
	}

	/* TX done by the write IRQ ("allwinner,tx-done-irq") or by a 1 ms poll */
	if (mbox->tx_done_irq) {
		mbox->controller.txdone_irq = true;
	} else {
		mbox->controller.txdone_poll = true;
		mbox->controller.txpoll_period = 1;
	}

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
MODULE_DESCRIPTION("Allwinner sun55i Message Box driver");
MODULE_LICENSE("GPL");
