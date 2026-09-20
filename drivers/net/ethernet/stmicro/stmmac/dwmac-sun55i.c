// SPDX-License-Identifier: GPL-2.0-only
/*
 * dwmac-sun55i.c - Allwinner sun55i GMAC200 specific glue layer
 *
 * Copyright (C) 2025 Chen-Yu Tsai <wens@csie.org>
 *
 * syscon parts taken from dwmac-sun8i.c, which is
 *
 * Copyright (C) 2017 Corentin Labbe <clabbe.montjoie@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"
#include "dwmac4_dma.h"

#define SYSCON_REG		0x34

/* RMII specific bits */
#define SYSCON_RMII_EN		BIT(13) /* 1: enable RMII (overrides EPIT) */
/* Generic system control EMAC_CLK bits */
#define SYSCON_ETXDC_MASK		GENMASK(12, 10)
#define SYSCON_ERXDC_MASK		GENMASK(9, 5)
/* EMAC PHY Interface Type */
#define SYSCON_EPIT			BIT(2) /* 1: RGMII, 0: MII */
#define SYSCON_ETCS_MASK		GENMASK(1, 0)
#define SYSCON_ETCS_MII		0x0
#define SYSCON_ETCS_EXT_GMII	0x1
#define SYSCON_ETCS_INT_GMII	0x2

/* GMAC210 Clock Gate Register at syscfg + 0x80 */
#define SUNXI_DWMAC210_CLK_GATE_CFG_REG	0x80
#define SUNXI_DWMAC210_CLK_GATE_ALL	0xF8
#define SUNXI_DWMAC210_CFG_ETXDC_H	GENMASK(17, 16)
#define SUNXI_DWMAC210_CFG_ETXDC_L	GENMASK(12, 10)

struct sun55i_gmac_priv {
	struct platform_device *pdev;
	struct regmap *regmap;
	void __iomem *syscfg_base;
	u32 syscon_val;
	bool is_gmac210;
};

static int sun55i_gmac_init(struct device *dev, void *priv)
{
	struct sun55i_gmac_priv *gmac = priv;

	if (gmac->syscfg_base) {
		writel(gmac->syscon_val, gmac->syscfg_base + 0x00);
		if (gmac->is_gmac210)
			writel(0, gmac->syscfg_base + SUNXI_DWMAC210_CLK_GATE_CFG_REG);
	} else if (gmac->regmap) {
		regmap_write(gmac->regmap, SYSCON_REG, gmac->syscon_val);
	}

	return 0;
}

static void sun55i_gmac_exit(struct device *dev, void *priv)
{
	struct sun55i_gmac_priv *gmac = priv;

	if (gmac->syscfg_base) {
		writel(0, gmac->syscfg_base + 0x00);
		if (gmac->is_gmac210)
			writel(SUNXI_DWMAC210_CLK_GATE_ALL, gmac->syscfg_base + SUNXI_DWMAC210_CLK_GATE_CFG_REG);
	}
}

static int sun55i_gmac200_set_syscon(struct platform_device *pdev,
				     struct plat_stmmacenet_data *plat,
				     struct sun55i_gmac_priv *gmac)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *res;
	u32 val, reg = 0;

	if (of_property_present(node, "syscon")) {
		gmac->regmap = syscon_regmap_lookup_by_phandle(node, "syscon");
		if (IS_ERR(gmac->regmap))
			return dev_err_probe(dev, PTR_ERR(gmac->regmap), "Unable to map syscon\n");
	} else {
		/* A733 / sun60i dedicated per-GMAC syscfg MMIO block */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "syscfg");
		if (!res)
			res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!res)
			return dev_err_probe(dev, -EINVAL, "No syscon or syscfg resource found\n");
		gmac->syscfg_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(gmac->syscfg_base))
			return PTR_ERR(gmac->syscfg_base);
	}

	if (!of_property_read_u32(node, "tx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "tx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set tx-delay to %x\n", val);
		if (gmac->is_gmac210) {
			if (val > 31)
				return dev_err_probe(dev, -EINVAL,
						     "TX clock delay exceeds maximum (%u00ps > 3100ps)\n",
						     val);
			reg |= FIELD_PREP(SUNXI_DWMAC210_CFG_ETXDC_H, val >> 3);
			reg |= FIELD_PREP(SUNXI_DWMAC210_CFG_ETXDC_L, val & 0x7);
		} else {
			if (!FIELD_FIT(SYSCON_ETXDC_MASK, val))
				return dev_err_probe(dev, -EINVAL,
						     "TX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
						     val, FIELD_MAX(SYSCON_ETXDC_MASK));

			reg |= FIELD_PREP(SYSCON_ETXDC_MASK, val);
		}
	}

	if (!of_property_read_u32(node, "rx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "rx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set rx-delay to %x\n", val);
		if (!FIELD_FIT(SYSCON_ERXDC_MASK, val))
			return dev_err_probe(dev, -EINVAL,
					     "RX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
					     val, FIELD_MAX(SYSCON_ERXDC_MASK));

		reg |= FIELD_PREP(SYSCON_ERXDC_MASK, val);
	}

	switch (plat->phy_interface) {
	case PHY_INTERFACE_MODE_MII:
		/* default */
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		reg |= SYSCON_EPIT | SYSCON_ETCS_INT_GMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg |= SYSCON_RMII_EN;
		break;
	default:
		return dev_err_probe(dev, -EINVAL, "Unsupported interface mode: %s",
				     phy_modes(plat->phy_interface));
	}

	gmac->syscon_val = reg;
	return sun55i_gmac_init(dev, gmac);
}

static int sun55i_gmac_fix_soc_reset(struct stmmac_priv *priv)
{
	struct sun55i_gmac_priv *gmac = priv->plat->bsp_priv;
	void __iomem *ioaddr = priv->ioaddr;
	u32 value;

	sun55i_gmac_init(priv->device, gmac);
	usleep_range(1000, 2000);

	/* DMA SW reset */
	value = readl(ioaddr + DMA_BUS_MODE);
	value |= DMA_BUS_MODE_SFT_RESET;
	writel(value, ioaddr + DMA_BUS_MODE);

	return readl_poll_timeout(ioaddr + DMA_BUS_MODE, value,
				 !(value & DMA_BUS_MODE_SFT_RESET),
				 10000, 1000000);
}

static int sun55i_gmac200_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct sun55i_gmac_priv *gmac_priv;
	struct device *dev = &pdev->dev;
	struct clk *clk;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	gmac_priv = devm_kzalloc(dev, sizeof(*gmac_priv), GFP_KERNEL);
	if (!gmac_priv)
		return -ENOMEM;

	gmac_priv->pdev = pdev;
	gmac_priv->is_gmac210 = of_device_is_compatible(dev->of_node, "allwinner,sun60i-a733-gmac210") ||
				of_device_is_compatible(dev->of_node, "allwinner,sunxi-gmac-210");

	/* BSP disables it */
	plat_dat->flags |= STMMAC_FLAG_SPH_DISABLE;
	if (gmac_priv->is_gmac210)
		plat_dat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	plat_dat->host_dma_width = 32;
	plat_dat->clk_csr = 4; /* MDC clock divider: AHB(200M)/102 = ~2MHz */
	plat_dat->bsp_priv = gmac_priv;
	plat_dat->init = sun55i_gmac_init;
	plat_dat->exit = sun55i_gmac_exit;
	plat_dat->fix_soc_reset = sun55i_gmac_fix_soc_reset;

	ret = sun55i_gmac200_set_syscon(pdev, plat_dat, gmac_priv);
	if (ret)
		return ret;

	clk = devm_clk_get_optional_enabled(dev, "mbus");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get or enable MBUS clock\n");

	ret = devm_regulator_get_enable_optional(dev, "phy");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get or enable PHY supply\n");

	return devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct of_device_id sun55i_gmac200_match[] = {
	{ .compatible = "allwinner,sun55i-a523-gmac200" },
	{ .compatible = "allwinner,sun60i-a733-gmac210" },
	{ .compatible = "allwinner,sunxi-gmac-210" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_gmac200_match);

static struct platform_driver sun55i_gmac200_driver = {
	.probe  = sun55i_gmac200_probe,
	.driver = {
		.name           = "dwmac-sun55i",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = sun55i_gmac200_match,
	},
};
module_platform_driver(sun55i_gmac200_driver);

MODULE_AUTHOR("Chen-Yu Tsai <wens@csie.org>");
MODULE_DESCRIPTION("Allwinner sun55i GMAC200 specific glue layer");
MODULE_LICENSE("GPL");
