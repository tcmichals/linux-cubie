/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Allwinner A733 / T527 Hardware ISP 602 Register Definitions
 *
 * Copyright (c) 2026 Tim Michals <tcmichals@gmail.com>
 */

#ifndef _SUN60I_ISP_REG_H_
#define _SUN60I_ISP_REG_H_

#include <linux/types.h>
#include <linux/bits.h>

/* Top Configuration Registers (Base: 0x05900000) */
#define ISP_TOP_CFG0_REG		0x0000
#define ISP_TOP_EN			BIT(0)
#define ISP_TOP_MODE_OFFLINE		BIT(1)
#define ISP_TOP_CAP_EN			BIT(2)
#define ISP_TOP_VER_RD_EN		BIT(3)

#define ISP_TOP_CFG1_REG		0x0004
#define ISP_TOP_LUT_LOAD_EN		BIT(0)

#define ISP_VER_CFG_REG			0x0010
#define ISP_VER_SMALL_MASK		GENMASK(11, 0)
#define ISP_VER_BIG_MASK		GENMASK(23, 12)

/* AHB Configuration Registers (Offset: +0x0020) */
#define ISP_AHB_CFG0_REG		0x0020
#define ISP_AHB_CAP_EN			BIT(0)
#define ISP_AHB_PARA_READY		BIT(1)
#define ISP_AHB_RSC_UPDATE		BIT(8)
#define ISP_AHB_GAMMA_UPDATE		BIT(9)
#define ISP_AHB_DRC_UPDATE		BIT(10)
#define ISP_AHB_D3D_UPDATE		BIT(13)
#define ISP_AHB_PLTM_UPDATE		BIT(14)
#define ISP_AHB_CEM_UPDATE		BIT(15)
#define ISP_AHB_MSC_UPDATE		BIT(16)
#define ISP_AHB_NR_MSC_UPDATE		BIT(18)

#define ISP_LOAD_ADDR_REG		0x0024
#define ISP_INT_BYPASS_REG		0x0028
#define ISP_INT_STATUS_REG		0x002c

#define ISP_INTER_STATUS0_REG		0x0030
#define ISP_INTER_STATUS1_REG		0x0034
#define ISP_INTER_STATUS2_REG		0x0038

#define ISP_SAVE_ADDR_REG		0x003c
#define ISP_SAVE_LOAD_ADDR_REG		0x0040

/* Interrupt bits for ISP_INT_BYPASS_REG and ISP_INT_STATUS_REG */
#define ISP_IRQ_FINISH			BIT(0)
#define ISP_IRQ_START			BIT(1)
#define ISP_IRQ_PARA_SAVE		BIT(2)
#define ISP_IRQ_PARA_LOAD		BIT(3)
#define ISP_IRQ_N_LINE_START		BIT(4)
#define ISP_IRQ_HB_SHORT		BIT(9)
#define ISP_IRQ_CFG_ERROR		BIT(10)
#define ISP_IRQ_FRAME_LOST		BIT(11)
#define ISP_IRQ_INTER_FIFO_FULL		BIT(16)
#define ISP_IRQ_WDMA_FIFO_FULL		BIT(17)
#define ISP_IRQ_WDMA_OVER_BND		BIT(18)
#define ISP_IRQ_RDMA_FIFO_FULL		BIT(19)
#define ISP_IRQ_LBC_ERROR		BIT(20)
#define ISP_IRQ_DDR_RW_ERROR		BIT(21)
#define ISP_IRQ_AHB_MBUS_W		BIT(22)

#define ISP_IRQ_ERRORS \
	(ISP_IRQ_HB_SHORT | ISP_IRQ_CFG_ERROR | ISP_IRQ_FRAME_LOST | \
	 ISP_IRQ_INTER_FIFO_FULL | ISP_IRQ_WDMA_FIFO_FULL | \
	 ISP_IRQ_WDMA_OVER_BND | ISP_IRQ_RDMA_FIFO_FULL | \
	 ISP_IRQ_LBC_ERROR | ISP_IRQ_DDR_RW_ERROR | ISP_IRQ_AHB_MBUS_W)

#define ISP_IRQ_ENABLE_MASK \
	(ISP_IRQ_PARA_LOAD | ISP_IRQ_PARA_SAVE | ISP_IRQ_ERRORS)

/*
 * Load Buffer DRAM Layout (Base: load_para[0/1])
 * Offset +0x0100 is ISP_LOAD_REG_OFFSET.
 */
#define ISP_LOAD_GLOBAL_CFG0		0x0100
#define ISP_LOAD_GLOBAL_CFG1		0x0104
#define ISP_LOAD_INPUT_SIZE		0x0120
#define ISP_LOAD_VALID_SIZE		0x0124
#define ISP_LOAD_VALID_START		0x0128
#define ISP_LOAD_MODULE_BYPASS0		0x0130
#define ISP_LOAD_MODULE_BYPASS1		0x0134
#define ISP_LOAD_D3D_CFG0		0x0134
#define ISP_LOAD_VIN_CFG0		0x0170
#define ISP_LOAD_CH0_EXPAND_CFG0	0x0198

/* Load Buffer Table Offsets (within ISP_LOAD_DRAM_SIZE = 0x7700) */
#define ISP_LOAD_GAMMA_OFFSET		0x5000
#define ISP_GAMMA_TBL_LEN		1024

/* Save-Load Buffer DRAM Layout */
#define ISP_SAVE_LOAD_RANDOM_SEED	0x0030

/* Input formats (Bayer order) */
#define ISP_BGGR			4
#define ISP_RGGB			5
#define ISP_GBRG			6
#define ISP_GRBG			7

#endif /* _SUN60I_ISP_REG_H_ */
