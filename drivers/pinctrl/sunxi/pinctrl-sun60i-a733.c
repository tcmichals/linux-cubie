// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/pinctrl.h>

#include "pinctrl-sunxi.h"

static const u8 a733_nr_bank_pins[SUNXI_PINCTRL_MAX_BANKS] =
	{  0, 15, 17, 24, 16,  7, 15, 20, 17, 28, 24 };

static const unsigned int a733_irq_bank_map[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

static const u8 a733_irq_bank_muxes[SUNXI_PINCTRL_MAX_BANKS] =
	{  0, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };

static struct sunxi_pinctrl_desc a733_pinctrl_data = {
	.irq_banks = ARRAY_SIZE(a733_irq_bank_map),
	.irq_bank_map = a733_irq_bank_map,
	.irq_read_needs_mux = true,
	.io_bias_cfg_variant = BIAS_VOLTAGE_PIO_POW_MODE_SEL,
};

static int a733_pinctrl_probe(struct platform_device *pdev)
{
	return sunxi_pinctrl_dt_table_init(pdev, a733_nr_bank_pins,
					   a733_irq_bank_muxes,
					   &a733_pinctrl_data,
					   SUNXI_PINCTRL_A733_LAYOUT |
					   SUNXI_PINCTRL_ELEVEN_BANKS);
}

static const struct of_device_id a733_pinctrl_match[] = {
	{ .compatible = "allwinner,sun60i-a733-pinctrl", },
	{}
};
MODULE_DEVICE_TABLE(of, a733_pinctrl_match);

static struct platform_driver a733_pinctrl_driver = {
	.probe	= a733_pinctrl_probe,
	.driver	= {
		.name		= "sun60i-a733-pinctrl",
		.of_match_table	= a733_pinctrl_match,
	},
};
builtin_platform_driver(a733_pinctrl_driver);
