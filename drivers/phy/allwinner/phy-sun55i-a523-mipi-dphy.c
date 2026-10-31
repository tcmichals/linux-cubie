// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner A523 MIPI CSI-2 receiver D-PHY
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SUN55I_A523_DPHY_TOP_CTL_REG			0x000
#define SUN55I_A523_DPHY_TOP_PWDNZ			BIT(0)
#define SUN55I_A523_DPHY_TOP_RSTN			BIT(1)
#define SUN55I_A523_DPHY_TOP_VREF_EN			BIT(2)
#define SUN55I_A523_DPHY_TOP_LVLDO_EN			BIT(3)
#define SUN55I_A523_DPHY_TOP_VREF_OP2			GENMASK(9, 8)
#define SUN55I_A523_DPHY_TOP_VREF_OP9			GENMASK(11, 10)

#define SUN55I_A523_DPHY_TOP_TRESCAL_REG		0x004
#define SUN55I_A523_DPHY_TOP_TRESCAL_AUTO		BIT(0)
#define SUN55I_A523_DPHY_TOP_TRESCAL_SOFT		BIT(1)
#define SUN55I_A523_DPHY_TOP_TRESCAL_RESETN		BIT(2)
#define SUN55I_A523_DPHY_TOP_TRESCAL_SET		GENMASK(8, 4)

#define SUN55I_A523_DPHY_CTL_REG			0x000
#define SUN55I_A523_DPHY_CTL_EN			BIT(0)
#define SUN55I_A523_DPHY_CTL_LANEDT_EN		GENMASK(7, 4)
#define SUN55I_A523_DPHY_CTL_LANECK_EN		GENMASK(9, 8)
#define SUN55I_A523_DPHY_CTL_LP_PEFI		BIT(12)
#define SUN55I_A523_DPHY_CTL_HS_PEFI		GENMASK(18, 16)
#define SUN55I_A523_DPHY_CTL_IBIAS_EN		BIT(26)
#define SUN55I_A523_DPHY_CTL_WORK_MODE		GENMASK(29, 28)

#define SUN55I_A523_DPHY_OFSCAL0_REG		0x008
#define SUN55I_A523_DPHY_OFSCAL0_AUTO		BIT(4)
#define SUN55I_A523_DPHY_OFSCAL0_SOFT		BIT(12)

#define SUN55I_A523_DPHY_OFSCAL1_REG		0x00c
#define SUN55I_A523_DPHY_OFSCAL1_SET		GENMASK(24, 20)

#define SUN55I_A523_DPHY_DESKEW1_REG		0x018
#define SUN55I_A523_DPHY_DESKEW1_LANECK_SET	GENMASK(24, 20)

#define SUN55I_A523_DPHY_TERM_CTL_REG		0x020
#define SUN55I_A523_DPHY_TERM_CTL_DT_EN		GENMASK(3, 0)
#define SUN55I_A523_DPHY_TERM_CTL_CK_EN		GENMASK(5, 4)
#define SUN55I_A523_DPHY_TERM_CTL_DLY		GENMASK(23, 16)

#define SUN55I_A523_DPHY_HS_CTL_REG		0x024
#define SUN55I_A523_DPHY_HS_CTL_DT_EN		GENMASK(3, 0)
#define SUN55I_A523_DPHY_HS_CTL_CK_EN		GENMASK(5, 4)
#define SUN55I_A523_DPHY_HS_CTL_DLY		GENMASK(23, 16)

#define SUN55I_A523_DPHY_S2P_CTL_REG		0x028
#define SUN55I_A523_DPHY_S2P_CTL_EN		GENMASK(3, 0)
#define SUN55I_A523_DPHY_S2P_CTL_WIDTH		GENMASK(9, 8)
#define SUN55I_A523_DPHY_S2P_CTL_DLY		GENMASK(23, 16)

#define SUN55I_A523_DPHY_MIPIRX_CTL_REG		0x02c
#define SUN55I_A523_DPHY_MIPIRX_SYNC_MODE	BIT(2)
#define SUN55I_A523_DPHY_MIPIRX_LPDT_EN	GENMASK(11, 8)
#define SUN55I_A523_DPHY_MIPIRX_LPCK_EN	GENMASK(13, 12)
#define SUN55I_A523_DPHY_MIPIRX_LP_DBC_EN	BIT(16)

#define SUN55I_A523_DPHY_FREQ_CNT_REG		0x034
#define SUN55I_A523_DPHY_FREQ_CNT_EN		BIT(0)

#define SUN55I_A523_DPHY_MAX_REG			0x0f0

#define SUN55I_A523_DPHY_FIRST_BASE		0x05810100
#define SUN55I_A523_DPHY_STRIDE			0x100
#define SUN55I_A523_DPHY_COUNT			4

/*
 * The vendor driver uses 0x28 when neither a sensor-specific settle time nor
 * an explicit settle-time override is supplied. The relationship between
 * this hardware field and the generic MIPI D-PHY timing values is not
 * documented, so retain the vendor fallback for now.
 */
#define SUN55I_A523_DPHY_S2P_DLY_DEFAULT	0x28

struct sun55i_a523_mipi_dphy {
	struct regmap *regs;
	struct regmap *top_regs;
	struct phy *phy;
	struct phy_configure_opts_mipi_dphy config;
	unsigned int id;
};

static void
sun55i_a523_mipi_dphy_top_power_on(struct sun55i_a523_mipi_dphy *dphy)
{
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_VREF_OP9,
                           FIELD_PREP(SUN55I_A523_DPHY_TOP_VREF_OP9, 0x2));
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_VREF_OP2,
                           FIELD_PREP(SUN55I_A523_DPHY_TOP_VREF_OP2, 0x3));

        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_TRESCAL_REG,
                           SUN55I_A523_DPHY_TOP_TRESCAL_AUTO,
                           SUN55I_A523_DPHY_TOP_TRESCAL_AUTO);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_TRESCAL_REG,
                           SUN55I_A523_DPHY_TOP_TRESCAL_SET,
                           FIELD_PREP(SUN55I_A523_DPHY_TOP_TRESCAL_SET, 0xa));
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_TRESCAL_REG,
                           SUN55I_A523_DPHY_TOP_TRESCAL_SOFT,
                           SUN55I_A523_DPHY_TOP_TRESCAL_SOFT);

        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_RSTN,
                           SUN55I_A523_DPHY_TOP_RSTN);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_PWDNZ,
                           SUN55I_A523_DPHY_TOP_PWDNZ);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_VREF_EN,
                           SUN55I_A523_DPHY_TOP_VREF_EN);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_LVLDO_EN,
                           SUN55I_A523_DPHY_TOP_LVLDO_EN);

        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_TRESCAL_REG,
                           SUN55I_A523_DPHY_TOP_TRESCAL_RESETN, 0);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_TRESCAL_REG,
                           SUN55I_A523_DPHY_TOP_TRESCAL_RESETN,
                           SUN55I_A523_DPHY_TOP_TRESCAL_RESETN);
}

static void
sun55i_a523_mipi_dphy_top_power_off(struct sun55i_a523_mipi_dphy *dphy)
{
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_RSTN, 0);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_PWDNZ, 0);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_VREF_EN, 0);
        regmap_update_bits(dphy->top_regs, SUN55I_A523_DPHY_TOP_CTL_REG,
                           SUN55I_A523_DPHY_TOP_LVLDO_EN, 0);
}

static int sun55i_a523_mipi_dphy_configure(struct phy *phy,
					    union phy_configure_opts *opts)
{
	struct sun55i_a523_mipi_dphy *dphy = phy_get_drvdata(phy);
	struct phy_configure_opts_mipi_dphy *config = &opts->mipi_dphy;
	int ret;

	ret = phy_mipi_dphy_config_validate(config);
	if (ret)
		return ret;

	/*
	 * A single A523 combo PHY handles up to two data lanes. Four-lane
	 * operation combines adjacent PHY instances and requires additional
	 * global link-mode programming.
	 */
	if (!config->lanes || config->lanes > 2)
		return -EINVAL;

	dphy->config = *config;

	return 0;
}

static int sun55i_a523_mipi_dphy_power_on(struct phy *phy)
{
	struct sun55i_a523_mipi_dphy *dphy = phy_get_drvdata(phy);
	unsigned int lanes = dphy->config.lanes;
	u32 lane_mask = GENMASK(lanes - 1, 0);
	u32 mask;
	u32 value;

	sun55i_a523_mipi_dphy_top_power_on(dphy);

	mask = SUN55I_A523_DPHY_CTL_LANEDT_EN |
	       SUN55I_A523_DPHY_CTL_LANECK_EN |
	       SUN55I_A523_DPHY_CTL_LP_PEFI |
	       SUN55I_A523_DPHY_CTL_HS_PEFI |
	       SUN55I_A523_DPHY_CTL_IBIAS_EN |
	       SUN55I_A523_DPHY_CTL_WORK_MODE;

	value = FIELD_PREP(SUN55I_A523_DPHY_CTL_LANEDT_EN, lane_mask) |
		FIELD_PREP(SUN55I_A523_DPHY_CTL_LANECK_EN, 1) |
		SUN55I_A523_DPHY_CTL_LP_PEFI |
		FIELD_PREP(SUN55I_A523_DPHY_CTL_HS_PEFI, 4) |
		SUN55I_A523_DPHY_CTL_IBIAS_EN;

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_CTL_REG, mask, value);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_OFSCAL0_REG,
			   SUN55I_A523_DPHY_OFSCAL0_AUTO |
			   SUN55I_A523_DPHY_OFSCAL0_SOFT,
			   SUN55I_A523_DPHY_OFSCAL0_SOFT);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_OFSCAL1_REG,
			   SUN55I_A523_DPHY_OFSCAL1_SET,
			   FIELD_PREP(SUN55I_A523_DPHY_OFSCAL1_SET, 0x13));

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_TERM_CTL_REG,
			   SUN55I_A523_DPHY_TERM_CTL_DT_EN |
			   SUN55I_A523_DPHY_TERM_CTL_CK_EN |
			   SUN55I_A523_DPHY_TERM_CTL_DLY, 0);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_HS_CTL_REG,
			   SUN55I_A523_DPHY_HS_CTL_DT_EN |
			   SUN55I_A523_DPHY_HS_CTL_CK_EN |
			   SUN55I_A523_DPHY_HS_CTL_DLY, 0);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_S2P_CTL_REG,
			   SUN55I_A523_DPHY_S2P_CTL_EN |
			   SUN55I_A523_DPHY_S2P_CTL_WIDTH |
			   SUN55I_A523_DPHY_S2P_CTL_DLY,
			   FIELD_PREP(SUN55I_A523_DPHY_S2P_CTL_WIDTH, 3) |
			   FIELD_PREP(SUN55I_A523_DPHY_S2P_CTL_DLY,
				      SUN55I_A523_DPHY_S2P_DLY_DEFAULT));

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_MIPIRX_CTL_REG,
			   SUN55I_A523_DPHY_MIPIRX_SYNC_MODE |
			   SUN55I_A523_DPHY_MIPIRX_LPDT_EN |
			   SUN55I_A523_DPHY_MIPIRX_LPCK_EN |
			   SUN55I_A523_DPHY_MIPIRX_LP_DBC_EN,
			   FIELD_PREP(SUN55I_A523_DPHY_MIPIRX_LPDT_EN,
				      lane_mask) |
			   FIELD_PREP(SUN55I_A523_DPHY_MIPIRX_LPCK_EN, 1));

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_CTL_REG,
			   SUN55I_A523_DPHY_CTL_EN,
			   SUN55I_A523_DPHY_CTL_EN);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_FREQ_CNT_REG,
			   SUN55I_A523_DPHY_FREQ_CNT_EN,
			   SUN55I_A523_DPHY_FREQ_CNT_EN);

	/*
	 * The vendor initialization uses 0x2 for PHY0 and 0x7 for the
	 * remaining PHY instances.
	 */
	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_DESKEW1_REG,
			   SUN55I_A523_DPHY_DESKEW1_LANECK_SET,
			   FIELD_PREP(SUN55I_A523_DPHY_DESKEW1_LANECK_SET,
				      dphy->id ? 0x7 : 0x2));

	return 0;
}

static int sun55i_a523_mipi_dphy_power_off(struct phy *phy)
{
	struct sun55i_a523_mipi_dphy *dphy = phy_get_drvdata(phy);

	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_CTL_REG,
			   SUN55I_A523_DPHY_CTL_EN, 0);
	regmap_update_bits(dphy->regs, SUN55I_A523_DPHY_FREQ_CNT_REG,
			   SUN55I_A523_DPHY_FREQ_CNT_EN, 0);

	sun55i_a523_mipi_dphy_top_power_off(dphy);

	return 0;
}

static const struct phy_ops sun55i_a523_mipi_dphy_ops = {
	.configure	= sun55i_a523_mipi_dphy_configure,
	.power_on	= sun55i_a523_mipi_dphy_power_on,
	.power_off	= sun55i_a523_mipi_dphy_power_off,
};

static const struct regmap_config sun55i_a523_mipi_dphy_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= SUN55I_A523_DPHY_MAX_REG,
	.name		= "mipi-dphy",
};

static const struct regmap_config sun55i_a523_mipi_dphy_top_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= SUN55I_A523_DPHY_TOP_TRESCAL_REG,
	.name		= "mipi-dphy-top",
};

static int sun55i_a523_mipi_dphy_probe(struct platform_device *pdev)
{
	struct sun55i_a523_mipi_dphy *dphy;
	struct phy_provider *provider;
	struct resource *res;
	resource_size_t offset;
	void __iomem *base;

	dphy = devm_kzalloc(&pdev->dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	if (res->start < SUN55I_A523_DPHY_FIRST_BASE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid PHY register address\n");

	offset = res->start - SUN55I_A523_DPHY_FIRST_BASE;
	if (offset % SUN55I_A523_DPHY_STRIDE ||
	    offset / SUN55I_A523_DPHY_STRIDE >= SUN55I_A523_DPHY_COUNT)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid PHY register address\n");

	dphy->id = offset / SUN55I_A523_DPHY_STRIDE;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	dphy->regs = devm_regmap_init_mmio(&pdev->dev, base,
					   &sun55i_a523_mipi_dphy_regmap_config);
	if (IS_ERR(dphy->regs))
		return PTR_ERR(dphy->regs);

	base = devm_platform_ioremap_resource_byname(pdev, "combo-top");
	if (IS_ERR(base))
		return PTR_ERR(base);

	dphy->top_regs = devm_regmap_init_mmio(
		&pdev->dev, base, &sun55i_a523_mipi_dphy_top_regmap_config);
	if (IS_ERR(dphy->top_regs))
		return PTR_ERR(dphy->top_regs);

	dphy->phy = devm_phy_create(&pdev->dev, NULL,
				    &sun55i_a523_mipi_dphy_ops);
	if (IS_ERR(dphy->phy))
		return PTR_ERR(dphy->phy);

	phy_set_drvdata(dphy->phy, dphy);

	provider = devm_of_phy_provider_register(&pdev->dev,
						 of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id sun55i_a523_mipi_dphy_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-mipi-dphy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_a523_mipi_dphy_of_match);

static struct platform_driver sun55i_a523_mipi_dphy_driver = {
	.probe = sun55i_a523_mipi_dphy_probe,
	.driver = {
		.name = "sun55i-a523-mipi-dphy",
		.of_match_table = sun55i_a523_mipi_dphy_of_match,
	},
};
module_platform_driver(sun55i_a523_mipi_dphy_driver);

MODULE_AUTHOR("Nick Sl <335926735+nickslkr@users.noreply.github.com>");
MODULE_DESCRIPTION("Allwinner A523 MIPI CSI-2 receiver D-PHY driver");
MODULE_LICENSE("GPL");
