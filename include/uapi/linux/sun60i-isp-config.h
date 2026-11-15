/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * Allwinner A733 / T527 Hardware ISP 602 Userspace API
 *
 * Copyright (c) 2026 Tim Michals <tcmichals@gmail.com>
 */

#ifndef _UAPI_SUN60I_ISP_CONFIG_H
#define _UAPI_SUN60I_ISP_CONFIG_H

#include <linux/types.h>

#define SUN60I_ISP_PARAM_API_V1		1
#define SUN60I_ISP_STAT_API_V1		1

/* Module enable / config update bitmasks */
#define SUN60I_ISP_MOD_BLC		(1U << 0)
#define SUN60I_ISP_MOD_WB		(1U << 1)
#define SUN60I_ISP_MOD_CCM		(1U << 2)
#define SUN60I_ISP_MOD_GAMMA		(1U << 3)

#define SUN60I_ISP_GAMMA_LUT_SIZE	1024

/**
 * struct sun60i_isp_blc_config - Black Level Correction (13-bit signed)
 * @r: Red channel offset (13-bit two's complement, e.g. -64)
 * @gr: Green-Red channel offset
 * @gb: Green-Blue channel offset
 * @b: Blue channel offset
 */
struct sun60i_isp_blc_config {
	__s16 r;
	__s16 gr;
	__s16 gb;
	__s16 b;
};

/**
 * struct sun60i_isp_wb_config - White Balance Gains (Q8, 256 = 1.0x, max 4095)
 * @r: Red channel gain
 * @gr: Green-Red channel gain
 * @gb: Green-Blue channel gain
 * @b: Blue channel gain
 */
struct sun60i_isp_wb_config {
	__u16 r;
	__u16 gr;
	__u16 gb;
	__u16 b;
};

/**
 * struct sun60i_isp_ccm_config - 3x3 Color Correction Matrix (Q8, 256 = 1.0)
 * @coeff: Row-major 3x3 matrix coefficients (12-bit signed)
 * @offset: Post-matrix R, G, B offsets (13-bit signed)
 */
struct sun60i_isp_ccm_config {
	__s16 coeff[9];
	__s16 offset[3];
};

/**
 * struct sun60i_isp_gamma_config - 1024-entry 10-bit RGB Gamma LUT
 * @lut_r: Red channel LUT entries (0..1023)
 * @lut_g: Green channel LUT entries (0..1023)
 * @lut_b: Blue channel LUT entries (0..1023)
 */
struct sun60i_isp_gamma_config {
	__u16 lut_r[SUN60I_ISP_GAMMA_LUT_SIZE];
	__u16 lut_g[SUN60I_ISP_GAMMA_LUT_SIZE];
	__u16 lut_b[SUN60I_ISP_GAMMA_LUT_SIZE];
};

/**
 * struct sun60i_isp_params_cfg - ISP 602 per-frame userspace parameters
 * @version: API version (must be SUN60I_ISP_PARAM_API_V1)
 * @module_en_update: Bitmask of SUN60I_ISP_MOD_* to enable/disable
 * @module_ens: Target enable state for bits in @module_en_update
 * @module_cfg_update: Bitmask of SUN60I_ISP_MOD_* whose config struct is valid
 * @blc: Black Level Correction config
 * @wb: White Balance gain config
 * @ccm: Color Correction Matrix config
 * @gamma: Gamma LUT config
 */
struct sun60i_isp_params_cfg {
	__u32 version;
	__u32 module_en_update;
	__u32 module_ens;
	__u32 module_cfg_update;
	struct sun60i_isp_blc_config blc;
	struct sun60i_isp_wb_config wb;
	struct sun60i_isp_ccm_config ccm;
	struct sun60i_isp_gamma_config gamma;
};

#define SUN60I_ISP_STAT_AE_SIZE		0x0360
#define SUN60I_ISP_STAT_AWB_SIZE	0x1800
#define SUN60I_ISP_STAT_HIST0_SIZE	0x0400

/**
 * struct sun60i_isp_stat_buffer - ISP 602 per-frame 3A statistics
 * @version: API version (SUN60I_ISP_STAT_API_V1)
 * @frame_id: Monotonic ISP frame counter
 * @ae: Raw hardware AE statistics block (save_buf + 0x0100, 864 bytes)
 * @awb: Raw hardware AWB RGB statistics block (save_buf + 0x4080, 6144 bytes)
 * @hist0: Raw hardware Histogram 0 block (save_buf + 0x5a80, 1024 bytes)
 */
struct sun60i_isp_stat_buffer {
	__u32 version;
	__u32 frame_id;
	__u8 ae[SUN60I_ISP_STAT_AE_SIZE];
	__u8 awb[SUN60I_ISP_STAT_AWB_SIZE];
	__u8 hist0[SUN60I_ISP_STAT_HIST0_SIZE];
};

#endif /* _UAPI_SUN60I_ISP_CONFIG_H */
