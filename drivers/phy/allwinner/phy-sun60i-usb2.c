// SPDX-License-Identifier: GPL-2.0+
/*
 * Allwinner A733 (sun60iw2) USB 2.0 PHY driver for DWC3
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#define SUN60I_DEFAULT_PHY_TUNE	0x143338d6

struct sun60i_usb2_phy {
	void __iomem *base;
	struct regulator *vbus;
	u32 tune_param;
};

#define PHY_USB2_ISCR		0x00
#define PHY_USB2_PHYCTL		0x10
#define PHY_USB2_PHYTUNE	0x18
#define SERDES_TOP_SUBSYS_BGR	0x06c00008

#define PHYCTL_VBUSVLDEXT	BIT(5)
#define PHYCTL_SIDDQ		BIT(3)
#define PHYCTL_COMMONONN	BIT(2)
#define PHYCTL_RESET		BIT(0)

static void sun60i_usb2_phy_hw_init(struct sun60i_usb2_phy *priv)
{
	void __iomem *subsys_bgr;
	void __iomem *syscfg;
	u32 val;

	/* 1. SerDes Top Bridge: ACLK/HCLK clock and reset deassertion */
	subsys_bgr = ioremap(SERDES_TOP_SUBSYS_BGR, 4);
	if (subsys_bgr) {
		val = readl(subsys_bgr);
		val |= BIT(17) | BIT(16) | BIT(4); /* ACLK_EN, HCLK_EN, USB2P0_PHY_RSTN */
		val &= ~BIT(21); /* Clear USB3P1_ONLY_UTMI_CLK_SEL: preserve 60MHz internal PLL */
		writel(val, subsys_bgr);
		iounmap(subsys_bgr);
	}

	/* 2. SYSCFG: Resistor Auto-Calibration (200-ohm target) */
	syscfg = ioremap(0x03000160, 0x10);
	if (syscfg) {
		/* Set target trim to 0xc8 (200 ohm decimal) */
		val = readl(syscfg + 0x08);
		val &= ~GENMASK(15, 8);
		val |= (0xc8 << 8);
		writel(val, syscfg + 0x08);

		/* Enable PCIE_USB 200 ohm trim and trigger auto-calibration */
		val = readl(syscfg + 0x00);
		val |= BIT(10) | BIT(0);
		writel(val, syscfg + 0x00);
		iounmap(syscfg);
	}

	/* Wait for analog bias and calibration currents to stabilize */
	usleep_range(200, 500);

	/* 3. ISCR: Force VBUS valid and ID low to lock Host mode */
	writel(0x0000b000, priv->base + PHY_USB2_ISCR);

	/* 4. Apply Analog Tuning Parameters before releasing reset */
	writel(priv->tune_param, priv->base + PHY_USB2_PHYTUNE);

	/* 5. PHYCTL: Assert analog macro reset, power on transceiver, clear SIDDQ */
	val = readl(priv->base + PHY_USB2_PHYCTL);
	val |= 0x000e2434;
	val &= ~PHYCTL_SIDDQ;
	val |= PHYCTL_RESET;
	writel(val, priv->base + PHY_USB2_PHYCTL);

	udelay(20);

	/* Deassert macro reset */
	val &= ~PHYCTL_RESET;
	writel(val, priv->base + PHY_USB2_PHYCTL);

	/* Allow 60 MHz UTMI clock and PLL lock */
	usleep_range(1500, 2000);
}

static int sun60i_usb2_phy_init(struct phy *phy)
{
	struct sun60i_usb2_phy *priv = phy_get_drvdata(phy);
	int ret;

	if (priv->vbus) {
		ret = regulator_enable(priv->vbus);
		if (ret)
			return ret;
		/* Let 20uF downstream rail charge and FE1.1S RC delay clear */
		msleep(50);
	}

	sun60i_usb2_phy_hw_init(priv);
	return 0;
}

static int sun60i_usb2_phy_exit(struct phy *phy)
{
	struct sun60i_usb2_phy *priv = phy_get_drvdata(phy);
	u32 val;

	val = readl(priv->base + PHY_USB2_PHYCTL);
	val &= ~(BIT(10) | BIT(5));
	val |= PHYCTL_SIDDQ;
	writel(val, priv->base + PHY_USB2_PHYCTL);

	if (priv->vbus)
		regulator_disable(priv->vbus);

	return 0;
}

static const struct phy_ops sun60i_usb2_phy_ops = {
	.init		= sun60i_usb2_phy_init,
	.exit		= sun60i_usb2_phy_exit,
	.owner		= THIS_MODULE,
};

static int sun60i_usb2_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun60i_usb2_phy *priv;
	struct phy_provider *provider;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(priv->vbus)) {
		if (PTR_ERR(priv->vbus) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		priv->vbus = NULL;
	}

	if (of_property_read_u32(dev->of_node, "aw,phy_tune_param", &priv->tune_param))
		priv->tune_param = SUN60I_DEFAULT_PHY_TUNE;

	pm_runtime_enable(dev);

	phy = devm_phy_create(dev, NULL, &sun60i_usb2_phy_ops);
	if (IS_ERR(phy)) {
		pm_runtime_disable(dev);
		return PTR_ERR(phy);
	}

	phy_set_drvdata(phy, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider)) {
		pm_runtime_disable(dev);
		return PTR_ERR(provider);
	}

	/* Initialize hardware registers and ungate SerDes bus bridge */
	sun60i_usb2_phy_hw_init(priv);

	dev_info(dev, "Allwinner A733 USB 2.0 PHY registered at %pr (tune=0x%08x)\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, 0), priv->tune_param);

	return 0;
}

static const struct of_device_id sun60i_usb2_phy_of_match[] = {
	{ .compatible = "allwinner,sun60i-a733-usb2-phy" },
	{ .compatible = "allwinner,sunxi-plat-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_usb2_phy_of_match);

static struct platform_driver sun60i_usb2_phy_driver = {
	.probe	= sun60i_usb2_phy_probe,
	.driver	= {
		.name		= "sun60i-a733-usb2-phy",
		.of_match_table	= sun60i_usb2_phy_of_match,
	},
};
module_platform_driver(sun60i_usb2_phy_driver);

MODULE_DESCRIPTION("Allwinner A733 USB 2.0 PHY driver");
MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_LICENSE("GPL");
