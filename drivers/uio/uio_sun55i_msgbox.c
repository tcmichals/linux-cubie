// SPDX-License-Identifier: GPL-2.0
/*
 * UIO access to the Allwinner A523/T527 message box (Cubie A5E project only,
 * not part of the upstream remoteproc/mailbox series).
 *
 * The UIO test profile hands the ARM message box bank and a shared SRAM window
 * to userspace. generic-uio cannot do this: it switches on neither the bus
 * clocks nor the resets, so the banks stay gated, register writes are dropped
 * and no doorbell interrupt is ever raised. This driver holds the clocks and
 * resets of every bank listed in the node on for as long as it is bound.
 *
 * The doorbell interrupt is level triggered and stays asserted until userspace
 * has read the FIFO empty, so the handler masks it and userspace unmasks it by
 * writing 1 to /dev/uioN once it has drained the FIFO.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/uio_driver.h>

struct sun55i_msgbox_uio {
	struct uio_info info;
	spinlock_t lock;	/* protects irq_masked */
	bool irq_masked;
};

static irqreturn_t sun55i_msgbox_uio_handler(int irq, struct uio_info *info)
{
	struct sun55i_msgbox_uio *priv = info->priv;

	spin_lock(&priv->lock);
	if (!priv->irq_masked) {
		priv->irq_masked = true;
		disable_irq_nosync(irq);
	}
	spin_unlock(&priv->lock);

	return IRQ_HANDLED;
}

static int sun55i_msgbox_uio_irqcontrol(struct uio_info *info, s32 irq_on)
{
	struct sun55i_msgbox_uio *priv = info->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (irq_on && priv->irq_masked) {
		priv->irq_masked = false;
		enable_irq(info->irq);
	} else if (!irq_on && !priv->irq_masked) {
		priv->irq_masked = true;
		disable_irq_nosync(info->irq);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static void sun55i_msgbox_uio_assert_reset(void *data)
{
	reset_control_assert(data);
}

static int sun55i_msgbox_uio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun55i_msgbox_uio *priv;
	struct clk_bulk_data *clks;
	struct reset_control *rst;
	int i, irq, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	spin_lock_init(&priv->lock);

	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	rst = devm_reset_control_array_get_shared(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get resets\n");
	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert reset\n");
	ret = devm_add_action_or_reset(dev, sun55i_msgbox_uio_assert_reset, rst);
	if (ret)
		return ret;

	for (i = 0; i < MAX_UIO_MAPS; i++) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, i);

		if (!res)
			break;
		if (res->start & ~PAGE_MASK)
			return dev_err_probe(dev, -EINVAL, "region %d is not page aligned\n", i);
		priv->info.mem[i].name = res->name;
		priv->info.mem[i].addr = res->start;
		priv->info.mem[i].size = resource_size(res);
		priv->info.mem[i].memtype = UIO_MEM_PHYS;
	}
	if (i == 0)
		return dev_err_probe(dev, -EINVAL, "no register region\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	priv->info.name = dev_name(dev);
	priv->info.version = "1";
	priv->info.irq = irq;
	priv->info.irq_flags = 0;
	priv->info.handler = sun55i_msgbox_uio_handler;
	priv->info.irqcontrol = sun55i_msgbox_uio_irqcontrol;
	priv->info.priv = priv;

	return devm_uio_register_device(dev, &priv->info);
}

static const struct of_device_id sun55i_msgbox_uio_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-msgbox-uio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_msgbox_uio_of_match);

static struct platform_driver sun55i_msgbox_uio_driver = {
	.probe = sun55i_msgbox_uio_probe,
	.driver = {
		.name = "sun55i-msgbox-uio",
		.of_match_table = sun55i_msgbox_uio_of_match,
	},
};
module_platform_driver(sun55i_msgbox_uio_driver);

MODULE_DESCRIPTION("UIO access to the Allwinner A523 message box (Cubie A5E project)");
MODULE_LICENSE("GPL");
