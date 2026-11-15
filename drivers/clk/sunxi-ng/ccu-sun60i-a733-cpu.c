// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner A733 (sun60i) CPU PLL clock controller driver
 * Based on ccu-sun55i-a523-cpu.c (Copyright (c) 2023-2025 Arm Ltd.)
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ccu_common.h"
#include "ccu_div.h"
#include "ccu_mult.h"
#include "ccu_mux.h"
#include "ccu_nm.h"

#include "ccu-sun60i-a733-cpu.h"

static const struct clk_parent_data dcxo_osc[] = {
	{ .fw_name = "hosc" }
};

#define SUN60I_A733_PLL_CPU_BACK_REG	0x0000
static struct ccu_mult pll_cpu_back_clk = {
	.enable	= BIT(27),
	.lock	= BIT(28),
	.mult	= _SUNXI_CCU_MULT(8, 8),
	.common	= {
		.reg		= SUN60I_A733_PLL_CPU_BACK_REG,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-cpu-back", dcxo_osc,
							   &ccu_mult_ops,
							   CLK_SET_RATE_UNGATE),
	},
};

static const struct clk_parent_data pll_cpu_back_hws[] = {
	{ .hw = &pll_cpu_back_clk.common.hw },
};

#define SUN60I_A733_PLL_CPU_L_REG	0x1000
static struct ccu_nm pll_cpu_l_clk = {
	.enable		= BIT(27),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT_OFFSET_MIN_MAX(8, 8, 0, 12, 108),
	.m		= _SUNXI_CCU_DIV(0, 3),
	.common		= {
		.reg		= SUN60I_A733_PLL_CPU_L_REG,
		.features	= CCU_FEATURE_UPDATE_BIT,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-cpu-l", dcxo_osc,
							   &ccu_nm_ops,
							   CLK_SET_RATE_UNGATE |
							   CLK_IS_CRITICAL),
	},
};

static const struct clk_parent_data pll_cpu_l_hws[] = {
	{ .hw = &pll_cpu_l_clk.common.hw },
};

#define SUN60I_A733_PLL_CPU_B_REG	0x2000
static struct ccu_nm pll_cpu_b_clk = {
	.enable		= BIT(27),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT_OFFSET_MIN_MAX(8, 8, 0, 12, 108),
	.m		= _SUNXI_CCU_DIV(0, 3),
	.common		= {
		.reg		= SUN60I_A733_PLL_CPU_B_REG,
		.features	= CCU_FEATURE_UPDATE_BIT,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-cpu-b", dcxo_osc,
							   &ccu_nm_ops,
							   CLK_SET_RATE_UNGATE |
							   CLK_IS_CRITICAL),
	},
};

static const struct clk_parent_data pll_cpu_b_hws[] = {
	{ .hw = &pll_cpu_b_clk.common.hw },
};

#define SUN60I_A733_PLL_CPU_DSU_REG	0x3000
static struct ccu_nm pll_cpu_dsu_clk = {
	.enable		= BIT(27),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT_OFFSET_MIN_MAX(8, 8, 0, 12, 108),
	.m		= _SUNXI_CCU_DIV(0, 3),
	.common		= {
		.reg		= SUN60I_A733_PLL_CPU_DSU_REG,
		.features	= CCU_FEATURE_UPDATE_BIT,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-cpu-dsu", dcxo_osc,
							   &ccu_nm_ops,
							   CLK_SET_RATE_UNGATE |
							   CLK_IS_CRITICAL),
	},
};

static const struct clk_parent_data pll_cpu_dsu_hws[] = {
	{ .hw = &pll_cpu_dsu_clk.common.hw },
};

#define SUN60I_CCU_P_DATA(_struct, _name, _parents, _reg, _pshift, _pwidth, _flags) \
	SUNXI_CCU_P_DATA_WITH_MUX_GATE(_struct, _name, _parents, _reg,		    \
				       _pshift, _pwidth, 0, 0, 0, _flags)

static SUN60I_CCU_P_DATA(pll_cpu_l_div_clk, "pll-cpu-l-div",
			 pll_cpu_l_hws, 0x101c, 16, 2, CLK_SET_RATE_PARENT);
static SUN60I_CCU_P_DATA(pll_cpu_b_div_clk, "pll-cpu-b-div",
			 pll_cpu_b_hws, 0x201c, 16, 2, CLK_SET_RATE_PARENT);
static SUN60I_CCU_P_DATA(pll_cpu_dsu_div_clk, "pll-cpu-dsu-div",
			 pll_cpu_dsu_hws, 0x301c, 16, 2, CLK_SET_RATE_PARENT);

static const struct clk_parent_data cpu_l_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "losc" },
	{ .fw_name = "iosc" },
	{ .hw = &pll_cpu_l_div_clk.common.hw },
	{ .fw_name = "pll-periph0-600m" },
	{ .hw = &pll_cpu_back_clk.common.hw },
};

static SUNXI_CCU_MUX_DATA(cpu_l_clk, "cpu-l", cpu_l_parents, 0x101c,
			  24, 3, CLK_SET_RATE_PARENT | CLK_IS_CRITICAL);

static const struct clk_parent_data cpu_b_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "losc" },
	{ .fw_name = "iosc" },
	{ .hw = &pll_cpu_b_div_clk.common.hw },
	{ .fw_name = "pll-periph0-600m" },
	{ .hw = &pll_cpu_back_clk.common.hw },
};

static SUNXI_CCU_MUX_DATA(cpu_b_clk, "cpu-b", cpu_b_parents, 0x201c,
			  24, 3, CLK_SET_RATE_PARENT | CLK_IS_CRITICAL);

static const struct clk_parent_data cpu_dsu_parents[] = {
	{ .fw_name = "hosc" },
	{ .fw_name = "losc" },
	{ .fw_name = "iosc" },
	{ .hw = &pll_cpu_dsu_div_clk.common.hw },
	{ .fw_name = "pll-periph0-2x" },
	{ .fw_name = "pll-periph0-600m" },
};

static SUNXI_CCU_MUX_DATA(cpu_dsu_clk, "cpu-dsu", cpu_dsu_parents, 0x301c,
			  24, 3, CLK_SET_RATE_PARENT | CLK_IS_CRITICAL);

static struct ccu_common *sun60i_a733_cpu_ccu_clks[] = {
	&pll_cpu_back_clk.common,
	&pll_cpu_l_clk.common,
	&pll_cpu_b_clk.common,
	&pll_cpu_dsu_clk.common,
	&pll_cpu_l_div_clk.common,
	&pll_cpu_b_div_clk.common,
	&pll_cpu_dsu_div_clk.common,
	&cpu_l_clk.common,
	&cpu_b_clk.common,
	&cpu_dsu_clk.common,
};

static struct clk_hw_onecell_data sun60i_a733_cpu_hw_clks = {
	.hws	= {
		[CLK_PLL_CPU_BACK]	= &pll_cpu_back_clk.common.hw,
		[CLK_PLL_CPU_L]		= &pll_cpu_l_clk.common.hw,
		[CLK_PLL_CPU_DSU]	= &pll_cpu_dsu_clk.common.hw,
		[CLK_PLL_CPU_B]		= &pll_cpu_b_clk.common.hw,
		[CLK_DIV_CPU_L]		= &pll_cpu_l_div_clk.common.hw,
		[CLK_DIV_CPU_DSU]	= &pll_cpu_dsu_div_clk.common.hw,
		[CLK_DIV_CPU_B]		= &pll_cpu_b_div_clk.common.hw,
		[CLK_CPU_L]		= &cpu_l_clk.common.hw,
		[CLK_CPU_DSU]		= &cpu_dsu_clk.common.hw,
		[CLK_CPU_B]		= &cpu_b_clk.common.hw,
	},
	.num	= CLK_NUMBER,
};

static const struct sunxi_ccu_desc sun60i_a733_cpu_ccu_desc = {
	.ccu_clks	= sun60i_a733_cpu_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_a733_cpu_ccu_clks),
	.hw_clks	= &sun60i_a733_cpu_hw_clks,
};

static const u32 pll_regs[] = {
	SUN60I_A733_PLL_CPU_BACK_REG,
	SUN60I_A733_PLL_CPU_L_REG,
	SUN60I_A733_PLL_CPU_B_REG,
	SUN60I_A733_PLL_CPU_DSU_REG,
};

static struct ccu_mux_nb sun60i_a733_cpu_l_nb = {
	.common		= &cpu_l_clk.common,
	.cm		= &cpu_l_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 4, /* pll-periph0-600m */
};

static struct ccu_mux_nb sun60i_a733_cpu_b_nb = {
	.common		= &cpu_b_clk.common,
	.cm		= &cpu_b_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 4, /* pll-periph0-600m */
};

static struct ccu_mux_nb sun60i_a733_cpu_dsu_nb = {
	.common		= &cpu_dsu_clk.common,
	.cm		= &cpu_dsu_clk.mux,
	.delay_us	= 1,
	.bypass_index	= 5, /* pll-periph0-600m */
};

static int sun60i_a733_start_pll(void __iomem *reg)
{
	u32 val = readl(reg);

	/* Leave already-running bootloader PLLs untouched if P/M0 == 1 */
	if ((val & GENMASK(31, 28)) == GENMASK(31, 28)) {
		if (!(val & GENMASK(21, 16)))
			return 0;
		return -EEXIST;
	}

	val |= BIT(31) | BIT(30) | BIT(29) | BIT(26);
	val &= ~GENMASK(21, 16);
	writel(val, reg);

	return readl_relaxed_poll_timeout(reg, val, val & BIT(28), 100, 70000);
}

static int sun60i_a733_cpu_ccu_probe(struct platform_device *pdev)
{
	const struct sunxi_ccu_desc *desc;
	void __iomem *reg;
	int i, ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	for (i = 0; i < ARRAY_SIZE(pll_regs); i++) {
		ret = sun60i_a733_start_pll(reg + pll_regs[i]);
		if (ret)
			return ret;
	}

	ret = devm_sunxi_ccu_probe(&pdev->dev, reg, desc);
	if (ret)
		return ret;

	ccu_mux_notifier_register(pll_cpu_l_clk.common.hw.clk,
				  &sun60i_a733_cpu_l_nb);
	ccu_mux_notifier_register(pll_cpu_b_clk.common.hw.clk,
				  &sun60i_a733_cpu_b_nb);
	ccu_mux_notifier_register(pll_cpu_dsu_clk.common.hw.clk,
				  &sun60i_a733_cpu_dsu_nb);

	return 0;
}

static const struct of_device_id sun60i_a733_cpu_ccu_ids[] = {
	{
		.compatible = "allwinner,sun60i-a733-cpu-ccu",
		.data = &sun60i_a733_cpu_ccu_desc,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_cpu_ccu_ids);

static struct platform_driver sun60i_a733_cpu_ccu_driver = {
	.probe	= sun60i_a733_cpu_ccu_probe,
	.driver	= {
		.name			= "sun60i-a733-cpu-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun60i_a733_cpu_ccu_ids,
	},
};
module_platform_driver(sun60i_a733_cpu_ccu_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_LICENSE("GPL");
