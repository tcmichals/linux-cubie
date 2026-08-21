// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/sun60i-a733-r-ccu.h>
#include <dt-bindings/reset/sun60i-a733-r-ccu.h>

#include "ccu_common.h"
#include "ccu_reset.h"
#include "ccu_gate.h"
#include "ccu_div.h"
#include "ccu_mux.h"

static const struct clk_parent_data r_ahb_parents[] = {
	{ .fw_name = "hosc", .name = "osc24M" },
	{ .fw_name = "losc", .name = "osc32k" },
};
static SUNXI_CCU_M_DATA_WITH_MUX(r_ahb_clk, "r-ahb", r_ahb_parents,
				 0x0000, 0, 5, 24, 2, CLK_IS_CRITICAL);

static const struct clk_hw *r_ahb_hws[] = {
	&r_ahb_clk.common.hw
};
static SUNXI_CCU_M_HWS(r_apbs0_clk, "r-apbs0", r_ahb_hws, 0x000c, 0, 5, CLK_IS_CRITICAL);
static SUNXI_CCU_M_HWS(r_apbs1_clk, "r-apbs1", r_ahb_hws, 0x0010, 0, 5, CLK_IS_CRITICAL);

static const struct clk_hw *r_apbs0_hws[] = {
	&r_apbs0_clk.common.hw
};
static const struct clk_hw *r_apbs1_hws[] = {
	&r_apbs1_clk.common.hw
};

static SUNXI_CCU_GATE_HWS(r_bus_uart0_clk, "r-bus-uart0", r_apbs1_hws, 0x018c, BIT(0), CLK_IS_CRITICAL);
static SUNXI_CCU_GATE_HWS(r_bus_uart1_clk, "r-bus-uart1", r_apbs1_hws, 0x018c, BIT(1), 0);
static SUNXI_CCU_GATE_HWS(r_bus_twi0_clk,  "r-bus-twi0",  r_apbs1_hws, 0x019c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(r_bus_twi1_clk,  "r-bus-twi1",  r_apbs1_hws, 0x019c, BIT(1), 0);
static SUNXI_CCU_GATE_HWS(r_bus_twi2_clk,  "r-bus-twi2",  r_apbs1_hws, 0x019c, BIT(2), 0);
static SUNXI_CCU_GATE_HWS(r_bus_ppu_clk,   "r-bus-ppu",   r_apbs0_hws, 0x01ac, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(r_bus_rtc_clk,   "r-bus-rtc",   r_apbs0_hws, 0x020c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(r_bus_cpucfg_clk,"r-bus-cpucfg",r_apbs0_hws, 0x022c, BIT(0), CLK_IS_CRITICAL);

/* XuanTie E907 RISC-V co-processor clocks */
static const struct clk_parent_data riscv_24m_parents[] = {
	{ .fw_name = "hosc", .name = "osc24M" },
	{ .fw_name = "losc", .name = "osc32k" },
};
static SUNXI_CCU_MUX_DATA_WITH_GATE(riscv_24m_clk, "riscv-24m",
				    riscv_24m_parents, 0x0210,
				    24, 2, BIT(31), 0);
static SUNXI_CCU_GATE_HWS(riscv_cfg_clk, "riscv-cfg", r_ahb_hws, 0x021c, BIT(1), 0);
static SUNXI_CCU_GATE_HWS(riscv_clk,     "riscv",     r_ahb_hws, 0x021c, BIT(0), 0);

static struct ccu_common *sun60i_a733_r_ccu_clks[] = {
	&r_ahb_clk.common,
	&r_apbs0_clk.common,
	&r_apbs1_clk.common,
	&r_bus_uart0_clk.common,
	&r_bus_uart1_clk.common,
	&r_bus_twi0_clk.common,
	&r_bus_twi1_clk.common,
	&r_bus_twi2_clk.common,
	&r_bus_ppu_clk.common,
	&r_bus_rtc_clk.common,
	&r_bus_cpucfg_clk.common,
	&riscv_24m_clk.common,
	&riscv_cfg_clk.common,
	&riscv_clk.common,
};

#define CLK_R_NUMBER (CLK_R_CPUCFG + 1)
static struct {
	unsigned int num;
	struct clk_hw *hws[CLK_R_NUMBER];
} sun60i_a733_r_hw_clks = {
	.num = CLK_R_NUMBER,
	.hws = {
		[CLK_R_AHB]		= &r_ahb_clk.common.hw,
		[CLK_R_APBS0]		= &r_apbs0_clk.common.hw,
		[CLK_R_APBS1]		= &r_apbs1_clk.common.hw,
		[CLK_R_UART0]		= &r_bus_uart0_clk.common.hw,
		[CLK_R_UART1]		= &r_bus_uart1_clk.common.hw,
		[CLK_R_TWI0]		= &r_bus_twi0_clk.common.hw,
		[CLK_R_TWI1]		= &r_bus_twi1_clk.common.hw,
		[CLK_R_TWI2]		= &r_bus_twi2_clk.common.hw,
		[CLK_R_PPU]		= &r_bus_ppu_clk.common.hw,
		[CLK_RTC]		= &r_bus_rtc_clk.common.hw,
		[CLK_R_CPUCFG]		= &r_bus_cpucfg_clk.common.hw,
		[CLK_RISCV_24M]		= &riscv_24m_clk.common.hw,
		[CLK_RISCV_CFG]		= &riscv_cfg_clk.common.hw,
		[CLK_RISCV]		= &riscv_clk.common.hw,
	},
};
static const struct ccu_reset_map sun60i_a733_r_ccu_resets[] = {
	[RST_BUS_R_TIMER]	= { 0x011c, BIT(16) },
	[RST_BUS_R_PWM]		= { 0x013c, BIT(16) },
	[RST_BUS_R_SPI]		= { 0x015c, BIT(16) },
	[RST_BUS_R_MBOX]	= { 0x017c, BIT(16) },
	[RST_BUS_R_UART0]	= { 0x018c, BIT(16) },
	[RST_BUS_R_UART1]	= { 0x018c, BIT(17) },
	[RST_BUS_R_TWI0]	= { 0x019c, BIT(16) },
	[RST_BUS_R_TWI1]	= { 0x019c, BIT(17) },
	[RST_BUS_R_TWI2]	= { 0x019c, BIT(18) },
	[RST_BUS_R_PPU]		= { 0x01ac, BIT(16) },
	[RST_BUS_R_IRRX]	= { 0x01cc, BIT(16) },
	[RST_BUS_RTC]		= { 0x020c, BIT(16) },
	[RST_BUS_RISCV_CFG]	= { 0x021c, BIT(16) },
	[RST_BUS_R_CPUCFG]	= { 0x022c, BIT(16) },
	[RST_BUS_MODULE]	= { 0x0260, BIT(0) },
};

static const struct sunxi_ccu_desc sun60i_a733_r_ccu_desc = {
	.ccu_clks	= sun60i_a733_r_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_a733_r_ccu_clks),
	.hw_clks	= (struct clk_hw_onecell_data *)&sun60i_a733_r_hw_clks,
	.resets		= sun60i_a733_r_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun60i_a733_r_ccu_resets),
};

static int sun60i_a733_r_ccu_probe(struct platform_device *pdev)
{
	void __iomem *reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);
	return devm_sunxi_ccu_probe(&pdev->dev, reg, &sun60i_a733_r_ccu_desc);
}

static const struct of_device_id sun60i_a733_r_ccu_ids[] = {
	{ .compatible = "allwinner,sun60i-a733-r-ccu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_r_ccu_ids);

static struct platform_driver sun60i_a733_r_ccu_driver = {
	.probe	= sun60i_a733_r_ccu_probe,
	.driver	= {
		.name			= "sun60i-a733-r-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun60i_a733_r_ccu_ids,
	},
};
module_platform_driver(sun60i_a733_r_ccu_driver);

MODULE_DESCRIPTION("Allwinner A733 R-CCU driver");
MODULE_LICENSE("GPL");
