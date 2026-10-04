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

static void sun60i_usb2_phy_hw_init(struct sun60i_usb2_phy *priv)
{
	void __iomem *subsys_bgr;
	u32 val;

	/*
	 * Configure SerDes top bridge:
	 * - 0x04: Interconnect mux routing (0x00070000) for DWC3 UTMI+ to USB 2.0 PHY.
	 * - 0x08: Deassert PHY reset and enable ACLK/HCLK (0x00030010).
	 */
	subsys_bgr = ioremap(0x06c00000, 0x10);
	if (subsys_bgr) {
		writel(0x00070000, subsys_bgr + 0x04);
		val = readl(subsys_bgr + 0x08);
		val |= BIT(17) | BIT(16) | BIT(4); /* ACLK_EN, HCLK_EN, USB2P0_PHY_RSTN */
		writel(val, subsys_bgr + 0x08);
		readl(subsys_bgr + 0x08);
		iounmap(subsys_bgr);
	}

	/* Configure 200-ohm resistor calibration in SYSCFG (0x03000000) */
	{
		void __iomem *syscfg = ioremap(0x03000160, 0x10);

		if (syscfg) {
			/* RESCAL_CTRL (0x160): 0x00c83532 matching golden dump */
			writel(0x00c83532, syscfg + 0x00);
			readl(syscfg + 0x00);

			/* RES1_CTRL (0x168): 0x00c80000 matching golden dump */
			writel(0x00c80000, syscfg + 0x08);
			readl(syscfg + 0x08);

			iounmap(syscfg);
		}
	}

	/* Ensure DCAP 24MHz calibration reference clock (0x02003a00) is enabled */
	{
		void __iomem *ccu_dcap = ioremap(0x02003a00, 4);

		if (ccu_dcap) {
			writel(readl(ccu_dcap) | BIT(3), ccu_dcap);
			readl(ccu_dcap);
			iounmap(ccu_dcap);
		}
	}

	/* Clear ISCR register */
	writel(0x00000000, priv->base + PHY_USB2_ISCR);
	readl(priv->base + PHY_USB2_ISCR);

	/*
	 * Clear SIDDQ (bit 3) and bit 9, set OTGDISABLE (bit 10) | VBUSVLDEXT (bit 5) in PHYCTL
	 * using read-modify-write to preserve factory analog calibration trim.
	 */
	val = readl(priv->base + PHY_USB2_PHYCTL);
	val |= BIT(10) | BIT(5);
	val &= ~(BIT(3) | BIT(9));
	writel(val, priv->base + PHY_USB2_PHYCTL);
	readl(priv->base + PHY_USB2_PHYCTL);

	/* Apply analog tuning (squelch threshold, pre-emphasis, DCAP) */
	writel(priv->tune_param, priv->base + PHY_USB2_PHYTUNE);
	readl(priv->base + PHY_USB2_PHYTUNE);

	/* Configure PHY analog bias trim (0x24) matching Debian golden dump */
	writel(0x00000008, priv->base + 0x24);
	readl(priv->base + 0x24);
}

static int sun60i_usb2_phy_init(struct phy *phy)
{
	struct sun60i_usb2_phy *priv = phy_get_drvdata(phy);
	int ret;

	if (priv->vbus) {
		ret = regulator_enable(priv->vbus);
		if (ret)
			return ret;
	}

	sun60i_usb2_phy_hw_init(priv);

	/* Set DWC3 GUSB2PHYCFG0 USBTRDTIM = 9 matching A733 UTMI pipeline latency and golden dump */
	{
		void __iomem *dwc3_phycfg = ioremap(0x06a0c200, 4);

		if (dwc3_phycfg) {
			u32 val = readl(dwc3_phycfg);

			val &= ~GENMASK(13, 10);
			val |= (9 << 10);
			writel(val, dwc3_phycfg);
			readl(dwc3_phycfg);
			iounmap(dwc3_phycfg);
		}
	}

	dev_info(&phy->dev, "A733 USB2 PHY initialized (tune=0x%08x)\n", priv->tune_param);
	return 0;
}

static int sun60i_usb2_phy_exit(struct phy *phy)
{
	struct sun60i_usb2_phy *priv = phy_get_drvdata(phy);
	u32 val;

	val = readl(priv->base + PHY_USB2_PHYCTL);
	val &= ~(BIT(10) | BIT(5));
	val |= BIT(3); /* Assert SIDDQ */
	writel(val, priv->base + PHY_USB2_PHYCTL);
	readl(priv->base + PHY_USB2_PHYCTL);

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
	int ret;

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

	if (of_property_read_u32(dev->of_node, "allwinner,phy-tune-param", &priv->tune_param) &&
	    of_property_read_u32(dev->of_node, "aw,phy_tune_param", &priv->tune_param))
		priv->tune_param = SUN60I_DEFAULT_PHY_TUNE;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	phy = devm_phy_create(dev, NULL, &sun60i_usb2_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return PTR_ERR(provider);

	/* Initialize hardware registers and ungate SerDes bus bridge for DWC3 GSNPSID */
	sun60i_usb2_phy_hw_init(priv);

	dev_info(dev, "Allwinner A733 USB 2.0 PHY probed at %pr (tune=0x%08x)\n",
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
