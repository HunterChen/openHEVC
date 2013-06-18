/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 Guillaume Martres
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/internal.h"
#include "cabac_functions.h"
#include "golomb.h"
#include "hevcdata.h"
#include "hevc.h"
#include "libavutil/opt.h"
#include "libavutil/md5.h"
#include "libavutil/atomic.h"

/**
 * NOTE: Each function hls_foo correspond to the function foo in the
 * specification (HLS stands for High Level Syntax).
 */

/**
 * Section 5.7
 */
//#define POC_DISPLAY_MD5

#define USE_AVX
#define WPP1
static void pic_arrays_free(HEVCContext *s)
{
    int i, j;

    av_freep(&s->sao);

    av_freep(&s->split_cu_flag);
    av_freep(&s->cu.skip_flag);

    av_freep(&s->cu.left_ct_depth);
    av_freep(&s->cu.top_ct_depth);

    av_freep(&s->pu.left_ipm);
    av_freep(&s->pu.top_ipm);
    av_freep(&s->horizontal_bs);
    av_freep(&s->vertical_bs);

    av_freep(&s->cbf_luma);
    av_freep(&s->is_pcm);

    av_freep(&s->qp_y_tab);

    av_freep(&s->sh.entry_point_offset);
    for (j = 0; j < MAX_ENTRIES && (s->enable_multithreads || !j); j++){
        for (i = 0; i < MAX_TRANSFORM_DEPTH; i++) {
            av_freep(&s->tt[j].cbf_cb[i]);
            av_freep(&s->tt[j].cbf_cr[i]);
        }
    }
    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        av_freep(&s->DPB[i].tab_mvf);
    }
}

static int pic_arrays_init(HEVCContext *s)
{
    int i, j;
    int pic_size = s->sps->pic_width_in_luma_samples * s->sps->pic_height_in_luma_samples;
    int pic_size_in_ctb = pic_size>>(s->sps->log2_min_coding_block_size<<1);
    int ctb_count = s->sps->pic_width_in_ctbs * s->sps->pic_height_in_ctbs;
    int pic_width_in_min_pu  = s->sps->pic_width_in_min_cbs * 4;
    int pic_height_in_min_pu = s->sps->pic_height_in_min_cbs * 4;
    s->bs_width = s->sps->pic_width_in_luma_samples >> 3;
    s->bs_height = s->sps->pic_height_in_luma_samples >> 3;
    s->sao = av_mallocz(ctb_count * sizeof(*s->sao));
    s->split_cu_flag = av_malloc(pic_size);
    if (!s->sao || !s->split_cu_flag)
        goto fail;

    s->cu.skip_flag     = av_malloc(pic_size_in_ctb);
    s->cu.left_ct_depth = av_malloc(s->sps->pic_height_in_min_cbs);
    s->cu.top_ct_depth  = av_malloc(s->sps->pic_width_in_min_cbs);
    if (!s->cu.skip_flag || !s->cu.left_ct_depth || !s->cu.top_ct_depth)
        goto fail;

    s->pu.left_ipm = av_malloc(pic_height_in_min_pu);
    s->pu.top_ipm  = av_malloc(pic_width_in_min_pu);
    if (!s->pu.left_ipm || !s->pu.top_ipm)
        goto fail;

    s->cbf_luma = av_malloc(pic_width_in_min_pu * pic_height_in_min_pu);
    s->is_pcm   = av_malloc(pic_width_in_min_pu * pic_height_in_min_pu);
    if (!s->cbf_luma ||!s->is_pcm)
        goto fail;

    s->qp_y_tab = av_malloc(pic_size_in_ctb*sizeof(int8_t));
    if (!s->qp_y_tab)
        goto fail;

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        s->DPB[i].tab_mvf = av_malloc(pic_width_in_min_pu  *
                                             pic_height_in_min_pu *
                                             sizeof(*s->DPB[i].tab_mvf));
        if (!s->DPB[i].tab_mvf)
            goto fail;
    }

    s->horizontal_bs = av_mallocz(2 * s->bs_width * s->bs_height);
    s->vertical_bs   = av_mallocz(2 * s->bs_width * s->bs_height);
    if (!s->horizontal_bs || !s->vertical_bs)
        goto fail;


    for (j = 0; j < MAX_ENTRIES && (s->enable_multithreads || !j); j++){
        for (i = 0; i < MAX_TRANSFORM_DEPTH; i++) {
            s->tt[j].cbf_cb[i] = av_malloc(MAX_CU_SIZE*MAX_CU_SIZE);
            s->tt[j].cbf_cr[i] = av_malloc(MAX_CU_SIZE*MAX_CU_SIZE);
            if (!s->tt[j].cbf_cb[i] || !s->tt[j].cbf_cr[i])
                goto fail;
        }
    }
    return 0;

fail:
    pic_arrays_free(s);
    return AVERROR(ENOMEM);
}

static void pred_weight_table(HEVCContext *s, GetBitContext *gb)
{
    int i = 0;
    int j = 0;
    int delta_chroma_log2_weight_denom;
    uint8_t luma_weight_l0_flag[16];
    uint8_t chroma_weight_l0_flag[16];
    uint8_t luma_weight_l1_flag[16];
    uint8_t chroma_weight_l1_flag[16];

    s->sh.luma_log2_weight_denom = get_ue_golomb(gb);
    if (s->sps->chroma_format_idc != 0) {
        delta_chroma_log2_weight_denom = get_se_golomb(gb);
        s->sh.chroma_log2_weight_denom = av_clip_c(s->sh.luma_log2_weight_denom + delta_chroma_log2_weight_denom, 0, 7);
    }
    for (i = 0; i < s->sh.num_ref_idx_l0_active; i++) {
        luma_weight_l0_flag[i] = get_bits1(gb);
        if (!luma_weight_l0_flag[i]) {
            s->sh.luma_weight_l0[i] = 1 << s->sh.luma_log2_weight_denom;
            s->sh.luma_offset_l0[i] = 0;
        }
    }
    if (s->sps->chroma_format_idc != 0) { //fix me ! invert "if" and "for"
        for (i = 0; i < s->sh.num_ref_idx_l0_active; i++) {
            chroma_weight_l0_flag[i] = get_bits1(gb);
        }
    } else {
        for (i = 0; i < s->sh.num_ref_idx_l0_active; i++) {
            chroma_weight_l0_flag[i] = 0;
        }
    }
    for (i = 0; i < s->sh.num_ref_idx_l0_active; i++) {
        if (luma_weight_l0_flag[i]) {
            int delta_luma_weight_l0 = get_se_golomb(gb);
            s->sh.luma_weight_l0[i]  = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l0;
            s->sh.luma_offset_l0[i]  = get_se_golomb(gb);
        }
        if (chroma_weight_l0_flag[i]) {
            for (j = 0; j < 2; j++) {
                int delta_chroma_weight_l0   = get_se_golomb(gb);
                int delta_chroma_offset_l0   = get_se_golomb(gb);
                s->sh.chroma_weight_l0[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l0;
                s->sh.chroma_offset_l0[i][j] = av_clip_c((delta_chroma_offset_l0 - ((128 * s->sh.chroma_weight_l0[i][j])
                                                          >>  s->sh.chroma_log2_weight_denom) + 128), -128, 127);
            }
        } else {
            s->sh.chroma_weight_l0[i][0] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][0] = 0;
            s->sh.chroma_weight_l0[i][1] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][1] = 0;
        }
    }
    if (s->sh.slice_type == B_SLICE) {
        for (i = 0; i < s->sh.num_ref_idx_l1_active; i++) {
            luma_weight_l1_flag[i] = get_bits1(gb);
            if (!luma_weight_l1_flag[i]) {
                s->sh.luma_weight_l1[i] = 1 << s->sh.luma_log2_weight_denom;
                s->sh.luma_offset_l1[i] = 0;
            }
        }
        if (s->sps->chroma_format_idc != 0) {
            for (i = 0; i < s->sh.num_ref_idx_l1_active; i++) {
                chroma_weight_l1_flag[i] = get_bits1(gb);
            }
        } else {
            for (i = 0; i < s->sh.num_ref_idx_l1_active; i++) {
                chroma_weight_l1_flag[i] = 0;
            }
        }
        for (i = 0; i < s->sh.num_ref_idx_l1_active; i++) {
            if (luma_weight_l1_flag[i]) {
                int delta_luma_weight_l1 = get_se_golomb(gb);
                s->sh.luma_weight_l1[i]  = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l1;
                s->sh.luma_offset_l1[i]  = get_se_golomb(gb);
            }
            if (chroma_weight_l1_flag[i]) {
                for (j = 0; j < 2; j++) {
                    int delta_chroma_weight_l1   = get_se_golomb(gb);
                    int delta_chroma_offset_l1   = get_se_golomb(gb);
                    s->sh.chroma_weight_l1[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l1;
                    s->sh.chroma_offset_l1[i][j] = av_clip_c((delta_chroma_offset_l1 - ((128 * s->sh.chroma_weight_l1[i][j])
                                                              >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
                }
            } else {
                s->sh.chroma_weight_l1[i][0] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][0] = 0;
                s->sh.chroma_weight_l1[i][1] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][1] = 0;
            }
        }
    }
}

static int hls_slice_header(HEVCContext *s)
{
    int i, ret, j;
    GetBitContext *gb = s->gb[0];
    SliceHeader *sh = &s->sh;
    int slice_address_length = 0;

    // initial values
    sh->beta_offset = 0;
    sh->tc_offset = 0;

    // Coded parameters

    sh->first_slice_in_pic_flag = get_bits1(gb);
    if (s->nal_unit_type == NAL_IDR_W_RADL && sh->first_slice_in_pic_flag) {
        ff_hevc_clear_refs(s);
    }
    if ((s->nal_unit_type == NAL_IDR_W_RADL || s->nal_unit_type == NAL_IDR_N_LP) &&
        sh->first_slice_in_pic_flag) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
    }
    if (s->nal_unit_type >= 16 && s->nal_unit_type <= 23)
        sh->no_output_of_prior_pics_flag = get_bits1(gb);

    sh->pps_id = get_ue_golomb(gb);
    if (sh->pps_id >= MAX_PPS_COUNT || !s->pps_list[sh->pps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", sh->pps_id);
        return AVERROR_INVALIDDATA;
    }
    s->pps = s->pps_list[sh->pps_id];
    if (s->sps != s->sps_list[s->pps->sps_id]) {
        s->sps = s->sps_list[s->pps->sps_id];
        s->vps = s->vps_list[s->sps->vps_id];
        //TODO: Handle switching between different SPS better
        pic_arrays_free(s);
        ret = pic_arrays_init(s);
        if (ret < 0)
            return AVERROR(ENOMEM);

        s->avctx->width = s->sps->pic_width_in_luma_samples;
        s->avctx->height = s->sps->pic_height_in_luma_samples;
        if (s->sps->chroma_format_idc == 0 || s->sps->separate_colour_plane_flag) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "TODO: s->sps->chroma_format_idc == 0 || "
                   "s->sps->separate_colour_plane_flag\n");
            return AVERROR_PATCHWELCOME;
        }

        if (s->sps->chroma_format_idc == 1) {
            switch (s->sps->bit_depth) {
            case 8:
                s->avctx->pix_fmt = PIX_FMT_YUV420P;
                break;
            case 9:
                s->avctx->pix_fmt = PIX_FMT_YUV420P9;
                break;
            case 10:
                s->avctx->pix_fmt = PIX_FMT_YUV420P10;
                break;
            }
        } else {
            av_log(s->avctx, AV_LOG_ERROR, "non-4:2:0 support is currently unspecified.\n");
            return AVERROR_PATCHWELCOME;
        }
        s->sps->hshift[0] = s->sps->vshift[0] = 0;
        s->sps->hshift[2] =
        s->sps->hshift[1] = av_pix_fmt_descriptors[s->avctx->pix_fmt].log2_chroma_w;
        s->sps->vshift[2] =
        s->sps->vshift[1] = av_pix_fmt_descriptors[s->avctx->pix_fmt].log2_chroma_h;

        s->sps->pixel_shift = s->sps->bit_depth > 8;

        ff_hevc_pred_init(&s->hpc, s->sps->bit_depth);
        ff_hevc_dsp_init(&s->hevcdsp, s->sps->bit_depth);

        ff_videodsp_init(&s->vdsp, s->sps->bit_depth);
    }
    sh->dependent_slice_segment_flag = 0;
    if (!sh->first_slice_in_pic_flag) {
        if (s->pps->dependent_slice_segments_enabled_flag)
            sh->dependent_slice_segment_flag = get_bits1(gb);

        slice_address_length = av_ceil_log2_c(s->sps->pic_width_in_ctbs *
                                              s->sps->pic_height_in_ctbs);
        sh->slice_address = get_bits(gb, slice_address_length);
    } else {
        sh->slice_address = 0;
    }

    if (!sh->dependent_slice_segment_flag) {
        for(i = 0; i < s->pps->num_extra_slice_header_bits; i++)
            skip_bits(gb, 1); // slice_reserved_undetermined_flag[]
        sh->slice_type = get_ue_golomb(gb);
        if (s->pps->output_flag_present_flag)
            sh->pic_output_flag = get_bits1(gb);

        if (s->sps->separate_colour_plane_flag == 1)
            sh->colour_plane_id = get_bits(gb, 2);

        if (s->nal_unit_type != NAL_IDR_W_RADL && s->nal_unit_type != NAL_IDR_N_LP) {
            int short_term_ref_pic_set_sps_flag;
            sh->pic_order_cnt_lsb = get_bits(gb, s->sps->log2_max_poc_lsb);
            ff_hevc_compute_poc(s, sh->pic_order_cnt_lsb);
            short_term_ref_pic_set_sps_flag = get_bits1(gb);
            if (!short_term_ref_pic_set_sps_flag) {
                ff_hevc_decode_short_term_rps(s, s->sps->num_short_term_ref_pic_sets, s->sps);
                sh->short_term_rps = &s->sps->short_term_rps_list[s->sps->num_short_term_ref_pic_sets];
            } else {
                int numbits = 0;
                int short_term_ref_pic_set_idx;
                while ((1 << numbits) < s->sps->num_short_term_ref_pic_sets)
                    numbits++;
                if (numbits > 0)
                    short_term_ref_pic_set_idx = get_bits(gb, numbits);
                else
                    short_term_ref_pic_set_idx = 0;
                sh->short_term_rps = &s->sps->short_term_rps_list[short_term_ref_pic_set_idx];
            }
            sh->long_term_rps.num_long_term_sps = 0;
            sh->long_term_rps.num_long_term_pics = 0;
            if (s->sps->long_term_ref_pics_present_flag) {
                if( s->sps->num_long_term_ref_pics_sps > 0 )
                    sh->long_term_rps.num_long_term_sps = get_ue_golomb(gb);
                sh->long_term_rps.num_long_term_pics = get_ue_golomb(gb);
                for( i = 0; i < sh->long_term_rps.num_long_term_sps + sh->long_term_rps.num_long_term_pics; i++ ) {
                    if( i < sh->long_term_rps.num_long_term_sps ) {
                        uint8_t lt_idx_sps = 0;
                        if( s->sps->num_long_term_ref_pics_sps > 1 )
                            lt_idx_sps = get_bits(gb, av_ceil_log2_c(s->sps->num_long_term_ref_pics_sps));
                        sh->long_term_rps.PocLsbLt[ i ] = s->sps->lt_ref_pic_poc_lsb_sps[ lt_idx_sps ];
                        sh->long_term_rps.UsedByCurrPicLt[ i ] = s->sps->used_by_curr_pic_lt_sps_flag[ lt_idx_sps ];
                    } else {
                        sh->long_term_rps.PocLsbLt[ i ] = get_bits(gb, s->sps->log2_max_poc_lsb);
                        sh->long_term_rps.UsedByCurrPicLt[ i ] = get_bits1(gb);
                    }
                    sh->long_term_rps.delta_poc_msb_present_flag[ i ] = get_bits1(gb);
                    if( sh->long_term_rps.delta_poc_msb_present_flag[ i ] == 1)
                        if( i == 0 || i == sh->long_term_rps.num_long_term_sps )
                            sh->long_term_rps.DeltaPocMsbCycleLt[ i ] = get_ue_golomb(gb);
                        else
                            sh->long_term_rps.DeltaPocMsbCycleLt[ i ] = get_ue_golomb(gb) + sh->long_term_rps.DeltaPocMsbCycleLt[ i - 1 ];
                }
            }
            if (s->sps->sps_temporal_mvp_enabled_flag)
                sh->slice_temporal_mvp_enabled_flag = get_bits1(gb);
            else
                sh->slice_temporal_mvp_enabled_flag = 0;
        } else {
            s->sh.short_term_rps = NULL;
            s->poc = 0;
        }
        if (s->temporal_id == 0)
            s->pocTid0 = s->poc;
//        av_log(s->avctx, AV_LOG_INFO, "Decode  : POC %d NAL %d\n", s->poc, s->nal_unit_type);
        if (!s->pps) {
            av_log(s->avctx, AV_LOG_ERROR, "No PPS active while decoding slice\n");
            return AVERROR_INVALIDDATA;
        }

        if (s->sps->sample_adaptive_offset_enabled_flag) {
            sh->slice_sample_adaptive_offset_flag[0] = get_bits1(gb);
            sh->slice_sample_adaptive_offset_flag[2] =
            sh->slice_sample_adaptive_offset_flag[1] = get_bits1(gb);
        }

        sh->num_ref_idx_l0_active = 0;
        sh->num_ref_idx_l1_active = 0;
        if (sh->slice_type == P_SLICE || sh->slice_type == B_SLICE) {
            int NumPocTotalCurr;
            sh->num_ref_idx_l0_active = s->pps->num_ref_idx_l0_default_active;
            if (sh->slice_type == B_SLICE)
                sh->num_ref_idx_l1_active = s->pps->num_ref_idx_l1_default_active;
            sh->num_ref_idx_active_override_flag = get_bits1(gb);

            if (sh->num_ref_idx_active_override_flag) {
                sh->num_ref_idx_l0_active = get_ue_golomb(gb) + 1;
                if (sh->slice_type == B_SLICE)
                    sh->num_ref_idx_l1_active = get_ue_golomb(gb) + 1;
            }
            sh->ref_pic_list_modification_flag_lx[0] = 0;
            sh->ref_pic_list_modification_flag_lx[1] = 0;
            NumPocTotalCurr = ff_hevc_get_NumPocTotalCurr(s);
            if (s->pps->lists_modification_present_flag && NumPocTotalCurr > 1) {
                sh->ref_pic_list_modification_flag_lx[0] = get_bits1(gb);
                if( sh->ref_pic_list_modification_flag_lx[0] == 1 )
                    for (i = 0; i < sh->num_ref_idx_l0_active; i++)
                        sh->list_entry_lx[0][i] = get_bits(gb, av_ceil_log2_c(NumPocTotalCurr));
                if (sh->slice_type == B_SLICE) {
                    sh->ref_pic_list_modification_flag_lx[1] = get_bits1(gb);
                    if (sh->ref_pic_list_modification_flag_lx[1] == 1)
                        for (i = 0; i < sh->num_ref_idx_l1_active; i++)
                            sh->list_entry_lx[1][i] = get_bits(gb, av_ceil_log2_c(NumPocTotalCurr));
                }
            }

            if (sh->slice_type == B_SLICE)
                sh->mvd_l1_zero_flag = get_bits1(gb);

            if (s->pps->cabac_init_present_flag) {
                sh->cabac_init_flag = get_bits1(gb);
            }
            if (sh->slice_temporal_mvp_enabled_flag) {
                sh->collocated_from_l0_flag = 1;
                if (sh->slice_type == B_SLICE) {
                    sh->collocated_from_l0_flag = get_bits1(gb);
                }
                if (( sh->collocated_from_l0_flag && sh->num_ref_idx_l0_active > 1) ||
                    (!sh->collocated_from_l0_flag && sh->num_ref_idx_l1_active > 1)) {
                    sh->collocated_ref_idx = get_ue_golomb(gb);
                }
            }
            if ((s->pps->weighted_pred_flag && sh->slice_type == P_SLICE) || (s->pps->weighted_bipred_flag && sh->slice_type == B_SLICE)) {
                pred_weight_table(s, gb);
            }

            sh->max_num_merge_cand = 5 - get_ue_golomb(gb);
        }
        ff_hevc_set_ref_poc_list(s);
        sh->slice_qp_delta = get_se_golomb(gb);
        if (s->pps->pic_slice_level_chroma_qp_offsets_present_flag) {
            sh->slice_cb_qp_offset = get_se_golomb(gb);
            sh->slice_cr_qp_offset = get_se_golomb(gb);
        }
        if (s->pps->deblocking_filter_control_present_flag) {
            int deblocking_filter_override_flag = 0;
            if (s->pps->deblocking_filter_override_enabled_flag)
                deblocking_filter_override_flag = get_bits1(gb);
            if (deblocking_filter_override_flag) {
                sh->disable_deblocking_filter_flag = get_bits1(gb);
                if (!sh->disable_deblocking_filter_flag) {
                    sh->beta_offset = get_se_golomb(gb) * 2;
                    sh->tc_offset = get_se_golomb(gb) * 2;
                }
            } else {
                sh->disable_deblocking_filter_flag = s->pps->pps_disable_deblocking_filter_flag;
            }
        }

        if (s->pps->seq_loop_filter_across_slices_enabled_flag
            && (sh->slice_sample_adaptive_offset_flag[0] ||
                sh->slice_sample_adaptive_offset_flag[1] ||
                !sh->disable_deblocking_filter_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = get_bits1(gb);
        } else {
            sh->slice_loop_filter_across_slices_enabled_flag =
            s->pps->seq_loop_filter_across_slices_enabled_flag;
        }
    }
    
    ///
    
    sh->num_entry_point_offsets = 0;
    if( s->pps->tiles_enabled_flag == 1 || s->pps->entropy_coding_sync_enabled_flag == 1) {
        sh->num_entry_point_offsets = get_ue_golomb(gb);
        if(sh->num_entry_point_offsets >= MAX_ENTRIES) {
            av_log(s->avctx, AV_LOG_ERROR, "The number of entry points : %d is higher than the maximum number of entry points : %d \n", sh->num_entry_point_offsets, MAX_ENTRIES);
        }
        if( sh->num_entry_point_offsets > 0 ) {
            int offset_len = get_ue_golomb(gb)+1;
            int segments = offset_len >> 4;
            int rest = (offset_len & 15);
            av_freep(&sh->entry_point_offset);
            sh->entry_point_offset = av_malloc(sh->num_entry_point_offsets*sizeof(int));
            for( i = 0; i < sh->num_entry_point_offsets; i++ ) {
                int val = 0;
                for(j = 0;  j < segments; j++){
                    val <<= 16;
                    val += get_bits(gb, 16);
                }
                if(rest) {
                    val <<= rest;
                    val += get_bits(gb, rest);
                }
                sh->entry_point_offset[i] = val + 1; // +1; // +1 to get the size
            }
        }
    }


    if (s->pps->slice_header_extension_present_flag) {
        int length = get_ue_golomb(gb);
        for (i = 0; i < length; i++)
            skip_bits(gb, 8); // slice_header_extension_data_byte
    }

    // Inferred parameters
    sh->slice_qp = 26 + s->pps->pic_init_qp_minus26 + sh->slice_qp_delta;
    sh->slice_ctb_addr_rs = sh->slice_address;
    sh->slice_cb_addr_zs = sh->slice_address <<
                           (s->sps->log2_diff_max_min_coding_block_size << 1);

    return 0;
}



#define CTB(tab, x, y) ((tab)[(y) * s->sps->pic_width_in_ctbs + (x)])

#define set_sao(elem, value)                            \
    if (!sao_merge_up_flag && !sao_merge_left_flag) {   \
        sao->elem = value;                              \
    } else if (sao_merge_left_flag) {                   \
        sao->elem = CTB(s->sao, rx-1, ry).elem;         \
    } else if (sao_merge_up_flag) {                     \
        sao->elem = CTB(s->sao, rx, ry-1).elem;         \
    } else {                                            \
        sao->elem = 0;                                  \
    }

static int hls_sao_param(HEVCContext *s, int rx, int ry, int entry)
{
    int c_idx, i;
    int sao_merge_left_flag = 0;
    int sao_merge_up_flag = 0;
    int shift = s->sps->bit_depth - FFMIN(s->sps->bit_depth, 10);

    SAOParams *sao = &CTB(s->sao, rx, ry);

    if (rx > 0) {
        if (s->ctb_left_flag[entry])
            sao_merge_left_flag = ff_hevc_sao_merge_flag_decode(s, entry);
    }
    if (ry > 0 && !sao_merge_left_flag) {
        if (s->ctb_up_flag[entry])
            sao_merge_up_flag = ff_hevc_sao_merge_flag_decode(s, entry);
    }
    for (c_idx = 0; c_idx < 3; c_idx++) {

        if (!s->sh.slice_sample_adaptive_offset_flag[c_idx])
            continue;

        if (c_idx == 2) {
            sao->type_idx[2] = sao->type_idx[1];
            sao->eo_class[2] = sao->eo_class[1];
        } else {
            set_sao(type_idx[c_idx], ff_hevc_sao_type_idx_decode(s, entry));
        }

        if (sao->type_idx[c_idx] == SAO_NOT_APPLIED)
            continue;

        for (i = 0; i < 4; i++)
            set_sao(offset_abs[c_idx][i], ff_hevc_sao_offset_abs_decode(s, entry));

        if (sao->type_idx[c_idx] == SAO_BAND) {
            for (i = 0; i < 4; i++) {
                if (sao->offset_abs[c_idx][i]) {
                    set_sao(offset_sign[c_idx][i], ff_hevc_sao_offset_sign_decode(s, entry));
                } else {
                    sao->offset_sign[c_idx][i] = 0;
                }
            }
            set_sao(band_position[c_idx], ff_hevc_sao_band_position_decode(s, entry));
        } else if (c_idx != 2) {
            set_sao(eo_class[c_idx], ff_hevc_sao_eo_class_decode(s, entry));
        }

        // Inferred parameters
        for (i = 0; i < 4; i++) {
            sao->offset_val[c_idx][i+1] = sao->offset_abs[c_idx][i] << shift;
            if (sao->type_idx[c_idx] == SAO_EDGE) {
                if (i > 1)
                    sao->offset_val[c_idx][i+1] = -sao->offset_val[c_idx][i+1];
            } else if (sao->offset_sign[c_idx][i]) {
                sao->offset_val[c_idx][i+1] = -sao->offset_val[c_idx][i+1];
            }
        }
    }
    return 0;
}

#undef set_sao
#undef CTB

static av_always_inline int min_cb_addr_zs(HEVCContext *s, int x, int y)
{
    return s->pps->min_cb_addr_zs[y * s->sps->pic_width_in_min_cbs + x];
}

static void hls_residual_coding(HEVCContext *s, int x0, int y0, int log2_trafo_size, enum ScanType scan_idx, int c_idx, int entry)
{
#define GET_COORD(offset, n)                                    \
    do {                                                        \
        x_c = (scan_x_cg[offset >> 4] << 2) + scan_x_off[n];    \
        y_c = (scan_y_cg[offset >> 4] << 2) + scan_y_off[n];    \
    } while (0)

    int i;

    int transform_skip_flag = 0;

    int last_significant_coeff_x, last_significant_coeff_y;
    int last_scan_pos;
    int n_end;
    int num_coeff = 0;
    int num_last_subset;
    int x_cg_last_sig, y_cg_last_sig;

    const uint8_t *scan_x_cg, *scan_y_cg, *scan_x_off, *scan_y_off;

    ptrdiff_t stride = s->frame->linesize[c_idx];
    int hshift = s->sps->hshift[c_idx];
    int vshift = s->sps->vshift[c_idx];
    uint8_t *dst = &s->frame->data[c_idx][(y0 >> vshift) * stride +
                                         ((x0 >> hshift) << s->sps->pixel_shift)];
#ifdef USE_AVX
    DECLARE_ALIGNED( 32, int16_t, coeffs[MAX_TB_SIZE * MAX_TB_SIZE] )= { 0 };
#else
    DECLARE_ALIGNED( 16, int16_t, coeffs[MAX_TB_SIZE * MAX_TB_SIZE] )= { 0 };
#endif
    int trafo_size = 1 << log2_trafo_size;

    memset(s->rc[entry].significant_coeff_group_flag, 0, 8*8);


    if (s->pps->transform_skip_enabled_flag && !s->cu.cu_transquant_bypass_flag[entry] &&
        log2_trafo_size == 2) {
        transform_skip_flag = ff_hevc_transform_skip_flag_decode(s, c_idx, entry);
    }

    last_significant_coeff_x =
    ff_hevc_last_significant_coeff_x_prefix_decode(s, c_idx, log2_trafo_size, entry);
    last_significant_coeff_y =
    ff_hevc_last_significant_coeff_y_prefix_decode(s, c_idx, log2_trafo_size, entry);


    if (last_significant_coeff_x > 3) {
        int suffix = ff_hevc_last_significant_coeff_suffix_decode(s, last_significant_coeff_x, entry);
        last_significant_coeff_x = (1 << ((last_significant_coeff_x >> 1) - 1)) *
                                   (2 + (last_significant_coeff_x & 1)) +
                                   suffix;
    }
    if (last_significant_coeff_y > 3) {
        int suffix = ff_hevc_last_significant_coeff_suffix_decode(s, last_significant_coeff_y, entry);
        last_significant_coeff_y = (1 << ((last_significant_coeff_y >> 1) - 1)) *
                                   (2 + (last_significant_coeff_y & 1)) +
                                   suffix;
    }

    if (scan_idx == SCAN_VERT)
        FFSWAP(int, last_significant_coeff_x, last_significant_coeff_y);

    x_cg_last_sig = last_significant_coeff_x >> 2;
    y_cg_last_sig = last_significant_coeff_y >> 2;

    switch (scan_idx) {
    case SCAN_DIAG: {
        int last_x_c = last_significant_coeff_x & 3;
        int last_y_c = last_significant_coeff_y & 3;

        scan_x_off = diag_scan4x4_x;
        scan_y_off = diag_scan4x4_y;
        num_coeff = diag_scan4x4_inv[last_y_c][last_x_c];
        if (trafo_size == 4) {
            scan_x_cg = scan_1x1;
            scan_y_cg = scan_1x1;
        } else if (trafo_size == 8) {
            num_coeff += diag_scan2x2_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan2x2_x;
            scan_y_cg = diag_scan2x2_y;
        } else if (trafo_size == 16) {
            num_coeff += diag_scan4x4_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan4x4_x;
            scan_y_cg = diag_scan4x4_y;
        } else { // trafo_size == 32
            num_coeff += diag_scan8x8_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan8x8_x;
            scan_y_cg = diag_scan8x8_y;
        }
        break;
    }
    case SCAN_HORIZ:
        scan_x_cg = horiz_scan2x2_x;
        scan_y_cg = horiz_scan2x2_y;
        scan_x_off = horiz_scan4x4_x;
        scan_y_off = horiz_scan4x4_y;
        num_coeff = horiz_scan8x8_inv[last_significant_coeff_y][last_significant_coeff_x];
        break;
    default: //SCAN_VERT
        scan_x_cg = horiz_scan2x2_y;
        scan_y_cg = horiz_scan2x2_x;
        scan_x_off = horiz_scan4x4_y;
        scan_y_off = horiz_scan4x4_x;
        num_coeff = horiz_scan8x8_inv[last_significant_coeff_x][last_significant_coeff_y];
        break;
    }
    num_coeff++;

    num_last_subset = (num_coeff - 1) >> 4;

    for (i = num_last_subset; i >= 0; i--) {
        int n, m;
        int first_nz_pos_in_cg, last_nz_pos_in_cg, num_sig_coeff, first_greater1_coeff_idx;
        int sign_hidden;
        int sum_abs;
        int x_cg, y_cg, x_c, y_c;
        int implicit_non_zero_coeff = 0;
        int trans_coeff_level;

        int offset = i << 4;

        uint8_t significant_coeff_flag_idx[16] = {0};
        uint8_t coeff_abs_level_greater1_flag[16] = {0};
        uint8_t coeff_abs_level_greater2_flag[16] = {0};
        uint16_t coeff_sign_flag;
        uint8_t nb_significant_coeff_flag = 0;

        int first_elem;

        x_cg = scan_x_cg[i];
        y_cg = scan_y_cg[i];

        if ((i < num_last_subset) && (i > 0)) {
            s->rc[entry].significant_coeff_group_flag[x_cg][y_cg] =
            ff_hevc_significant_coeff_group_flag_decode(s, c_idx, x_cg, y_cg,
                                                        log2_trafo_size, entry);
            implicit_non_zero_coeff = 1;
        } else {
            s->rc[entry].significant_coeff_group_flag[x_cg][y_cg] =
            ((x_cg == x_cg_last_sig && y_cg == y_cg_last_sig) ||
             (x_cg == 0 && y_cg == 0));
        }

        last_scan_pos = num_coeff - offset - 1;

        if (i == num_last_subset) {
            n_end = last_scan_pos - 1;
            significant_coeff_flag_idx[0] = last_scan_pos;
            nb_significant_coeff_flag = 1;
        } else {
            n_end = 15;
        }

        for (n = n_end; n >= 0; n--) {
            GET_COORD(offset, n);

            if (s->rc[entry].significant_coeff_group_flag[x_cg][y_cg] &&
                (n > 0 || implicit_non_zero_coeff == 0)) {
                if (ff_hevc_significant_coeff_flag_decode(s, c_idx, x_c, y_c, log2_trafo_size, scan_idx, entry) == 1) {
                    significant_coeff_flag_idx[nb_significant_coeff_flag] = n;
                    nb_significant_coeff_flag = nb_significant_coeff_flag + 1;
                    implicit_non_zero_coeff = 0;
                }
            } else {
                int last_cg = (x_c == (x_cg << 2) && y_c == (y_cg << 2));
                if (last_cg && implicit_non_zero_coeff && s->rc[entry].significant_coeff_group_flag[x_cg][y_cg]) {
                    significant_coeff_flag_idx[nb_significant_coeff_flag] = n;
                    nb_significant_coeff_flag = nb_significant_coeff_flag + 1;
                }
            }

        }

        n_end = nb_significant_coeff_flag;

        first_nz_pos_in_cg = 16;
        last_nz_pos_in_cg = -1;
        num_sig_coeff = 0;
        first_greater1_coeff_idx = -1;
        for (m = 0; m < n_end; m++) {
            n = significant_coeff_flag_idx[m];
                if (num_sig_coeff < 8) {
                    coeff_abs_level_greater1_flag[n] =
                    ff_hevc_coeff_abs_level_greater1_flag_decode(s, c_idx, i, n,
                                                                 (num_sig_coeff == 0),
                                                                 (i == num_last_subset), entry);
                    num_sig_coeff++;
                    if (coeff_abs_level_greater1_flag[n] &&
                        first_greater1_coeff_idx == -1)
                        first_greater1_coeff_idx = n;
                }
                if (last_nz_pos_in_cg == -1)
                    last_nz_pos_in_cg = n;
                first_nz_pos_in_cg = n;
            }

        sign_hidden = (last_nz_pos_in_cg - first_nz_pos_in_cg >= 4 &&
                       !s->cu.cu_transquant_bypass_flag[entry]);
        if (first_greater1_coeff_idx != -1) {
            coeff_abs_level_greater2_flag[first_greater1_coeff_idx] =
            ff_hevc_coeff_abs_level_greater2_flag_decode(s, c_idx, i, first_greater1_coeff_idx, entry);
        }
        if (!s->pps->sign_data_hiding_flag || !sign_hidden ) {
            coeff_sign_flag = ff_hevc_coeff_sign_flag(s, nb_significant_coeff_flag, entry) << (16 - nb_significant_coeff_flag);
        } else {
            coeff_sign_flag = ff_hevc_coeff_sign_flag(s, nb_significant_coeff_flag-1, entry) << (16 - (nb_significant_coeff_flag - 1));
        }

        num_sig_coeff = 0;
        sum_abs = 0;
        first_elem = 1;
        for (m = 0; m < n_end; m++) {
            n = significant_coeff_flag_idx[m];
                GET_COORD(offset, n);
                trans_coeff_level = 1 + coeff_abs_level_greater1_flag[n] +
                                    coeff_abs_level_greater2_flag[n];
                if (trans_coeff_level == ((num_sig_coeff < 8) ?
                                          ((n == first_greater1_coeff_idx) ? 3 : 2) : 1)) {
                    trans_coeff_level += ff_hevc_coeff_abs_level_remaining(s, first_elem, trans_coeff_level, entry);
                    first_elem = 0;
                }
                if (s->pps->sign_data_hiding_flag && sign_hidden) {
                    sum_abs += trans_coeff_level;
                    if (n == first_nz_pos_in_cg && ((sum_abs&1) == 1))
                        trans_coeff_level = -trans_coeff_level;
                }
                if (coeff_sign_flag >> 15)
                    trans_coeff_level = -trans_coeff_level;
                coeff_sign_flag <<= 1;
                num_sig_coeff++;
                coeffs[y_c * trafo_size + x_c] = trans_coeff_level;

            }
        }

    if (s->cu.cu_transquant_bypass_flag[entry]) {
        s->hevcdsp.transquant_bypass[log2_trafo_size-2](dst, coeffs, stride);
    } else {
        int qp;
        int qp_y = s->qp_y[entry];
        static int qp_c[] = { 29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37 };
        if (c_idx == 0) {
            qp = qp_y + s->sps->qp_bd_offset;
        } else {
            int qp_i, offset;

            if (c_idx == 1) {
                offset = s->pps->cb_qp_offset + s->sh.slice_cb_qp_offset;
            } else {
                offset = s->pps->cr_qp_offset + s->sh.slice_cr_qp_offset;
            }
            qp_i = av_clip_c(qp_y + offset, - s->sps->qp_bd_offset, 57);
            if (qp_i < 30) {
                qp = qp_i;
            } else if (qp_i > 43) {
                qp = qp_i - 6;
            } else {
                qp = qp_c[qp_i - 30];
            }

            qp += s->sps->qp_bd_offset;

        }
        s->hevcdsp.dequant[log2_trafo_size-2](coeffs, qp);
        if (transform_skip_flag) {
            s->hevcdsp.transform_skip(dst, coeffs, stride);
        } else if (s->cu.pred_mode[entry] == MODE_INTRA && c_idx == 0 && log2_trafo_size == 2) {
            s->hevcdsp.transform_4x4_luma_add(dst, coeffs, stride);
        } else {
            s->hevcdsp.transform_add[log2_trafo_size-2](dst, coeffs, stride);
        }
    }
}

static void hls_transform_unit(HEVCContext *s, int x0, int  y0, int xBase, int yBase, int cb_xBase, int cb_yBase,
                               int log2_trafo_size, int trafo_depth, int blk_idx, int entry) {
    int scan_idx = SCAN_DIAG;
    int scan_idx_c = SCAN_DIAG;
    if (s->cu.pred_mode[entry] == MODE_INTRA) {
        s->hpc.intra_pred(s, x0, y0, log2_trafo_size, 0, entry);
        if (log2_trafo_size > 2) {
            s->hpc.intra_pred(s, x0, y0, log2_trafo_size - 1, 1, entry);
            s->hpc.intra_pred(s, x0, y0, log2_trafo_size - 1, 2, entry);
        } else if (blk_idx == 3) {
            s->hpc.intra_pred(s, xBase, yBase, log2_trafo_size, 1, entry);
            s->hpc.intra_pred(s, xBase, yBase, log2_trafo_size, 2, entry);
        }
    }

    if (s->tt[entry].cbf_luma ||
        SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0) ||
        SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0)) {
        if (s->pps->cu_qp_delta_enabled_flag && !s->tu[entry].is_cu_qp_delta_coded) {
            s->tu[entry].cu_qp_delta = ff_hevc_cu_qp_delta_abs(s, entry);
            if (s->tu[entry].cu_qp_delta != 0)
                if (ff_hevc_cu_qp_delta_sign_flag(s, entry) == 1)
                    s->tu[entry].cu_qp_delta = -s->tu[entry].cu_qp_delta;
            s->tu[entry].is_cu_qp_delta_coded = 1;
            ff_hevc_set_qPy(s, x0, y0, cb_xBase, cb_yBase, entry);
        }

        if (s->cu.pred_mode[entry] == MODE_INTRA && log2_trafo_size < 4) {
            if (s->tu[entry].cur_intra_pred_mode >= 6 &&
                s->tu[entry].cur_intra_pred_mode <= 14) {
                scan_idx = SCAN_VERT;
            } else if (s->tu[entry].cur_intra_pred_mode >= 22 &&
                       s->tu[entry].cur_intra_pred_mode <= 30) {
                scan_idx = SCAN_HORIZ;
            }

            if (s->pu.intra_pred_mode_c[entry] >= 6 &&
                s->pu.intra_pred_mode_c[entry] <= 14) {
                scan_idx_c = SCAN_VERT;
            } else if (s->pu.intra_pred_mode_c[entry] >= 22 &&
                       s->pu.intra_pred_mode_c[entry] <= 30) {
                scan_idx_c = SCAN_HORIZ;
            }
        }

        if (s->tt[entry].cbf_luma)
            hls_residual_coding(s, x0, y0, log2_trafo_size, scan_idx, 0, entry);
        if (log2_trafo_size > 2) {
            if (SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0))
                hls_residual_coding(s, x0, y0, log2_trafo_size - 1, scan_idx_c, 1, entry);
            if (SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0))
                hls_residual_coding(s, x0, y0, log2_trafo_size - 1, scan_idx_c, 2, entry);
        } else if (blk_idx == 3) {
            if (SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], xBase, yBase))
                hls_residual_coding(s, xBase, yBase, log2_trafo_size, scan_idx_c, 1, entry);
            if (SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], xBase, yBase))
                hls_residual_coding(s, xBase, yBase, log2_trafo_size, scan_idx_c, 2, entry);
        }
    }
}

static void hls_transform_tree(HEVCContext *s, int x0, int y0, int xBase, int yBase, int cb_xBase, int cb_yBase,
                               int log2_cb_size, int log2_trafo_size, int trafo_depth, int blk_idx, int entry)
{
    uint8_t split_transform_flag;
    if (trafo_depth > 0 && log2_trafo_size == 2) {
        SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0) =
        SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth - 1], xBase, yBase);
        SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0) =
        SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth - 1], xBase, yBase);
    } else {
        SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0) =
        SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0) = 0;
    }

    if (s->cu.intra_split_flag[entry]) {
        if (trafo_depth == 1)
            s->tu[entry].cur_intra_pred_mode = s->pu.intra_pred_mode[entry][blk_idx];
    } else {
        s->tu[entry].cur_intra_pred_mode = s->pu.intra_pred_mode[entry][0];
    }

    s->tt[entry].cbf_luma = 1;

    s->tt[entry].inter_split_flag = (s->sps->max_transform_hierarchy_depth_inter == 0 &&
                              s->cu.pred_mode[entry] == MODE_INTER &&
                              s->cu.part_mode[entry] != PART_2Nx2N && trafo_depth == 0);

    if (log2_trafo_size <= s->sps->log2_max_trafo_size &&
        log2_trafo_size > s->sps->log2_min_transform_block_size &&
        trafo_depth < s->cu.max_trafo_depth[entry] &&
        !(s->cu.intra_split_flag[entry] && trafo_depth == 0)) {
        split_transform_flag =
        ff_hevc_split_transform_flag_decode(s, log2_trafo_size, entry);
    } else {
        split_transform_flag =
        (log2_trafo_size > s->sps->log2_max_trafo_size ||
         (s->cu.intra_split_flag[entry] && (trafo_depth == 0)) ||
         s->tt[entry].inter_split_flag);
    }

    if (log2_trafo_size > 2) {
        if (trafo_depth == 0 || SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth - 1], xBase, yBase)) {
            SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0) =
            ff_hevc_cbf_cb_cr_decode(s, trafo_depth, entry);
        }
        if (trafo_depth == 0 || SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth - 1], xBase, yBase)) {
            SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0) =
            ff_hevc_cbf_cb_cr_decode(s, trafo_depth, entry);
        }
    }

    if (split_transform_flag) {
        int x1 = x0 + (( 1 << log2_trafo_size ) >> 1);
        int y1 = y0 + (( 1 << log2_trafo_size ) >> 1);

        hls_transform_tree(s, x0, y0, x0, y0, cb_xBase, cb_yBase, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 0, entry);
        hls_transform_tree(s, x1, y0, x0, y0, cb_xBase, cb_yBase, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 1, entry);
        hls_transform_tree(s, x0, y1, x0, y0, cb_xBase, cb_yBase, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 2, entry);
        hls_transform_tree(s, x1, y1, x0, y0, cb_xBase, cb_yBase, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 3, entry);
    } else {
        int i,j;
        int min_pu_size = 1 << s->sps->log2_min_pu_size;
        int log2_min_pu_size = s->sps->log2_min_pu_size;
        int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;
        if (s->cu.pred_mode[entry] == MODE_INTRA || trafo_depth != 0 ||
            SAMPLE_CBF(s->tt[entry].cbf_cb[trafo_depth], x0, y0) ||
            SAMPLE_CBF(s->tt[entry].cbf_cr[trafo_depth], x0, y0)) {
            s->tt[entry].cbf_luma = ff_hevc_cbf_luma_decode(s, trafo_depth, entry);
        }

        hls_transform_unit(s, x0, y0, xBase, yBase, cb_xBase, cb_yBase,
                           log2_trafo_size, trafo_depth, blk_idx, entry);

        // TODO: store cbf_luma somewhere else
        if (s->tt[entry].cbf_luma)
            for (i = 0; i < (1<<log2_trafo_size); i += min_pu_size)
                for (j = 0; j < (1<<log2_trafo_size); j += min_pu_size) {
                    int x_pu = (x0 + j) >> log2_min_pu_size;
                    int y_pu = (y0 + i) >> log2_min_pu_size;
                    s->cbf_luma[y_pu * pic_width_in_min_pu + x_pu] = 1;
                }
        ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_trafo_size);
    }
}

static void hls_pcm_sample(HEVCContext *s, int x0, int y0, int log2_cb_size, int entry)
{
    //TODO: non-4:2:0 support
    int i, j;
    int log2_min_pu_size = s->sps->log2_min_pu_size - 1;
    int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;
    GetBitContext gb;
    int cb_size = 1 << log2_cb_size;
    int stride0 = s->frame->linesize[0];
    uint8_t *dst0 = &s->frame->data[0][y0 * stride0 + x0];
    int stride1 = s->frame->linesize[1];
    uint8_t *dst1 = &s->frame->data[1][(y0 >> s->sps->vshift[1]) * stride1 + (x0 >> s->sps->hshift[1])];
    int stride2 = s->frame->linesize[2];
    uint8_t *dst2 = &s->frame->data[2][(y0 >> s->sps->vshift[2]) * stride2 + (x0 >> s->sps->hshift[2])];

    int length = cb_size * cb_size * 3 / 2 * s->sps->pcm.bit_depth;
    const uint8_t *pcm = skip_bytes(s->cc[entry], length >> 3);

    for (j = y0 >> log2_min_pu_size; j < ((y0 + cb_size) >> log2_min_pu_size); j++)
        for (i = x0 >> log2_min_pu_size; i < ((x0 + cb_size) >> log2_min_pu_size); i++)
            s->is_pcm[i + j * pic_width_in_min_pu] = 1;
    if((y0 & 7) == 0)
        for(i = 0; i < cb_size; i+=4)
            s->horizontal_bs[((x0 + i) + y0 * s->bs_width) >> 2] = 2;
    if((x0 & 7) == 0)
        for(i = 0; i < cb_size; i+=4)
            s->vertical_bs[(x0 >> 3) + ((y0 + i) * s->bs_width) >> 2] = 2;

    init_get_bits(&gb, pcm, length);

    s->hevcdsp.put_pcm(dst0, stride0, cb_size, &gb, s->sps->pcm.bit_depth);
    s->hevcdsp.put_pcm(dst1, stride1, cb_size/2, &gb, s->sps->pcm.bit_depth);
    s->hevcdsp.put_pcm(dst2, stride2, cb_size/2, &gb, s->sps->pcm.bit_depth);
}

static void hls_mvd_coding(HEVCContext *s, int x0, int y0, int log2_cb_size, int entry)
{
    int x = ff_hevc_abs_mvd_greater0_flag_decode(s, entry);
    int y = ff_hevc_abs_mvd_greater0_flag_decode(s, entry);

    if (x)
        x += ff_hevc_abs_mvd_greater1_flag_decode(s, entry);
    if (y)
        y += ff_hevc_abs_mvd_greater1_flag_decode(s, entry);

    switch (x) {
        case 2:
            s->pu.mvd[entry].x = ff_hevc_mvd_decode(s, entry);
            break;
        case 1:
            s->pu.mvd[entry].x = ff_hevc_mvd_sign_flag_decode(s,entry);
            break;
        case 0:
            s->pu.mvd[entry].x = 0;
    }

    switch (y) {
        case 2:
            s->pu.mvd[entry].y = ff_hevc_mvd_decode(s, entry);
            break;
        case 1:
            s->pu.mvd[entry].y = ff_hevc_mvd_sign_flag_decode(s,entry);
            break;
        case 0:
            s->pu.mvd[entry].y = 0;
    }
    return;
}

/**
 * 8.5.3.2.2.1 Luma sample interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 */
static void luma_mc(HEVCContext *s, int16_t *dst, ptrdiff_t dststride, AVFrame *ref,
                    const Mv *mv, int x_off, int y_off, int block_w, int block_h, int entry)
{
    uint8_t *src = ref->data[0];
    ptrdiff_t srcstride = ref->linesize[0];

    int pic_width = s->sps->pic_width_in_luma_samples;
    int pic_height = s->sps->pic_height_in_luma_samples;

    int mx = mv->x & 3;
    int my = mv->y & 3;
    int extra_left = qpel_extra_before[mx];
    int extra_top = qpel_extra_before[my];

    x_off += mv->x >> 2;
    y_off += mv->y >> 2;
    src += y_off * srcstride + (x_off << s->sps->pixel_shift);

    if (x_off < extra_left || x_off >= pic_width - block_w - qpel_extra_after[mx] ||
        y_off < extra_top || y_off >= pic_height - block_h - qpel_extra_after[my]) {
        int offset = extra_top * srcstride + (extra_left << s->sps->pixel_shift);
        s->vdsp.emulated_edge_mc(s->edge_emu_buffer[entry], src - offset, srcstride,
                                block_w + qpel_extra[mx], block_h + qpel_extra[my],
                                x_off - extra_left, y_off - extra_top,
                                pic_width, pic_height);
        src = s->edge_emu_buffer[entry] + offset;
    }
    s->hevcdsp.put_hevc_qpel[my][mx](dst, dststride, src, srcstride, block_w, block_h);
}

/**
 * 8.5.3.2.2.2 Chroma sample interpolation process
 *
 * @param s HEVC decoding context
 * @param dst1 target buffer for block data at block position (U plane)
 * @param dst2 target buffer for block data at block position (V plane)
 * @param dststride stride of the dst1 and dst2 buffers
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 */
static void chroma_mc(HEVCContext *s, int16_t *dst1, int16_t *dst2, ptrdiff_t dststride, AVFrame *ref,
                      const Mv *mv, int x_off, int y_off, int block_w, int block_h, int entry)
{
    uint8_t *src1 = ref->data[1];
    uint8_t *src2 = ref->data[2];
    ptrdiff_t src1stride = ref->linesize[1];
    ptrdiff_t src2stride = ref->linesize[2];

    int pic_width = s->sps->pic_width_in_luma_samples >> 1;
    int pic_height = s->sps->pic_height_in_luma_samples >> 1;

    int mx = mv->x & 7;
    int my = mv->y & 7;

    x_off += mv->x >> 3;
    y_off += mv->y >> 3;
    src1 += y_off * src1stride + (x_off << s->sps->pixel_shift);
    src2 += y_off * src2stride + (x_off << s->sps->pixel_shift);

    if (x_off < epel_extra_before || x_off >= pic_width - block_w - epel_extra_after ||
        y_off < epel_extra_after || y_off >= pic_height - block_h - epel_extra_after) {
        int offset1 = epel_extra_before * (src1stride + (1 << s->sps->pixel_shift));
        int offset2 = epel_extra_before * (src2stride + (1 << s->sps->pixel_shift));
        s->vdsp.emulated_edge_mc(s->edge_emu_buffer[entry], src1 - offset1, src1stride,
                                block_w + epel_extra, block_h + epel_extra,
                                x_off - epel_extra_before, y_off - epel_extra_before,
                                pic_width, pic_height);
        src1 = s->edge_emu_buffer[entry] + offset1;
        s->hevcdsp.put_hevc_epel[!!my][!!mx](dst1, dststride, src1, src1stride, block_w, block_h, mx, my);

        s->vdsp.emulated_edge_mc(s->edge_emu_buffer[entry], src2 - offset2, src2stride,
                                block_w + epel_extra, block_h + epel_extra,
                                x_off - epel_extra_before, y_off - epel_extra_before,
                                pic_width, pic_height);
        src2 = s->edge_emu_buffer[entry] + offset2;
        s->hevcdsp.put_hevc_epel[!!my][!!mx](dst2, dststride, src2, src2stride, block_w, block_h, mx, my);
    } else {
        s->hevcdsp.put_hevc_epel[!!my][!!mx](dst1, dststride, src1, src1stride, block_w, block_h, mx, my);
        s->hevcdsp.put_hevc_epel[!!my][!!mx](dst2, dststride, src2, src2stride, block_w, block_h, mx, my);
    }
}
/*
static int identical_mvs(MvField *mv, RefPicList *refPicList) {
    if (mv->pred_flag[0] + mv->pred_flag[1] == 2)
        return (refPicList[0].list[mv->ref_idx[0]] == refPicList[1].list[mv->ref_idx[1]] && mv->mv[0].x == mv->mv[1].x && mv->mv[0].y == mv->mv[1].y);
    else
        return 0;
}
*/
static void hls_prediction_unit(HEVCContext *s, int x0, int y0, int nPbW, int nPbH, int log2_cb_size, int partIdx, int entry)
{
#define POS(c_idx, x, y)                                                              \
    &s->frame->data[c_idx][((y) >> s->sps->vshift[c_idx]) * s->frame->linesize[c_idx] + \
                           (((x) >> s->sps->hshift[c_idx]) << s->sps->pixel_shift)]
    int merge_idx = 0;
    enum InterPredIdc inter_pred_idc = PRED_L0;
    int ref_idx[2];
    int mvp_flag[2];
    struct MvField current_mv = {{{ 0 }}};
    int i, j;
    int x_pu, y_pu;
    int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;
    MvField *tab_mvf = s->ref->tab_mvf;
    RefPicList  *refPicList =  s->ref->refPicList;

    int tmpstride = MAX_PB_SIZE;

    uint8_t *dst0 = POS(0, x0, y0);
    uint8_t *dst1 = POS(1, x0, y0);
    uint8_t *dst2 = POS(2, x0, y0);
    int log2_min_cb_size = s->sps->log2_min_coding_block_size;
    int pic_width_in_ctb = s->sps->pic_width_in_luma_samples>>log2_min_cb_size;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;

    if (SAMPLE_CTB(s->cu.skip_flag, x_cb, y_cb)) {
        if (s->sh.max_num_merge_cand > 1)
            merge_idx = ff_hevc_merge_idx_decode(s, entry);
        else
            merge_idx = 0;
            
        ff_hevc_luma_mv_merge_mode(s, x0, y0, 1 << log2_cb_size, 1 << log2_cb_size, log2_cb_size, partIdx, merge_idx, &current_mv, entry);
        x_pu = x0 >> s->sps->log2_min_pu_size;
        y_pu = y0 >> s->sps->log2_min_pu_size;
        for(i = 0; i < nPbW >> s->sps->log2_min_pu_size; i++) {
            for(j = 0; j < nPbH >> s->sps->log2_min_pu_size; j++) {
                tab_mvf[(y_pu + j) * pic_width_in_min_pu + x_pu + i] = current_mv;
            }
        }

    } else {/* MODE_INTER */
        s->pu.merge_flag[entry] = ff_hevc_merge_flag_decode(s, entry);
        if (s->pu.merge_flag[entry]) {
            if (s->sh.max_num_merge_cand > 1)
                merge_idx = ff_hevc_merge_idx_decode(s, entry);
            else
                merge_idx = 0;

            ff_hevc_luma_mv_merge_mode(s, x0, y0, nPbW, nPbH, log2_cb_size, partIdx, merge_idx, &current_mv, entry);
            x_pu = x0 >> s->sps->log2_min_pu_size;
            y_pu = y0 >> s->sps->log2_min_pu_size;
            for(i = 0; i < nPbW >> s->sps->log2_min_pu_size; i++) {
                for(j = 0; j < nPbH >> s->sps->log2_min_pu_size; j++) {
                    tab_mvf[(y_pu + j) * pic_width_in_min_pu + x_pu + i] = current_mv;
                }
            }
        } else {
            if (s->sh.slice_type == B_SLICE) {
                inter_pred_idc = ff_hevc_inter_pred_idc_decode(s, nPbW, nPbH, entry);
            }
            if (inter_pred_idc != PRED_L1) {
                if (s->sh.num_ref_idx_l0_active > 1) {
                    ref_idx[0] = ff_hevc_ref_idx_lx_decode(s, s->sh.num_ref_idx_l0_active, entry);
                    current_mv.ref_idx[0] = ref_idx[0];
                }
                current_mv.pred_flag[0] = 1;
                hls_mvd_coding(s, x0, y0, 0 , entry);
                mvp_flag[0] = ff_hevc_mvp_lx_flag_decode(s, entry);
                ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size, partIdx, merge_idx, &current_mv, mvp_flag[0], 0, entry);
                current_mv.mv[0].x += s->pu.mvd[entry].x;
                current_mv.mv[0].y += s->pu.mvd[entry].y;
            }
            if (inter_pred_idc != PRED_L0) {
                if (s->sh.num_ref_idx_l1_active > 1) {
                    ref_idx[1] = ff_hevc_ref_idx_lx_decode(s, s->sh.num_ref_idx_l1_active, entry);
                    current_mv.ref_idx[1] = ref_idx[1];
                }
                if (s->sh.mvd_l1_zero_flag == 1 && inter_pred_idc == PRED_BI) {
                    s->pu.mvd[entry].x = 0;
                    s->pu.mvd[entry].y = 0;
                    //mvd_l1[ x0 ][ y0 ][ 0 ] = 0
                    //mvd_l1[ x0 ][ y0 ][ 1 ] = 0
                } else {
                    hls_mvd_coding(s, x0, y0, 1, entry);
                }
                current_mv.pred_flag[1] = 1;
                mvp_flag[1] = ff_hevc_mvp_lx_flag_decode(s, entry);
                ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size, partIdx, merge_idx, &current_mv, mvp_flag[1], 1, entry);
                current_mv.mv[1].x += s->pu.mvd[entry].x;
                current_mv.mv[1].y += s->pu.mvd[entry].y;
            }
            x_pu = x0 >> s->sps->log2_min_pu_size;
            y_pu = y0 >> s->sps->log2_min_pu_size;
            for(i = 0; i < nPbW >> s->sps->log2_min_pu_size; i++) {
                for(j = 0; j < nPbH >> s->sps->log2_min_pu_size; j++) {
                    tab_mvf[(y_pu + j) * pic_width_in_min_pu + x_pu + i] = current_mv;
                }
            }
        }
    }
    if (current_mv.pred_flag[0] && !current_mv.pred_flag[1]) {
#ifdef USE_AVX
        DECLARE_ALIGNED( 32, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 32, int16_t, tmp2[MAX_PB_SIZE * MAX_PB_SIZE] );
#else
        DECLARE_ALIGNED( 16, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 16, int16_t, tmp2[MAX_PB_SIZE * MAX_PB_SIZE] );
#endif
        if (! s->pps->weighted_pred_flag){
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.put_unweighted_pred(dst0, s->frame->linesize[0], tmp, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                      s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                      &current_mv.mv[0], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            s->hevcdsp.put_unweighted_pred(dst1, s->frame->linesize[1], tmp, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.put_unweighted_pred(dst2, s->frame->linesize[2], tmp2, tmpstride, nPbW/2, nPbH/2);
        } else {
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.weighted_pred(s->sh.luma_log2_weight_denom,
                                          s->sh.luma_weight_l0[current_mv.ref_idx[0]],
                                          s->sh.luma_offset_l0[current_mv.ref_idx[0]],
                                          dst0, s->frame->linesize[0], tmp, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                      s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                      &current_mv.mv[0], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            s->hevcdsp.weighted_pred(s->sh.chroma_log2_weight_denom,
                                            s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0],
                                            s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0],
                                            dst1, s->frame->linesize[1], tmp, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.weighted_pred(s->sh.chroma_log2_weight_denom,
                                            s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1],
                                            s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1],
                                            dst2, s->frame->linesize[2], tmp2, tmpstride, nPbW/2, nPbH/2);
        }

    } else if (!current_mv.pred_flag[0] && current_mv.pred_flag[1]) {

#ifdef USE_AVX
        DECLARE_ALIGNED( 32, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 32, int16_t, tmp2[MAX_PB_SIZE * MAX_PB_SIZE] );
#else
        DECLARE_ALIGNED( 16, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 16, int16_t, tmp2[MAX_PB_SIZE * MAX_PB_SIZE] );
#endif
        if (! s->pps->weighted_pred_flag){
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.put_unweighted_pred(dst0, s->frame->linesize[0], tmp, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                      s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                      &current_mv.mv[1], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            s->hevcdsp.put_unweighted_pred(dst1, s->frame->linesize[1], tmp, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.put_unweighted_pred(dst2, s->frame->linesize[2], tmp2, tmpstride, nPbW/2, nPbH/2);
        } else {
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.weighted_pred(s->sh.luma_log2_weight_denom,
                                          s->sh.luma_weight_l1[current_mv.ref_idx[1]],
                                          s->sh.luma_offset_l1[current_mv.ref_idx[1]],
                                          dst0, s->frame->linesize[0], tmp, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                     s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                      &current_mv.mv[1], x0/2, y0/2, nPbW/2, nPbH/2, entry);

            s->hevcdsp.weighted_pred(s->sh.chroma_log2_weight_denom,
                                            s->sh.chroma_weight_l1[current_mv.ref_idx[1]][0],
                                            s->sh.chroma_offset_l1[current_mv.ref_idx[1]][0],
                                            dst1, s->frame->linesize[1], tmp, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.weighted_pred(s->sh.chroma_log2_weight_denom,
                                            s->sh.chroma_weight_l1[current_mv.ref_idx[1]][1],
                                            s->sh.chroma_offset_l1[current_mv.ref_idx[1]][1],
                                            dst2, s->frame->linesize[2], tmp2, tmpstride, nPbW/2, nPbH/2);
        }

    } else if (current_mv.pred_flag[0] && current_mv.pred_flag[1]) {
#ifdef USE_AVX
    	DECLARE_ALIGNED( 32, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
    	        DECLARE_ALIGNED( 32, int16_t, tmp2[MAX_PB_SIZE*MAX_PB_SIZE] );
    	        DECLARE_ALIGNED( 32, int16_t, tmp3[MAX_PB_SIZE*MAX_PB_SIZE] );
    	        DECLARE_ALIGNED( 32, int16_t, tmp4[MAX_PB_SIZE*MAX_PB_SIZE] );
#else
        DECLARE_ALIGNED( 16, int16_t, tmp[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 16, int16_t, tmp2[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 16, int16_t, tmp3[MAX_PB_SIZE*MAX_PB_SIZE] );
        DECLARE_ALIGNED( 16, int16_t, tmp4[MAX_PB_SIZE*MAX_PB_SIZE] );
#endif
        if (! s->pps->weighted_bipred_flag){
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH, entry);
            luma_mc(s, tmp2, tmpstride,
                    s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.put_weighted_pred_avg(dst0, s->frame->linesize[0], tmp, tmp2, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                      s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                      &current_mv.mv[0], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            chroma_mc(s, tmp3, tmp4, tmpstride,
                      s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                      &current_mv.mv[1], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            s->hevcdsp.put_weighted_pred_avg(dst1, s->frame->linesize[1], tmp, tmp3, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.put_weighted_pred_avg(dst2, s->frame->linesize[2], tmp2, tmp4, tmpstride, nPbW/2, nPbH/2);
        } else {
            luma_mc(s, tmp, tmpstride,
                    s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH, entry);
            luma_mc(s, tmp2, tmpstride,
                    s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH, entry);
            s->hevcdsp.weighted_pred_avg(s->sh.luma_log2_weight_denom,
                                              s->sh.luma_weight_l0[current_mv.ref_idx[0]],
                                              s->sh.luma_weight_l1[current_mv.ref_idx[1]],
                                              s->sh.luma_offset_l0[current_mv.ref_idx[0]],
                                              s->sh.luma_offset_l1[current_mv.ref_idx[1]],
                                              dst0, s->frame->linesize[0], tmp, tmp2, tmpstride, nPbW, nPbH);
            chroma_mc(s, tmp, tmp2, tmpstride,
                      s->DPB[refPicList[0].idx[current_mv.ref_idx[0]]].frame,
                      &current_mv.mv[0], x0/2, y0/2, nPbW/2, nPbH/2, entry);
            chroma_mc(s, tmp3, tmp4, tmpstride,
                      s->DPB[refPicList[1].idx[current_mv.ref_idx[1]]].frame,
                      &current_mv.mv[1], x0/2, y0/2, nPbW/2, nPbH/2, entry);

            s->hevcdsp.weighted_pred_avg(s->sh.chroma_log2_weight_denom ,
                                                s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0],
                                                s->sh.chroma_weight_l1[current_mv.ref_idx[1]][0],
                                                s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0],
                                                s->sh.chroma_offset_l1[current_mv.ref_idx[1]][0],
                                                dst1, s->frame->linesize[1], tmp, tmp3, tmpstride, nPbW/2, nPbH/2);
            s->hevcdsp.weighted_pred_avg(s->sh.chroma_log2_weight_denom ,
                                                s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1],
                                                s->sh.chroma_weight_l1[current_mv.ref_idx[1]][1],
                                                s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1],
                                                s->sh.chroma_offset_l1[current_mv.ref_idx[1]][1],
                                                dst2, s->frame->linesize[2], tmp2, tmp4, tmpstride, nPbW/2, nPbH/2);
        }
    }
    return;
}


/**
 * 8.4.1
 */
static int luma_intra_pred_mode(HEVCContext *s, int x0, int y0, int pu_size,
                                int prev_intra_luma_pred_flag, int entry)
{
    int i, j;
    int candidate[3];
    int intra_pred_mode;

    int x_pu = x0 >> s->sps->log2_min_pu_size;
    int y_pu = y0 >> s->sps->log2_min_pu_size;
    int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs << 2;
    int size_in_pus = pu_size >> s->sps->log2_min_pu_size;
    int x0b = x0 & ((1 << s->sps->log2_ctb_size) - 1);
    int y0b = y0 & ((1 << s->sps->log2_ctb_size) - 1);

    int cand_up   = (s->ctb_up_flag[entry] || y0b) ? s->pu.top_ipm[x_pu] : INTRA_DC ;
    int cand_left = (s->ctb_left_flag[entry] || x0b) ? s->pu.left_ipm[y_pu] : INTRA_DC ;

    int y_ctb = (y0 >> (s->sps->log2_ctb_size)) << (s->sps->log2_ctb_size);
    MvField *tab_mvf = s->ref->tab_mvf;

    // intra_pred_mode prediction does not cross vertical CTB boundaries
    if ((y0 - 1) < y_ctb)
        cand_up = INTRA_DC;


    if (cand_left == cand_up) {
        if (cand_left < 2) {
            candidate[0] = INTRA_PLANAR;
            candidate[1] = INTRA_DC;
            candidate[2] = INTRA_ANGULAR_26;
        } else {
            candidate[0] = cand_left;
            candidate[1] = 2 + ((cand_left - 2 - 1 + 32) & 31);
            candidate[2] = 2 + ((cand_left - 2 + 1) & 31);
        }
    } else {
        candidate[0] = cand_left;
        candidate[1] = cand_up;
        if (candidate[0] != INTRA_PLANAR && candidate[1] != INTRA_PLANAR) {
            candidate[2] = INTRA_PLANAR;
        } else if (candidate[0] != INTRA_DC && candidate[1] != INTRA_DC) {
            candidate[2] = INTRA_DC;
        } else {
            candidate[2] = INTRA_ANGULAR_26;
        }
    }

    if (prev_intra_luma_pred_flag) {
        intra_pred_mode = candidate[s->pu.mpm_idx[entry]];
    } else {
        if (candidate[0] > candidate[1])
            FFSWAP(uint8_t, candidate[0], candidate[1]);
        if (candidate[0] > candidate[2])
            FFSWAP(uint8_t, candidate[0], candidate[2]);
        if (candidate[1] > candidate[2])
            FFSWAP(uint8_t, candidate[1], candidate[2]);

        intra_pred_mode = s->pu.rem_intra_luma_pred_mode[entry];
        for (i = 0; i < 3; i++) {
            if (intra_pred_mode >= candidate[i])
                intra_pred_mode++;
        }
    }
    memset(&s->pu.top_ipm[x_pu], intra_pred_mode, size_in_pus);
    memset(&s->pu.left_ipm[y_pu], intra_pred_mode, size_in_pus);
    
    /* write the intra prediction units into the mv array */
    for(i = 0; i <size_in_pus; i++) {
        for(j = 0; j <size_in_pus; j++) {
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].is_intra = 1;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].pred_flag[0] = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].pred_flag[1] = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].ref_idx[0] = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].ref_idx[1] = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].mv[0].x = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].mv[0].y = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].mv[1].x = 0;
            tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+i].mv[1].y = 0;
        }
    }

    return intra_pred_mode;
}

static av_always_inline void set_ct_depth(HEVCContext *s, int x0, int y0,
                                          int log2_cb_size, int ct_depth)
{
    int length = (1 << log2_cb_size) >> s->sps->log2_min_coding_block_size;
    int x_cb = x0 >> s->sps->log2_min_coding_block_size;
    int y_cb = y0 >> s->sps->log2_min_coding_block_size;

    memset(&s->cu.top_ct_depth[x_cb], ct_depth, length);
    memset(&s->cu.left_ct_depth[y_cb], ct_depth, length);
}

static void intra_prediction_unit(HEVCContext *s, int x0, int y0, int log2_cb_size, int entry)
{
    int i, j;
    uint8_t prev_intra_luma_pred_flag[4];
    int chroma_mode;
    static const uint8_t intra_chroma_table[4] = {0, 26, 10, 1};

    int split = s->cu.part_mode[entry] == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side = split + 1;

    for (i = 0; i < side; i++)
        for (j = 0; j < side; j++) {
            prev_intra_luma_pred_flag[2*i+j] = ff_hevc_prev_intra_luma_pred_flag_decode(s, entry);
        }

    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            if (prev_intra_luma_pred_flag[2*i+j]) {
                s->pu.mpm_idx[entry] = ff_hevc_mpm_idx_decode(s, entry);
            } else {
                s->pu.rem_intra_luma_pred_mode[entry] = ff_hevc_rem_intra_luma_pred_mode_decode(s, entry);
            }
            s->pu.intra_pred_mode[entry][2*i+j] =
            luma_intra_pred_mode(s, x0 + pb_size * j, y0 + pb_size * i, pb_size,
                                 prev_intra_luma_pred_flag[2*i+j], entry);
        }
    }

    chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s, entry);
    if (chroma_mode != 4) {
        if (s->pu.intra_pred_mode[entry][0] == intra_chroma_table[chroma_mode]) {
            s->pu.intra_pred_mode_c[entry] = 34;
        } else {
            s->pu.intra_pred_mode_c[entry] = intra_chroma_table[chroma_mode];
        }
    } else {
        s->pu.intra_pred_mode_c[entry] = s->pu.intra_pred_mode[entry][0];
    }

}
static void intra_prediction_unit_default_value(HEVCContext *s, int x0, int y0, int log2_cb_size, int entry)
{
    int i, j, k;
    int split = s->cu.part_mode[entry] == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side = split + 1;
    int size_in_pus = pb_size >> s->sps->log2_min_pu_size;
    int pic_width_in_min_pu  = s->sps->pic_width_in_min_cbs * 4;
    MvField *tab_mvf = s->ref->tab_mvf;
    for (i = 0; i < side; i++) {
        int x_pu = (x0 + pb_size * i) >> s->sps->log2_min_pu_size;
        int y_pu = (y0 + pb_size * i) >> s->sps->log2_min_pu_size;
        memset(&s->pu.top_ipm[x_pu], INTRA_DC, size_in_pus);
        memset(&s->pu.left_ipm[y_pu], INTRA_DC, size_in_pus);
        for(j = 0; j <size_in_pus; j++) {
            for(k = 0; k <size_in_pus; k++) {
                tab_mvf[(y_pu+j)*pic_width_in_min_pu + x_pu+k].is_intra = s->cu.pred_mode[entry] == MODE_INTRA;
            }
        }
    }

}

static void hls_coding_unit(HEVCContext *s, int x0, int y0, int log2_cb_size, int entry)
{
    int cb_size          = 1 << log2_cb_size;
    int log2_min_cb_size = s->sps->log2_min_coding_block_size;
    int length           = cb_size >> log2_min_cb_size;
    int pic_width_in_ctb = s->sps->pic_width_in_luma_samples >> log2_min_cb_size;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int x, y;
    int qp_y;
    s->cu.x[entry] = x0;
    s->cu.y[entry] = y0;
    s->cu.rqt_root_cbf[entry] = 1;

    s->cu.pred_mode[entry] = MODE_INTRA;
    s->cu.part_mode[entry] = PART_2Nx2N;
    s->cu.intra_split_flag[entry] = 0;
    s->cu.pcm_flag[entry] = 0;
    SAMPLE_CTB(s->cu.skip_flag, x_cb, y_cb) = 0;
    for (x = 0; x < 4; x++) {
        s->pu.intra_pred_mode[entry][x] = 1;
    }
    if (s->pps->transquant_bypass_enable_flag)
        s->cu.cu_transquant_bypass_flag[entry] = ff_hevc_cu_transquant_bypass_flag_decode(s, entry);

    if (s->sh.slice_type != I_SLICE) {
        uint8_t skip_flag = ff_hevc_skip_flag_decode(s, x0, y0, x_cb, y_cb, entry);
        s->cu.pred_mode[entry] = MODE_SKIP;
        x = y_cb * pic_width_in_ctb + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->cu.skip_flag[x], skip_flag, length);
            x += pic_width_in_ctb;
        }
        s->cu.pred_mode[entry] = skip_flag ? MODE_SKIP : MODE_INTER;
    }

    if (SAMPLE_CTB(s->cu.skip_flag, x_cb, y_cb)) {
        hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, entry);
        intra_prediction_unit_default_value(s, x0, y0, log2_cb_size, entry);
        ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
    } else {
        if (s->sh.slice_type != I_SLICE) {
            s->cu.pred_mode[entry] = ff_hevc_pred_mode_decode(s, entry);
        }
        if (s->cu.pred_mode[entry] != MODE_INTRA ||
            log2_cb_size == s->sps->log2_min_coding_block_size) {
            s->cu.part_mode[entry] = ff_hevc_part_mode_decode(s, log2_cb_size, entry);
            s->cu.intra_split_flag[entry] = s->cu.part_mode[entry] == PART_NxN &&
                                     s->cu.pred_mode[entry] == MODE_INTRA;
        }

        if (s->cu.pred_mode[entry] == MODE_INTRA) {
            if (s->cu.part_mode[entry] == PART_2Nx2N && s->sps->pcm_enabled_flag &&
                log2_cb_size >= s->sps->pcm.log2_min_pcm_cb_size &&
                log2_cb_size <= s->sps->pcm.log2_max_pcm_cb_size) {
                s->cu.pcm_flag[entry] = ff_hevc_pcm_flag_decode(s, entry);
           }
            if (s->cu.pcm_flag[entry]) {
                hls_pcm_sample(s, x0, y0, log2_cb_size, entry);
                intra_prediction_unit_default_value(s, x0, y0, log2_cb_size, entry);
            } else {
                intra_prediction_unit(s, x0, y0, log2_cb_size, entry);
            }
        } else {
            intra_prediction_unit_default_value(s, x0, y0, log2_cb_size, entry);
            switch (s->cu.part_mode[entry]) {
            case PART_2Nx2N:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, entry);
                break;
            case PART_2NxN:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size / 2, log2_cb_size, 0, entry);
                hls_prediction_unit(s, x0, y0 + cb_size / 2, cb_size, cb_size/2, log2_cb_size, 1, entry);
                break;
            case PART_Nx2N:
                hls_prediction_unit(s, x0, y0, cb_size / 2, cb_size, log2_cb_size, 0, entry);
                hls_prediction_unit(s, x0 + cb_size / 2, y0, cb_size / 2, cb_size, log2_cb_size, 1, entry);
                break;
            case PART_2NxnU:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size / 4, log2_cb_size, 0, entry);
                hls_prediction_unit(s, x0, y0 + cb_size / 4, cb_size, cb_size * 3 / 4, log2_cb_size, 1, entry);
                break;
            case PART_2NxnD:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size * 3 / 4, log2_cb_size, 0, entry);
                hls_prediction_unit(s, x0, y0 + cb_size * 3 / 4, cb_size, cb_size / 4, log2_cb_size, 1, entry);
                break;
            case PART_nLx2N:
                hls_prediction_unit(s, x0, y0, cb_size / 4, cb_size, log2_cb_size,0, entry);
                hls_prediction_unit(s, x0 + cb_size / 4, y0, cb_size * 3 / 4, cb_size, log2_cb_size, 1, entry);
                break;
            case PART_nRx2N:
                hls_prediction_unit(s, x0, y0, cb_size * 3 / 4, cb_size, log2_cb_size,0, entry);
                hls_prediction_unit(s, x0 + cb_size * 3 / 4, y0, cb_size/4, cb_size, log2_cb_size, 1, entry);
                break;
            case PART_NxN:
                hls_prediction_unit(s, x0, y0, cb_size / 2, cb_size / 2, log2_cb_size, 0, entry);
                hls_prediction_unit(s, x0 + cb_size / 2, y0, cb_size / 2, cb_size / 2, log2_cb_size, 1, entry);
                hls_prediction_unit(s, x0, y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 2, entry);
                hls_prediction_unit(s, x0 + cb_size / 2, y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 3, entry);
                break;
            }
        }
        if (!s->cu.pcm_flag[entry]) {
            if (s->cu.pred_mode[entry] != MODE_INTRA &&
                !(s->cu.part_mode[entry] == PART_2Nx2N && s->pu.merge_flag[entry])) {
                s->cu.rqt_root_cbf[entry] = ff_hevc_no_residual_syntax_flag_decode(s, entry);
            }
            if (s->cu.rqt_root_cbf[entry]) {
                s->cu.max_trafo_depth[entry] = s->cu.pred_mode[entry] == MODE_INTRA ?
                                        s->sps->max_transform_hierarchy_depth_intra + s->cu.intra_split_flag[entry] :
                                        s->sps->max_transform_hierarchy_depth_inter;
                hls_transform_tree(s, x0, y0, x0, y0, x0, y0, log2_cb_size,
                                   log2_cb_size, 0, 0, entry);
            } else {
                ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
            }
        }
    }
    qp_y = s->qp_y[entry];
    if( s->pps->cu_qp_delta_enabled_flag && s->tu[entry].is_cu_qp_delta_coded == 0) {
        int curr_qp_y = s->qp_y[entry];
        ff_hevc_set_qPy(s, x0, y0, x0, y0, entry);
        qp_y = s->qp_y[entry];
        if ( log2_cb_size < s->sps->log2_ctb_size - s->pps->diff_cu_qp_delta_depth ) {
            if ((x0&cb_size)==0 && (y0&cb_size)==0)
                s->curr_qp_y[entry] = qp_y;
            if ( (x0&((1<<s->sps->log2_ctb_size)-1))!=0 || (y0&((1<<s->sps->log2_ctb_size)-1))!=0 )
           		s->qp_y[entry] = curr_qp_y;
            if ((x0&cb_size)!=0 && (y0&cb_size)!=0)
                s->qp_y[entry] = s->curr_qp_y[entry];
        }
    }
    x = y_cb * pic_width_in_ctb + x_cb;
    for (y = 0; y < length; y++) {
        memset(&s->qp_y_tab[x], qp_y, length);
        x += pic_width_in_ctb;
    }
    set_ct_depth(s, x0, y0, log2_cb_size, s->ct.depth[entry]);
}

static int hls_coding_quadtree(HEVCContext *s, int x0, int y0, int log2_cb_size, int cb_depth, int entry)
{
    s->isFirstQPgroup[entry] = x0 == 0 && (y0&((1<<s->sps->log2_ctb_size)-1)) == 0 ? s->pps->entropy_coding_sync_enabled_flag : 0;
    s->ct.depth[entry] = cb_depth;
    if ((x0 + (1 << log2_cb_size) <= s->sps->pic_width_in_luma_samples) &&
        (y0 + (1 << log2_cb_size) <= s->sps->pic_height_in_luma_samples) &&
        log2_cb_size > s->sps->log2_min_coding_block_size) {
        SAMPLE(s->split_cu_flag, x0, y0) =
        ff_hevc_split_coding_unit_flag_decode(s, cb_depth, x0, y0, entry);
    } else {
        SAMPLE(s->split_cu_flag, x0, y0) =
        (log2_cb_size > s->sps->log2_min_coding_block_size);
    }
    if( s->pps->cu_qp_delta_enabled_flag
       && log2_cb_size >= s->sps->log2_ctb_size - s->pps->diff_cu_qp_delta_depth ) {
        s->tu[entry].is_cu_qp_delta_coded = 0;
        s->tu[entry].cu_qp_delta = 0;
    }

    if (SAMPLE(s->split_cu_flag, x0, y0)) {
        int more_data = 0;
        int cb_size = (1 << (log2_cb_size)) >> 1;
        int x1 = x0 + cb_size;
        int y1 = y0 + cb_size;
        more_data = hls_coding_quadtree(s, x0, y0, log2_cb_size - 1, cb_depth + 1, entry);

        if (more_data && x1 < s->sps->pic_width_in_luma_samples)
            more_data = hls_coding_quadtree(s, x1, y0, log2_cb_size - 1, cb_depth + 1,  entry);
        if (more_data && y1 < s->sps->pic_height_in_luma_samples)
            more_data = hls_coding_quadtree(s, x0, y1, log2_cb_size - 1, cb_depth + 1,  entry);
        if (more_data && x1 < s->sps->pic_width_in_luma_samples &&
           y1 < s->sps->pic_height_in_luma_samples) {
            return hls_coding_quadtree(s, x1, y1, log2_cb_size - 1, cb_depth + 1,  entry);
        }
        if (more_data)
            return ((x1 + cb_size) < s->sps->pic_width_in_luma_samples ||
                    (y1 + cb_size) < s->sps->pic_height_in_luma_samples);
        else
            return 0;
    } else {
        hls_coding_unit(s, x0, y0, log2_cb_size,  entry);
        if ((!((x0 + (1 << log2_cb_size)) %
               (1 << (s->sps->log2_ctb_size))) ||
             (x0 + (1 << log2_cb_size) >= s->sps->pic_width_in_luma_samples)) &&
            (!((y0 + (1 << log2_cb_size)) %
               (1 << (s->sps->log2_ctb_size))) ||
             (y0 + (1 << log2_cb_size) >= s->sps->pic_height_in_luma_samples))) {
            int end_of_slice_flag = ff_hevc_end_of_slice_flag_decode(s, entry);
            return !end_of_slice_flag;
        } else {
            return 1;
        }
    }

    return 0;
}

/**
 * 7.3.4
 */
static int hls_decode_entry(AVCodecContext *avctxt, void *isFilterThread)
{
    HEVCContext *s  = avctxt->priv_data;
    int ctb_size    = 1 << s->sps->log2_ctb_size;
    int more_data   = 1;
    int x_ctb       = 0;
    int y_ctb       = 0;
    int ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs];
    while (more_data) {
        int ctb_addr_rs       = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        int ctb_addr_in_slice = ctb_addr_rs - s->SliceAddrRs;
        x_ctb = (ctb_addr_rs % ((s->sps->pic_width_in_luma_samples + (ctb_size - 1))>> s->sps->log2_ctb_size)) << s->sps->log2_ctb_size;
        y_ctb = (ctb_addr_rs / ((s->sps->pic_width_in_luma_samples + (ctb_size - 1))>> s->sps->log2_ctb_size)) << s->sps->log2_ctb_size;
        s->ctb_left_flag[0] = ((x_ctb > 0) && (ctb_addr_in_slice > 0) &&
                            (s->pps->tile_id[ctb_addr_ts] == s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs-1]]));
        s->ctb_up_flag[0]   = ((y_ctb > 0)  && (ctb_addr_in_slice >= s->sps->pic_width_in_ctbs) &&
                            (s->pps->tile_id[ctb_addr_ts] == s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs - s->sps->pic_width_in_ctbs]]));
        ff_hevc_cabac_init(s, ctb_addr_ts, 0);
        if (s->sh.slice_sample_adaptive_offset_flag[0] || s->sh.slice_sample_adaptive_offset_flag[1])
            hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size, 0);
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, 0);
        ctb_addr_ts++;
        save_states(s, ctb_addr_ts, 0);
        hls_filters(s, x_ctb, y_ctb, ctb_size);
    }
    hls_filter(s, x_ctb, y_ctb);
    return ctb_addr_ts;
}

static int hls_slice_data(HEVCContext *s)
{
    int arg[2];
    int ret[2];
    arg[0] = 0;
    arg[1] = 1;
    if (s->sh.first_slice_in_pic_flag == 1) {
        s->SliceAddrRs = s->sh.slice_address;
    } else {
        s->SliceAddrRs = (s->sh.dependent_slice_segment_flag == 0 ? s->sh.slice_address : s->SliceAddrRs);
    }
    av_atomic_int_set(&s->coding_tree_count, 0);
    s->avctx->execute(s->avctx, hls_decode_entry, arg, ret , 1, sizeof(int));
    return ret[0];
}

#define SHIFT_CTB_WPP 2

static int hls_decode_entry_wpp(AVCodecContext *avctxt, void *input_ctb_row)
{
    HEVCContext *s  = avctxt->priv_data;
    int ctb_size    = 1<< s->sps->log2_ctb_size;
    int more_data   = 1;
    int *ctb_row    = input_ctb_row;
    int ctb_addr_rs = s->sh.slice_ctb_addr_rs + (*ctb_row) * ((s->sps->pic_width_in_luma_samples + (ctb_size - 1))>> s->sps->log2_ctb_size);
    int ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    while(more_data) {
        int ctb_addr_in_slice = ctb_addr_rs - s->SliceAddrRs;
        int x_ctb = (ctb_addr_rs % ((s->sps->pic_width_in_luma_samples + (ctb_size - 1))>> s->sps->log2_ctb_size)) << s->sps->log2_ctb_size;
        int y_ctb = (ctb_addr_rs / ((s->sps->pic_width_in_luma_samples + (ctb_size - 1))>> s->sps->log2_ctb_size)) << s->sps->log2_ctb_size;
        s->ctb_left_flag[*ctb_row] = (x_ctb > 0) && (ctb_addr_in_slice > 0);
        s->ctb_up_flag[*ctb_row]   = (y_ctb > 0) && (ctb_addr_in_slice >= s->sps->pic_width_in_ctbs);
        while(*ctb_row && (av_atomic_int_get(&s->ctb_entry_count[(*ctb_row)-1])-av_atomic_int_get(&s->ctb_entry_count[(*ctb_row)]))<SHIFT_CTB_WPP);
        if (av_atomic_int_get(&s->ERROR)){
        	av_atomic_int_add_and_fetch(&s->ctb_entry_count[*ctb_row],SHIFT_CTB_WPP);
        	return 0;
        }
        ff_hevc_cabac_init(s, ctb_addr_ts, *ctb_row);
        if (s->sh.slice_sample_adaptive_offset_flag[0] ||
            s->sh.slice_sample_adaptive_offset_flag[1])
            hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size, *ctb_row);
        
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, *ctb_row);

        ctb_addr_ts++;
        ctb_addr_rs       = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        save_states(s, ctb_addr_ts, *ctb_row);
        av_atomic_int_add_and_fetch(&s->ctb_entry_count[*ctb_row],1);
        hls_filters(s, x_ctb, y_ctb, ctb_size);

        if (!more_data && (x_ctb+ctb_size) < s->sps->pic_width_in_luma_samples && (y_ctb+ctb_size) < s->sps->pic_height_in_luma_samples) {
        	av_atomic_int_set(&s->ERROR,  1);
            av_atomic_int_add_and_fetch(&s->ctb_entry_count[*ctb_row],SHIFT_CTB_WPP);
            return 0;
        }

        if (!more_data) {
            hls_filter(s, x_ctb, y_ctb);
            av_atomic_int_add_and_fetch(&s->ctb_entry_count[*ctb_row],SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }
        x_ctb+=ctb_size;

        if(x_ctb >= s->sps->pic_width_in_luma_samples) {
            break;
        }
    }
    av_atomic_int_add_and_fetch(&s->ctb_entry_count[*ctb_row],SHIFT_CTB_WPP);
    return 0;
}

static int hls_slice_data_wpp(HEVCContext *s, AVPacket *avpkt)
{
    int *ret = av_malloc((s->sh.num_entry_point_offsets+1)*sizeof(int));
    int *arg = av_malloc((s->sh.num_entry_point_offsets+1)*sizeof(int));
    int i, res = 0;
    int startheader, j, cmpt = 0;
    int offset = (s->gb[0]->index>>3);
    if(!s->ctb_entry_count) {
        s->ctb_entry_count = av_malloc((s->sh.num_entry_point_offsets+1)*sizeof(int));
        for(i=1; i< MAX_ENTRIES-1; i++) {
            s->gb[i] = av_malloc(sizeof(GetBitContext));
            s->cc[i] = av_malloc(sizeof(CABACContext));
            s->cabac_state [i+1] = av_malloc(HEVC_CONTEXTS);
            s->edge_emu_buffer[i] = av_malloc((MAX_PB_SIZE + 7) * s->frame->linesize[0]);
        }
        s->edge_emu_buffer[MAX_ENTRIES-1] = av_malloc((MAX_PB_SIZE + 7) * s->frame->linesize[0]);
        s->gb[MAX_ENTRIES-1] = av_malloc(sizeof(GetBitContext));
        s->cc[MAX_ENTRIES-1] = av_malloc(sizeof(CABACContext));
    }
    for(i=1; i< MAX_ENTRIES; i++) {
        s->qp_y[i] = s->qp_y[0];
        s->isFirstQPgroup[i] = 1;
    }

#ifdef WPP1
    for(j=0, cmpt = 0,startheader=offset+s->sh.entry_point_offset[0]; j< s->skipped_bytes; j++){
        if(s->skipped_bytes_pos[j] >= offset && s->skipped_bytes_pos[j] < startheader){
            startheader--;
            cmpt++;
        }
    }
#endif
    memset(s->ctb_entry_count, 0, (s->sh.num_entry_point_offsets+1)*sizeof(int));
    for(i=1; i< s->sh.num_entry_point_offsets; i++) {

        //cmpt = 0;
        offset += (s->sh.entry_point_offset[i-1]-cmpt);
#ifdef WPP1
        for(j=0, cmpt=0, startheader=offset+s->sh.entry_point_offset[i]; j< s->skipped_bytes; j++){
            if(s->skipped_bytes_pos[j] >= offset && s->skipped_bytes_pos[j] < startheader){
                startheader--;
                cmpt++;
            }
        }
#endif
        init_get_bits(s->gb[i], avpkt->data+offset, (s->sh.entry_point_offset[i]-cmpt)*8);
        ff_init_cabac_decoder(s->cc[i], avpkt->data+offset, s->sh.entry_point_offset[i]-cmpt);
    }
    if(s->sh.num_entry_point_offsets!= 0) {
        offset += s->sh.entry_point_offset[s->sh.num_entry_point_offsets-1]-cmpt;
        init_get_bits(s->gb[s->sh.num_entry_point_offsets], avpkt->data+offset, (avpkt->size-offset)*8);
        ff_init_cabac_decoder(s->cc[s->sh.num_entry_point_offsets], avpkt->data+offset, (avpkt->size-offset));
    }

    if (s->sh.first_slice_in_pic_flag == 1) {
        s->SliceAddrRs = s->sh.slice_address;
    } else {
        s->SliceAddrRs = (s->sh.dependent_slice_segment_flag == 0 ? s->sh.slice_address : s->SliceAddrRs);
    }
    av_atomic_int_set(&s->ERROR,  0);
    for(i=0; i<=s->sh.num_entry_point_offsets; i++) {
        arg[i] = i;
        ret[i] = 0;
    }
    s->avctx->execute(s->avctx, hls_decode_entry_wpp, arg, ret ,s->sh.num_entry_point_offsets+1, sizeof(int));
    for(i=0; i<=s->sh.num_entry_point_offsets; i++)
        res += ret[i];
    av_free(ret);
    av_free(arg);
    return res;
}


/**
 * @return AVERROR_INVALIDDATA if the packet is not a valid NAL unit,
 * 0 if the unit should be skipped, 1 otherwise
 */
static int hls_nal_unit(HEVCContext *s)
{
    GetBitContext *gb = s->gb[0];
    int nuh_layer_id;

    if (get_bits1(gb) != 0)
        return AVERROR_INVALIDDATA;

    s->nal_unit_type = get_bits(gb, 6);

    nuh_layer_id = get_bits(gb, 6);
    s->temporal_id = get_bits(gb, 3) - 1;
    if (s->temporal_id < 0)
        return AVERROR_INVALIDDATA;

    av_log(s->avctx, AV_LOG_DEBUG,
           "nal_unit_type: %d, nuh_layer_id: %dtemporal_id: %d\n",
           s->nal_unit_type, nuh_layer_id, s->temporal_id);

    return (nuh_layer_id == 0);
}

static void printf_ref_pic_list(HEVCContext *s)
{
    RefPicList  *refPicList = s->ref->refPicList;
    uint8_t i, list_idx;
    if (s->sh.slice_type == I_SLICE)
        printf("\nPOC %4d TId: %1d ( I-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->sh.slice_qp);
    else if (s->sh.slice_type == B_SLICE)
        printf("\nPOC %4d TId: %1d ( B-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->sh.slice_qp);
    else
        printf("\nPOC %4d TId: %1d ( P-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->sh.slice_qp);

    for ( list_idx = 0; list_idx < 2; list_idx++) {
        printf("[L%d ",list_idx);
        for(i = 0; i < refPicList[list_idx].numPic; i++) {
            int currIsLongTerm = refPicList[list_idx].isLongTerm[i];
            if (currIsLongTerm)
                printf("%d* ",refPicList[list_idx].list[i]);
            else
                printf("%d ",refPicList[list_idx].list[i]);
        }
        printf("] ");
    }
}

static void print_md5(int poc, uint8_t md5[3][16]) {
    int i, j;
    printf("\n[MD5:");
    for (j = 0; j < 3; j++) {
        printf("\n");
        for (i = 0; i < 16; i++)
            printf("%02x", md5[j][i]);
    }
    printf("\n] ");

}

static void calc_md5(uint8_t *md5, uint8_t* src, int stride, int width, int height) {
    uint8_t *buf;
    int y,x;
    buf = av_malloc(width * height);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            buf[y * width + x] = src[x];

        src += stride;
    }
    av_md5_sum(md5, buf, width * height);
    av_free(buf);
}


static int hevc_decode_frame(AVCodecContext *avctx, void *data, int *got_output,
                             AVPacket *avpkt)
{
    HEVCContext *s = avctx->priv_data;
    GetBitContext *gb = s->gb[0];
    int poc_display;
    
    int ctb_addr_ts;
    int ret;

    if (!avpkt->size) {
        if ((ret = ff_hevc_find_display(s, data, 1, &poc_display)) < 0)
            return ret;

        *got_output = ret;
        return 0;
    }

    init_get_bits(gb, avpkt->data, avpkt->size*8);
    av_log(s->avctx, AV_LOG_DEBUG, "=================\n");

    ret = hls_nal_unit(s);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit %d, skipping.\n", s->nal_unit_type);
        return avpkt->size;
    } else if (!ret)
        return avpkt->size;

    switch (s->nal_unit_type) {
    case NAL_VPS:
        ff_hevc_decode_nal_vps(s);
        break;
    case NAL_SPS:
        ff_hevc_decode_nal_sps(s);
        break;
    case NAL_PPS:
        ff_hevc_decode_nal_pps(s);
        break;
    case NAL_SEI_PREFIX:
    case NAL_SEI_SUFFIX:
        ff_hevc_decode_nal_sei(s);
        break;
    case NAL_TRAIL_R:
    case NAL_TRAIL_N:
    case NAL_TSA_N:
    case NAL_TSA_R:
    case NAL_STSA_N:
    case NAL_STSA_R:
    case NAL_BLA_W_LP:
    case NAL_BLA_W_RADL:
    case NAL_BLA_N_LP:
    case NAL_IDR_W_RADL:
    case NAL_IDR_N_LP:
    case NAL_CRA_NUT:
    case NAL_RADL_N:
    case NAL_RADL_R:
    case NAL_RASL_N:
    case NAL_RASL_R:
        s->isFirstQPgroup[0] = 1;
        ret = hls_slice_header(s);
        if (ret < 0)
            if (ret == AVERROR_INVALIDDATA)
                return avpkt->size;
            else
	            return ret;
        if (s->max_ra == INT_MAX) {
            if (s->nal_unit_type == NAL_CRA_NUT ||
                    s->nal_unit_type == NAL_BLA_W_LP ||
                    s->nal_unit_type == NAL_BLA_N_LP ||
                    s->nal_unit_type == NAL_BLA_N_LP) {
                s->max_ra = s->poc;
            } else {
                if (s->nal_unit_type == NAL_IDR_W_RADL || s->nal_unit_type == NAL_IDR_N_LP)
                    s->max_ra = INT_MIN;
            }
        }

        if (s->nal_unit_type == NAL_RASL_R && s->poc <= s->max_ra) {
            s->is_decoded = 0;
            printf("not decoded %d %d\n", s->poc, s->max_ra);
            break;
        } else {
            if (s->nal_unit_type == NAL_RASL_R && s->poc > s->max_ra)
                s->max_ra = INT_MIN;
        }

        if(!s->sh.disable_deblocking_filter_flag && s->sh.first_slice_in_pic_flag) {
            int pic_width_in_min_pu  = s->sps->pic_width_in_min_cbs * 4;
            int pic_height_in_min_pu = s->sps->pic_height_in_min_cbs * 4;
            memset(s->horizontal_bs, 0, 2 * s->bs_width * s->bs_height);
            memset(s->vertical_bs, 0, s->bs_width * 2 * s->bs_height);
            memset(s->cbf_luma, 0 , pic_width_in_min_pu * pic_height_in_min_pu);
            memset(s->is_pcm, 0 , pic_width_in_min_pu * pic_height_in_min_pu);
        }
        s->qp_y[0] = ((s->sh.slice_qp + 52 + 2 * s->sps->qp_bd_offset) %
                (52 + s->sps->qp_bd_offset)) - s->sps->qp_bd_offset;

        if (s->sh.first_slice_in_pic_flag) {
			if (s->sps->sample_adaptive_offset_enabled_flag) {
			    av_frame_unref(s->tmp_frame);
                if ((ret = ff_reget_buffer(s->avctx, s->tmp_frame)) < 0)
                    return ret;
                s->frame = s->tmp_frame;
                if ((ret = ff_hevc_set_new_ref(s, &s->sao_frame, s->poc))< 0)
                    return ret;
            } else {
                if ((ret = ff_hevc_set_new_ref(s, &s->frame, s->poc))< 0)
                    return ret;
            }
        }
        if (!s->edge_emu_buffer[0])
            s->edge_emu_buffer[0] = av_malloc((MAX_PB_SIZE + 7) * s->frame->linesize[0]);
        if (!s->edge_emu_buffer[0])
            return -1;
        if(s->pps->entropy_coding_sync_enabled_flag && s->enable_multithreads && s->sh.num_entry_point_offsets > 0 ) {
            ctb_addr_ts = hls_slice_data_wpp(s, avpkt);
        } else {
            ctb_addr_ts = hls_slice_data(s);
        }
        if (s->sh.first_slice_in_pic_flag)
            if ((ret = ff_hevc_find_display(s, data, 0, &poc_display)) < 0)
                return ret;
        if (ctb_addr_ts >= (s->sps->pic_width_in_ctbs * s->sps->pic_height_in_ctbs)) {
            if (s->decode_checksum_sei == 1) {
#ifdef POC_DISPLAY_MD5
                AVFrame *frame = (AVFrame *) data;
                int poc        = poc_display;
#else
                AVFrame *frame = s->ref->frame;
                int poc        = s->poc;
#endif
                calc_md5(s->md5[0], frame->data[0], frame->linesize[0], frame->width  , frame->height  );
                calc_md5(s->md5[1], frame->data[1], frame->linesize[1], frame->width/2, frame->height/2);
                calc_md5(s->md5[2], frame->data[2], frame->linesize[2], frame->width/2, frame->height/2);
                s->is_decoded = 1;
                printf_ref_pic_list(s);
                print_md5(poc, s->md5);
            }
            s->frame->pict_type = AV_PICTURE_TYPE_I;
            s->frame->key_frame = 1;
            *got_output = ret;
        } else {
            return 0;
        }
        break;
    case NAL_AUD:
    case NAL_EOS_NUT:
        return avpkt->size;
    default:
        av_log(s->avctx, AV_LOG_INFO, "Skipping NAL unit %d\n", s->nal_unit_type);
        return avpkt->size;
    }

    av_log(s->avctx, AV_LOG_DEBUG, "%d bits left in unit\n", get_bits_left(gb));
    return avpkt->size;
}

static av_cold int hevc_decode_init(AVCodecContext *avctx)
{
    int i;
    HEVCContext *s = avctx->priv_data;

    s->avctx = avctx;
    s->tmp_frame = av_frame_alloc();
    s->gb[0] = av_malloc(sizeof(GetBitContext));
    s->cc[0] = av_malloc(sizeof(CABACContext));
    s->cabac_state[0] = av_malloc(HEVC_CONTEXTS);
    s->cabac_state[1] = av_malloc(HEVC_CONTEXTS);
    memset(&s->ctx_set[0], 0, MAX_ENTRIES*sizeof(int));
    memset(&s->greater1_ctx[0], 0, MAX_ENTRIES*sizeof(int));
    memset(&s->last_coeff_abs_level_greater1_flag[0], 0, MAX_ENTRIES*sizeof(int));

    if (!s->tmp_frame)
        return AVERROR(ENOMEM);
    s->max_ra = INT_MAX;
    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        s->DPB[i].frame = av_frame_alloc();
        if (!s->DPB[i].frame)
            return AVERROR(ENOMEM);
    }

    memset(s->sps_list, 0, sizeof(s->sps_list));
    memset(s->pps_list, 0, sizeof(s->pps_list));
    s->ctb_entry_count  = NULL;
    s->skipped_buf_size = 0;
    return 0;
}

static av_cold int hevc_decode_free(AVCodecContext *avctx)
{
    int i;
    HEVCContext *s = avctx->priv_data;
    av_free(s->skipped_bytes_pos);
    av_frame_free(&s->tmp_frame);
    av_free(s->gb[0]);
    av_free(s->cc[0]);
    av_free(s->cabac_state[0]);
    av_free(s->cabac_state[1]);
    av_free(s->edge_emu_buffer[0]);
    if(s->ctb_entry_count) {
        av_freep(&s->sh.entry_point_offset);
        for (i = 1; i < MAX_ENTRIES-1; i++) {
            av_free(s->gb[i]);
            av_free(s->cc[i]);
            av_free(s->edge_emu_buffer[i]);
            av_free(s->cabac_state[i+1]);
        }
        av_free(s->gb[MAX_ENTRIES-1]);
        av_free(s->cc[MAX_ENTRIES-1]);
        av_free(s->edge_emu_buffer[MAX_ENTRIES-1]);
        av_free(s->ctb_entry_count);
    }
    
  
    
    
    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        av_frame_free(&s->DPB[i].frame);
    }


    for (i = 0; i < MAX_VPS_COUNT; i++) {
        av_freep(&s->vps_list[i]);
    }

    for (i = 0; i < MAX_SPS_COUNT; i++) {
        av_freep(&s->sps_list[i]);
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (s->pps_list[i]) {
            av_freep(&s->pps_list[i]->column_width);
            av_freep(&s->pps_list[i]->row_height);
            av_freep(&s->pps_list[i]->col_bd);
            av_freep(&s->pps_list[i]->row_bd);
            av_freep(&s->pps_list[i]->ctb_addr_rs_to_ts);
            av_freep(&s->pps_list[i]->ctb_addr_ts_to_rs);
            av_freep(&s->pps_list[i]->tile_id);
            av_freep(&s->pps_list[i]->min_cb_addr_zs);
            av_freep(&s->pps_list[i]->min_tb_addr_zs);
        }
        av_freep(&s->pps_list[i]);
    }

    pic_arrays_free(s);

    return 0;
}

static void hevc_decode_flush(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    ff_hevc_clean_refs(s);
    s->max_ra = INT_MAX;
}

#define OFFSET(x) offsetof(HEVCContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption options[] = {
    { "decode-checksum", "decode picture checksum SEI message", OFFSET(decode_checksum_sei),
      AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { NULL },
};

static const AVClass hevc_decoder_class = {
    .class_name = "HEVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_decoder = {
    .name           = "hevc",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .priv_data_size = sizeof(HEVCContext),
    .priv_class     = &hevc_decoder_class,
    .init           = hevc_decode_init,
    .close          = hevc_decode_free,
    .decode         = hevc_decode_frame,
    .capabilities   = CODEC_CAP_DR1 | CODEC_CAP_DELAY | CODEC_CAP_SLICE_THREADS, // Add by wassim to support multi-threading
    .flush          = hevc_decode_flush,
    .long_name      = NULL_IF_CONFIG_SMALL("HEVC (High Efficiency Video Coding)"),
};
