// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/sun60i-a733-ccu.h>
#include <dt-bindings/reset/sun60i-a733-ccu.h>

#include "ccu_common.h"
#include "ccu_reset.h"
#include "ccu_div.h"
#include "ccu_gate.h"
#include "ccu_mp.h"
#include "ccu_mult.h"
#include "ccu_nk.h"
#include "ccu_nkm.h"
#include "ccu_nkmp.h"
#include "ccu_nm.h"

static const struct clk_parent_data osc24M[] = {
	{ .fw_name = "hosc" }
};

static struct ccu_nkmp pll_periph0_4x_clk = {
	.enable		= BIT(27),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT_MIN(8, 8, 12),
	.m		= _SUNXI_CCU_DIV(1, 1),
	.p		= _SUNXI_CCU_DIV(0, 1),
	.common		= {
		.reg		= 0x020,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("pll-periph0-4x", osc24M,
							   &ccu_nkmp_ops,
							   CLK_SET_RATE_GATE | CLK_IS_CRITICAL),
	},
};

static const struct clk_hw *pll_periph0_4x_hws[] = {
	&pll_periph0_4x_clk.common.hw
};

static SUNXI_CCU_M_HWS(pll_periph0_2x_clk, "pll-periph0-2x",
		       pll_periph0_4x_hws, 0x020, 16, 3, CLK_IS_CRITICAL);

static const struct clk_hw *pll_periph0_2x_hws[] = {
	&pll_periph0_2x_clk.common.hw
};

static SUNXI_CCU_M_HWS(pll_periph0_1x_clk, "pll-periph0-1x",
		       pll_periph0_2x_hws, 0x020, 20, 3, CLK_IS_CRITICAL);

static const struct clk_parent_data ahb_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.common.hw },
};
static SUNXI_CCU_M_DATA_WITH_MUX(psi_ahb1_ahb2_clk, "psi-ahb1-ahb2",
				 ahb_parents, 0x510, 0, 5, 24, 2, CLK_IS_CRITICAL);

static const struct clk_hw *ahb_hws[] = {
	&psi_ahb1_ahb2_clk.common.hw
};

static const struct clk_parent_data apb1_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.common.hw },
};
static SUNXI_CCU_M_DATA_WITH_MUX(apb1_clk, "apb1", apb1_parents,
				 0x520, 0, 5, 24, 2, CLK_IS_CRITICAL);

static const struct clk_hw *apb1_hws[] = {
	&apb1_clk.common.hw
};

static const struct clk_parent_data apb2_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.common.hw },
};
static SUNXI_CCU_M_DATA_WITH_MUX(apb2_clk, "apb2", apb2_parents,
				 0x524, 0, 5, 24, 2, CLK_IS_CRITICAL);

static const struct clk_hw *apb2_hws[] = {
	&apb2_clk.common.hw
};

static SUNXI_CCU_GATE_HWS(bus_uart0_clk, "bus-uart0", apb2_hws, 0x0e00, BIT(0), CLK_IS_CRITICAL);
static SUNXI_CCU_GATE_HWS(bus_uart1_clk, "bus-uart1", apb2_hws, 0x0e04, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart2_clk, "bus-uart2", apb2_hws, 0x0e08, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart3_clk, "bus-uart3", apb2_hws, 0x0e0c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart4_clk, "bus-uart4", apb2_hws, 0x0e10, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart5_clk, "bus-uart5", apb2_hws, 0x0e14, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart6_clk, "bus-uart6", apb2_hws, 0x0e18, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_uart7_clk, "bus-uart7", apb2_hws, 0x0e1c, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_i2c0_clk,  "bus-i2c0",  apb2_hws, 0x0e80, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_i2c1_clk,  "bus-i2c1",  apb2_hws, 0x0e84, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_i2c2_clk,  "bus-i2c2",  apb2_hws, 0x0e88, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_i2c3_clk,  "bus-i2c3",  apb2_hws, 0x0e8c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_i2c4_clk,  "bus-i2c4",  apb2_hws, 0x0e90, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_i2c5_clk,  "bus-i2c5",  apb2_hws, 0x0e94, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_spi0_clk,  "bus-spi0",  ahb_hws,  0x0f04, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_spi1_clk,  "bus-spi1",  ahb_hws,  0x0f0c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_spi2_clk,  "bus-spi2",  ahb_hws,  0x0f14, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_spi3_clk,  "bus-spi3",  ahb_hws,  0x0f1c, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_mmc0_clk,  "bus-mmc0",  ahb_hws,  0x0d0c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_mmc1_clk,  "bus-mmc1",  ahb_hws,  0x0d1c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_mmc2_clk,  "bus-mmc2",  ahb_hws,  0x0d2c, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_msgbox0_clk, "bus-msgbox0", ahb_hws,  0x0744, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_gmac0_clk, "bus-gmac0", ahb_hws,  0x141c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_gmac1_clk, "bus-gmac1", ahb_hws,  0x142c, BIT(0), 0);

static SUNXI_CCU_GATE_HWS(bus_usb0_clk, "bus-usb0", ahb_hws, 0x1304, BIT(4) | BIT(0), 0);
static SUNXI_CCU_GATE_HWS(usb_ohci0_clk, "usb-ohci0", ahb_hws, 0x1304, BIT(4), 0);
static SUNXI_CCU_GATE_HWS(bus_usb1_clk, "bus-usb1", ahb_hws, 0x130c, BIT(4) | BIT(0), 0);
static SUNXI_CCU_GATE_HWS(usb_ohci1_clk, "usb-ohci1", ahb_hws, 0x130c, BIT(4), 0);
/* USB2 (DWC3) core auxiliary, bus and transport clocks */
static SUNXI_CCU_GATE_HWS(bus_usb2_clk, "bus-usb2", ahb_hws,
			  0x135c, BIT(0), CLK_IS_CRITICAL);

static const struct clk_parent_data usb2_ref_parents[] = {
	{ .fw_name = "hosc" },
};
static SUNXI_CCU_MUX_DATA_WITH_GATE(usb2_u2_ref_clk, "usb2-u2-ref", usb2_ref_parents,
				    0x1348,
				    24, 3,	/* mux */
				    BIT(31),	/* gate */
				    0);

static const struct clk_parent_data usb2_suspend_parents[] = {
	{ .fw_name = "losc" },
	{ .fw_name = "hosc" },
};
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(usb2_suspend_clk, "usb2-suspend", usb2_suspend_parents,
				      0x1350,
				      0, 5,	/* M */
				      24, 1,	/* mux */
				      BIT(31),	/* gate */
				      0);

static const struct clk_parent_data usb2_mf_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_1x_clk.common.hw },
	{ .fw_name = "hosc" },
};
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(usb2_mf_clk, "usb2-mf", usb2_mf_parents,
				      0x1354,
				      0, 5,	/* M */
				      24, 3,	/* mux */
				      BIT(31),	/* gate */
				      0);

/*
 * The MSI-Lite2 interconnect slice carries all CPU MMIO traffic to the
 * USB subsystem. Accessing any USB controller or PHY register while
 * this gate is closed stalls the bus, so it must stay enabled.
 */
static SUNXI_CCU_GATE_HWS(bus_msi_lite2_clk, "bus-msi-lite2", ahb_hws,
			  0x05a4, BIT(0), CLK_IS_CRITICAL);

/* 24 MHz reference for the USB PHY resistance calibration (DCAP). */
static SUNXI_CCU_GATE_DATA(res_dcap_24m_clk, "res-dcap-24m", osc24M,
			   0x1a00, BIT(3), CLK_IS_CRITICAL);

/* USB reference and PHY clocks */
static SUNXI_CCU_GATE_DATA(usb_ref_clk, "usb-ref", osc24M, 0x1340, BIT(31), CLK_IS_CRITICAL);
static SUNXI_CCU_GATE_DATA(usb_phy0_clk, "usb-phy0", osc24M, 0x1300, BIT(31), 0);
static SUNXI_CCU_GATE_DATA(usb_phy1_clk, "usb-phy1", osc24M, 0x1308, BIT(31), 0);

/* GMAC IEEE 1588 reference clock; vendor uses the 24 MHz parent by default. */
static SUNXI_CCU_GATE_DATA(gmac_ptp_clk, "gmac-ptp", osc24M, 0x1400, BIT(31), 0);

static const struct clk_parent_data mmc_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.common.hw },
};
static SUNXI_CCU_MP_MUX_GATE_POSTDIV_DUALDIV(mmc0_clk, "mmc0", mmc_parents,
					     0xd00,
					     0, 5,	/* M */
					     8, 5,	/* P */
					     24, 3,	/* mux */
					     BIT(31),	/* gate */
					     2,		/* post div */
					     0);
static SUNXI_CCU_MP_MUX_GATE_POSTDIV_DUALDIV(mmc1_clk, "mmc1", mmc_parents,
					     0xd10,
					     0, 5,	/* M */
					     8, 5,	/* P */
					     24, 3,	/* mux */
					     BIT(31),	/* gate */
					     2,		/* post div */
					     0);
static SUNXI_CCU_MP_MUX_GATE_POSTDIV_DUALDIV(mmc2_clk, "mmc2", mmc_parents,
					     0xd20,
					     0, 5,	/* M */
					     8, 5,	/* P */
					     24, 3,	/* mux */
					     BIT(31),	/* gate */
					     2,		/* post div */
					     0);

static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(spi0_clk, "spi0", mmc_parents, 0x0f00,
				       0, 4, 8, 5, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(spi1_clk, "spi1", mmc_parents, 0x0f08,
				       0, 4, 8, 5, 24, 2, BIT(31), 0);

static struct ccu_common *sun60i_a733_ccu_clks[] = {
	&pll_periph0_4x_clk.common,
	&pll_periph0_2x_clk.common,
	&pll_periph0_1x_clk.common,
	&psi_ahb1_ahb2_clk.common,
	&apb1_clk.common,
	&apb2_clk.common,
	&bus_uart0_clk.common,
	&bus_uart1_clk.common,
	&bus_uart2_clk.common,
	&bus_uart3_clk.common,
	&bus_uart4_clk.common,
	&bus_uart5_clk.common,
	&bus_uart6_clk.common,
	&bus_uart7_clk.common,
	&bus_i2c0_clk.common,
	&bus_i2c1_clk.common,
	&bus_i2c2_clk.common,
	&bus_i2c3_clk.common,
	&bus_i2c4_clk.common,
	&bus_i2c5_clk.common,
	&bus_spi0_clk.common,
	&bus_spi1_clk.common,
	&bus_spi2_clk.common,
	&bus_spi3_clk.common,
	&bus_mmc0_clk.common,
	&bus_mmc1_clk.common,
	&bus_mmc2_clk.common,
	&bus_msgbox0_clk.common,
	&bus_gmac0_clk.common,
	&bus_gmac1_clk.common,
	&bus_usb0_clk.common,
	&usb_ohci0_clk.common,
	&bus_usb1_clk.common,
	&usb_ohci1_clk.common,
	&bus_usb2_clk.common,
	&usb2_u2_ref_clk.common,
	&usb2_suspend_clk.common,
	&usb2_mf_clk.common,
	&bus_msi_lite2_clk.common,
	&res_dcap_24m_clk.common,
	&usb_ref_clk.common,
	&usb_phy0_clk.common,
	&usb_phy1_clk.common,
	&gmac_ptp_clk.common,
	&mmc0_clk.common,
	&mmc1_clk.common,
	&mmc2_clk.common,
	&spi0_clk.common,
	&spi1_clk.common,
};

#define CLK_NUMBER (CLK_USB2_MF + 1)
static struct {
	unsigned int num;
	struct clk_hw *hws[CLK_NUMBER];
} sun60i_a733_hw_clks = {
	.num = CLK_NUMBER,
	.hws = {
		[CLK_PLL_PERIPH0_4X]	= &pll_periph0_4x_clk.common.hw,
		[CLK_PLL_PERIPH0_2X]	= &pll_periph0_2x_clk.common.hw,
		[CLK_PLL_PERIPH0_1X]	= &pll_periph0_1x_clk.common.hw,
		[CLK_PSI_AHB1_AHB2]	= &psi_ahb1_ahb2_clk.common.hw,
		[CLK_APB1]		= &apb1_clk.common.hw,
		[CLK_APB2]		= &apb2_clk.common.hw,
		[CLK_BUS_UART0]		= &bus_uart0_clk.common.hw,
		[CLK_BUS_UART1]		= &bus_uart1_clk.common.hw,
		[CLK_BUS_UART2]		= &bus_uart2_clk.common.hw,
		[CLK_BUS_UART3]		= &bus_uart3_clk.common.hw,
		[CLK_BUS_UART4]		= &bus_uart4_clk.common.hw,
		[CLK_BUS_UART5]		= &bus_uart5_clk.common.hw,
		[CLK_BUS_UART6]		= &bus_uart6_clk.common.hw,
		[CLK_BUS_UART7]		= &bus_uart7_clk.common.hw,
		[CLK_MSGBOX0]		= &bus_msgbox0_clk.common.hw,
		[CLK_BUS_I2C0]		= &bus_i2c0_clk.common.hw,
		[CLK_BUS_I2C1]		= &bus_i2c1_clk.common.hw,
		[CLK_BUS_I2C2]		= &bus_i2c2_clk.common.hw,
		[CLK_BUS_I2C3]		= &bus_i2c3_clk.common.hw,
		[CLK_BUS_I2C4]		= &bus_i2c4_clk.common.hw,
		[CLK_BUS_I2C5]		= &bus_i2c5_clk.common.hw,
		[CLK_BUS_SPI0]		= &bus_spi0_clk.common.hw,
		[CLK_BUS_SPI1]		= &bus_spi1_clk.common.hw,
		[CLK_BUS_SPI2]		= &bus_spi2_clk.common.hw,
		[CLK_BUS_SPI3]		= &bus_spi3_clk.common.hw,
		[CLK_BUS_MMC0]		= &bus_mmc0_clk.common.hw,
		[CLK_BUS_MMC1]		= &bus_mmc1_clk.common.hw,
		[CLK_BUS_MMC2]		= &bus_mmc2_clk.common.hw,
		[CLK_BUS_GMAC0]		= &bus_gmac0_clk.common.hw,
		[CLK_BUS_GMAC1]		= &bus_gmac1_clk.common.hw,
		[CLK_BUS_USB0]		= &bus_usb0_clk.common.hw,
		[CLK_BUS_USB1]		= &bus_usb1_clk.common.hw,
		[CLK_BUS_USB2]		= &bus_usb2_clk.common.hw,
		[CLK_USB2_U2_REF]	= &usb2_u2_ref_clk.common.hw,
		[CLK_USB2_SUSPEND]	= &usb2_suspend_clk.common.hw,
		[CLK_USB2_MF]		= &usb2_mf_clk.common.hw,
		[CLK_USB_OHCI0]		= &usb_ohci0_clk.common.hw,
		[CLK_USB_OHCI1]		= &usb_ohci1_clk.common.hw,
		[CLK_MMC0]		= &mmc0_clk.common.hw,
		[CLK_MMC1]		= &mmc1_clk.common.hw,
		[CLK_MMC2]		= &mmc2_clk.common.hw,
		[CLK_SPI0]		= &spi0_clk.common.hw,
		[CLK_SPI1]		= &spi1_clk.common.hw,
		[CLK_USB_PHY0]		= &usb_phy0_clk.common.hw,
		[CLK_USB_PHY1]		= &usb_phy1_clk.common.hw,
		[CLK_BUS_MSI_LITE2]	= &bus_msi_lite2_clk.common.hw,
		[CLK_RES_DCAP_24M]	= &res_dcap_24m_clk.common.hw,
		[CLK_GMAC_PTP]		= &gmac_ptp_clk.common.hw,
	},
};

static const struct ccu_reset_map sun60i_a733_ccu_resets[] = {
	[RST_BUS_MMC0]	= { 0x0d0c, BIT(16) },
	[RST_BUS_MMC1]	= { 0x0d1c, BIT(16) },
	[RST_BUS_MMC2]	= { 0x0d2c, BIT(16) },
	[RST_BUS_MSGBOX0] = { 0x0744, BIT(16) },
	[RST_BUS_UART0]	= { 0x0e00, BIT(16) },
	[RST_BUS_UART1]	= { 0x0e04, BIT(16) },
	[RST_BUS_UART2]	= { 0x0e08, BIT(16) },
	[RST_BUS_UART3]	= { 0x0e0c, BIT(16) },
	[RST_BUS_UART4]	= { 0x0e10, BIT(16) },
	[RST_BUS_UART5]	= { 0x0e14, BIT(16) },
	[RST_BUS_UART6]	= { 0x0e18, BIT(16) },
	[RST_BUS_UART7]	= { 0x0e1c, BIT(16) },
	[RST_BUS_I2C0]	= { 0x0e80, BIT(16) },
	[RST_BUS_I2C1]	= { 0x0e84, BIT(16) },
	[RST_BUS_I2C2]	= { 0x0e88, BIT(16) },
	[RST_BUS_I2C3]	= { 0x0e8c, BIT(16) },
	[RST_BUS_I2C4]	= { 0x0e90, BIT(16) },
	[RST_BUS_I2C5]	= { 0x0e94, BIT(16) },
	[RST_BUS_SPI0]	= { 0x0f04, BIT(16) },
	[RST_BUS_SPI1]	= { 0x0f0c, BIT(16) },
	[RST_BUS_SPI2]	= { 0x0f14, BIT(16) },
	[RST_BUS_SPI3]	= { 0x0f1c, BIT(16) },
	[RST_BUS_GMAC0]	= { 0x141c, BIT(16) },
	[RST_BUS_GMAC1]	= { 0x142c, BIT(16) },
	[RST_BUS_GMAC0_AXI] = { 0x141c, BIT(17) },
	[RST_BUS_GMAC1_AXI] = { 0x142c, BIT(17) },
	[RST_BUS_USB0]	= { 0x1304, BIT(20) | BIT(16) },
	[RST_BUS_USB1]	= { 0x130c, BIT(20) | BIT(16) },
	[RST_BUS_USB2]	= { 0x135c, BIT(16) },
	[RST_USB_PHY0]	= { 0x1300, BIT(30) },
	[RST_USB_PHY1]	= { 0x1308, BIT(30) },
};

static const struct sunxi_ccu_desc sun60i_a733_ccu_desc = {
	.ccu_clks	= sun60i_a733_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_a733_ccu_clks),
	.hw_clks	= (struct clk_hw_onecell_data *)&sun60i_a733_hw_clks,
	.resets		= sun60i_a733_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun60i_a733_ccu_resets),
};

static int sun60i_a733_ccu_probe(struct platform_device *pdev)
{
	void __iomem *reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	/*
	 * Open the key-protected interconnect master gates. These are
	 * security/fabric gates, not rate clocks, and the BSP kernel keeps
	 * every one of them enabled (CLK_IS_CRITICAL or CLK_IGNORE_UNUSED
	 * in ccu-sun60iw2.c). Accessing a peripheral behind a closed gate
	 * stalls the bus with no fault, so mirror the BSP state once at
	 * probe time, preserving whatever the boot chain already set:
	 * - 0x05c0 AHB_GATE_EN     (key 0x10000ff):  cpus/store/msilite0/
	 *   usb/serdes/gpu/npu/de/video/ve, bits 28,24,16,9..0
	 * - 0x05e0 MBUS_MAT_GATING (key 0x41055800): msilite2/store/
	 *   msilite0/serdes/vid-in/npu/gpu/ve/de/iommu, bits 31..28,24,18,
	 *   16,14,12,11,1,0
	 * - 0x05e4 MBUS_GATE_EN    (key 0x40302):    ve/gmac/isp/csi/nand/
	 *   dma/ce, bits 18,12,11,9,8,5,3..0
	 */
	writel(0x010000FF | 0x110103FF, reg + 0x05C0);
	writel(0x41055800 | 0xF1055803, reg + 0x05E0);
	writel(0x00040302 | 0x0007FFFF, reg + 0x05E4);

	/* Enable GMAC0 PHY 25MHz fanout clock (150MHz / (5+1) = 25MHz) */
	writel(BIT(31) | 5, reg + 0x1410);

	/* Ensure USB2 transport and bus clocks are unmasked */
	writel(readl(reg + 0x1340) | BIT(31), reg + 0x1340);
	writel(readl(reg + 0x1348) | BIT(31), reg + 0x1348);
	writel(readl(reg + 0x1350) | BIT(31), reg + 0x1350);
	writel(0x81000000, reg + 0x1354);
	writel(readl(reg + 0x135c) | BIT(16) | BIT(0), reg + 0x135c);
	writel(0x81000004, reg + 0x1360);
	writel(readl(reg + 0x1364) | BIT(31), reg + 0x1364);
	writel(readl(reg + 0x1a00) | BIT(3), reg + 0x1a00);
	writel(readl(reg + 0x13c0) | BIT(31), reg + 0x13c0);
	writel(readl(reg + 0x13c4) | BIT(16), reg + 0x13c4);
	writel(readl(reg + 0x05a4) | BIT(0), reg + 0x05a4);

	return devm_sunxi_ccu_probe(&pdev->dev, reg, &sun60i_a733_ccu_desc);
}

static const struct of_device_id sun60i_a733_ccu_ids[] = {
	{ .compatible = "allwinner,sun60i-a733-ccu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_ccu_ids);

static struct platform_driver sun60i_a733_ccu_driver = {
	.probe	= sun60i_a733_ccu_probe,
	.driver	= {
		.name			= "sun60i-a733-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun60i_a733_ccu_ids,
	},
};
builtin_platform_driver(sun60i_a733_ccu_driver);
MODULE_LICENSE("GPL");
