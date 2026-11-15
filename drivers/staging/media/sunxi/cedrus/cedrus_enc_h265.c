// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus Video Engine Driver - H.265 (HEVC) Encoder
 *
 * Copyright 2026 Tim Michals <tcmichals@gmail.com>
 */

#include <linux/delay.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_context.h"
#include "cedrus_enc.h"
#include "cedrus_enc_h265.h"
#include "cedrus_engine.h"
#include "cedrus_proc.h"
#include "cedrus_regs.h"

#define CEDRUS_ENC_H265_AUX_DEBLK	0x00000
#define CEDRUS_ENC_H265_AUX_9C		0x10000
#define CEDRUS_ENC_H265_AUX_MV		0x20000
#define CEDRUS_ENC_H265_AUX_REF1	0x60000
#define CEDRUS_ENC_H265_AUX_SIZE	0xa0000

static inline u32 cedrus_enc_h265_addr(struct cedrus_device *dev, dma_addr_t addr)
{
	return addr >> dev->enc_addr_shift;
}

static u8 cedrus_enc_h265_level_idc(int level)
{
	switch (level) {
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4:
		return 120;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1:
		return 123;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_5:
		return 150;
	default:
		return 123;
	}
}

/* Put-bits helpers */

static void cedrus_enc_h265_coded_append(struct cedrus_device *dev,
					 u32 value, unsigned int count)
{
	if (!count || count > 32)
		return;

	cedrus_poll(dev, VE_ENC_AVC_STATUS_REG, VE_ENC_AVC_STATUS_PUT_BITS_READY);
	cedrus_write(dev, VE_ENC_AVC_PUTBITSDATA_REG, value);
	cedrus_write(dev, VE_ENC_AVC_STARTTRIG_REG,
		     VE_ENC_AVC_STARTTRIG_ENCODE_MODE_HEVC |
		     VE_ENC_AVC_STARTTRIG_NUM_BITS(count) |
		     VE_ENC_AVC_STARTTRIG_TYPE_PUT_BITS);
}

static void cedrus_enc_h265_coded_ue(struct cedrus_device *dev,
				     unsigned int value)
{
	unsigned int leading_zeros_count;
	unsigned int bits_count;

	if (!value) {
		cedrus_enc_h265_coded_append(dev, 1, 1);
		return;
	}

	leading_zeros_count = ilog2(value + 1);
	bits_count = leading_zeros_count * 2 + 1;
	cedrus_enc_h265_coded_append(dev, value + 1, bits_count);
}

static void cedrus_enc_h265_coded_u8(struct cedrus_device *dev, u8 value)
{
	cedrus_enc_h265_coded_append(dev, value, 8);
}

static void cedrus_enc_h265_coded_u16(struct cedrus_device *dev, u16 value)
{
	cedrus_enc_h265_coded_append(dev, value, 16);
}

static void cedrus_enc_h265_coded_u32(struct cedrus_device *dev, u32 value)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		cedrus_enc_h265_coded_u8(dev, (value >> (24 - i * 8)) & 0xff);
}

static void cedrus_enc_h265_coded_bit(struct cedrus_device *dev, bool value)
{
	cedrus_enc_h265_coded_append(dev, value ? 1 : 0, 1);
}

static void cedrus_enc_h265_coded_align(struct cedrus_device *dev)
{
	unsigned int bits_count;
	u32 value;

	value = cedrus_read(dev, VE_ENC_AVC_STM_BIT_LEN_REG);
	bits_count = value % 8;
	if (!bits_count)
		return;

	cedrus_enc_h265_coded_append(dev, 0, 8 - bits_count);
}

static void cedrus_enc_h265_coded_eptb(struct cedrus_device *dev, bool enable)
{
	u32 value = cedrus_read(dev, VE_ENC_AVC_PARA0_REG);

	if (enable)
		value &= ~VE_ENC_AVC_PARA0_EPTB_DIS;
	else
		value |= VE_ENC_AVC_PARA0_EPTB_DIS;

	cedrus_write(dev, VE_ENC_AVC_PARA0_REG, value);
}

static void cedrus_enc_h265_coded_start_code(struct cedrus_device *dev)
{
	cedrus_enc_h265_coded_eptb(dev, false);
	cedrus_enc_h265_coded_u32(dev, 0x00000001);
	cedrus_enc_h265_coded_eptb(dev, true);
}

static void cedrus_enc_h265_nal_header(struct cedrus_device *dev, u8 nal_type)
{
	cedrus_enc_h265_coded_bit(dev, 0);			/* forbidden_zero_bit */
	cedrus_enc_h265_coded_append(dev, nal_type, 6);		/* nal_unit_type */
	cedrus_enc_h265_coded_append(dev, 0, 6);		/* nuh_layer_id */
	cedrus_enc_h265_coded_append(dev, 1, 3);		/* nuh_temporal_id_plus1 */
}

static void cedrus_enc_h265_rbsp_trailing(struct cedrus_device *dev)
{
	cedrus_enc_h265_coded_bit(dev, 1);
	cedrus_enc_h265_coded_align(dev);
}

static void cedrus_enc_h265_profile_tier_level(struct cedrus_device *dev,
					       u8 level_idc)
{
	int i;

	cedrus_enc_h265_coded_append(dev, 0, 2);	/* general_profile_space */
	cedrus_enc_h265_coded_bit(dev, 0);		/* general_tier_flag */
	cedrus_enc_h265_coded_append(dev, 1, 5);	/* general_profile_idc = Main */
	/* general_profile_compatibility_flag[0..31]: bit 1 = Main */
	cedrus_enc_h265_coded_u16(dev, 0x6000);
	cedrus_enc_h265_coded_u16(dev, 0x0000);
	/* constraint flags (48 bits) */
	for (i = 0; i < 6; i++)
		cedrus_enc_h265_coded_u8(dev, 0x00);
	cedrus_enc_h265_coded_u8(dev, level_idc);	/* general_level_idc */
}

static void cedrus_enc_h265_put_vps(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_device *dev = cedrus_ctx->proc->dev;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	u8 level_idc = cedrus_enc_h265_level_idc(h265_ctx->level);

	cedrus_enc_h265_coded_start_code(dev);
	cedrus_enc_h265_nal_header(dev, CEDRUS_ENC_H265_NALU_VPS);

	cedrus_enc_h265_coded_append(dev, 0, 4);	/* vps_video_parameter_set_id */
	cedrus_enc_h265_coded_bit(dev, 1);		/* vps_base_layer_internal_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* vps_base_layer_available_flag */
	cedrus_enc_h265_coded_append(dev, 0, 6);	/* vps_max_layers_minus1 */
	cedrus_enc_h265_coded_append(dev, 0, 3);	/* vps_max_sub_layers_minus1 */
	cedrus_enc_h265_coded_bit(dev, 1);		/* vps_temporal_id_nesting_flag */
	cedrus_enc_h265_coded_u16(dev, 0xffff);		/* vps_reserved_0xffff_16bits */

	cedrus_enc_h265_profile_tier_level(dev, level_idc);

	cedrus_enc_h265_coded_bit(dev, 1);		/* vps_sub_layer_ordering_info_present_flag */
	cedrus_enc_h265_coded_ue(dev, 1);		/* vps_max_dec_pic_buffering_minus1[0] */
	cedrus_enc_h265_coded_ue(dev, 0);		/* vps_max_num_reorder_pics[0] */
	cedrus_enc_h265_coded_ue(dev, 0);		/* vps_max_latency_increase_plus1[0] */
	cedrus_enc_h265_coded_append(dev, 0, 6);	/* vps_max_layer_id */
	cedrus_enc_h265_coded_ue(dev, 0);		/* vps_num_layer_sets_minus1 */
	cedrus_enc_h265_coded_bit(dev, 0);		/* vps_timing_info_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* vps_extension_flag */

	cedrus_enc_h265_rbsp_trailing(dev);
}

static void cedrus_enc_h265_put_sps(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_device *dev = cedrus_ctx->proc->dev;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	struct v4l2_pix_format *pix = &cedrus_ctx->v4l2.format_picture.fmt.pix;
	u8 level_idc = cedrus_enc_h265_level_idc(h265_ctx->level);

	cedrus_enc_h265_coded_start_code(dev);
	cedrus_enc_h265_nal_header(dev, CEDRUS_ENC_H265_NALU_SPS);

	cedrus_enc_h265_coded_append(dev, 0, 4);	/* sps_video_parameter_set_id */
	cedrus_enc_h265_coded_append(dev, 0, 3);	/* sps_max_sub_layers_minus1 */
	cedrus_enc_h265_coded_bit(dev, 1);		/* sps_temporal_id_nesting_flag */

	cedrus_enc_h265_profile_tier_level(dev, level_idc);

	cedrus_enc_h265_coded_ue(dev, 0);		/* sps_seq_parameter_set_id */
	cedrus_enc_h265_coded_ue(dev, 1);		/* chroma_format_idc = 4:2:0 */
	cedrus_enc_h265_coded_ue(dev, pix->width);	/* pic_width_in_luma_samples */
	cedrus_enc_h265_coded_ue(dev, pix->height);	/* pic_height_in_luma_samples */
	cedrus_enc_h265_coded_bit(dev, 0);		/* conformance_window_flag */
	cedrus_enc_h265_coded_ue(dev, 0);		/* bit_depth_luma_minus8 */
	cedrus_enc_h265_coded_ue(dev, 0);		/* bit_depth_chroma_minus8 */
	cedrus_enc_h265_coded_ue(dev, 4);		/* log2_max_pic_order_cnt_lsb_minus4 (max=256) */
	cedrus_enc_h265_coded_bit(dev, 1);		/* sps_sub_layer_ordering_info_present_flag */
	cedrus_enc_h265_coded_ue(dev, 1);		/* sps_max_dec_pic_buffering_minus1[0] */
	cedrus_enc_h265_coded_ue(dev, 0);		/* sps_max_num_reorder_pics[0] */
	cedrus_enc_h265_coded_ue(dev, 0);		/* sps_max_latency_increase_plus1[0] */
	cedrus_enc_h265_coded_ue(dev, 0);		/* log2_min_luma_coding_block_size_minus3 (8) */
	cedrus_enc_h265_coded_ue(dev, 2);		/* log2_diff_max_min_luma_coding_block_size (32) */
	cedrus_enc_h265_coded_ue(dev, 0);		/* log2_min_luma_transform_block_size_minus2 (4) */
	cedrus_enc_h265_coded_ue(dev, 3);		/* log2_diff_max_min_luma_transform_block_size (32) */
	cedrus_enc_h265_coded_ue(dev, 1);		/* max_transform_hierarchy_depth_inter */
	cedrus_enc_h265_coded_ue(dev, 1);		/* max_transform_hierarchy_depth_intra */
	cedrus_enc_h265_coded_bit(dev, 0);		/* scaling_list_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* amp_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* sample_adaptive_offset_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* pcm_enabled_flag */
	cedrus_enc_h265_coded_ue(dev, 1);		/* num_short_term_ref_pic_sets */
	/* short_term_ref_pic_set(0): 1 negative ref pic (delta_poc = -1) */
	cedrus_enc_h265_coded_ue(dev, 1);		/* num_negative_pics */
	cedrus_enc_h265_coded_ue(dev, 0);		/* num_positive_pics */
	cedrus_enc_h265_coded_ue(dev, 0);		/* delta_poc_s0_minus1[0] */
	cedrus_enc_h265_coded_bit(dev, 1);		/* used_by_curr_pic_s0_flag[0] */
	cedrus_enc_h265_coded_bit(dev, 0);		/* long_term_ref_pics_present_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* sps_temporal_mvp_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* strong_intra_smoothing_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* vui_parameters_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* sps_extension_present_flag */

	cedrus_enc_h265_rbsp_trailing(dev);
}

static void cedrus_enc_h265_put_pps(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_device *dev = cedrus_ctx->proc->dev;

	cedrus_enc_h265_coded_start_code(dev);
	cedrus_enc_h265_nal_header(dev, CEDRUS_ENC_H265_NALU_PPS);

	cedrus_enc_h265_coded_ue(dev, 0);		/* pps_pic_parameter_set_id */
	cedrus_enc_h265_coded_ue(dev, 0);		/* pps_seq_parameter_set_id */
	cedrus_enc_h265_coded_bit(dev, 0);		/* dependent_slice_segments_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* output_flag_present_flag */
	cedrus_enc_h265_coded_append(dev, 0, 3);	/* num_extra_slice_header_bits */
	cedrus_enc_h265_coded_bit(dev, 0);		/* sign_data_hiding_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* cabac_init_present_flag */
	cedrus_enc_h265_coded_ue(dev, 0);		/* num_ref_idx_l0_default_active_minus1 */
	cedrus_enc_h265_coded_ue(dev, 0);		/* num_ref_idx_l1_default_active_minus1 */
	cedrus_enc_h265_coded_bit(dev, 1);		/* init_qp_minus26 = 0 -> se(0)=bit 1 */
	cedrus_enc_h265_coded_bit(dev, 0);		/* constrained_intra_pred_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* transform_skip_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* cu_qp_delta_enabled_flag */
	cedrus_enc_h265_coded_ue(dev, 1);		/* diff_cu_qp_delta_depth */
	cedrus_enc_h265_coded_bit(dev, 1);		/* pps_cb_qp_offset = 0 -> se(0)=bit 1 */
	cedrus_enc_h265_coded_bit(dev, 1);		/* pps_cr_qp_offset = 0 -> se(0)=bit 1 */
	cedrus_enc_h265_coded_bit(dev, 0);		/* pps_slice_chroma_qp_offsets_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* weighted_pred_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* weighted_bipred_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* transquant_bypass_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* tiles_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* entropy_coding_sync_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* loop_filter_across_slices_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* deblocking_filter_control_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* deblocking_filter_override_enabled_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* pps_deblocking_filter_disabled_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* pps_beta_offset_div2 = 0 -> se(0)=bit 1 */
	cedrus_enc_h265_coded_bit(dev, 1);		/* pps_tc_offset_div2 = 0 -> se(0)=bit 1 */
	cedrus_enc_h265_coded_bit(dev, 0);		/* pps_scaling_list_data_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* lists_modification_present_flag */
	cedrus_enc_h265_coded_ue(dev, 0);		/* log2_parallel_merge_level_minus2 */
	cedrus_enc_h265_coded_bit(dev, 0);		/* slice_segment_header_extension_present_flag */
	cedrus_enc_h265_coded_bit(dev, 0);		/* pps_extension_present_flag */

	cedrus_enc_h265_rbsp_trailing(dev);
}

static void cedrus_enc_h265_put_slice_header(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_device *dev = cedrus_ctx->proc->dev;
	struct cedrus_enc_h265_job *job = cedrus_ctx->engine_job;
	bool is_idr = (job->frame_type == CEDRUS_ENC_H265_FRAME_TYPE_IDR);
	int qp_delta = (int)job->qp - 26;
	unsigned int qp_delta_ue;

	cedrus_enc_h265_coded_start_code(dev);
	cedrus_enc_h265_nal_header(dev, is_idr ? CEDRUS_ENC_H265_NALU_IDR_W_RADL :
						 CEDRUS_ENC_H265_NALU_TRAIL_R);

	cedrus_enc_h265_coded_bit(dev, 1);		/* first_slice_segment_in_pic_flag */
	if (is_idr)
		cedrus_enc_h265_coded_bit(dev, 0);	/* no_output_of_prior_pics_flag */

	cedrus_enc_h265_coded_ue(dev, 0);		/* slice_pic_parameter_set_id */
	cedrus_enc_h265_coded_ue(dev, is_idr ? CEDRUS_ENC_H265_SLICE_TYPE_I :
					       CEDRUS_ENC_H265_SLICE_TYPE_P);

	if (!is_idr) {
		cedrus_enc_h265_coded_append(dev, job->poc & 0xff, 8); /* slice_pic_order_cnt_lsb */
		cedrus_enc_h265_coded_bit(dev, 1);	/* short_term_ref_pic_set_sps_flag */
		cedrus_enc_h265_coded_bit(dev, 1);	/* slice_temporal_mvp_enabled_flag */
	}

	cedrus_enc_h265_coded_bit(dev, 1);		/* slice_sao_luma_flag */
	cedrus_enc_h265_coded_bit(dev, 1);		/* slice_sao_chroma_flag */

	if (!is_idr) {
		cedrus_enc_h265_coded_bit(dev, 0);	/* num_ref_idx_active_override_flag */
		cedrus_enc_h265_coded_bit(dev, 0);	/* cabac_init_flag */
		cedrus_enc_h265_coded_ue(dev, 0);	/* five_minus_max_num_merge_cand (5) */
	}

	/* slice_qp_delta (se(v)) */
	if (qp_delta <= 0)
		qp_delta_ue = (unsigned int)(-qp_delta) * 2;
	else
		qp_delta_ue = (unsigned int)qp_delta * 2 - 1;
	cedrus_enc_h265_coded_ue(dev, qp_delta_ue);

	cedrus_enc_h265_coded_bit(dev, 1);		/* slice_loop_filter_across_slices_enabled_flag */

	/* byte_alignment(): bit 1 + align to byte */
	cedrus_enc_h265_coded_bit(dev, 1);
	cedrus_enc_h265_coded_align(dev);
}

/* Context & Buffers */

static int cedrus_enc_h265_setup(struct cedrus_context *cedrus_ctx)
{
	struct device *dev = cedrus_ctx->proc->dev->dev;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	struct v4l2_pix_format *pix_format =
		&cedrus_ctx->v4l2.format_picture.fmt.pix;

	h265_ctx->width_mbs = DIV_ROUND_UP(pix_format->width, 16);
	h265_ctx->height_mbs = DIV_ROUND_UP(pix_format->height, 16);

	h265_ctx->mb_info_size = DIV_ROUND_UP(h265_ctx->width_mbs, 32) * SZ_4K;
	h265_ctx->mb_info = dma_alloc_attrs(dev, h265_ctx->mb_info_size,
					    &h265_ctx->mb_info_dma, GFP_KERNEL,
					    DMA_ATTR_NO_KERNEL_MAPPING);
	if (!h265_ctx->mb_info)
		return -ENOMEM;

	h265_ctx->aux = dma_alloc_coherent(dev, CEDRUS_ENC_H265_AUX_SIZE,
					   &h265_ctx->aux_dma, GFP_KERNEL);
	if (!h265_ctx->aux) {
		dma_free_attrs(dev, h265_ctx->mb_info_size, h265_ctx->mb_info,
			       h265_ctx->mb_info_dma,
			       DMA_ATTR_NO_KERNEL_MAPPING);
		return -ENOMEM;
	}

	h265_ctx->state.gop_index = 0;
	h265_ctx->state.poc = 0;
	h265_ctx->subpix_last_dma = 0;

	h265_ctx->profile = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_HEVC_PROFILE);
	h265_ctx->level = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_HEVC_LEVEL);
	h265_ctx->qp_i = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP);
	h265_ctx->qp_p = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP);
	h265_ctx->gop_closure = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_GOP_CLOSURE);
	h265_ctx->gop_size = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_GOP_SIZE);

	return 0;
}

static void cedrus_enc_h265_cleanup(struct cedrus_context *cedrus_ctx)
{
	struct device *dev = cedrus_ctx->proc->dev->dev;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;

	dma_free_attrs(dev, h265_ctx->mb_info_size, h265_ctx->mb_info,
		       h265_ctx->mb_info_dma, DMA_ATTR_NO_KERNEL_MAPPING);
	dma_free_coherent(dev, CEDRUS_ENC_H265_AUX_SIZE, h265_ctx->aux,
			  h265_ctx->aux_dma);
}

static int cedrus_enc_h265_buffer_setup(struct cedrus_context *cedrus_ctx,
					struct cedrus_buffer *cedrus_buffer)
{
	struct device *dev = cedrus_ctx->proc->dev->dev;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	struct cedrus_enc_h265_buffer *h265_buf = cedrus_buffer->engine_buffer;
	unsigned int width_aligned = h265_ctx->width_mbs * 16;
	unsigned int height_aligned = h265_ctx->height_mbs * 16;

	h265_buf->rec_luma_size =
		ALIGN(width_aligned * height_aligned + SZ_8K, SZ_4K);
	h265_buf->rec_chroma_size =
		ALIGN(width_aligned * height_aligned / 2 + SZ_8K, SZ_4K);
	h265_buf->rec_size = h265_buf->rec_luma_size + h265_buf->rec_chroma_size;

	h265_buf->rec = dma_alloc_attrs(dev, h265_buf->rec_size,
					&h265_buf->rec_dma, GFP_KERNEL,
					DMA_ATTR_NO_KERNEL_MAPPING);
	if (!h265_buf->rec)
		return -ENOMEM;

	h265_buf->subpix_size =
		ALIGN(width_aligned * height_aligned / 4, SZ_4K);
	h265_buf->subpix = dma_alloc_attrs(dev, h265_buf->subpix_size,
					   &h265_buf->subpix_dma, GFP_KERNEL,
					   DMA_ATTR_NO_KERNEL_MAPPING);
	if (!h265_buf->subpix) {
		dma_free_attrs(dev, h265_buf->rec_size, h265_buf->rec,
			       h265_buf->rec_dma, DMA_ATTR_NO_KERNEL_MAPPING);
		return -ENOMEM;
	}

	return 0;
}

static void cedrus_enc_h265_buffer_cleanup(struct cedrus_context *cedrus_ctx,
					   struct cedrus_buffer *cedrus_buffer)
{
	struct device *dev = cedrus_ctx->proc->dev->dev;
	struct cedrus_enc_h265_buffer *h265_buf = cedrus_buffer->engine_buffer;

	dma_free_attrs(dev, h265_buf->subpix_size, h265_buf->subpix,
		       h265_buf->subpix_dma, DMA_ATTR_NO_KERNEL_MAPPING);
	dma_free_attrs(dev, h265_buf->rec_size, h265_buf->rec,
		       h265_buf->rec_dma, DMA_ATTR_NO_KERNEL_MAPPING);
}

/* Job */

static int cedrus_enc_h265_job_prepare(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	struct cedrus_enc_h265_state *state = &h265_ctx->state;
	struct cedrus_enc_h265_job *job = cedrus_ctx->engine_job;
	struct vb2_v4l2_buffer *v4l2_buffer = cedrus_ctx->job.buffer_coded;

	h265_ctx->gop_size = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_GOP_SIZE);
	h265_ctx->gop_closure = cedrus_context_ctrl_value(cedrus_ctx,
				V4L2_CID_MPEG_VIDEO_GOP_CLOSURE);

	if (h265_ctx->force_key_frame || state->gop_index == 0) {
		h265_ctx->force_key_frame = false;
		job->frame_type = CEDRUS_ENC_H265_FRAME_TYPE_IDR;
		job->qp = h265_ctx->qp_i;
		state->poc = 0;
		job->poc = 0;
		state->gop_index = 1;
		v4l2_buffer->flags |= V4L2_BUF_FLAG_KEYFRAME;
	} else {
		job->frame_type = CEDRUS_ENC_H265_FRAME_TYPE_P;
		job->qp = h265_ctx->qp_p;
		state->poc = (state->poc + 1) & 0xff;
		job->poc = state->poc;
		state->gop_index++;
		v4l2_buffer->flags |= V4L2_BUF_FLAG_PFRAME;
	}

	if (h265_ctx->gop_size > 0 && state->gop_index >= h265_ctx->gop_size)
		state->gop_index = 0;

	return 0;
}

static int cedrus_enc_h265_job_configure(struct cedrus_context *cedrus_ctx)
{
	struct cedrus_device *dev = cedrus_ctx->proc->dev;
	struct cedrus_enc_h265_job *job = cedrus_ctx->engine_job;
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;
	struct cedrus_enc_h265_buffer *h265_buf =
		cedrus_job_engine_buffer(cedrus_ctx);
	struct v4l2_pix_format *pix = &cedrus_ctx->v4l2.format_picture.fmt.pix;
	bool is_idr = (job->frame_type == CEDRUS_ENC_H265_FRAME_TYPE_IDR);
	unsigned int stride_mbs_div_48;
	unsigned int luma_size;
	unsigned int size;
	dma_addr_t addr;
	u32 value;

	cedrus_write(dev, VE_ENC_AVC_STARTTRIG_REG,
		     VE_ENC_AVC_STARTTRIG_ENCODE_MODE_HEVC);
	cedrus_write(dev, VE_ENC_AVC_STM_BIT_OFFSET_REG, 0);

	cedrus_job_buffer_coded_dma(cedrus_ctx, &addr, &size);

	cedrus_write(dev, VE_ENC_AVC_STM_START_ADDR_REG,
		     cedrus_enc_h265_addr(dev, addr));
	cedrus_write(dev, VE_ENC_AVC_STM_END_ADDR_REG,
		     (addr + size - 1) >> dev->enc_addr_shift);
	cedrus_write(dev, VE_ENC_AVC_STM_BIT_MAX_REG, size * 8);

	cedrus_write(dev, VE_ENC_AVC_STM_BIT_LEN_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_HEADER_BITS_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_RESIDUAL_BITS_REG, 0);

	cedrus_write(dev, VE_ENC_AVC_MB_INFO_ADDR_REG,
		     cedrus_enc_h265_addr(dev, h265_ctx->mb_info_dma));

	cedrus_write(dev, VE_ENC_AVC_MV_BUF_ADDR_REG,
		     cedrus_enc_h265_addr(dev, h265_ctx->aux_dma +
					  ((job->poc & 1) ? 0x40000 : 0x20000)));
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x64,
		     cedrus_enc_h265_addr(dev, h265_ctx->aux_dma +
					  (is_idr ? 0x20000 :
					   ((job->poc & 1) ? 0x20000 : 0x40000))));
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x9c,
		     cedrus_enc_h265_addr(dev, h265_ctx->aux_dma +
					  CEDRUS_ENC_H265_AUX_9C));
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0xa8,
		     cedrus_enc_h265_addr(dev, h265_ctx->aux_dma +
					  CEDRUS_ENC_H265_AUX_REF1));

	cedrus_write(dev, VE_ENC_AVC_REC_ADDR_Y_REG,
		     cedrus_enc_h265_addr(dev, h265_buf->rec_dma));
	cedrus_write(dev, VE_ENC_AVC_REC_ADDR_C_REG,
		     cedrus_enc_h265_addr(dev, h265_buf->rec_dma +
					  h265_buf->rec_luma_size));

	if (!is_idr && h265_ctx->rec_last_dma) {
		addr = h265_ctx->rec_last_dma;
		luma_size = h265_ctx->rec_last_luma_size;
	} else {
		addr = h265_buf->rec_dma;
		luma_size = h265_buf->rec_luma_size;
	}

	cedrus_write(dev, VE_ENC_AVC_REF0_ADDR_Y_REG,
		     cedrus_enc_h265_addr(dev, addr));
	cedrus_write(dev, VE_ENC_AVC_REF0_ADDR_C_REG,
		     cedrus_enc_h265_addr(dev, addr + luma_size));

	h265_ctx->rec_last_dma = h265_buf->rec_dma;
	h265_ctx->rec_last_luma_size = h265_buf->rec_luma_size;

	cedrus_write(dev, VE_ENC_AVC_SUBPIX_ADDR_NEW_REG,
		     cedrus_enc_h265_addr(dev, h265_buf->subpix_dma));

	if (!h265_ctx->subpix_last_dma)
		h265_ctx->subpix_last_dma = h265_buf->subpix_dma;

	cedrus_write(dev, VE_ENC_AVC_SUBPIX_ADDR_LAST_REG,
		     cedrus_enc_h265_addr(dev, h265_ctx->subpix_last_dma));
	h265_ctx->subpix_last_dma = h265_buf->subpix_dma;

	cedrus_write(dev, VE_ENC_AVC_DEBLK_ADDR_REG,
		     cedrus_enc_h265_addr(dev, h265_ctx->aux_dma +
					  CEDRUS_ENC_H265_AUX_DEBLK));

	/* A733 HEVC block constants from vendor sweep */
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x00,
		     cedrus_read(dev, VE_ISP_PIC_INFO_REG));
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x40, 0x18000014);
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x44, 0x0c1e0084);
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x6c,
		     is_idr ? 0x3cc80020 : 0x3cc80072);
	cedrus_write(dev, VE_ENGINE_ENC_H264_BASE + 0x94,
		     is_idr ? 0x00000020 : 0x00000072);

	cedrus_write(dev, VE_ENC_AVC_CYCLIC_INTRA_REFRESH_REG, 0);

	cedrus_write(dev, VE_ENC_AVC_PARA0_REG,
		     is_idr ? 0x00000100 : 0x00000110);

	stride_mbs_div_48 = DIV_ROUND_UP(pix->bytesperline / 16, 48);
	value = (0x1c << 16) |
		VE_ENC_AVC_PARA1_STRIDE_MBS_DIV_48(stride_mbs_div_48) |
		VE_ENC_AVC_PARA1_FIXED_QP(job->qp);
	cedrus_write(dev, VE_ENC_AVC_PARA1_REG, value);
	cedrus_write(dev, VE_ENC_AVC_PARA2_REG, 0);

	cedrus_write(dev, VE_ENC_AVC_PARA2_REG, 0);

	cedrus_write(dev, VE_ENC_AVC_DYNAMIC_ME_PAR0_REG,
		     is_idr ? 0 : 0x00180004);
	cedrus_write(dev, VE_ENC_AVC_DYNAMIC_ME_PAR1_REG,
		     is_idr ? 0 : 0x00800068);

	cedrus_write(dev, VE_ENC_AVC_RC_INIT_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_RC_MAD_TH0_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_RC_MAD_TH1_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_RC_MAD_TH2_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_RC_MAD_TH3_REG, 0);

	cedrus_write(dev, VE_ENC_AVC_ME_PARA_REG,
		     is_idr ? 0x08c00000 : 0x08400000);

	cedrus_write(dev, VE_ENC_AVC_MAD_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_OVERTIME_MB_REG, 0);
	cedrus_write(dev, VE_ENC_AVC_ME_INFO_REG, 0);

	if (is_idr) {
		cedrus_enc_h265_put_vps(cedrus_ctx);
		cedrus_enc_h265_put_sps(cedrus_ctx);
		cedrus_enc_h265_put_pps(cedrus_ctx);
	}
	cedrus_enc_h265_put_slice_header(cedrus_ctx);
	cedrus_poll(dev, dev->top_reg_offset + VE_RESET_REG,
		    VE_RESET_CACHE_SYNC_IDLE | VE_RESET_SYNC_IDLE);

	return 0;
}

static void cedrus_enc_h265_job_trigger(struct cedrus_context *ctx)
{
	struct cedrus_device *dev = ctx->proc->dev;

	cedrus_write(dev, VE_ENC_AVC_INT_EN_REG,
		     VE_ENC_AVC_INT_EN_STALL |
		     VE_ENC_AVC_INT_EN_FINISH);

	cedrus_write(dev, VE_ENC_AVC_STARTTRIG_REG,
		     VE_ENC_AVC_STARTTRIG_ENCODE_MODE_HEVC |
		     VE_ENC_AVC_STARTTRIG_TYPE_ENC_START);
}

static void cedrus_enc_h265_job_finish(struct cedrus_context *ctx, int state)
{
	struct cedrus_device *dev = ctx->proc->dev;
	struct vb2_v4l2_buffer *v4l2_buffer = ctx->job.buffer_coded;
	struct vb2_buffer *vb2_buffer = &v4l2_buffer->vb2_buf;
	u32 length;

	if (state != VB2_BUF_STATE_DONE) {
		vb2_set_plane_payload(vb2_buffer, 0, 0);
		return;
	}

	length = cedrus_read(dev, VE_ENC_AVC_STM_BIT_LEN_REG);
	WARN_ON(length % 8);
	length /= 8;
	WARN_ON(length > vb2_plane_size(vb2_buffer, 0));

	vb2_set_plane_payload(vb2_buffer, 0, length);
}

static int cedrus_enc_h265_irq_status(struct cedrus_context *ctx)
{
	struct cedrus_device *dev = ctx->proc->dev;
	u32 status = cedrus_read(dev, VE_ENC_AVC_STATUS_REG);

	if (!(status & VE_ENC_AVC_STATUS_MASK))
		return CEDRUS_IRQ_NONE;

	if (status & VE_ENC_AVC_STATUS_FINISH)
		return CEDRUS_IRQ_SUCCESS;

	return CEDRUS_IRQ_ERROR;
}

static void cedrus_enc_h265_irq_clear(struct cedrus_context *ctx)
{
	struct cedrus_device *dev = ctx->proc->dev;

	cedrus_write(dev, VE_ENC_AVC_STATUS_REG, VE_ENC_AVC_STATUS_MASK);
}

static void cedrus_enc_h265_irq_disable(struct cedrus_context *ctx)
{
	struct cedrus_device *dev = ctx->proc->dev;

	cedrus_write(dev, VE_ENC_AVC_INT_EN_REG, 0);
}

static int cedrus_enc_h265_ctrl_prepare(struct cedrus_context *cedrus_ctx,
					struct v4l2_ctrl *ctrl);

static const struct cedrus_engine_ops cedrus_enc_h265_ops = {
	.ctrl_prepare		= cedrus_enc_h265_ctrl_prepare,
	.format_prepare		= cedrus_enc_format_coded_prepare,
	.format_configure	= cedrus_enc_format_coded_configure,
	.setup			= cedrus_enc_h265_setup,
	.cleanup		= cedrus_enc_h265_cleanup,
	.buffer_setup		= cedrus_enc_h265_buffer_setup,
	.buffer_cleanup		= cedrus_enc_h265_buffer_cleanup,
	.job_prepare		= cedrus_enc_h265_job_prepare,
	.job_configure		= cedrus_enc_h265_job_configure,
	.job_trigger		= cedrus_enc_h265_job_trigger,
	.job_finish		= cedrus_enc_h265_job_finish,
	.irq_status		= cedrus_enc_h265_irq_status,
	.irq_clear		= cedrus_enc_h265_irq_clear,
	.irq_disable		= cedrus_enc_h265_irq_disable,
};

/* Controls */

static int cedrus_enc_h265_ctrl_prepare(struct cedrus_context *cedrus_ctx,
					struct v4l2_ctrl *ctrl)
{
	struct cedrus_enc_h265_context *h265_ctx = cedrus_ctx->engine_ctx;

	if (cedrus_ctx->engine != &cedrus_enc_h265 || !h265_ctx)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		h265_ctx->profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		h265_ctx->level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP:
		h265_ctx->qp_i = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP:
		h265_ctx->qp_p = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_CLOSURE:
		h265_ctx->gop_closure = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		h265_ctx->gop_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		h265_ctx->force_key_frame = true;
		break;
	default:
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_config cedrus_enc_h265_ctrl_configs[] = {
	{
		.id		= V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
		.max		= V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.def		= V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.menu_skip_mask	= ~BIT(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN),
		.ops		= &cedrus_context_ctrl_ops,
	},
	{
		.id		= V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
		.max		= V4L2_MPEG_VIDEO_HEVC_LEVEL_5,
		.def		= V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1,
		.menu_skip_mask	= ~(BIT(V4L2_MPEG_VIDEO_HEVC_LEVEL_4) |
				    BIT(V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1) |
				    BIT(V4L2_MPEG_VIDEO_HEVC_LEVEL_5)),
		.ops		= &cedrus_context_ctrl_ops,
	},
	{
		.id		= V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP,
		.step		= 1,
		.min		= 0,
		.max		= 51,
		.def		= 28,
		.ops		= &cedrus_context_ctrl_ops,
	},
	{
		.id		= V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
		.step		= 1,
		.min		= 0,
		.max		= 51,
		.def		= 30,
		.ops		= &cedrus_context_ctrl_ops,
	},
};

static const struct v4l2_frmsize_stepwise cedrus_enc_h265_frmsize = {
	.min_width	= 64,
	.max_width	= 4096,
	.step_width	= 16,

	.min_height	= 64,
	.max_height	= 4096,
	.step_height	= 8,
};

const struct cedrus_engine cedrus_enc_h265 = {
	.codec			= CEDRUS_CODEC_H265,
	.role			= CEDRUS_ROLE_ENCODER,
	.capabilities		= CEDRUS_CAPABILITY_H265_ENC,

	.ops			= &cedrus_enc_h265_ops,

	.pixelformat		= V4L2_PIX_FMT_HEVC,
	.ctrl_configs		= cedrus_enc_h265_ctrl_configs,
	.ctrl_configs_count	= ARRAY_SIZE(cedrus_enc_h265_ctrl_configs),
	.frmsize		= &cedrus_enc_h265_frmsize,

	.ctx_size		= sizeof(struct cedrus_enc_h265_context),
	.job_size		= sizeof(struct cedrus_enc_h265_job),
	.buffer_size		= sizeof(struct cedrus_enc_h265_buffer),
};
