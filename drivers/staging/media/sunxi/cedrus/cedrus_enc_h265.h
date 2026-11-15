/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cedrus Video Engine Driver - H.265 (HEVC) Encoder
 *
 * Copyright 2026 Tim Michals <tcmichals@gmail.com>
 */

#ifndef _CEDRUS_ENC_H265_H_
#define _CEDRUS_ENC_H265_H_

#include <linux/types.h>

#define CEDRUS_ENC_H265_NALU_TRAIL_R		1
#define CEDRUS_ENC_H265_NALU_IDR_W_RADL		19
#define CEDRUS_ENC_H265_NALU_VPS		32
#define CEDRUS_ENC_H265_NALU_SPS		33
#define CEDRUS_ENC_H265_NALU_PPS		34

#define CEDRUS_ENC_H265_SLICE_TYPE_P		1
#define CEDRUS_ENC_H265_SLICE_TYPE_I		2

enum cedrus_enc_h265_frame_type {
	CEDRUS_ENC_H265_FRAME_TYPE_IDR,
	CEDRUS_ENC_H265_FRAME_TYPE_P,
};

struct cedrus_enc_h265_job {
	unsigned int	frame_type;
	unsigned int	poc;
	unsigned int	qp;
};

struct cedrus_enc_h265_state {
	unsigned int	gop_index;
	unsigned int	poc;
};

struct cedrus_enc_h265_context {
	struct cedrus_enc_h265_state	state;

	void				*mb_info;
	dma_addr_t			mb_info_dma;
	unsigned int			mb_info_size;

	void				*aux;
	dma_addr_t			aux_dma;

	dma_addr_t			subpix_last_dma;
	dma_addr_t			rec_last_dma;
	unsigned int			rec_last_luma_size;

	unsigned int			width_mbs;
	unsigned int			height_mbs;

	int				profile;
	int				level;
	int				qp_i;
	int				qp_p;
	int				gop_closure;
	int				gop_size;
	bool				force_key_frame;
};

struct cedrus_enc_h265_buffer {
	void		*rec;
	dma_addr_t	rec_dma;
	unsigned int	rec_size;
	unsigned int	rec_luma_size;
	unsigned int	rec_chroma_size;

	void		*subpix;
	dma_addr_t	subpix_dma;
	unsigned int	subpix_size;
};

extern const struct cedrus_engine cedrus_enc_h265;

#endif
