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

#include "hevc.h"
#include "internal.h"
//#define TEST_DPB
static int find_ref_idx(HEVCContext *s, int poc)
{
    int i;
    HEVCSharedContext *sc = s->HEVCsc;
    int LtMask = (1 << sc->sps->log2_max_poc_lsb) - 1;
    for (i = 0; i < FF_ARRAY_ELEMS(sc->DPB); i++) {
        HEVCFrame *ref = &sc->DPB[i];
        if (ref->frame->buf[0] && (ref->sequence == sc->seq_decode)) {
            if ((ref->flags & HEVC_FRAME_FLAG_SHORT_REF) != 0 && ref->poc == poc)
                return i;
            if ((ref->flags & HEVC_FRAME_FLAG_SHORT_REF) != 0 && (ref->poc & LtMask) == poc)
	            return i;
	    }
    }
    av_log(s->avctx, AV_LOG_ERROR,
           "Could not find ref with POC %d\n", poc);
    return 0;
}
#ifdef SVC_EXTENSION

static int find_upsample_ref_idx(HEVCContext *s, int poc)
{
    int i;
    HEVCSharedContext *sc = s->HEVCsc;
    for (i = 0; i < FF_ARRAY_ELEMS(sc->DPB); i++) {
        HEVCFrame *ref = &sc->DPB[i];
        if (ref->frame->buf[0] && (ref->up_sample_base ==1 )) {
            if ((ref->flags & HEVC_FRAME_FLAG_SHORT_REF) == 0 && ref->poc == poc)
                return i;
	    }
    }
    av_log(s->avctx, AV_LOG_ERROR,
           "Could not find ref with POC %d\n", poc);
    return 0;
}


void ff_unref_upsampled_frame(HEVCSharedContext *s){
    for (int i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        HEVCFrame *ref = &s->DPB[i];
        if(ref->frame->buf[0] && ref->poc == s->poc && ref->up_sample_base){
            av_frame_unref(ref->frame);
            ref->flags = 0;
            ref->refPicList[0].numPic = 0;
            ref->refPicList[1].numPic = 0;
        }
    }
}
#endif

static void update_refs(HEVCContext *s)
{
    int i, j;
    HEVCSharedContext *sc = s->HEVCsc;
    int used[FF_ARRAY_ELEMS(sc->DPB)] = { 0 };
    for (i = 0; i < 5; i++) {
        RefPicList *rpl = &sc->sh.refPocList[i];
        for (j = 0; j < rpl->numPic; j++)
            used[rpl->idx[j]] = 1;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(sc->DPB); i++) {
        HEVCFrame *ref = &sc->DPB[i];
        if (ref->frame->buf[0] && (!used[i] /*|| ref->up_sample_base*/))
            ref->flags &= ~HEVC_FRAME_FLAG_SHORT_REF;
        if (ref->frame->buf[0] && !ref->flags) {
#ifdef TEST_DPB
            printf("\t\t\t\t\t\t%d\t%d\n",i, ref->poc);
#endif
            av_frame_unref(ref->frame);
            ref->refPicList[0].numPic = 0;
            ref->refPicList[1].numPic = 0;
        }
    }
}

void ff_hevc_clear_refs(HEVCContext *s)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(s->HEVCsc->DPB); i++) {
        HEVCFrame *ref = &s->HEVCsc->DPB[i];
        if (!(ref->flags & HEVC_FRAME_FLAG_OUTPUT) ) {
#ifdef TEST_DPB
            printf("\t\t\t\t\t\t%d\t%d\n",i, ref->poc);
#endif
            av_frame_unref(ref->frame);
            ref->flags = 0;
            ref->refPicList[0].numPic = 0;
            ref->refPicList[1].numPic = 0;
        }
    }
}

void ff_hevc_clean_refs(HEVCContext *s)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(s->HEVCsc->DPB); i++) {
        HEVCFrame *ref = &s->HEVCsc->DPB[i];
#ifdef TEST_DPB
        printf("\t\t\t\t\t\t%d\t%d\n",i, ref->poc);
#endif
        av_frame_unref(ref->frame);
        ref->flags = 0;
    }
}

int ff_hevc_find_next_ref(HEVCContext *s, int poc)
{
    int i;
    if (!s->HEVCsc->sh.first_slice_in_pic_flag)
        return find_ref_idx(s, poc);

    update_refs(s);

    for (i = 0; i < FF_ARRAY_ELEMS(s->HEVCsc->DPB); i++) {
        HEVCFrame *ref = &s->HEVCsc->DPB[i];
        if (!ref->frame->buf[0]) {
            return i;
        }
    }
    av_log(s->avctx, AV_LOG_ERROR,
           "could not free room for POC %d\n", poc);
    return -1;
}
#ifdef SVC_EXTENSION
int ff_hevc_set_new_ref(HEVCContext *s, AVFrame **frame, int poc, int up_sample)
#else
int ff_hevc_set_new_ref(HEVCContext *s, AVFrame **frame, int poc)
#endif

{
    int i;
    
    for (i = 0; i < FF_ARRAY_ELEMS(s->HEVCsc->DPB); i++) {
        HEVCFrame *ref = &s->HEVCsc->DPB[i];
        if (!ref->frame->buf[0]) {
            *frame     = ref->frame;
            s->HEVCsc->ref     = ref;
            ref->poc   = poc;
            ref->frame->pts = s->HEVCsc->pts;
#ifdef SVC_EXTENSION
            ref->up_sample_base = up_sample; 
#endif
            ref->flags = HEVC_FRAME_FLAG_OUTPUT | HEVC_FRAME_FLAG_SHORT_REF;
            ref->sequence = s->HEVCsc->seq_decode;
#ifdef TEST_DPB
            printf("%d\t%d\n",i, poc);
#endif
            return ff_reget_buffer(s->avctx, *frame);
        }
    }
    av_log(s->avctx, AV_LOG_ERROR,
           "DPB is full, could not add ref with POC %d\n", poc);
    return -1;
}

int ff_hevc_find_display(HEVCContext *s, AVFrame *out, int flush, int* poc_display)
{
    int nb_output = 0;
    int min_poc   = 0xFFFF;
    int i, min_idx, ret;
    min_idx = 0;
    HEVCSharedContext *sc = s->HEVCsc;
    uint8_t run = 1;
    while (run) {
        for (i = 0; i < FF_ARRAY_ELEMS(sc->DPB); i++) {
            HEVCFrame *frame = &sc->DPB[i];
#ifdef SVC_EXTENSION
            if ((frame->flags & HEVC_FRAME_FLAG_OUTPUT) &&
            	 frame->sequence == sc->seq_output && !frame->up_sample_base) {
#else
                if ((frame->flags & HEVC_FRAME_FLAG_OUTPUT) &&
                    frame->sequence == sc->seq_output) {
#endif
                nb_output++;
                if (frame->poc < min_poc) {
                    min_poc = frame->poc;
                    min_idx = i;
                }
            }
        }
        /* wait for more frames before output */
        if (!flush && sc->seq_output == sc->seq_decode &&
            nb_output <= sc->sps->temporal_layer[0].num_reorder_pics+1)
            return 0;

        if (nb_output) {
#ifdef TEST_DPB
            printf("\t\t\t%d\t%d\n", min_idx, min_poc);
#endif
//            av_log(s->avctx, AV_LOG_INFO, "Display : POC %d\n", min_poc);
            HEVCFrame *frame = &sc->DPB[min_idx];

            frame->flags &= ~HEVC_FRAME_FLAG_OUTPUT;
            *poc_display = frame->poc;
            frame->frame->display_picture_number = frame->poc;
            ret = av_frame_ref(out, frame->frame);
            if (ret < 0)
                return ret;
            return 1;
        }

        if (sc->seq_output != sc->seq_decode)
            sc->seq_output = (sc->seq_output + 1) & 0xff;
        else
            run = 0;
    }
    
    return 0;
}

void ff_hevc_compute_poc(HEVCContext *s, int poc_lsb)
{
    HEVCSharedContext *sc = s->HEVCsc;
    int iMaxPOClsb  = 1 << sc->sps->log2_max_poc_lsb;
    int iPrevPOClsb = sc->pocTid0 % iMaxPOClsb;
    int iPrevPOCmsb = sc->pocTid0 - iPrevPOClsb;
    int iPOCmsb;
    if ((poc_lsb < iPrevPOClsb) && ((iPrevPOClsb - poc_lsb) >= (iMaxPOClsb / 2))) {
        iPOCmsb = iPrevPOCmsb + iMaxPOClsb;
    } else if ((poc_lsb > iPrevPOClsb) && ((poc_lsb - iPrevPOClsb) > (iMaxPOClsb / 2))) {
        iPOCmsb = iPrevPOCmsb - iMaxPOClsb;
    } else {
        iPOCmsb = iPrevPOCmsb;
    }
    sc->poc = iPOCmsb + poc_lsb;
}
#if REF_IDX_FRAMEWORK
static void init_upsampled_mv_fields(HEVCContext *s) {
    int i, list;
    int pic_width_in_min_pu = s->HEVCsc->sps->pic_width_in_luma_samples >> s->HEVCsc->sps->log2_min_pu_size;
    int pic_height_in_min_pu = s->HEVCsc->sps->pic_height_in_luma_samples >> s->HEVCsc->sps->log2_min_pu_size;
  //  int indexEL =  ;
 //   int indexBL = ;
    HEVCFrame *refEL = &s->HEVCsc->DPB[find_upsample_ref_idx( s,  s->HEVCsc->poc)];
  //  HEVCFrame *refBL = &((HEVCContext*)s->avctx->priv_data)->HEVCsc->DPB[find_ref_idx(s->avctx->priv_data, s->HEVCsc->poc)];
    RefPicList   *refPocList = s->HEVCsc->sh.refPocList;
    refPocList[ST_CURR_BEF].numPic = 0;
    refPocList[ST_CURR_AFT].numPic = 0;
    refPocList[LT_CURR].numPic = 0;
    
    for(i = 0; i < pic_width_in_min_pu  * pic_height_in_min_pu; i++) {
        refEL->tab_mvf[i].is_intra = 1;
        for(int list = 0; list < 2; list++){
            refEL->tab_mvf[i].mv[list].x = 0;
            refEL->tab_mvf[i].mv[list].y = 0;
            refEL->tab_mvf[i].pred_flag[list] = 0;
            refEL->tab_mvf[i].ref_idx[list] = 0;
        }
    }
}

static void scale_upsampled_mv_field(HEVCContext *s) {
    int xEL, yEL, xBL, yBL, list, i;
    HEVCFrame  *refBL, *refEL;
    HEVCSharedContext *sc = s->HEVCsc;
    int pic_width_in_min_pu = sc->sps->pic_width_in_luma_samples>>sc->sps->log2_min_pu_size;
    int pic_width_in_min_puBL = ((HEVCContext*)s->avctx->priv_data)->HEVCsc->sps->pic_width_in_luma_samples>> ((HEVCContext*)s->avctx->priv_data)->HEVCsc->sps->log2_min_pu_size;
    refBL = &((HEVCContext*)s->avctx->priv_data)->HEVCsc->DPB[find_ref_idx(s->avctx->priv_data, s->HEVCsc->poc)];
    refEL = &s->HEVCsc->DPB[find_upsample_ref_idx( s,  s->HEVCsc->poc)];
    
    for(list = 0; list < 2; list++) {
        refEL->refPicList[list].numPic = refBL->refPicList[list].numPic;
        for(i=0; i< refBL->refPicList[list].numPic; i++){
            refEL->refPicList[list].list[i] = refBL->refPicList[list].list[i];
            refEL->refPicList[list].idx[i] = find_ref_idx(s,refBL->refPicList[list].list[i]);
            refEL->refPicList[list].isLongTerm[i] = refBL->refPicList[list].isLongTerm[i];
        }
         for(i=refBL->refPicList[list].numPic; i< REF_LIST_SIZE; i++)   {
             refEL->refPicList[list].list[i] = 0;
             refEL->refPicList[list].idx[i] = 0;
             refEL->refPicList[list].isLongTerm[i] = 0;
         }
    }
    for(yEL=0; yEL < sc->sps->pic_height_in_luma_samples; yEL+=16){
    for(xEL=0; xEL < sc->sps->pic_width_in_luma_samples ; xEL+=16) {
        int xELIndex = xEL>>2;
        int yELIndex = yEL>>2;
        xBL = (((xEL+8) - sc->sps->pic_conf_win.left_offset)*sc->sh.ScalingPosition[sc->layer_id][0] + (1<<15)) >> 16;
        yBL = (((yEL+8) - sc->sps->pic_conf_win.top_offset )*sc->sh.ScalingPosition[sc->layer_id][1] + (1<<15)) >> 16;
        
   
        xBL = (xBL >>=4)<<2; /*xBL & 0xFFFFFFF0*/
        yBL = (yBL >>=4)<<2;  /*yBL & 0xFFFFFFF0*/
             if(!refBL->tab_mvf[(yBL*pic_width_in_min_puBL)+xBL].is_intra) {
                refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].is_intra = 0;
                for( list=0; list < 2; list++) {
                    int x = refBL->tab_mvf[(yBL*pic_width_in_min_puBL)+xBL].mv[list].x;
                    int y = refBL->tab_mvf[(yBL*pic_width_in_min_puBL)+xBL].mv[list].y;
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].mv[list].x  = av_clip_c( (sc->sh.ScalingFactor[s->HEVCsc->layer_id][0] * x + 127 + (sc->sh.ScalingFactor[sc->layer_id][0] * x < 0)) >> 8 , -32768, 32767);
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].mv[list].y = av_clip_c( (sc->sh.ScalingFactor[s->HEVCsc->layer_id][1] * y + 127 + (s->HEVCsc->sh.ScalingFactor[sc->layer_id][1] * y < 0)) >> 8, -32768, 32767);
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].ref_idx[list] = refBL->tab_mvf[yBL*pic_width_in_min_puBL+xBL].ref_idx[list];
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].pred_flag[list] = refBL->tab_mvf[yBL*pic_width_in_min_puBL+xBL].pred_flag[list];
                }
            } else {
                refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].is_intra = 1;
                for( list=0; list < 2; list++) {
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].mv[list].x  = 0;
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].mv[list].y = 0;
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].ref_idx[list] = refBL->tab_mvf[yBL*pic_width_in_min_puBL+xBL].ref_idx[list];;
                    refEL->tab_mvf[(yELIndex*pic_width_in_min_pu)+xELIndex].pred_flag[list] = refBL->tab_mvf[yBL*pic_width_in_min_puBL+xBL].pred_flag[list];
                }
            }
            for(int e =0; e < 4; e++)
            for(int k =0; k < 4; k++)   {
               if(e || k) {
                   refEL->tab_mvf[((yELIndex+e) *pic_width_in_min_pu)+xELIndex+k].is_intra = refEL->tab_mvf[yELIndex*pic_width_in_min_pu+xELIndex].is_intra;
                   for(int list=0; list < 2; list++) {
                        refEL->tab_mvf[((yELIndex+e) *pic_width_in_min_pu)+xELIndex+k].mv[list].x  = refEL->tab_mvf[yELIndex*pic_width_in_min_pu+xELIndex].mv[list].x;
                        refEL->tab_mvf[((yELIndex+e) *pic_width_in_min_pu)+xELIndex+k].mv[list].y = refEL->tab_mvf[yELIndex*pic_width_in_min_pu+xELIndex].mv[list].y;
                        refEL->tab_mvf[((yELIndex+e) *pic_width_in_min_pu)+xELIndex+k].ref_idx[list] = refEL->tab_mvf[yELIndex*pic_width_in_min_pu+xELIndex].ref_idx[list];
                        refEL->tab_mvf[((yELIndex+e) *pic_width_in_min_pu)+xELIndex+k].pred_flag[list] = refEL->tab_mvf[yELIndex*pic_width_in_min_pu+xELIndex].pred_flag[list];
                    }
                }
            }
        }
    }
}
#endif
static void set_ref_pic_list(HEVCContext *s)
{
    HEVCSharedContext *sc = s->HEVCsc;
    SliceHeader *sh = &sc->sh;
    RefPicList  *refPocList = sc->sh.refPocList;
    RefPicList  *refPicList = sc->DPB[ff_hevc_find_next_ref(s, sc->poc)].refPicList;
    RefPicList  refPicListTmp[2]= {{{0}}};

    uint8_t num_ref_idx_lx_act[2];
    uint8_t cIdx;
    uint8_t num_poc_total_curr;
    uint8_t num_rps_curr_lx;
    uint8_t first_list;
    uint8_t sec_list;
    uint8_t i, list_idx;
#if REF_IDX_FRAMEWORK
    VPS *vps = s->HEVCsc->vps;
#endif
#if REF_IDX_FRAMEWORK
#if REF_IDX_MFM
#if ZERO_NUM_DIRECT_LAYERS
    if( s->HEVCsc->layer_id > 0 && vps->max_one_active_ref_layer_flag > 0 )
#else
        if (s->HEVCsc->layer_id)
#endif
        {
            if(!(s->HEVCsc->nal_unit_type >= NAL_BLA_W_LP && s->HEVCsc->nal_unit_type <= NAL_CRA_NUT) && s->HEVCsc->sps->set_mfm_enabled_flag)  {
                scale_upsampled_mv_field(s);
            }   else    {
                init_upsampled_mv_fields(s);
            }
#endif
        }
#endif
        
    num_ref_idx_lx_act[0] = sh->num_ref_idx_l0_active;
    num_ref_idx_lx_act[1] = sh->num_ref_idx_l1_active;
    for ( list_idx = 0; list_idx < 2; list_idx++) {
        /* The order of the elements is
         * ST_CURR_BEF - ST_CURR_AFT - LT_CURR for the RefList0 and
         * ST_CURR_AFT - ST_CURR_BEF - LT_CURR for the RefList1
         */
        first_list = list_idx == 0 ? ST_CURR_BEF : ST_CURR_AFT;
        sec_list   = list_idx == 0 ? ST_CURR_AFT : ST_CURR_BEF;

        /* even if num_ref_idx_lx_act is inferior to num_poc_total_curr we fill in
         * all the element from the Rps because we might reorder the list. If
         * we reorder the list might need a reference picture located after
         * num_ref_idx_lx_act.
         */
        
#if REF_IDX_FRAMEWORK
#if JCTVC_M0458_INTERLAYER_RPS_SIG
        num_poc_total_curr = refPocList[ST_CURR_BEF].numPic + refPocList[ST_CURR_AFT].numPic + refPocList[LT_CURR].numPic + vps->max_one_active_ref_layer_flag;
#else
        num_poc_total_curr = refPocList[ST_CURR_BEF].numPic + refPocList[ST_CURR_AFT].numPic + refPocList[LT_CURR].numPic /*+ m_numILRRefIdx*/;
#endif
#else
        num_poc_total_curr = refPocList[ST_CURR_BEF].numPic + refPocList[ST_CURR_AFT].numPic + refPocList[LT_CURR].numPic;
#endif
        num_rps_curr_lx    = num_poc_total_curr<num_ref_idx_lx_act[list_idx] ? num_poc_total_curr : num_ref_idx_lx_act[list_idx];
        cIdx = 0;
        for(i = 0; i < refPocList[first_list].numPic; i++) {
            refPicListTmp[list_idx].list[cIdx] = refPocList[first_list].list[i];
            refPicListTmp[list_idx].idx[cIdx]  = refPocList[first_list].idx[i];
            refPicListTmp[list_idx].isLongTerm[cIdx]  = 0;
            cIdx++;
        }
        for(i = 0; i < refPocList[sec_list].numPic; i++) {
            refPicListTmp[list_idx].list[cIdx] = refPocList[sec_list].list[i];
            refPicListTmp[list_idx].idx[cIdx]  = refPocList[sec_list].idx[i];
            refPicListTmp[list_idx].isLongTerm[cIdx]  = 0;
            cIdx++;
        }
        for(i = 0; i < refPocList[LT_CURR].numPic; i++) {
            refPicListTmp[list_idx].list[cIdx] = refPocList[LT_CURR].list[i];
            refPicListTmp[list_idx].idx[cIdx]  = refPocList[LT_CURR].idx[i];
            refPicListTmp[list_idx].isLongTerm[cIdx]  = 1;
            cIdx++;
        }
        //if(sc->poc ==48)
       //  printf("###############  ADD refernec frame POC vps->max_one_active_ref_layer_flag %d num_poc_total_curr %d %d \n", vps->max_one_active_ref_layer_flag, num_poc_total_curr, cIdx);
#if REF_IDX_FRAMEWORK
        if(s->HEVCsc->layer_id) {
#if JCTVC_M0458_INTERLAYER_RPS_SIG
            
               

            for( i = 0; i < vps->max_one_active_ref_layer_flag && cIdx < num_poc_total_curr; i ++) {
#else
                for( i = 0; i < m_numILRRefIdx && cIdx < cIdx < num_poc_total_curr; i ++) {
#endif
                {
                    refPicListTmp[list_idx].list[cIdx] = sc->poc;
                    refPicListTmp[list_idx].idx[cIdx]  = find_upsample_ref_idx(s, sc->poc);
                    refPicListTmp[list_idx].isLongTerm[cIdx]  = 1;
                    cIdx++;
                }
        }
    }
#endif
        refPicList[list_idx].numPic = num_rps_curr_lx;
        if (sc->sh.ref_pic_list_modification_flag_lx[list_idx] == 1) {
            for(i = 0; i < num_rps_curr_lx; i++) {
                refPicList[list_idx].list[i] = refPicListTmp[list_idx].list[sh->list_entry_lx[list_idx][ i ]];
                refPicList[list_idx].idx[i]  = refPicListTmp[list_idx].idx[sh->list_entry_lx[list_idx][ i ]];
                refPicList[list_idx].isLongTerm[i]  = refPicListTmp[list_idx].isLongTerm[sh->list_entry_lx[list_idx][ i ]];
            }
        } else {
            for(i = 0; i < num_rps_curr_lx; i++) {
                refPicList[list_idx].list[i] = refPicListTmp[list_idx].list[i];
                refPicList[list_idx].idx[i]  = refPicListTmp[list_idx].idx[i];
                refPicList[list_idx].isLongTerm[i]  = refPicListTmp[list_idx].isLongTerm[i];
            }
        }
    }
}

void ff_hevc_set_ref_poc_list(HEVCContext *s)
{
    int i, j = 0, k = 0;
    HEVCSharedContext *sc = s->HEVCsc;
    ShortTermRPS *rps        = sc->sh.short_term_rps;
    LongTermRPS *long_rps    = &sc->sh.long_term_rps;
    RefPicList   *refPocList = sc->sh.refPocList;
    int MaxPicOrderCntLsb = 1 << sc->sps->log2_max_poc_lsb;
    if (rps != NULL) {
#if REF_IDX_FRAMEWORK
         VPS *vps = s->HEVCsc->vps;
#if ZERO_NUM_DIRECT_LAYERS
         if(s->HEVCsc->layer_id == 0 || ( s->HEVCsc->layer_id > 0 && ( vps->max_one_active_ref_layer_flag == 0 || !((s->HEVCsc->nal_unit_type >= NAL_BLA_W_LP) && (s->HEVCsc->nal_unit_type <= NAL_CRA_NUT)) ) ) )
#else
        if ((s->HEVCsc->layer_id == 0) ||
            ((s->HEVCsc->layer_id) &&  !((s->HEVCsc->nal_unit_type >= NAL_BLA_W_LP) &&
                                            (s->HEVCsc->nal_unit_type <= NAL_CRA_NUT)) ))
#endif
        {
#endif
            for (i = 0; i < rps->num_negative_pics; i ++) {
                if ( rps->used[i] == 1 ) {
                    refPocList[ST_CURR_BEF].list[j] = sc->poc + rps->delta_poc[i];
                    refPocList[ST_CURR_BEF].idx[j]  = find_ref_idx(s, refPocList[ST_CURR_BEF].list[j]);
                    j++;
                } else {
                    refPocList[ST_FOLL].list[k] = sc->poc + rps->delta_poc[i];
                    refPocList[ST_FOLL].idx[k]  = find_ref_idx(s, refPocList[ST_FOLL].list[k]);
                    k++;
                }
            }
            refPocList[ST_CURR_BEF].numPic = j;
            j = 0;
            for (i = rps->num_negative_pics; i < rps->num_delta_pocs; i ++) {
                if (rps->used[i] == 1) {
                    refPocList[ST_CURR_AFT].list[j] = sc->poc + rps->delta_poc[i];
                    refPocList[ST_CURR_AFT].idx[j]  = find_ref_idx(s, refPocList[ST_CURR_AFT].list[j]);
                    j++;
                } else {
                    refPocList[ST_FOLL].list[k] = sc->poc + rps->delta_poc[i];
                    refPocList[ST_FOLL].idx[k]  = find_ref_idx(s, refPocList[ST_FOLL].list[k]);
                    k++;
                }
            }
            refPocList[ST_CURR_AFT].numPic = j;
            refPocList[ST_FOLL].numPic = k;

            for( i = 0, j= 0, k = 0; i < long_rps->num_long_term_sps + long_rps->num_long_term_pics; i++) {
                int pocLt = long_rps->PocLsbLt[i];
                if (long_rps->delta_poc_msb_present_flag[i])
                pocLt += sc->poc - long_rps->DeltaPocMsbCycleLt[i] * MaxPicOrderCntLsb - sc->sh.pic_order_cnt_lsb;
                if (long_rps->UsedByCurrPicLt[i]) {
                    refPocList[LT_CURR].idx[j]  = find_ref_idx(s, pocLt);
                    refPocList[LT_CURR].list[j] = sc->DPB[refPocList[LT_CURR].idx[j]].poc;
                    j++;
                } else {
                    refPocList[LT_FOLL].idx[k]  = find_ref_idx(s, pocLt);
                    refPocList[LT_FOLL].list[k] = sc->DPB[refPocList[LT_FOLL].idx[k]].poc;
                    k++;
                }
            }
            refPocList[LT_CURR].numPic = j;
            refPocList[LT_FOLL].numPic = k;
#if REF_IDX_FRAMEWORK
        }
#endif
         set_ref_pic_list(s);
#if REF_IDX_FRAMEWORK
    } else  {
        set_ref_pic_list(s);
    }
#else
    }
#endif
}


int ff_hevc_get_NumPocTotalCurr(HEVCContext *s) {
    int NumPocTotalCurr = 0;
    int i;
#if REF_IDX_FRAMEWORK
    if( s->HEVCsc->sh.slice_type == I_SLICE || (s->HEVCsc->layer_id &&
                                    (s->HEVCsc->nal_unit_type >= NAL_BLA_W_LP) &&
                                    (s->HEVCsc->nal_unit_type<= NAL_CRA_NUT ) ) )
#else
        if (s->HEVCsc->sh.slice_type == I_SLICE)
#endif
        {
#if REF_IDX_FRAMEWORK
#if JCTVC_M0458_INTERLAYER_RPS_SIG
            return s->HEVCsc->sh.active_num_ILR_ref_idx;
#else
            return s->HEVCsc->vps->m_numDirectRefLayers[s->HEVCsc->layer_id];
#endif 
#else
            return 0;
#endif
        }
    ShortTermRPS *rps     = s->HEVCsc->sh.short_term_rps;
    LongTermRPS *long_rps = &s->HEVCsc->sh.long_term_rps;
    if (rps != NULL) {
        for( i = 0; i < rps->num_negative_pics; i++ )
            if( rps->used[i] == 1 )
                NumPocTotalCurr++;
        for (i = rps->num_negative_pics; i < rps->num_delta_pocs; i ++)
            if( rps->used[i] == 1 )
                NumPocTotalCurr++;
        for( i = 0; i < long_rps->num_long_term_sps + long_rps->num_long_term_pics; i++ )
            if( long_rps->UsedByCurrPicLt[ i ] == 1 )
                NumPocTotalCurr++;
    }
#if REF_IDX_FRAMEWORK
    if(s->HEVCsc->layer_id)
    {
#if JCTVC_M0458_INTERLAYER_RPS_SIG
        NumPocTotalCurr += s->HEVCsc->sh.active_num_ILR_ref_idx;
#else
        NumPocTotalCurr += s->HEVCsc->vps->m_numDirectRefLayers[s->HEVCsc->layer_id];
#endif
    }
#endif
    return NumPocTotalCurr;
}
