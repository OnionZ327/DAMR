﻿/* ====================================================================================================================

  The copyright in this software is being made available under the License included below.
  This software may be subject to other third party and contributor rights, including patent rights, and no such
  rights are granted under this license.

  Copyright (c) 2018, HUAWEI TECHNOLOGIES CO., LTD. All rights reserved.
  Copyright (c) 2018, SAMSUNG ELECTRONICS CO., LTD. All rights reserved.
  Copyright (c) 2018, PEKING UNIVERSITY SHENZHEN GRADUATE SCHOOL. All rights reserved.
  Copyright (c) 2018, PENGCHENG LABORATORY. All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are permitted only for
  the purpose of developing standards within Audio and Video Coding Standard Workgroup of China (AVS) and for testing and
  promoting such standards. The following conditions are required to be met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
      the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
      the following disclaimer in the documentation and/or other materials provided with the distribution.
    * The name of HUAWEI TECHNOLOGIES CO., LTD. or SAMSUNG ELECTRONICS CO., LTD. may not be used to endorse or promote products derived from
      this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

* ====================================================================================================================
*/

#include "enc_def.h"
#include "com_ipred.h"
#include <math.h>

/* Define the Search Range for int-pel */
#define SEARCH_RANGE_IPEL_RA               384
#define SEARCH_RANGE_IPEL_LD               64
/* Define the Search Range for sub-pel ME */
#define SEARCH_RANGE_SPEL                  3

#define MV_COST(pi, mv_bits) (u32)(((pi)->lambda_mv * mv_bits + (1 << 15)) >> 16)
#define SWAP(a, b, t) { (t) = (a); (a) = (b); (b) = (t); }

#if SIMD_AFFINE
#define CALC_EQUAL_COEFF_8PXLS(x1,x2,y1,y2,tmp0,tmp1,tmp2,tmp3,inter0,inter1,inter2,inter3,load_location)      \
{                                                                                                              \
inter0 = _mm_mul_epi32(x1, y1);                                                                                \
inter1 = _mm_mul_epi32(tmp0, tmp2);                                                                            \
inter2 = _mm_mul_epi32(x2, y2);                                                                                \
inter3 = _mm_mul_epi32(tmp1, tmp3);                                                                            \
inter2 = _mm_add_epi64(inter0, inter2);                                                                        \
inter3 = _mm_add_epi64(inter1, inter3);                                                                        \
inter0 = _mm_loadl_epi64(load_location);                                                                       \
inter3 = _mm_add_epi64(inter2, inter3);                                                                        \
inter1 = _mm_srli_si128(inter3, 8);                                                                            \
inter3 = _mm_add_epi64(inter1, inter3);                                                                        \
inter3 = _mm_add_epi64(inter0, inter3);                                                                        \
}

#define CALC_EQUAL_COEFF_4PXLS(x1,y1,tmp0,tmp1,inter0,inter1,inter2,load_location)        \
{                                                                                         \
inter0 = _mm_mul_epi32(x1, y1);                                                           \
inter1 = _mm_mul_epi32(tmp0, tmp1);                                                       \
inter2 = _mm_loadl_epi64(load_location);                                                  \
inter1 = _mm_add_epi64(inter0, inter1);                                                   \
inter0 = _mm_srli_si128(inter1, 8);                                                       \
inter0 = _mm_add_epi64(inter0, inter1);                                                   \
inter0 = _mm_add_epi64(inter2, inter0);                                                   \
}
#endif

/* q-pel search pattern */
static s8 tbl_search_pattern_qpel_8point[8][2] =
{
    {-1,  0}, { 0,  1}, { 1,  0}, { 0, -1},
    {-1,  1}, { 1,  1}, {-1, -1}, { 1, -1}
};

static const s8 tbl_diapos_partial[2][16][2] =
{
    {
        {-2, 0}, {-1, 1}, {0, 2}, {1, 1}, {2, 0}, {1, -1}, {0, -2}, {-1, -1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}
    },
    {
        {-4, 0}, {-3, 1}, {-2, 2}, {-1, 3}, {0, 4}, {1, 3}, {2, 2}, {3, 1}, {4, 0}, {3, -1}, {2, -2}, {1, -3}, {0, -4}, {-1, -3}, {-2, -2}, {-3, -1}
    }
};

static s8 tbl_search_pattern_hpel_partial[8][2] =
{
    {-2, 0}, {-2, 2}, {0, 2}, {2, 2}, {2, 0}, {2, -2}, {0, -2}, {-2, -2}
};

__inline static u32 get_exp_golomb_bits(u32 abs_mvd)
{
    int bits = 0;
    int len_i, len_c, nn;
    /* abs(mvd) */
    nn = ((abs_mvd + 1) >> 1);
    for (len_i = 0; len_i < 16 && nn != 0; len_i++)
    {
        nn >>= 1;
    }
    len_c = (len_i << 1) + 1;
    bits += len_c;
    /* sign */
    if (abs_mvd)
    {
        bits++;
    }
    return bits;
}

static int get_mv_bits_with_mvr(int mvd_x, int mvd_y, int num_refp, int refi, u8 mvr_idx)
{
    int bits = 0;
    bits = ((mvd_x >> mvr_idx) > 2048 || (mvd_x >> mvr_idx) <= -2048) ? get_exp_golomb_bits(COM_ABS(mvd_x) >> mvr_idx) : enc_tbl_mv_bits[mvd_x >> mvr_idx];
    bits += ((mvd_y >> mvr_idx) > 2048 || (mvd_y >> mvr_idx) <= -2048) ? get_exp_golomb_bits(COM_ABS(mvd_y) >> mvr_idx) : enc_tbl_mv_bits[mvd_y >> mvr_idx];
    bits += enc_tbl_refi_bits[num_refp][refi];
    if (mvr_idx == MAX_NUM_MVR - 1)
    {
        bits += mvr_idx;
    }
    else
    {
        bits += mvr_idx + 1;
    }
    return bits;
}

static void get_range_ipel(ENC_PINTER * pi, s16 mvc[MV_D], s16 range[MV_RANGE_DIM][MV_D], int ref_idx, int lidx)
{
    int offset = pi->gop_size >> 1;

    int max_search_range = COM_CLIP3(pi->max_search_range >> 2, pi->max_search_range, (pi->max_search_range * COM_ABS(pi->ptr - (int)pi->refp[ref_idx][lidx].ptr) + offset) / pi->gop_size);
    int offset_x, offset_y, rangexy;
    int range_offset = 12 * (1 << (pi->curr_mvr - 1));
    if (pi->curr_mvr == 0)
    {
        int max_qpel_sr = pi->max_search_range >> 3;
        rangexy = COM_CLIP3(max_qpel_sr >> 2, max_qpel_sr, (max_qpel_sr * COM_ABS(pi->ptr - (int)pi->refp[ref_idx][lidx].ptr) + offset) / pi->gop_size);
    }
    else if (pi->curr_mvr == 1)
    {
        int max_hpel_sr = pi->max_search_range >> 2;
        rangexy = COM_CLIP3(max_hpel_sr >> 2, max_hpel_sr, (max_hpel_sr * COM_ABS(pi->ptr - (int)pi->refp[ref_idx][lidx].ptr) + offset) / pi->gop_size);
    }
    else if (pi->curr_mvr == 2)
    {
        int max_ipel_sr = pi->max_search_range >> 1;
        rangexy = COM_CLIP3(max_ipel_sr >> 2, max_ipel_sr, (max_ipel_sr * COM_ABS(pi->ptr - (int)pi->refp[ref_idx][lidx].ptr) + offset) / pi->gop_size);
    }
    else
    {
        int max_spel_sr = pi->max_search_range;
        rangexy = COM_CLIP3(max_spel_sr >> 2, max_spel_sr, (max_spel_sr * COM_ABS(pi->ptr - (int)pi->refp[ref_idx][lidx].ptr) + offset) / pi->gop_size);
    }

    assert(rangexy <= max_search_range);

    if (pi->curr_mvr > 0)
    {
        if ((abs(pi->max_imv[MV_X]) + range_offset) > rangexy)
        {
            offset_x = rangexy;
        }
        else
        {
            offset_x = abs(pi->max_imv[MV_X]) + range_offset;
        }
        if ((abs(pi->max_imv[MV_Y]) + range_offset) > rangexy)
        {
            offset_y = rangexy;
        }
        else
        {
            offset_y = abs(pi->max_imv[MV_Y]) + range_offset;
        }
    }
    else
    {
        offset_x = rangexy;
        offset_y = rangexy;
    }
    /* define search range for int-pel search and clip it if needs */
    range[MV_RANGE_MIN][MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mvc[MV_X] - (s16)offset_x);
    range[MV_RANGE_MAX][MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mvc[MV_X] + (s16)offset_x);
    range[MV_RANGE_MIN][MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mvc[MV_Y] - (s16)offset_y);
    range[MV_RANGE_MAX][MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mvc[MV_Y] + (s16)offset_y);

    com_assert(range[MV_RANGE_MIN][MV_X] <= range[MV_RANGE_MAX][MV_X]);
    com_assert(range[MV_RANGE_MIN][MV_Y] <= range[MV_RANGE_MAX][MV_Y]);
}

#if AWP
static u32 calc_sad_mask_16b(int pu_w, int pu_h, void *src1, void *src2, void * hardmask, int s_src1, int s_src2, int s_mask, int bit_depth)
{
    u32 cost = 0;
    int num_seg_in_pu_w = 1, num_seg_in_pu_h = 1;
    int seg_w_log2 = com_tbl_log2[pu_w];
    int seg_h_log2 = com_tbl_log2[pu_h];
    s16 *src1_seg, *src2_seg, *mask_seg;
    s16* s1 = (s16 *)src1;
    s16* s2 = (s16 *)src2;
    s16* m = (s16 *)hardmask;

    if (seg_w_log2 == -1)
    {
        num_seg_in_pu_w = 3;
        seg_w_log2 = (pu_w == 48) ? 4 : (pu_w == 24 ? 3 : 2);
    }

    if (seg_h_log2 == -1)
    {
        num_seg_in_pu_h = 3;
        seg_h_log2 = (pu_h == 48) ? 4 : (pu_h == 24 ? 3 : 2);
    }

    if (num_seg_in_pu_w == 1 && num_seg_in_pu_h == 1)
    {
        cost += enc_sad_mask_16b(seg_w_log2, seg_h_log2, s1, s2, m, s_src1, s_src2, s_mask, bit_depth);
        return cost;
    }

    for (int j = 0; j < num_seg_in_pu_h; j++)
    {
        for (int i = 0; i < num_seg_in_pu_w; i++)
        {
            src1_seg = s1 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src1;
            src2_seg = s2 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src2;
            mask_seg = m + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_mask;
            cost += enc_sad_mask_16b(seg_w_log2, seg_h_log2, src1_seg, src2_seg, mask_seg, s_src1, s_src2, s_mask, bit_depth);
        }
    }
    return cost;
}
#endif

#if TB_SPLIT_EXT
static u32 calc_sad_16b(int pu_w, int pu_h, void *src1, void *src2, int s_src1, int s_src2, int bit_depth)
{
    u32 cost = 0;
    int num_seg_in_pu_w = 1, num_seg_in_pu_h = 1;
    int seg_w_log2 = com_tbl_log2[pu_w];
    int seg_h_log2 = com_tbl_log2[pu_h];
    s16 *src1_seg, *src2_seg;
    s16* s1 = (s16 *)src1;
    s16* s2 = (s16 *)src2;

    if (seg_w_log2 == -1)
    {
        num_seg_in_pu_w = 3;
        seg_w_log2 = (pu_w == 48) ? 4 : (pu_w == 24 ? 3 : 2);
    }

    if (seg_h_log2 == -1)
    {
        num_seg_in_pu_h = 3;
        seg_h_log2 = (pu_h == 48) ? 4 : (pu_h == 24 ? 3 : 2);
    }

    if (num_seg_in_pu_w == 1 && num_seg_in_pu_h == 1)
    {
        cost += enc_sad_16b(seg_w_log2, seg_h_log2, s1, s2, s_src1, s_src2, bit_depth);
        return cost;
    }

    for (int j = 0; j < num_seg_in_pu_h; j++)
    {
        for (int i = 0; i < num_seg_in_pu_w; i++)
        {
            src1_seg = s1 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src1;
            src2_seg = s2 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src2;
            cost += enc_sad_16b(seg_w_log2, seg_h_log2, src1_seg, src2_seg, s_src1, s_src2, bit_depth);
        }
    }
    return cost;
}

static u32 calc_satd_16b(int pu_w, int pu_h, void *src1, void *src2, int s_src1, int s_src2, int bit_depth)
{
    u32 cost = 0;
    int num_seg_in_pu_w = 1, num_seg_in_pu_h = 1;
    int seg_w_log2 = com_tbl_log2[pu_w];
    int seg_h_log2 = com_tbl_log2[pu_h];
    s16 *src1_seg, *src2_seg;
    s16* s1 = (s16 *)src1;
    s16* s2 = (s16 *)src2;

    if (seg_w_log2 == -1)
    {
        num_seg_in_pu_w = 3;
        seg_w_log2 = (pu_w == 48) ? 4 : (pu_w == 24 ? 3 : 2);
    }

    if (seg_h_log2 == -1)
    {
        num_seg_in_pu_h = 3;
        seg_h_log2 = (pu_h == 48) ? 4 : (pu_h == 24 ? 3 : 2);
    }

    if (num_seg_in_pu_w == 1 && num_seg_in_pu_h == 1)
    {
        cost += enc_satd_16b(seg_w_log2, seg_h_log2, s1, s2, s_src1, s_src2, bit_depth);
        return cost;
    }

    for (int j = 0; j < num_seg_in_pu_h; j++)
    {
        for (int i = 0; i < num_seg_in_pu_w; i++)
        {
            src1_seg = s1 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src1;
            src2_seg = s2 + (1 << seg_w_log2) * i + (1 << seg_h_log2) * j * s_src2;
            cost += enc_satd_16b(seg_w_log2, seg_h_log2, src1_seg, src2_seg, s_src1, s_src2, bit_depth);
        }
    }
    return cost;
}
#endif

/* Get original dummy buffer for bi prediction */
static void get_org_bi(pel * org, pel * pred, int s_o, int cu_width, int cu_height, s16 * org_bi, int s_pred, int x_offset, int y_offset)
{
    int i, j;
    //it's safer not to change input pointers
    pel *org2 = org + x_offset + y_offset * s_o;
    pel *pred2 = pred + x_offset + y_offset * s_pred;
    s16 * org_bi2 = org_bi + x_offset + y_offset * s_pred;

    for (j = 0; j < cu_height; j++)
    {
        for (i = 0; i < cu_width; i++)
        {
            org_bi2[i] = ((s16)(org2[i]) << 1) - (s16)pred2[i];
        }
        org2 += s_o;
        pred2 += s_pred;
        org_bi2 += s_pred;
    }
}

#if FAST_EXT_AMVR_HMVP
static void update_mv_cands(s16 mv_x, s16 mv_y, u32 cost, int cand_max_size, int *cand_size, s16 mv_cands_uni[64][MV_D], u32 mv_cands_uni_cost[64])
{
    for (int i = 0; i < *cand_size; i++)
    {
        if (mv_cands_uni[i][MV_X] == mv_x && mv_cands_uni[i][MV_Y] == mv_y)
            return;
    }

    int shift = 0;
    int size = min(*cand_size + 1, cand_max_size);
    while (shift < size && cost < mv_cands_uni_cost[size - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int j = 1; j < shift; j++)
        {
            mv_cands_uni[size - j][MV_X] = mv_cands_uni[size - 1 - j][MV_X];
            mv_cands_uni[size - j][MV_Y] = mv_cands_uni[size - 1 - j][MV_Y];
            mv_cands_uni_cost[size - j] = mv_cands_uni_cost[size - 1 - j];
        }
        mv_cands_uni[size - shift][MV_X] = mv_x;
        mv_cands_uni[size - shift][MV_Y] = mv_y;
        mv_cands_uni_cost[size - shift] = cost;

        *cand_size = min(size, cand_max_size);
    }
}
#endif

static u32 me_raster(ENC_PINTER * pi, int x, int y, int w, int h, s8 refi, int lidx, s16 range[MV_RANGE_DIM][MV_D], s16 gmvp[MV_D], s16 mv[MV_D])
{
    int bit_depth = pi->bit_depth;
    COM_PIC *ref_pic;
    pel      *org, *ref;
    u32        mv_bits, best_mv_bits;
    u32       cost_best, cost;
    int       i, j;
    s16       mv_x, mv_y;
    s32       search_step_x = max(RASTER_SEARCH_STEP, (w >> 1)); /* Adaptive step size : Half of CU dimension */
    s32       search_step_y = max(RASTER_SEARCH_STEP, (h >> 1)); /* Adaptive step size : Half of CU dimension */
    s16       center_mv[MV_D];
    s32       search_step;
    search_step_x = search_step_y = max(RASTER_SEARCH_STEP, min(w >> 1, h >> 1));
#if OBMC
    org = pi->org_obmc;
#else
    org = pi->Yuv_org[Y_C] + y * pi->stride_org[Y_C] + x;
#endif
    ref_pic = pi->refp[refi][lidx].pic;
    best_mv_bits = 0;
    cost_best = COM_UINT32_MAX;
#if MULTI_REF_ME_STEP
    for (i = range[MV_RANGE_MIN][MV_Y]; i <= range[MV_RANGE_MAX][MV_Y]; i += (search_step_y * (refi + 1)))
    {
        for (j = range[MV_RANGE_MIN][MV_X]; j <= range[MV_RANGE_MAX][MV_X]; j += (search_step_x * (refi + 1)))
#else
    for (i = range[MV_RANGE_MIN][MV_Y]; i <= range[MV_RANGE_MAX][MV_Y]; i += search_step_y)
    {
        for (j = range[MV_RANGE_MIN][MV_X]; j <= range[MV_RANGE_MAX][MV_X]; j += search_step_x)
#endif
        {
            mv_x = (s16)j;
            mv_y = (s16)i;
            if (pi->curr_mvr > 2)
            {
                com_mv_rounding_s16(mv_x, mv_y, &mv_x, &mv_y, pi->curr_mvr - 2, pi->curr_mvr - 2);
            }
            /* get MVD bits */
            mv_bits = (u32)get_mv_bits_with_mvr((mv_x << 2) - gmvp[MV_X], (mv_y << 2) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
            mv_bits += 2; // add inter_dir bits, raster me is only performed for uni-prediction
            /* get MVD cost_best */
            cost = MV_COST(pi, mv_bits);
            ref = ref_pic->y + mv_x + mv_y * ref_pic->stride_luma;
            /* get sad */
#if FAST_EXT_AMVR_HMVP
#if OBMC
            u32 cost_sad = calc_sad_16b(w, h, org, ref, w, ref_pic->stride_luma, bit_depth);
#else
            u32 cost_sad = calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
            cost += cost_sad;

            update_mv_cands(mv_x, mv_y, cost_sad, pi->mv_cands_uni_max_size, &pi->mv_cands_uni_size[lidx][refi], pi->mv_cands_uni[lidx][refi], pi->mv_cands_uni_cost[lidx][refi]);
#else
#if OBMC
            cost += calc_sad_16b(w, h, org, ref, w, ref_pic->stride_luma, bit_depth);
#else
            cost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
#endif
            /* check if motion cost_best is less than minimum cost_best */
            if (cost < cost_best)
            {
                mv[MV_X] = ((mv_x - (s16)x) << 2);
                mv[MV_Y] = ((mv_y - (s16)y) << 2);
                cost_best = cost;
                best_mv_bits = mv_bits;
            }
        }
    }
    /* Grid search around best mv for all dyadic step sizes till integer pel */
#if MULTI_REF_ME_STEP
    search_step = (refi + 1) * max(search_step_x, search_step_y) >> 1;
#else
    search_step = max(search_step_x, search_step_y) >> 1;
#endif
    while (search_step > 0)
    {
        center_mv[MV_X] = mv[MV_X];
        center_mv[MV_Y] = mv[MV_Y];
        for (i = -search_step; i <= search_step; i += search_step)
        {
            for (j = -search_step; j <= search_step; j += search_step)
            {
                mv_x = (center_mv[MV_X] >> 2) + (s16)x + (s16)j;
                mv_y = (center_mv[MV_Y] >> 2) + (s16)y + (s16)i;
                if ((mv_x < range[MV_RANGE_MIN][MV_X]) || (mv_x > range[MV_RANGE_MAX][MV_X]))
                    continue;
                if ((mv_y < range[MV_RANGE_MIN][MV_Y]) || (mv_y > range[MV_RANGE_MAX][MV_Y]))
                    continue;

                if (pi->curr_mvr > 2)
                {
                    com_mv_rounding_s16(mv_x, mv_y, &mv_x, &mv_y, pi->curr_mvr - 2, pi->curr_mvr - 2);
                }
                /* get MVD bits */
                mv_bits = get_mv_bits_with_mvr((mv_x << 2) - gmvp[MV_X], (mv_y << 2) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
                mv_bits += 2; // add inter_dir bits
                /* get MVD cost_best */
                cost = MV_COST(pi, mv_bits);
                ref = ref_pic->y + mv_x + mv_y * ref_pic->stride_luma;
                /* get sad */
#if FAST_EXT_AMVR_HMVP
#if OBMC
                u32 cost_sad = calc_sad_16b(w, h, org, ref, w, ref_pic->stride_luma, bit_depth);
#else
                u32 cost_sad = calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
                cost += cost_sad;

                update_mv_cands(mv_x, mv_y, cost_sad, pi->mv_cands_uni_max_size, &pi->mv_cands_uni_size[lidx][refi], pi->mv_cands_uni[lidx][refi], pi->mv_cands_uni_cost[lidx][refi]);
#else
#if OBMC
                cost += calc_sad_16b(w, h, org, ref, w, ref_pic->stride_luma, bit_depth);
#else
                cost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
#endif
                /* check if motion cost_best is less than minimum cost_best */
                if (cost < cost_best)
                {
                    mv[MV_X] = ((mv_x - (s16)x) << 2);
                    mv[MV_Y] = ((mv_y - (s16)y) << 2);
                    cost_best = cost;
                    best_mv_bits = mv_bits;
                }
            }
        }
        /* Halve the step size */
        search_step >>= 1;
    }
    if (best_mv_bits > 0)
    {
        pi->mot_bits[lidx] = best_mv_bits;
    }
    return cost_best;
}

static u32 me_ipel_diamond(ENC_PINTER *pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 refi, int lidx, s16 range[MV_RANGE_DIM][MV_D], s16 gmvp[MV_D], s16 mvi[MV_D], s16 mv[MV_D], int bi, int *beststep, int faststep)
{
    int bit_depth = pi->bit_depth;
    COM_PIC      *ref_pic;
    pel           *org, *ref;
    u32            cost, cost_best = COM_UINT32_MAX;
    int            mv_bits, best_mv_bits = 0;
    s16            mv_x, mv_y, mv_best_x, mv_best_y;
    int            lidx_r = (lidx == REFP_0) ? REFP_1 : REFP_0;
    s16           *org_bi = pi->org_bi + (x - cu_x) + (y - cu_y) * cu_stride;
    s16            mvc[MV_D];
    int            step = 0, i, j;
    int            min_cmv_x, min_cmv_y, max_cmv_x, max_cmv_y;
    s16            imv_x, imv_y;
    int            mvsize = 1;

#if OBMC
    org = pi->org_obmc + (x - cu_x) + (y - cu_y) * cu_stride;
#else
    org = pi->Yuv_org[Y_C] + y * pi->stride_org[Y_C] + x;
#endif
    ref_pic = pi->refp[refi][lidx].pic;
    mv_best_x = (mvi[MV_X] >> 2);
    mv_best_y = (mvi[MV_Y] >> 2);
    mv_best_x = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mv_best_x);
    mv_best_y = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mv_best_y);

    if (pi->curr_mvr > 2)
    {
        com_mv_rounding_s16(mv_best_x, mv_best_y, &mv_best_x, &mv_best_y, pi->curr_mvr - 2, pi->curr_mvr - 2);
    }

    imv_x = mv_best_x;
    imv_y = mv_best_y;
    while (1)
    {
        if (step <= 2)
        {
            if (pi->curr_mvr > 2)
            {
                min_cmv_x = (mv_best_x <= range[MV_RANGE_MIN][MV_X]) ? mv_best_x : mv_best_x - ((bi ? (BI_STEP - 2) : 1) << (pi->curr_mvr - 1));
                min_cmv_y = (mv_best_y <= range[MV_RANGE_MIN][MV_Y]) ? mv_best_y : mv_best_y - ((bi ? (BI_STEP - 2) : 1) << (pi->curr_mvr - 1));
                max_cmv_x = (mv_best_x >= range[MV_RANGE_MAX][MV_X]) ? mv_best_x : mv_best_x + ((bi ? (BI_STEP - 2) : 1) << (pi->curr_mvr - 1));
                max_cmv_y = (mv_best_y >= range[MV_RANGE_MAX][MV_Y]) ? mv_best_y : mv_best_y + ((bi ? (BI_STEP - 2) : 1) << (pi->curr_mvr - 1));
            }
            else
            {
                min_cmv_x = (mv_best_x <= range[MV_RANGE_MIN][MV_X]) ? mv_best_x : mv_best_x - (bi ? BI_STEP : 2);
                min_cmv_y = (mv_best_y <= range[MV_RANGE_MIN][MV_Y]) ? mv_best_y : mv_best_y - (bi ? BI_STEP : 2);
                max_cmv_x = (mv_best_x >= range[MV_RANGE_MAX][MV_X]) ? mv_best_x : mv_best_x + (bi ? BI_STEP : 2);
                max_cmv_y = (mv_best_y >= range[MV_RANGE_MAX][MV_Y]) ? mv_best_y : mv_best_y + (bi ? BI_STEP : 2);
            }
            if (pi->curr_mvr > 2)
            {
                mvsize = 1 << (pi->curr_mvr - 2);
            }
            else
            {
                mvsize = 1;
            }
            for (i = min_cmv_y; i <= max_cmv_y; i += mvsize)
            {
                for (j = min_cmv_x; j <= max_cmv_x; j += mvsize)
                {
                    mv_x = (s16)j;
                    mv_y = (s16)i;
                    if (mv_x > range[MV_RANGE_MAX][MV_X] ||
                            mv_x < range[MV_RANGE_MIN][MV_X] ||
                            mv_y > range[MV_RANGE_MAX][MV_Y] ||
                            mv_y < range[MV_RANGE_MIN][MV_Y])
                    {
                        cost = COM_UINT32_MAX;
                    }
                    else
                    {
                        /* get MVD bits */
                        mv_bits = get_mv_bits_with_mvr((mv_x << 2) - gmvp[MV_X], (mv_y << 2) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
                        mv_bits += bi ? 1 : 2; // add inter_dir bits
                        if (bi)
                        {
                            mv_bits += pi->mot_bits[lidx_r];
                        }
                        /* get MVD cost_best */
                        cost = MV_COST(pi, mv_bits);
                        ref = ref_pic->y + mv_x + mv_y * ref_pic->stride_luma;
                        if (bi)
                        {
                            /* get sad */
                            cost += calc_sad_16b(w, h, org_bi, ref, cu_stride, ref_pic->stride_luma, bit_depth) >> 1;
                        }
                        else
                        {
                            /* get sad */
#if FAST_EXT_AMVR_HMVP
#if OBMC
                            u32 cost_sad = calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, bit_depth);
#else
                            u32 cost_sad = calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
                            cost += cost_sad;

                            update_mv_cands(mv_x, mv_y, cost_sad, pi->mv_cands_uni_max_size, &pi->mv_cands_uni_size[lidx][refi], pi->mv_cands_uni[lidx][refi], pi->mv_cands_uni_cost[lidx][refi]);
#else
#if OBMC
                            cost += calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, bit_depth);
#else
                            cost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
#endif
                        }
                        /* check if motion cost_best is less than minimum cost_best */
                        if (cost < cost_best)
                        {
                            mv_best_x = mv_x;
                            mv_best_y = mv_y;
                            *beststep = 2;
                            cost_best = cost;
                            best_mv_bits = mv_bits;
                        }
                    }
                }
            }
            mvc[MV_X] = mv_best_x;
            mvc[MV_Y] = mv_best_y;
            get_range_ipel(pi, mvc, range, refi, lidx);
            step += 2;
        }
        else
        {
            int meidx = step > 8 ? 2 : 1;
            int multi = pi->curr_mvr > 2 ? (step * (1 << (pi->curr_mvr - 2))) : step;
            for (i = 0; i < 16; i++)
            {
                if (meidx == 1 && i > 8)
                {
                    continue;
                }
                if ((step == 4) && (i == 1 || i == 3 || i == 5 || i == 7))
                {
                    continue;
                }

                mv_x = imv_x + ((s16)(multi >> meidx) * tbl_diapos_partial[meidx - 1][i][MV_X]);
                mv_y = imv_y + ((s16)(multi >> meidx) * tbl_diapos_partial[meidx - 1][i][MV_Y]);

                if (mv_x > range[MV_RANGE_MAX][MV_X] ||
                        mv_x < range[MV_RANGE_MIN][MV_X] ||
                        mv_y > range[MV_RANGE_MAX][MV_Y] ||
                        mv_y < range[MV_RANGE_MIN][MV_Y])
                {
                    cost = COM_UINT32_MAX;
                }
                else
                {
                    /* get MVD bits */
                    mv_bits = get_mv_bits_with_mvr((mv_x << 2) - gmvp[MV_X], (mv_y << 2) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
                    mv_bits += bi ? 1 : 2; // add inter_dir bits
                    if (bi)
                    {
                        mv_bits += pi->mot_bits[lidx_r];
                    }
                    /* get MVD cost_best */
                    cost = MV_COST(pi, mv_bits);
                    ref = ref_pic->y + mv_x + mv_y * ref_pic->stride_luma;
                    if (bi)
                    {
                        /* get sad */
                        cost += calc_sad_16b(w, h, org_bi, ref, cu_stride, ref_pic->stride_luma, bit_depth) >> 1;
                    }
                    else
                    {
                        /* get sad */
#if FAST_EXT_AMVR_HMVP
#if OBMC
                        u32 cost_sad = calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, bit_depth);
#else
                        u32 cost_sad = calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
                        cost += cost_sad;

                        update_mv_cands(mv_x, mv_y, cost_sad, pi->mv_cands_uni_max_size, &pi->mv_cands_uni_size[lidx][refi], pi->mv_cands_uni[lidx][refi], pi->mv_cands_uni_cost[lidx][refi]);
#else
#if OBMC
                        cost += calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, bit_depth);
#else
                        cost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, bit_depth);
#endif
#endif
                    }
                    /* check if motion cost_best is less than minimum cost_best */
                    if (cost < cost_best)
                    {
                        mv_best_x = mv_x;
                        mv_best_y = mv_y;
                        *beststep = step;
                        cost_best = cost;
                        best_mv_bits = mv_bits;
                    }
                }
            }
        }
        if (step >= faststep)
        {
            break;
        }
        if (bi)
        {
            break;
        }
        step <<= 1;
    }
    /* set best MV */
    mv[MV_X] = ((mv_best_x - (s16)x) << 2);
    mv[MV_Y] = ((mv_best_y - (s16)y) << 2);
    if (!bi && best_mv_bits > 0)
    {
        pi->mot_bits[lidx] = best_mv_bits;
    }
    return cost_best;
}

static u32 me_spel_pattern(ENC_PINTER *pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 refi, int lidx, s16 gmvp[MV_D], s16 mvi[MV_D], s16 mv[MV_D], int bi)
{
    int bit_depth = pi->bit_depth;
    pel     *org, *ref, *pred;
    s16     *org_bi;
    u32      cost, cost_best = COM_UINT32_MAX;
    s16      mv_x, mv_y, cx, cy;
    int      lidx_r = (lidx == REFP_0) ? REFP_1 : REFP_0;
    int      i, mv_bits, s_org, s_ref, best_mv_bits;
#if OBMC
    s_org = cu_stride;
    org = pi->org_obmc + (x - cu_x) + (y - cu_y) * cu_stride;
#else
    s_org = pi->stride_org[Y_C];
    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
#endif
    s_ref = pi->refp[refi][lidx].pic->stride_luma;
    ref = pi->refp[refi][lidx].pic->y;
    org_bi = pi->org_bi + (x - cu_x) + (y - cu_y) * cu_stride;
    pred = pi->pred_buf + (x - cu_x) + (y - cu_y) * cu_stride;
    best_mv_bits = 0;
    /* make MV to be global coordinate */
    cx = mvi[MV_X] + ((s16)x << 2);
    cy = mvi[MV_Y] + ((s16)y << 2);
    /* intial value */
    mv[MV_X] = mvi[MV_X];
    mv[MV_Y] = mvi[MV_Y];

    // get initial satd cost as cost_best
    mv_bits = get_mv_bits_with_mvr(cx - gmvp[MV_X], cy - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
    mv_bits += bi ? 1 : 2; // add inter_dir bits
    if (bi)
    {
        mv_bits += pi->mot_bits[lidx_r];
    }
    /* get MVD cost_best */
    cost_best = MV_COST(pi, mv_bits);
    pel * ref_tmp = ref + (cx >> 2) + (cy >> 2)* s_ref;
    if (bi)
    {
        /* get satd */
        cost_best += calc_satd_16b(w, h, org_bi, ref_tmp, cu_stride, s_ref, bit_depth) >> 1;
    }
    else
    {
        /* get satd */
        cost_best += calc_satd_16b(w, h, org, ref_tmp, s_org, s_ref, bit_depth);
    }

    /* search upto hpel-level from here */
    /* search of large diamond pattern */
    for (i = 0; i < pi->search_pattern_hpel_cnt; i++)
    {
        mv_x = cx + pi->search_pattern_hpel[i][0];
        mv_y = cy + pi->search_pattern_hpel[i][1];
        /* get MVD bits */
        mv_bits = get_mv_bits_with_mvr(mv_x - gmvp[MV_X], mv_y - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
        mv_bits += bi ? 1 : 2; // add inter_dir bits
        if (bi)
        {
            mv_bits += pi->mot_bits[lidx_r];
        }
        /* get MVD cost_best */
        cost = MV_COST(pi, mv_bits);
        /* get the interpolated(predicted) image */
        com_mc_l(mv_x, mv_y, ref, mv_x, mv_y, s_ref, cu_stride, pred, w, h, bit_depth);

        if (bi)
        {
            /* get sad */
            cost += calc_satd_16b(w, h, org_bi, pred, cu_stride, cu_stride, bit_depth) >> 1;
        }
        else
        {
            /* get sad */
            cost += calc_satd_16b(w, h, org, pred, s_org, cu_stride, bit_depth);
        }
        /* check if motion cost_best is less than minimum cost_best */
        if (cost < cost_best)
        {
            mv[MV_X] = mv_x - ((s16)x << 2);
            mv[MV_Y] = mv_y - ((s16)y << 2);
            cost_best = cost;
            best_mv_bits = mv_bits;
        }
    }

    /* search qpel-level motion vector*/
    if (pi->me_level > ME_LEV_HPEL && pi->curr_mvr == 0)
    {
        /* make MV to be absolute coordinate */
        cx = mv[MV_X] + ((s16)x << 2);
        cy = mv[MV_Y] + ((s16)y << 2);
        for (i = 0; i < pi->search_pattern_qpel_cnt; i++)
        {
            mv_x = cx + pi->search_pattern_qpel[i][0];
            mv_y = cy + pi->search_pattern_qpel[i][1];
            /* get MVD bits */
            mv_bits = get_mv_bits_with_mvr(mv_x - gmvp[MV_X], mv_y - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
            mv_bits += bi ? 1 : 2; // add inter_dir bits
            if (bi)
            {
                mv_bits += pi->mot_bits[lidx_r];
            }
            /* get MVD cost_best */
            cost = MV_COST(pi, mv_bits);
            /* get the interpolated(predicted) image */
            com_mc_l(mv_x, mv_y, ref, mv_x, mv_y, s_ref, cu_stride, pred, w, h, bit_depth);

            if (bi)
            {
                /* get sad */
                cost += calc_satd_16b(w, h, org_bi, pred, cu_stride, cu_stride, bit_depth) >> 1;
            }
            else
            {
                /* get sad */
                cost += calc_satd_16b(w, h, org, pred, s_org, cu_stride, bit_depth);
            }
            /* check if motion cost_best is less than minimum cost_best */
            if (cost < cost_best)
            {
                mv[MV_X] = mv_x - ((s16)x << 2);
                mv[MV_Y] = mv_y - ((s16)y << 2);
                cost_best = cost;
                best_mv_bits = mv_bits;
            }
        }
    }
#if !ENC_ME_IMP
    if (!bi && best_mv_bits > 0)
    {
        pi->mot_bits[lidx] = best_mv_bits;
    }
#endif
    return cost_best;
}

#if ENC_ME_IMP
static u32 me_grad_search(ENC_PINTER *pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 refi, int lidx, s16 gmvp[MV_D], s16 mvi[MV_D], s16 mv[MV_D], int bi);
#endif

#if INTER_ME_MVLIB
#if OBMC
void sort_unimv_list(int mvr_idx, int cand_idx, u32 sad_cost, u8 mvr_idx_list[5], u8 cand_idx_list[5], u32 cost_cand_list[5])
{
    int shift = 0;
    while (shift < 5 && sad_cost < cost_cand_list[4 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            mvr_idx_list[5 - i] = mvr_idx_list[4 - i];
            cand_idx_list[5 - i] = cand_idx_list[4 - i];
            cost_cand_list[5 - i] = cost_cand_list[4 - i];
        }
        mvr_idx_list[5 - shift] = mvr_idx;
        cand_idx_list[5 - shift] = cand_idx;
        cost_cand_list[5 - shift] = sad_cost;
    }
}
#endif

#if ENC_ME_IMP
static u32 pinter_me_epzs(ENC_CTX * ctx, ENC_PINTER * pi, ENC_CORE * core, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 * refi, int lidx, s16 mvp[MV_D], s16 mv[MV_D], int bi)
#else
static u32 pinter_me_epzs(ENC_PINTER * pi, ENC_CORE * core, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 * refi, int lidx, s16 mvp[MV_D], s16 mv[MV_D], int bi)
#endif
#else
#if ENC_ME_IMP
static u32 pinter_me_epzs(ENC_CTX * ctx, ENC_PINTER * pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 * refi, int lidx, s16 mvp[MV_D], s16 mv[MV_D], int bi)
#else
static u32 pinter_me_epzs(ENC_PINTER * pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 * refi, int lidx, s16 mvp[MV_D], s16 mv[MV_D], int bi)
#endif
#endif
{
    s16 mvc[MV_D];  /* MV center for search */
    s16 gmvp[MV_D]; /* MVP in frame coordinate */
    s16 range[MV_RANGE_DIM][MV_D]; /* search range after clipping */
    s16 mvi[MV_D];
    s16 mvt[MV_D];
    u32 cost, cost_best = COM_UINT32_MAX;
    s8 ref_idx = *refi;  /* reference buffer index */
    int tmpstep = 0;
    int beststep = 0;
    gmvp[MV_X] = mvp[MV_X] + ((s16)x << 2);
    gmvp[MV_Y] = mvp[MV_Y] + ((s16)y << 2);
#if INTER_ME_MVLIB
    COM_PIC *ref_pic = pi->refp[*refi][lidx].pic;
#if OBMC
    pel     *org = pi->org_obmc + (x - cu_x) + (y - cu_y) * cu_stride;
#else
    pel     *org     = pi->Yuv_org[Y_C] + y * pi->stride_org[Y_C] + x;
#endif
    s16     *org_bi  = pi->org_bi + (x - cu_x) + (y - cu_y) * cu_stride;
    pel     *ref;
    s16     tmp_mv[MV_D];
#if OBMC
    u8     mvr_idx_list[5];
    u8     cand_idx_list[5];
    u32    cost_cand_list[5];
    if (ctx->info.sqh.obmc_enable_flag && !bi && (pi->curr_mvr < 2) && (pi->me_level > ME_LEV_IPEL))
    {
        for (int i = 0; i < 5; i++)
        {
            cost_cand_list[i] = COM_UINT32_MAX;
        }
    }
#endif
#endif

#if ENC_ME_IMP
    s16 best_grad_mv[MV_D];
#endif

    if (!bi && pi->mvp_from_hmvp_flag == 0 && pi->imv_valid[lidx][ref_idx] && pi->curr_mvr < 3)
    {
        mvi[MV_X] = pi->imv[lidx][ref_idx][MV_X] + ((s16)x << 2);
        mvi[MV_Y] = pi->imv[lidx][ref_idx][MV_Y] + ((s16)y << 2);
        mvc[MV_X] = (s16)x + (pi->imv[lidx][ref_idx][MV_X] >> 2);
        mvc[MV_Y] = (s16)y + (pi->imv[lidx][ref_idx][MV_Y] >> 2);
#if ENC_ME_IMP
        best_grad_mv[MV_X] = pi->imv[lidx][ref_idx][MV_X];
        best_grad_mv[MV_Y] = pi->imv[lidx][ref_idx][MV_Y];
#endif
    }
    else
    {
        if (bi)
        {
            mvi[MV_X] = mv[MV_X] + ((s16)x << 2);
            mvi[MV_Y] = mv[MV_Y] + ((s16)y << 2);
            mvc[MV_X] = (s16)x + (mv[MV_X] >> 2);
            mvc[MV_Y] = (s16)y + (mv[MV_Y] >> 2);
#if ENC_ME_IMP
            best_grad_mv[MV_X] = mv[MV_X];
            best_grad_mv[MV_Y] = mv[MV_Y];
#endif
        }
        else
        {
            mvi[MV_X] = mvp[MV_X] + ((s16)x << 2);
            mvi[MV_Y] = mvp[MV_Y] + ((s16)y << 2);
            mvc[MV_X] = (s16)x + (mvp[MV_X] >> 2);
            mvc[MV_Y] = (s16)y + (mvp[MV_Y] >> 2);
#if ENC_ME_IMP
            best_grad_mv[MV_X] = mvp[MV_X];
            best_grad_mv[MV_Y] = mvp[MV_Y];
#endif
        }
    }

    ref_idx = *refi;
    mvc[MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mvc[MV_X]);
    mvc[MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mvc[MV_Y]);
#if INTER_ME_MVLIB
    if (pi->curr_mvr > 2)
    {
        com_mv_rounding_s16(mvc[MV_X], mvc[MV_Y], &mvc[MV_X], &mvc[MV_Y], pi->curr_mvr - 2, pi->curr_mvr - 2);
    }
    int mv_bits = get_mv_bits_with_mvr((mvc[MV_X] << 2) - gmvp[MV_X], (mvc[MV_Y] << 2) - gmvp[MV_Y], pi->num_refp, ref_idx, pi->curr_mvr);
    u32 initCost = MV_COST(pi, mv_bits);
    ref = ref_pic->y + mvc[MV_X] + mvc[MV_Y] * ref_pic->stride_luma;
    if (bi)
    {
        /* get sad */
        initCost += calc_sad_16b(w, h, org_bi, ref, cu_stride, ref_pic->stride_luma, pi->bit_depth) >> 1;
    }
    else
    {
        /* get sad */
#if OBMC
        initCost += calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, pi->bit_depth);
#else
        initCost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, pi->bit_depth);
#endif
    }

    for (int mvr = 0; mvr < MAX_NUM_MVR; mvr++)
    {
    if (!((ctx->info.sqh.obmc_enable_flag && !bi && (pi->curr_mvr < 2) && (pi->me_level > ME_LEV_IPEL)) || (mvr == pi->curr_mvr)))
    {
        continue;
    }
    ENC_ME_MVLIB *ptr = &core->s_enc_me_mvlib[mvr];
    for (int i = 0; i < ptr->uni_mv_list_size; i++)
    {
        BLK_UNI_MV_INFO* curMvInfo = ptr->uni_mv_list + ((ptr->uni_mv_list_idx - 1 - i + ptr->uni_mv_list_max_size) % (ptr->uni_mv_list_max_size));

        int j = 0;
        for (; j < i; j++)
        {
            BLK_UNI_MV_INFO *prevMvInfo = ptr->uni_mv_list + ((ptr->uni_mv_list_idx - 1 - j + ptr->uni_mv_list_max_size) % (ptr->uni_mv_list_max_size));
            if (SAME_MV(curMvInfo->uniMvs[lidx][ref_idx], prevMvInfo->uniMvs[lidx][ref_idx]))
            {
                break;
            }
        }
        if (j < i)
        {
            continue;
        }

        tmp_mv[MV_X] = (s16)x + (curMvInfo->uniMvs[lidx][ref_idx][MV_X] >> 2);
        tmp_mv[MV_Y] = (s16)y + (curMvInfo->uniMvs[lidx][ref_idx][MV_Y] >> 2);
        tmp_mv[MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], tmp_mv[MV_X]);
        tmp_mv[MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], tmp_mv[MV_Y]);
        if (pi->curr_mvr > 2)
        {
            com_mv_rounding_s16(tmp_mv[MV_X], tmp_mv[MV_Y], &tmp_mv[MV_X], &tmp_mv[MV_Y], pi->curr_mvr - 2, pi->curr_mvr - 2);
        }
        mv_bits = get_mv_bits_with_mvr((tmp_mv[MV_X] << 2) - gmvp[MV_X], (tmp_mv[MV_Y] << 2) - gmvp[MV_Y], pi->num_refp, ref_idx, pi->curr_mvr);
        u32 tmpCost = MV_COST(pi, mv_bits);
        ref = ref_pic->y + tmp_mv[MV_X] + tmp_mv[MV_Y] * ref_pic->stride_luma;
        if (bi)
        {
            /* get sad */
            tmpCost += calc_sad_16b(w, h, org_bi, ref, cu_stride, ref_pic->stride_luma, pi->bit_depth) >> 1;
        }
        else
        {
            /* get sad */
#if OBMC
            tmpCost += calc_sad_16b(w, h, org, ref, cu_stride, ref_pic->stride_luma, pi->bit_depth);
#else
            tmpCost += calc_sad_16b(w, h, org, ref, pi->stride_org[Y_C], ref_pic->stride_luma, pi->bit_depth);
#endif
        }

#if OBMC
        if (ctx->info.sqh.obmc_enable_flag && !bi && (pi->curr_mvr < 2) && (pi->me_level > ME_LEV_IPEL))
        {
            sort_unimv_list(mvr, (u8)i, tmpCost, mvr_idx_list, cand_idx_list, cost_cand_list);
        }
#endif
        if ((tmpCost < initCost) && (mvr == pi->curr_mvr))
        {
            initCost = tmpCost;
            mvc[MV_X] = tmp_mv[MV_X];
            mvc[MV_Y] = tmp_mv[MV_Y];
        }
    }
    }

    mvi[MV_X] = mvc[MV_X] << 2;
    mvi[MV_Y] = mvc[MV_Y] << 2;
#endif
    get_range_ipel(pi, mvc, range, ref_idx, lidx);
    cost = me_ipel_diamond(pi, x, y, w, h, cu_x, cu_y, cu_stride, ref_idx, lidx, range, gmvp, mvi, mvt, bi, &tmpstep, MAX_FIRST_SEARCH_STEP);
    if (cost < cost_best)
    {
        cost_best = cost;
        mv[MV_X] = mvt[MV_X];
        mv[MV_Y] = mvt[MV_Y];
        if (abs(mvp[MV_X] - mv[MV_X]) < 2 && abs(mvp[MV_Y] - mv[MV_Y]) < 2)
        {
            beststep = 0;
        }
        else
        {
            beststep = tmpstep;
        }
    }
    if (!bi && beststep > RASTER_SEARCH_THD)
    {
        cost = me_raster(pi, x, y, w, h, ref_idx, lidx, range, gmvp, mvt);
        if (cost < cost_best)
        {
            beststep = RASTER_SEARCH_THD;
            cost_best = cost;
            mv[MV_X] = mvt[MV_X];
            mv[MV_Y] = mvt[MV_Y];
        }
    }
    if (!bi && beststep > REFINE_SEARCH_THD)
    {
        mvc[MV_X] = (s16)x + (mv[MV_X] >> 2);
        mvc[MV_Y] = (s16)y + (mv[MV_Y] >> 2);
        get_range_ipel(pi, mvc, range, ref_idx, lidx);
        mvi[MV_X] = mv[MV_X] + ((s16)x << 2);
        mvi[MV_Y] = mv[MV_Y] + ((s16)y << 2);
        cost = me_ipel_diamond(pi, x, y, w, h, cu_x, cu_y, cu_stride, ref_idx, lidx, range, gmvp, mvi, mvt, bi, &tmpstep, MAX_REFINE_SEARCH_STEP);
        if (cost < cost_best)
        {
            cost_best = cost;
            mv[MV_X] = mvt[MV_X];
            mv[MV_Y] = mvt[MV_Y];
        }
    }

#if FAST_EXT_AMVR_HMVP
    if (!bi && pi->mvp_from_hmvp_flag == 0 && pi->imv_valid[lidx][ref_idx] == 0)
#else
    if (!bi && pi->imv_valid[lidx][ref_idx] == 0)
#endif
    {
        pi->imv[lidx][ref_idx][MV_X] = mv[MV_X];
        pi->imv[lidx][ref_idx][MV_Y] = mv[MV_Y];
        pi->imv_valid[lidx][ref_idx] = 1;
    }

    if (pi->me_level > ME_LEV_IPEL && (pi->curr_mvr == 0 || pi->curr_mvr == 1))
    {
        /* sub-pel ME */
        cost = me_spel_pattern(pi, x, y, w, h, cu_x, cu_y, cu_stride, ref_idx, lidx, gmvp, mv, mvt, bi);
        cost_best = cost;
        mv[MV_X] = mvt[MV_X];
        mv[MV_Y] = mvt[MV_Y];
    }

#if ENC_ME_IMP
#if OBMC
    BOOL testGradME = FALSE;
    if (!ctx->info.sqh.obmc_enable_flag || ctx->info.pic_header.is_lowdelay)
    {
        testGradME = (pi->curr_mvr < 1);
    }
    else
    {
        if (!bi)
        {
            testGradME = TRUE;
        }
        else
        {
            testGradME = (pi->curr_mvr < 1);
        }
    }
    if (testGradME)
#else
    if (pi->curr_mvr < 1)
#endif
    {
        if (!(ctx->info.pic_header.is_lowdelay && bi))
        {
            cost = me_grad_search(pi, x, y, w, h, cu_x, cu_y, cu_stride, ref_idx, lidx, gmvp, best_grad_mv, mvt, bi);
            if (cost < cost_best)
            {
                cost_best = cost;
                mv[MV_X] = mvt[MV_X];
                mv[MV_Y] = mvt[MV_Y];
            }
        }
    }

#if OBMC
    if (ctx->info.sqh.obmc_enable_flag && !bi && (pi->curr_mvr < 2) && (pi->me_level > ME_LEV_IPEL))
    {
        int  pic_w = pi->pic_org->width_luma;
        int  pic_h = pi->pic_org->height_luma;
        s16  cur_mv[MV_D];

        pel* org = pi->org_obmc + (x - cu_x) + (y - cu_y) * cu_stride;
        int  s_org = cu_stride;
        pel* ref = pi->refp[ref_idx][lidx].pic->y;
        int  s_ref = pi->refp[ref_idx][lidx].pic->stride_luma;
        pel* pred = pi->pred_buf + (x - cu_x) + (y - cu_y) * cu_stride;
        int  s_pred = cu_stride;

        for (int i = 0; i < 5 && (cost_cand_list[i] != COM_UINT32_MAX); i++)
        {
            int idx = cand_idx_list[i];
            int mvr_idx = mvr_idx_list[i];
            ENC_ME_MVLIB *ptr = &core->s_enc_me_mvlib[mvr_idx];
            BLK_UNI_MV_INFO* curMvInfo = ptr->uni_mv_list + ((ptr->uni_mv_list_idx - 1 - idx + ptr->uni_mv_list_max_size) % (ptr->uni_mv_list_max_size));
            cur_mv[MV_X] = curMvInfo->uniMvs[lidx][ref_idx][MV_X];
            cur_mv[MV_Y] = curMvInfo->uniMvs[lidx][ref_idx][MV_Y];
            cur_mv[MV_X] = (cur_mv[MV_X] >> pi->curr_mvr) << pi->curr_mvr;
            cur_mv[MV_Y] = (cur_mv[MV_Y] >> pi->curr_mvr) << pi->curr_mvr;

            if ((cur_mv[MV_X] != mv[MV_X]) || (cur_mv[MV_Y] != mv[MV_Y]))
            {
                s16 mv_clip[MV_D];
                valid_mv_clip(x, y, pic_w, pic_h, w, h, cur_mv, mv_clip);
                s16 cx = cur_mv[MV_X] + ((s16)x << 2);
                s16 cy = cur_mv[MV_Y] + ((s16)y << 2);
                int bits = get_mv_bits_with_mvr(cx - gmvp[MV_X], cy - gmvp[MV_Y], pi->num_refp, ref_idx, pi->curr_mvr);
                bits += 2;
                cost = MV_COST(pi, bits);

                cx = mv_clip[MV_X] + ((s16)x << 2);
                cy = mv_clip[MV_Y] + ((s16)y << 2);
                com_mc_l(cur_mv[MV_X], cur_mv[MV_Y], ref, cx, cy, s_ref, cu_stride, pred, w, h, pi->bit_depth);
                cost += calc_satd_16b(w, h, org, pred, s_org, s_pred, pi->bit_depth);

                if (cost < cost_best)
                {
                    cost_best = cost;
                    mv[MV_X] = cur_mv[MV_X];
                    mv[MV_Y] = cur_mv[MV_Y];
                }
            }
        }
    }
#endif

    if (!bi)
    {
        s16 cx = mv[MV_X] + ((s16)x << 2);
        s16 cy = mv[MV_Y] + ((s16)y << 2);
        s32 bits = get_mv_bits_with_mvr(cx - gmvp[MV_X], cy - gmvp[MV_Y], pi->num_refp, ref_idx, pi->curr_mvr);
        bits += 2;
        pi->mot_bits[lidx] = bits;
    }
#endif

    return cost_best;
}

#if UMVE_ENH
void sort_cand_list(int cand_idx, double satd_cost, double* candCostList, int* candIndexList, int totalNumCands)
{
    int shift = 0;
    while (shift < totalNumCands && satd_cost < candCostList[totalNumCands - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            candIndexList[totalNumCands - i] = candIndexList[totalNumCands - 1 - i];
            candCostList[totalNumCands - i] = candCostList[totalNumCands - 1 - i];
        }
        candIndexList[totalNumCands - shift] = cand_idx;
        candCostList[totalNumCands - shift] = satd_cost;
    }
}
#endif

static void make_cand_list(ENC_CORE *core, ENC_CTX *ctx, int *mode_list, double *cost_list, int num_cands_woUMVE, int num_cands_all, int num_rdo, s16 pmv_skip_cand[MAX_SKIP_NUM][REFP_NUM][MV_D],  s8 refi_skip_cand[MAX_SKIP_NUM][REFP_NUM])
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int bit_depth = ctx->info.bit_depth_internal;
#if DMVR
    COM_DMVR dmvr;
#endif

    int skip_idx, i;
    //s16 mvp[REFP_NUM][MV_D];
    //s8 refi[REFP_NUM];
    u32 cy/*, cu, cv*/;
    double cost_y;

    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width  = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
    int cu_width_log2  = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;

    pel* y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    pel* u_org = pi->Yuv_org[U_C] + (x >> 1) + ((y >> 1) * pi->stride_org[U_C]);
    pel* v_org = pi->Yuv_org[V_C] + (x >> 1) + ((y >> 1) * pi->stride_org[V_C]);

    mod_info_curr->cu_mode = MODE_SKIP;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
    mod_info_curr->cs2_flag = 0; 
#if RSD_OPT
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
    for (i = 0; i < num_rdo; i++)
    {
        mode_list[i] = 0;
        cost_list[i] = MAX_COST;
    }
#if UMVE_ENH
    for (i = 0; i < num_cands_all && ctx->info.sqh.umve_enh_enable_flag; i++)
    {
        core->cu_cand_list[i] = -1;
        core->cu_candCost_list[i] = MAX_COST;
    }
#endif
#if OBMC
    memset(core->uni_interpf_ind, 0, sizeof(core->uni_interpf_ind));
#endif

    for (skip_idx = 0; skip_idx < num_cands_all; skip_idx++)
    {
        int bit_cnt, shift = 0;

        mod_info_curr->mv[REFP_0][MV_X] = pmv_skip_cand[skip_idx][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = pmv_skip_cand[skip_idx][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = pmv_skip_cand[skip_idx][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = pmv_skip_cand[skip_idx][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = refi_skip_cand[skip_idx][REFP_0];
        mod_info_curr->refi[REFP_1] = refi_skip_cand[skip_idx][REFP_1];
#if BGC
        mod_info_curr->bgc_flag = mod_info_curr->bgc_flag_cands[skip_idx];
        mod_info_curr->bgc_idx  = mod_info_curr->bgc_idx_cands[skip_idx];
#endif

        if (!REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            continue;
        }

#if OBMC
        if (ctx->info.sqh.obmc_enable_flag && !(REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && REFI_IS_VALID(mod_info_curr->refi[REFP_1])))
        {
            core->uni_interpf_ind[skip_idx] = TRUE;
        }
#endif

        if (skip_idx < num_cands_woUMVE)
        {
            mod_info_curr->umve_flag = 0;
            mod_info_curr->skip_idx = skip_idx;
        }
        else
        {
            mod_info_curr->umve_flag = 1;
            mod_info_curr->umve_idx = skip_idx - num_cands_woUMVE;
        }

        // skip index 1 and 2 for P slice
        if ((ctx->info.pic_header.slice_type == SLICE_P) && (mod_info_curr->skip_idx == 1 || mod_info_curr->skip_idx == 2) && (mod_info_curr->umve_flag == 0))
        {
            continue;
        }

#if DMVR
        dmvr.poc_c = ctx->ptr; 
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = (mod_info_curr->umve_flag == 0) && ctx->info.sqh.dmvr_enable_flag;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
#if OBMC
        int obmc_blk_width = cu_width;
        int obmc_blk_height = cu_height;
#endif
#if BGC_TM
        u8 tm_bgc_flag = mod_info_curr->bgc_flag;
        u8 tm_bgc_idx = mod_info_curr->bgc_idx;
        if ((!((ctx->info.sqh.sbtmvp_enable_flag && skip_idx == 0 && cu_width >= SBTMVP_MIN_SIZE && cu_height >= SBTMVP_MIN_SIZE) ||
            ((skip_idx < (core->valid_mvap_num + TRADITIONAL_SKIP_NUM)) && (skip_idx >= TRADITIONAL_SKIP_NUM)))) 
            && (mod_info_curr->cu_mode == MODE_SKIP || mod_info_curr->cu_mode == MODE_DIR) && ctx->info.sqh.bgc_tm_enable_flag)
        {
            derive_bgc(&ctx->info, ctx->pinter.refp, ctx->pic[PIC_IDX_REC], mod_info_curr, ctx->map.map_scu, bit_depth, &tm_bgc_flag, &tm_bgc_idx);
        }
#endif
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag && skip_idx == 0 && cu_width >= SBTMVP_MIN_SIZE && cu_height >= SBTMVP_MIN_SIZE)
        {
#if OBMC
            if (ctx->info.sqh.obmc_enable_flag)
            {
                obmc_blk_width = cu_width / SBTMVP_NUM_1D;
                obmc_blk_height = cu_height / SBTMVP_NUM_1D;
                store_sbtmvp_mvfield(core->sbTmvp, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
#if DISABLE_OBMC_AFFINE
                core->uni_interpf_ind[skip_idx] = TRUE;
#else
                core->uni_interpf_ind[skip_idx] = FALSE;
#endif
            }
#endif
            com_sbTmvp_mc(&ctx->info, mod_info_curr, (cu_width / SBTMVP_NUM_1D), (cu_height / SBTMVP_NUM_1D), core->sbTmvp, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 0, 0
#endif
            );
        }
        else
        {
#endif
#if MVAP
            if ((skip_idx < (core->valid_mvap_num + TRADITIONAL_SKIP_NUM)) && (skip_idx >= TRADITIONAL_SKIP_NUM))
            {
#if OBMC
                obmc_blk_width = MIN_SUB_BLOCK_SIZE;
                obmc_blk_height = MIN_SUB_BLOCK_SIZE;
#endif
                set_mvap_mvfield(cu_width >> 2, cu_height >> 2, core->valid_mvap_index[skip_idx - TRADITIONAL_SKIP_NUM], core->neighbor_motions, core->tmp_cu_mvfield);
#if OBMC
                if (ctx->info.sqh.obmc_enable_flag)
                {
                    store_subblk_mvfield(core->tmp_cu_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
#if DISABLE_OBMC_AFFINE
                    core->uni_interpf_ind[skip_idx] = TRUE;
#else
                    core->uni_interpf_ind[skip_idx] = FALSE;
#endif
                }
#endif
                com_mvap_mc(&ctx->info, mod_info_curr, (void*)core->tmp_cu_mvfield, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
                );
            }
            else
            {
#endif
#if OBMC
                if (ctx->info.sqh.obmc_enable_flag)
                {
                    store_blk_mvfield(mod_info_curr, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                }
#endif
                com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
#if MVAP
                    , 0
#endif
#if SUB_TMVP
                    , 0
#endif
#if BGC
#if BGC_TM
                    , tm_bgc_flag, tm_bgc_idx
#else
                    , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
                );
#if MVAP
            }
#endif
#if SUB_TMVP
        }
#endif
#if OBMC
        if (ctx->info.sqh.obmc_enable_flag)
        {
            pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, TRUE, FALSE, ctx->info.bit_depth_internal
                , obmc_blk_width, obmc_blk_height
                , ctx->ptr
#if BGC
#if BGC_TM
                , tm_bgc_flag, tm_bgc_idx
#else
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
#if OBMC_TEMP
                , ctx->pic[PIC_IDX_REC]
#endif
            );
        }
#endif
#if UMVE_ENH
        memcpy(core->predBuf[skip_idx], mod_info_curr->pred[Y_C], (cu_width*cu_height) * sizeof(pel));
#endif
        cy = calc_satd_16b(cu_width, cu_height, y_org, mod_info_curr->pred[Y_C], pi->stride_org[Y_C], cu_width, bit_depth);
        cost_y = (double)cy;

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);

#if SBT_FAST
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif

        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        SBAC_STORE(core->s_temp_best, core->s_temp_run);

        cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
        while (shift < num_rdo && cost_y < cost_list[num_rdo - 1 - shift])
        {
            shift++;
        }
        if (shift != 0)
        {
            for (i = 1; i < shift; i++)
            {
                mode_list[num_rdo - i] = mode_list[num_rdo - 1 - i];
                cost_list[num_rdo - i] = cost_list[num_rdo - 1 - i];
            }
            mode_list[num_rdo - shift] = skip_idx;
            cost_list[num_rdo - shift] = cost_y;
        }
#if UMVE_ENH
        if (ctx->info.sqh.umve_enh_enable_flag)
        {
            sort_cand_list(skip_idx, cost_y, core->cu_candCost_list, core->cu_cand_list, num_cands_all);
        }        
#endif
    }

}

#if INTERPF
static int make_cand_inter_filter_list(ENC_CORE *core, ENC_CTX *ctx, int *mode_list, int *inter_filter_list, double *cost_list, int num_cands_woUMVE, int num_cands_all, int num_rdo, s16 pmv_skip_cand[MAX_SKIP_NUM][REFP_NUM][MV_D],  s8 refi_skip_cand[MAX_SKIP_NUM][REFP_NUM])
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int bit_depth = ctx->info.bit_depth_internal;
#if DMVR && !UMVE_ENH
    COM_DMVR dmvr;
#endif
    int skip_idx, i;
    //s16 mvp[REFP_NUM][MV_D];
    //s8 refi[REFP_NUM];
    u32 cy/*, cu, cv*/;
    double cost_y;

    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width  = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
    int cu_width_log2  = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;

    pel* y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    pel* u_org = pi->Yuv_org[U_C] + (x >> 1) + ((y >> 1) * pi->stride_org[U_C]);
    pel* v_org = pi->Yuv_org[V_C] + (x >> 1) + ((y >> 1) * pi->stride_org[V_C]);

    int num_rdo_with_inter_filter = num_rdo;
    assert( num_rdo + NUM_RDO_INTER_FILTER <= MAX_INTER_SKIP_RDO );
    for( i = 0; i < NUM_RDO_INTER_FILTER; i++ )
    {
        cost_list[num_rdo + i] = MAX_COST;
    }
#if !UMVE_ENH
    pel inter_pred[MAX_CU_DIM];
#endif

    mod_info_curr->cu_mode = MODE_SKIP;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
#if RSD_OPT
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
    if (( mod_info_curr->cu_width * mod_info_curr->cu_height < 64 ) || ( mod_info_curr->cu_width > 64 ) || ( mod_info_curr->cu_height > 64 ))
    {
        return num_rdo;
    }
    
#if IPC
    for (skip_idx = 0; skip_idx < MAX_INTER_SKIP_RDO; skip_idx++)
#else    
    for (skip_idx = 0; skip_idx < num_rdo; skip_idx++)
#endif
    {
        inter_filter_list[skip_idx] = 0;
    }

#if UMVE_ENH
    int numTestedCands = (ctx->info.sqh.umve_enh_enable_flag ? min(24, num_cands_all) : num_rdo);
    for (skip_idx = 0; skip_idx < numTestedCands; skip_idx++)
#else
    for (skip_idx = 0; skip_idx < num_rdo; skip_idx++)
#endif
    {
        int bit_cnt, shift = 0;
#if UMVE_ENH
        int skip_idx_true = ctx->info.sqh.umve_enh_enable_flag ? core->cu_cand_list[skip_idx] : mode_list[skip_idx];
        if (skip_idx_true < 0)
        {
            continue;
        }
#else
        int skip_idx_true = mode_list[skip_idx];
#endif
#if OBMC
        BOOL single_uni_interpf = core->uni_interpf_ind[skip_idx_true];
#endif

        if (skip_idx_true < num_cands_woUMVE)
        {
            mod_info_curr->umve_flag = 0;
            mod_info_curr->skip_idx = skip_idx_true;
        }
        else
        {
            mod_info_curr->umve_flag = 1;
            mod_info_curr->umve_idx = skip_idx_true - num_cands_woUMVE;
#if UMVE_ENH
            if (!ctx->info.sqh.umve_enh_enable_flag)
#endif
            continue;
        }

        mod_info_curr->mv[REFP_0][MV_X] = pmv_skip_cand[skip_idx_true][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = pmv_skip_cand[skip_idx_true][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = pmv_skip_cand[skip_idx_true][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = pmv_skip_cand[skip_idx_true][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = refi_skip_cand[skip_idx_true][REFP_0];
        mod_info_curr->refi[REFP_1] = refi_skip_cand[skip_idx_true][REFP_1];
#if BGC
        mod_info_curr->bgc_flag = mod_info_curr->bgc_flag_cands[skip_idx_true];
        mod_info_curr->bgc_idx  = mod_info_curr->bgc_idx_cands[skip_idx_true];
#endif

        if (!REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            continue;
        }

        // skip index 1 and 2 for P slice
        if ((ctx->info.pic_header.slice_type == SLICE_P) && (mod_info_curr->skip_idx == 1 || mod_info_curr->skip_idx == 2) && (mod_info_curr->umve_flag == 0))
        {
            continue;
        }

#if UMVE_ENH
        memcpy(mod_info_curr->pred[Y_C], core->predBuf[skip_idx_true], (cu_width*cu_height) * sizeof(pel));
#else
#if DMVR
        dmvr.poc_c = ctx->ptr;
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = (mod_info_curr->umve_flag == 0) && ctx->info.sqh.dmvr_enable_flag;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag && skip_idx_true == 0 && cu_width >= SBTMVP_MIN_SIZE && cu_height >= SBTMVP_MIN_SIZE)
        {
            com_sbTmvp_mc(&ctx->info, mod_info_curr, (cu_width / SBTMVP_NUM_1D), (cu_height / SBTMVP_NUM_1D), core->sbTmvp, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 0, 0
#endif
            );
        }
        else
        {
#endif
#if MVAP
            if ((skip_idx_true < (core->valid_mvap_num + TRADITIONAL_SKIP_NUM)) && (skip_idx_true >= TRADITIONAL_SKIP_NUM))
            {
                set_mvap_mvfield(cu_width >> 2, cu_height >> 2, core->valid_mvap_index[skip_idx_true - TRADITIONAL_SKIP_NUM], core->neighbor_motions, core->tmp_cu_mvfield);
                com_mvap_mc(&ctx->info, mod_info_curr, (void*)core->tmp_cu_mvfield, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
                );
            }
            else
            {
#endif
                com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
#if MVAP
                    , 0
#endif
#if SUB_TMVP
                    , 0
#endif
#if BGC
                    , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
                );
#if MVAP
            }
#endif
#if SUB_TMVP
        }
#endif
        memcpy(inter_pred, mod_info_curr->pred[Y_C], (cu_width*cu_height) * sizeof(pel));
#endif
        int add_to_list = 0;
        for (int pfIdx = 1; pfIdx <= 2; pfIdx++)  /*  1:interpf   2:ipf   */
        {
#if OBMC
            if (pfIdx == 2 && (single_uni_interpf || add_to_list == 1))   /* ipf is done only when interpf is selected */
#else
            if (pfIdx == 2 && add_to_list == 1)   /* ipf is done only when interpf is selected */
#endif
            {
#if UMVE_ENH
                memcpy(mod_info_curr->pred[Y_C], core->predBuf[skip_idx_true], (cu_width*cu_height) * sizeof(pel));
#else
                memcpy(mod_info_curr->pred[Y_C], inter_pred, (cu_width*cu_height) * sizeof(pel));
#endif
            }
            else if (pfIdx == 2)
            {
                break;
            }
            add_to_list = 0;
            pred_inter_filter(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                              , x, y, cu_width, cu_height, bit_depth, CHANNEL_L, pfIdx);

            cy = calc_satd_16b(cu_width, cu_height, y_org, mod_info_curr->pred[Y_C], pi->stride_org[Y_C], cu_width, bit_depth);
            cost_y = (double)cy;

            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);

#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif

            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;

            cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);

            for (i = 0; i < num_rdo; i++)
            {
                if (cost_y < cost_list[i] * 1.25) //not much worse than existing candidate
                {
                    add_to_list = 1;
                    break;
                }

            }
            if (add_to_list) //add the best inter-filter candidate(s) at the end of the list
            {
                for (i = 0; i < NUM_RDO_INTER_FILTER; i++)
                {
                    if (cost_y < cost_list[num_rdo + i])
                    {
                        for (int j = NUM_RDO_INTER_FILTER - 1; j > i; j--)
                        {
                            mode_list[num_rdo + j] = mode_list[num_rdo + j - 1];
                            cost_list[num_rdo + j] = cost_list[num_rdo + j - 1];
                            inter_filter_list[num_rdo + j] = inter_filter_list[num_rdo + j - 1];
                        }
#if UMVE_ENH
                        mode_list[num_rdo + i] = skip_idx_true;
#else
                        mode_list[num_rdo + i] = mod_info_curr->skip_idx;
#endif
                        cost_list[num_rdo + i] = cost_y;
                        inter_filter_list[num_rdo + i] = pfIdx;
                        num_rdo_with_inter_filter += num_rdo_with_inter_filter < num_rdo + NUM_RDO_INTER_FILTER ? 1 : 0;
                        break;
                    }
                }
            }
        }
    }

    for( i = num_rdo_with_inter_filter - 1; i >= num_rdo + 2; i-- ) //at least try 2
    {
        if( cost_list[i] > cost_list[num_rdo] * 1.5 )
        {
            num_rdo_with_inter_filter--;
#if IPC
            inter_filter_list[i] = 0;
#endif            
        }
        else
        {
            break;
        }
    }

    assert( mode_list[num_rdo_with_inter_filter - 1] >= 0 );
    return num_rdo_with_inter_filter;
}
#endif
#if IPC
static int make_ipc_rdo_list(ENC_CORE *core, ENC_CTX *ctx, int *mode_list, int *ipc_list, double *cost_list, int num_cands_woUMVE, int num_cands_all, int num_rdo, s16 pmv_skip_cand[MAX_SKIP_NUM][REFP_NUM][MV_D],  s8 refi_skip_cand[MAX_SKIP_NUM][REFP_NUM], int no_interpf_rdo)
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int bit_depth = ctx->info.bit_depth_internal;
#if DMVR && (!UMVE_ENH || IPC)
    COM_DMVR dmvr;
#endif
    int skip_idx, i;
    u32 cy;
    double cost_y;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width  = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
    int cu_width_log2  = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;

    pel* y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    pel* u_org = pi->Yuv_org[U_C] + (x >> 1) + ((y >> 1) * pi->stride_org[U_C]);
    pel* v_org = pi->Yuv_org[V_C] + (x >> 1) + ((y >> 1) * pi->stride_org[V_C]);

    int num_rdo_with_ic = num_rdo;
    assert( num_rdo + NUM_RDO_WITH_IPC <= MAX_INTER_SKIP_RDO );
    for( i = 0; i < NUM_RDO_WITH_IPC; i++ )
    {
        cost_list[num_rdo + i] = MAX_COST;
    }
#if !UMVE_ENH || IPC
    pel inter_pred[MAX_CU_DIM];
#endif
#if IPC_SB8
    int ipc_size = 16;
#endif
    mod_info_curr->cu_mode = MODE_SKIP;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
#if RSD_OPT 
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
    if (mod_info_curr->cu_width * mod_info_curr->cu_height < IPC_MIN_BLK || mod_info_curr->cu_width > IPC_MAX_WD || mod_info_curr->cu_height > IPC_MAX_HT)
    {
        return num_rdo;
    }   
    for (skip_idx = 0; skip_idx < num_rdo; skip_idx++)
    {
        ipc_list[skip_idx] = 0;
    }
#if UMVE_ENH
#if IPC
    int numTestedCands = (ctx->info.sqh.umve_enh_enable_flag ? min(24, num_cands_all) : no_interpf_rdo);
#else
    int numTestedCands = (ctx->info.sqh.umve_enh_enable_flag ? min(24, num_cands_all) : num_rdo);
#endif                                                                                              
    for (skip_idx = 0; skip_idx < numTestedCands; skip_idx++)
#else
#if IPC
    for (skip_idx = 0; skip_idx < no_interpf_rdo; skip_idx++)
#else
    for (skip_idx = 0; skip_idx < num_rdo; skip_idx++)
#endif        
#endif
    {
        int bit_cnt, shift = 0;
#if UMVE_ENH
        int skip_idx_true = ctx->info.sqh.umve_enh_enable_flag ? core->cu_cand_list[skip_idx] : mode_list[skip_idx];
        if (skip_idx_true < 0)
        {
            continue;
        }
#else
        int skip_idx_true = mode_list[skip_idx];
#endif
        if (skip_idx_true < num_cands_woUMVE)
        {
            mod_info_curr->umve_flag = 0;
            mod_info_curr->skip_idx = skip_idx_true;
        }
        else
        {
            mod_info_curr->umve_flag = 1;
            mod_info_curr->umve_idx = skip_idx_true - num_cands_woUMVE;
#if UMVE_ENH
            if (!ctx->info.sqh.umve_enh_enable_flag)
#endif
            continue;
        }
        mod_info_curr->mv[REFP_0][MV_X] = pmv_skip_cand[skip_idx_true][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = pmv_skip_cand[skip_idx_true][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = pmv_skip_cand[skip_idx_true][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = pmv_skip_cand[skip_idx_true][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = refi_skip_cand[skip_idx_true][REFP_0];
        mod_info_curr->refi[REFP_1] = refi_skip_cand[skip_idx_true][REFP_1];
#if BGC
        mod_info_curr->bgc_flag = mod_info_curr->bgc_flag_cands[skip_idx_true];
        mod_info_curr->bgc_idx  = mod_info_curr->bgc_idx_cands[skip_idx_true];
#endif
#if IPC    
        mod_info_curr->ipc_flag = TRUE;
#endif
#if IPC 
        mod_info_curr->bgc_flag = FALSE;
#endif
        if (!REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            continue;
        }
        // skip index 1 and 2 for P slice
        if ((ctx->info.pic_header.slice_type == SLICE_P) && (mod_info_curr->skip_idx == 1 || mod_info_curr->skip_idx == 2) && (mod_info_curr->umve_flag == 0))
        {
            continue;
        }
#if UMVE_ENH && !IPC
        memcpy(mod_info_curr->pred[Y_C], core->predBuf[skip_idx_true], (cu_width*cu_height) * sizeof(pel));
#else
#if DMVR
        dmvr.poc_c = ctx->ptr;
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = (mod_info_curr->umve_flag == 0) && ctx->info.sqh.dmvr_enable_flag;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
        int obmc_blk_width = cu_width;
        int obmc_blk_height = cu_height;
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag && skip_idx_true == 0 && cu_width >= SBTMVP_MIN_SIZE && cu_height >= SBTMVP_MIN_SIZE)
        {
            if (ctx->info.sqh.obmc_enable_flag)
            {
                obmc_blk_width = cu_width / SBTMVP_NUM_1D;
                obmc_blk_height = cu_height / SBTMVP_NUM_1D;
                store_sbtmvp_mvfield(core->sbTmvp, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
            }
            com_sbTmvp_mc(&ctx->info, mod_info_curr, (cu_width / SBTMVP_NUM_1D), (cu_height / SBTMVP_NUM_1D), core->sbTmvp, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 0, 0
#endif
            );
#if IPC_SB8
            ipc_size = 8;
#endif
        }
        else
        {
#endif
#if MVAP
            if ((skip_idx_true < (core->valid_mvap_num + TRADITIONAL_SKIP_NUM)) && (skip_idx_true >= TRADITIONAL_SKIP_NUM))
            {
                obmc_blk_width = MIN_SUB_BLOCK_SIZE;
                obmc_blk_height = MIN_SUB_BLOCK_SIZE;
                set_mvap_mvfield(cu_width >> 2, cu_height >> 2, core->valid_mvap_index[skip_idx_true - TRADITIONAL_SKIP_NUM], core->neighbor_motions, core->tmp_cu_mvfield);
                if (ctx->info.sqh.obmc_enable_flag)
                {
                    store_subblk_mvfield(core->tmp_cu_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                }
                com_mvap_mc(&ctx->info, mod_info_curr, (void*)core->tmp_cu_mvfield, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
                );
#if IPC_SB8
                ipc_size = 8;
#endif
            }
            else
            {
#endif
                if (ctx->info.sqh.obmc_enable_flag)
                {
                    store_blk_mvfield(mod_info_curr, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                }
                com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, 0
#endif
#if MVAP
                    , 0
#endif
#if SUB_TMVP
                    , 0
#endif
#if BGC
                    , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
                );
#if MVAP
            }
#endif
#if SUB_TMVP
        }
#endif
        if (ctx->info.sqh.obmc_enable_flag)
        {
            pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, TRUE, FALSE, ctx->info.bit_depth_internal
                , obmc_blk_width, obmc_blk_height
                , ctx->ptr
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#if OBMC_TEMP
                , ctx->pic[PIC_IDX_REC]
#endif
            );
        }
        memcpy(inter_pred, mod_info_curr->pred[Y_C], (cu_width*cu_height) * sizeof(pel));
#endif
        int add_to_list = 0;
        for (int pfIdx = 1; pfIdx <= 3; pfIdx++) 
        {
#if UMVE_ENH && !IPC
            memcpy(mod_info_curr->pred[Y_C], core->predBuf[skip_idx_true], (cu_width*cu_height) * sizeof(pel));
#else
            memcpy(mod_info_curr->pred[Y_C], inter_pred, (cu_width*cu_height) * sizeof(pel));
#endif
            add_to_list = 0;
            pred_inter_pred_correction(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                              , x, y, cu_width, cu_height, bit_depth, CHANNEL_L, pfIdx
#if IPC_SB8
                              , ipc_size
#endif
            );
            cy = calc_satd_16b(cu_width, cu_height, y_org, mod_info_curr->pred[Y_C], pi->stride_org[Y_C], cu_width, bit_depth);
            cost_y = (double)cy;
            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
            for (i = 0; i < num_rdo; i++)
            {
                if (cost_y < cost_list[i] * 1.25) //not much worse than existing candidate
                {
                    add_to_list = 1;
                    break;
                }
            }
            if (add_to_list) //add the best inter-filter candidate(s) at the end of the list
            {
                for (i = 0; i < NUM_RDO_WITH_IPC; i++)
                {
                    if (cost_y < cost_list[num_rdo + i])
                    {
                        for (int j = NUM_RDO_WITH_IPC - 1; j > i; j--)
                        {
                            mode_list[num_rdo + j] = mode_list[num_rdo + j - 1];
                            cost_list[num_rdo + j] = cost_list[num_rdo + j - 1];
                            ipc_list[num_rdo + j] = ipc_list[num_rdo + j - 1];
                        }
#if UMVE_ENH
                        mode_list[num_rdo + i] = skip_idx_true;
#else
                        mode_list[num_rdo + i] = mod_info_curr->skip_idx;
#endif
                        cost_list[num_rdo + i] = cost_y;
                        ipc_list[num_rdo + i] = pfIdx;
                        num_rdo_with_ic += num_rdo_with_ic < num_rdo + NUM_RDO_WITH_IPC ? 1 : 0;
                        break;
                    }
                }
            }
        }
    }
    for( i = num_rdo_with_ic - 1; i >= num_rdo + 2; i-- ) //at least try 2
    {
        if( cost_list[i] > cost_list[num_rdo] * 1.5 )
        {
            num_rdo_with_ic--;
            ipc_list[i] = 0;
        }
        else
        {
            break;
        }
    }
    assert( mode_list[num_rdo_with_ic - 1] >= 0 );
    return num_rdo_with_ic;
}
#endif


void check_best_mode(ENC_CORE *core, ENC_PINTER *pi, const double cost_curr, double *cost_best)
{
    COM_MODE *bst_info = &core->mod_info_best;
    COM_MODE *cur_info = &core->mod_info_curr;
#if PRINT_CU_LEVEL_2
    if (cur_info->cu_mode != MODE_INTRA)
        printf("\nMODE %d skipIdx %2d UMVEIdx %2d mvrIdx %2d cost_curr %10.1f ", cur_info->cu_mode, cur_info->skip_idx, cur_info->umve_idx, cur_info->mvr_idx, cost_curr);
    double val = 2786890.7;
    if (cost_curr - val > -0.1 && cost_curr - val < 0.1)
    {
        int a = 0;
    }
#endif
    if (cost_curr < *cost_best)
    {
        int j, lidx;
        int cu_width_log2 = cur_info->cu_width_log2;
        int cu_height_log2 = cur_info->cu_height_log2;
        int cu_width = 1 << cu_width_log2;
        int cu_height = 1 << cu_height_log2;
        bst_info->cu_mode = cur_info->cu_mode;
#if USE_IBC
        bst_info->ibc_flag = cur_info->ibc_flag;
#endif
#if BGC
        bst_info->bgc_flag = cur_info->bgc_flag;
        bst_info->bgc_idx = cur_info->bgc_idx;
#endif
#if USE_SP
        bst_info->sp_flag = cur_info->sp_flag;
        bst_info->cs2_flag = cur_info->cs2_flag;
#if RSD_OPT
        bst_info->sp_rsd_flag = cur_info->sp_rsd_flag;
#endif
#endif

#if TB_SPLIT_EXT
        check_tb_part(cur_info);
        bst_info->pb_part = cur_info->pb_part;
        bst_info->tb_part = cur_info->tb_part;
        memcpy(&bst_info->pb_info, &cur_info->pb_info, sizeof(COM_PART_INFO));
        memcpy(&bst_info->tb_info, &cur_info->tb_info, sizeof(COM_PART_INFO));
#endif
#if SBT
        bst_info->sbt_info = cur_info->sbt_info;
#endif
#if INTER_CCNPM
        bst_info->inter_ccnpm_flag = cur_info->inter_ccnpm_flag;
#endif
#if ISTS
        bst_info->ist_tu_flag = cur_info->ist_tu_flag;
        bst_info->ph_ists_enable_flag = cur_info->ph_ists_enable_flag;
#endif
#if TS_INTER
        bst_info->ph_ts_inter_enable_flag = cur_info->ph_ts_inter_enable_flag;
#endif
        bst_info->umve_flag = cur_info->umve_flag;

        *cost_best = cost_curr;
        core->cost_best = cost_curr;

        SBAC_STORE(core->s_next_best[cu_width_log2 - 2][cu_height_log2 - 2], core->s_temp_best);
        if (bst_info->cu_mode != MODE_INTRA)
        {
            if (cur_info->affine_flag)
            {
#if BD_AFFINE_AMVR
                assert(pi->curr_mvr < MAX_NUM_AFFINE_MVR);
#else
                assert(pi->curr_mvr == 0);
#endif
            }

            if (bst_info->cu_mode == MODE_SKIP || bst_info->cu_mode == MODE_DIR)
            {
                bst_info->mvr_idx = 0;
            }
            else
            {
#if EXT_AMVR_HMVP
                bst_info->mvp_from_hmvp_flag = cur_info->mvp_from_hmvp_flag;
#endif
                bst_info->mvr_idx = pi->curr_mvr;
            }

#if MVAP
            if (core->mvap_flag)
            {
                core->best_mvap_flag = 1;
                for (s32 h = 0; h < (cu_height >> 2); h++)
                {
                    for (s32 w = 0; w < (cu_width >> 2); w++)
                    {
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_X] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_X];
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_Y] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_Y];
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_X] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_X];
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_Y] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_Y];
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_0] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_0];
                        core->best_cu_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_1] = core->tmp_cu_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_1];
                    }
                }
                for (lidx = 0; lidx < REFP_NUM; lidx++)
                {
                    bst_info->mvd[lidx][MV_X] = 0;
                    bst_info->mvd[lidx][MV_Y] = 0;
                }
            }
            else
            {
                core->best_mvap_flag   = 0;
#if ETMVP
                if (cur_info->etmvp_flag)
                {
                    for (s32 h = 0; h < (cu_height >> 2); h++)
                    {
                        for (s32 w = 0; w < (cu_width >> 2); w++)
                        {
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_X] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_X];
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_Y] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_0][MV_Y];
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_X] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_X];
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_Y] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].mv[REFP_1][MV_Y];
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_0] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_0];
                            core->best_etmvp_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_1] = core->tmp_etmvp_mvfield[w + h * (cu_width >> 2)].ref_idx[REFP_1];
                        }
                    }
                    for (lidx = 0; lidx < REFP_NUM; lidx++)
                    {
                        bst_info->mvd[lidx][MV_X] = 0;
                        bst_info->mvd[lidx][MV_Y] = 0;
                    }
                }
                else
                {
#endif
                    bst_info->refi[REFP_0] = cur_info->refi[REFP_0];
                    bst_info->refi[REFP_1] = cur_info->refi[REFP_1];
                    for (lidx = 0; lidx < REFP_NUM; lidx++)
                    {
                        bst_info->mv[lidx][MV_X] = cur_info->mv[lidx][MV_X];
                        bst_info->mv[lidx][MV_Y] = cur_info->mv[lidx][MV_Y];
                        bst_info->mvd[lidx][MV_X] = cur_info->mvd[lidx][MV_X];
                        bst_info->mvd[lidx][MV_Y] = cur_info->mvd[lidx][MV_Y];
                    }
#if ETMVP
                }
#endif
            }
#else
            bst_info->refi[REFP_0] = cur_info->refi[REFP_0];
            bst_info->refi[REFP_1] = cur_info->refi[REFP_1];
            for (lidx = 0; lidx < REFP_NUM; lidx++)
            {
                bst_info->mv[lidx][MV_X] = cur_info->mv[lidx][MV_X];
                bst_info->mv[lidx][MV_Y] = cur_info->mv[lidx][MV_Y];
                bst_info->mvd[lidx][MV_X] = cur_info->mvd[lidx][MV_X];
                bst_info->mvd[lidx][MV_Y] = cur_info->mvd[lidx][MV_Y];
            }
#endif
#if SUB_TMVP
            core->best_sbTmvp_flag = core->sbTmvp_flag;
            if (core->sbTmvp_flag)
            {
#if MVAP
                assert(core->mvap_flag == 0);
#endif
                
                for (int i = 0; i<SBTMVP_NUM; i++)
                {
                    core->best_sbTmvp[i].mv[REFP_0][MV_X] = core->sbTmvp[i].mv[REFP_0][MV_X];
                    core->best_sbTmvp[i].mv[REFP_0][MV_Y] = core->sbTmvp[i].mv[REFP_0][MV_Y];
                    core->best_sbTmvp[i].mv[REFP_1][MV_X] = core->sbTmvp[i].mv[REFP_1][MV_X];
                    core->best_sbTmvp[i].mv[REFP_1][MV_Y] = core->sbTmvp[i].mv[REFP_1][MV_Y];
                    core->best_sbTmvp[i].ref_idx[REFP_0] = core->sbTmvp[i].ref_idx[REFP_0];
                    core->best_sbTmvp[i].ref_idx[REFP_1] = core->sbTmvp[i].ref_idx[REFP_1];
                
                }
                for (lidx = 0; lidx < REFP_NUM; lidx++)
                {
                    bst_info->mvd[lidx][MV_X] = 0;
                    bst_info->mvd[lidx][MV_Y] = 0;
                }
            }
#endif
#if SMVD
            bst_info->smvd_flag = cur_info->smvd_flag;
            if ( cur_info->smvd_flag )
            {
                assert( cur_info->affine_flag == 0 );
            }
#endif
#if ETMVP
            bst_info->etmvp_flag = cur_info->etmvp_flag;
#endif
#if UNIFIED_HMVP_1
            bst_info->mvap_flag     = cur_info->mvap_flag;
            bst_info->sub_tmvp_flag = cur_info->sub_tmvp_flag;
#endif
            bst_info->affine_flag = cur_info->affine_flag;
            if (bst_info->affine_flag)
            {
                int vertex;
                int vertex_num = bst_info->affine_flag + 1;
                for (lidx = 0; lidx < REFP_NUM; lidx++)
                {
                    for (vertex = 0; vertex < vertex_num; vertex++)
                    {
                        bst_info->affine_mv[lidx][vertex][MV_X] = cur_info->affine_mv[lidx][vertex][MV_X];
                        bst_info->affine_mv[lidx][vertex][MV_Y] = cur_info->affine_mv[lidx][vertex][MV_Y];
                        bst_info->affine_mvd[lidx][vertex][MV_X] = cur_info->affine_mvd[lidx][vertex][MV_X];
                        bst_info->affine_mvd[lidx][vertex][MV_Y] = cur_info->affine_mvd[lidx][vertex][MV_Y];
                    }
                }
            }

#if AWP
            bst_info->awp_flag = cur_info->awp_flag;
            if (bst_info->awp_flag)
            {
                bst_info->skip_idx = cur_info->skip_idx;
                bst_info->awp_idx0 = cur_info->awp_idx0;
                bst_info->awp_idx1 = cur_info->awp_idx1;
                bst_info->awp_mv0[REFP_0][MV_X] = cur_info->awp_mv0[REFP_0][MV_X];
                bst_info->awp_mv0[REFP_0][MV_Y] = cur_info->awp_mv0[REFP_0][MV_Y];
                bst_info->awp_mv0[REFP_1][MV_X] = cur_info->awp_mv0[REFP_1][MV_X];
                bst_info->awp_mv0[REFP_1][MV_Y] = cur_info->awp_mv0[REFP_1][MV_Y];
                bst_info->awp_mv1[REFP_0][MV_X] = cur_info->awp_mv1[REFP_0][MV_X];
                bst_info->awp_mv1[REFP_0][MV_Y] = cur_info->awp_mv1[REFP_0][MV_Y];
                bst_info->awp_mv1[REFP_1][MV_X] = cur_info->awp_mv1[REFP_1][MV_X];
                bst_info->awp_mv1[REFP_1][MV_Y] = cur_info->awp_mv1[REFP_1][MV_Y];
                bst_info->awp_refi0[REFP_0] = cur_info->awp_refi0[REFP_0];
                bst_info->awp_refi0[REFP_1] = cur_info->awp_refi0[REFP_1];
                bst_info->awp_refi1[REFP_0] = cur_info->awp_refi1[REFP_0];
                bst_info->awp_refi1[REFP_1] = cur_info->awp_refi1[REFP_1];
#if AWP_ENH
                bst_info->awp_blend_idx = cur_info->awp_blend_idx;
                bst_info->dawp_idx = cur_info->dawp_idx;
#endif
            }
#endif

#if INTER_TM
            bst_info->tm_flag = cur_info->tm_flag;
            if (bst_info->tm_flag)
            {
                bst_info->tm_idx = cur_info->tm_idx;
            }
#endif

#if AWP_MVR
            bst_info->awp_mvr_flag0 = cur_info->awp_mvr_flag0;
            if (bst_info->awp_mvr_flag0)
            {
                bst_info->awp_mvr_idx0 = cur_info->awp_mvr_idx0;
            }
            bst_info->awp_mvr_flag1 = cur_info->awp_mvr_flag1;
            if (bst_info->awp_mvr_flag1)
            {
                bst_info->awp_mvr_idx1 = cur_info->awp_mvr_idx1;
            }
#endif

            if (bst_info->cu_mode == MODE_SKIP)
            {
                if (bst_info->umve_flag != 0)
                {
                    bst_info->umve_idx = cur_info->umve_idx;
                }
                else
                {
                    bst_info->skip_idx = cur_info->skip_idx;
#if AFFINE_UMVE
                    bst_info->affine_umve_flag = cur_info->affine_umve_flag;
                    if (bst_info->affine_umve_flag)
                    {
                        for (j = 0; j < VER_NUM; j++)
                        {
                            bst_info->affine_umve_idx[j] = cur_info->affine_umve_idx[j];
                        }
                    }
#endif
                }

                for (j = 0; j < N_C; j++)
                {
                    int size_tmp = (cu_width * cu_height) >> (j == 0 ? 0 : 2);
                    com_mcpy(bst_info->pred[j], cur_info->pred[j], size_tmp * sizeof(pel));
                }
                com_mset(bst_info->num_nz, 0, sizeof(int)*N_C*MAX_NUM_TB);
                com_mset(bst_info->coef[Y_C], 0, sizeof(s16)*cu_width*cu_height);
                com_mset(bst_info->coef[U_C], 0, sizeof(s16)*cu_width*cu_height / 4);
                com_mset(bst_info->coef[V_C], 0, sizeof(s16)*cu_width*cu_height / 4);
#if INTERPF
                bst_info->inter_filter_flag = 0;
                assert( cur_info->inter_filter_flag == 0 );
#endif
#if IPC
                bst_info->ipc_flag = cur_info->ipc_flag;
#endif
                assert(bst_info->pb_part == SIZE_2Nx2N);
            }
            else
            {
                if (bst_info->cu_mode == MODE_DIR)
                {
                    if (bst_info->umve_flag)
                    {
                        bst_info->umve_idx = cur_info->umve_idx;
                    }
                    else
                    {
                        bst_info->skip_idx = cur_info->skip_idx;
#if AFFINE_UMVE
                        bst_info->affine_umve_flag = cur_info->affine_umve_flag;
                        if (bst_info->affine_umve_flag)
                        {
                            for (j = 0; j < VER_NUM; j++)
                            {
                                bst_info->affine_umve_idx[j] = cur_info->affine_umve_idx[j];
                            }
                        }
#endif
                    }
                }
#if INTERPF
                bst_info->inter_filter_flag = cur_info->inter_filter_flag;
#endif
#if IPC
                bst_info->ipc_flag = cur_info->ipc_flag;
#endif
                for (j = 0; j < N_C; j++)
                {
                    int size_tmp = (cu_width * cu_height) >> (j == 0 ? 0 : 2);
                    cu_plane_nz_cpy(bst_info->num_nz, cur_info->num_nz, j);
                    com_mcpy(bst_info->pred[j], cur_info->pred[j], size_tmp * sizeof(pel));
                    com_mcpy(bst_info->coef[j], cur_info->coef[j], size_tmp * sizeof(s16));
                }
            }
        }
    }
#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
    double skip_mode_2_thread = pi->qp_y / 60.0 * (THRESHOLD_MVPS_CHECK - 1) + 1;
    if (bst_info->cu_mode == MODE_SKIP && cur_info->cu_mode == MODE_INTER && ((*cost_best)*skip_mode_2_thread) > cost_curr)
    {
        core->skip_mvps_check = 0;
    }
#endif
}

static s16    resi_t[N_C][MAX_CU_DIM];

#if TR_SAVE_LOAD
#if SBT_SAVELOAD
static u8 search_inter_tr_info( ENC_CORE *core, u16 cu_ssd, u8* tb_part_size, u8* sbt_info
#if IST
    , u8* ist_tu_flag
#endif
#if INTER_CCNPM
    , u8* inter_ccnpm_flag
#endif
)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    *tb_part_size = 255;
    *sbt_info = 255;
#if IST
    *ist_tu_flag = 255;
#endif
#if INTER_CCNPM
    *inter_ccnpm_flag = 255;
#endif
    ENC_BEF_DATA* p_data = &core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup];

    for( int idx = 0; idx < p_data->num_inter_pred; idx++ )
    {
        if( p_data->inter_pred_dist[idx] == cu_ssd )
        {
            *tb_part_size = p_data->inter_tb_part[idx];
            *sbt_info = p_data->sbt_info_save[idx];
#if IST
            *ist_tu_flag = p_data->ist_save[idx];
#endif
#if INTER_CCNPM
            *inter_ccnpm_flag = p_data->inter_ccnpm_save[idx];
#endif
            return 1;
        }
    }
    return 0;
}

static void save_inter_tr_info( ENC_CORE *core, u16 cu_ssd, u8 tb_part_size, u8 sbt_info
#if IST
    , u8 ist_tu_flag
#endif
#if INTER_CCNPM
    , u8 inter_ccnpm_flag
#endif
)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_BEF_DATA* p_data = &core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup];
    if( p_data->num_inter_pred == NUM_SL_INTER )
        return;

    p_data->inter_pred_dist[p_data->num_inter_pred] = cu_ssd;
    p_data->inter_tb_part[p_data->num_inter_pred] = tb_part_size;
    p_data->sbt_info_save[p_data->num_inter_pred] = sbt_info;
#if IST
    p_data->ist_save[p_data->num_inter_pred] = ist_tu_flag;
#endif
#if INTER_CCNPM
    p_data->inter_ccnpm_save[p_data->num_inter_pred] = inter_ccnpm_flag;
#endif
    p_data->num_inter_pred++;
}
#else
static u8 search_inter_tr_info(ENC_CORE *core, u16 cu_ssd)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    u8 tb_part_size = 255;
    ENC_BEF_DATA* p_data = &core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup];

    for (int idx = 0; idx < p_data->num_inter_pred; idx++)
    {
        if (p_data->inter_pred_dist[idx] == cu_ssd)
            return p_data->inter_tb_part[idx];
    }
    return tb_part_size;
}

static void save_inter_tr_info(ENC_CORE *core, u16 cu_ssd, u8 tb_part_size)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_BEF_DATA* p_data = &core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup];
    if (p_data->num_inter_pred == NUM_SL_INTER)
        return;

    p_data->inter_pred_dist[p_data->num_inter_pred] = cu_ssd;
    p_data->inter_tb_part[p_data->num_inter_pred] = tb_part_size;
    p_data->num_inter_pred++;
}
#endif
#endif

static double pinter_residue_rdo(ENC_CTX *ctx, ENC_CORE *core, int bForceAllZero
#if DMVR
                                 , BOOL apply_dmvr
#endif
)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    s16 (*coef)[MAX_CU_DIM] = mod_info_curr->coef;
    s16 (*mv)[MV_D] = mod_info_curr->mv;
    s8 *refi = mod_info_curr->refi;
    ENC_PINTER *pi = &ctx->pinter;
    pel(*pred)[MAX_CU_DIM] = mod_info_curr->pred;
    int bit_depth = ctx->info.bit_depth_internal;
    int(*num_nz_coef)[N_C], tnnz, width[N_C], height[N_C], log2_w[N_C], log2_h[N_C];
    pel(*rec)[MAX_CU_DIM];
    s64    dist[2][N_C], dist_pred[N_C];
    double cost, cost_best = MAX_COST;
    int    cbf_best[N_C], nnz_store[MAX_NUM_TB][N_C], tb_part_store;
    int    bit_cnt;
    int    i, cbf_y, cbf_u, cbf_v;
    pel   *org[N_C];
    double cost_comp_best = MAX_COST;
    int    cbf_comps[N_C] = { 0, };
    int    j;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
#if ST_CHROMA
    int use_secTrans[MAX_NUM_CHANNEL][MAX_NUM_TB] = { { 0, 0, 0, 0 },{ 0, 0, 0, 0 } }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans[MAX_NUM_CHANNEL] = { 0,0 };
#else
    int use_secTrans[MAX_NUM_TB] = { 0, 0, 0, 0 }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans = 0;
#endif
    int run_comp[3] = { 1, ctx->tree_status == TREE_LC, ctx->tree_status == TREE_LC };
#if RDO_DBK
    u8  is_from_mv_field = 0;
#endif
#if IPC_SB8
    int ipc_size = 16;
#endif
#if SBT // sbt rdo
    mod_info_curr->sbt_info = 0;
#endif

#if INTER_CCNPM
    mod_info_curr->inter_ccnpm_flag = 0;
#endif

#if !BD_AFFINE_AMVR
    if (mod_info_curr->affine_flag)
    {
        pi->curr_mvr = 0;
    }
#endif

    rec = mod_info_curr->rec;
    num_nz_coef = mod_info_curr->num_nz;
    width [Y_C] = 1 << cu_width_log2 ;
    height[Y_C] = 1 << cu_height_log2;
    width [U_C] = width[V_C] = 1 << (cu_width_log2 - 1);
    height[U_C] = height[V_C] = 1 << (cu_height_log2 - 1);
    log2_w[Y_C] = cu_width_log2;
    log2_h[Y_C] = cu_height_log2;
    log2_w[U_C] = log2_w[V_C] = cu_width_log2 - 1;
    log2_h[U_C] = log2_h[V_C] = cu_height_log2 - 1;
    org[Y_C] = pi->Yuv_org[Y_C] + (y * pi->stride_org[Y_C]) + x;
    org[U_C] = pi->Yuv_org[U_C] + ((y >> 1) * pi->stride_org[U_C]) + (x >> 1);
    org[V_C] = pi->Yuv_org[V_C] + ((y >> 1) * pi->stride_org[V_C]) + (x >> 1);
#if IST
    mod_info_curr->slice_type = ctx->slice_type;
#endif
#if ISTS
    mod_info_curr->ph_ists_enable_flag = ctx->info.pic_header.ph_ists_enable_flag;
#endif
#if TS_INTER
    mod_info_curr->ph_ts_inter_enable_flag = ctx->info.pic_header.ph_ts_inter_enable_flag;
#endif
#if DEST_PH
    mod_info_curr->ph_dest_enable_flag = ctx->info.pic_header.ph_dest_enable_flag;
#endif
    /* prediction */
#if BAWP
    if (ctx->info.pic_header.slice_type == SLICE_P && mod_info_curr->awp_flag != 1)
#else
    if (ctx->info.pic_header.slice_type == SLICE_P)
#endif
    {
        assert(REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]));
    }

    if (mod_info_curr->affine_flag)
    {
#if OBMC
        int sub_w = 4, sub_h = 4;
        if (ctx->info.sqh.obmc_enable_flag)
        {
            if (ctx->info.pic_header.affine_subblock_size_idx == 1)
            {
                sub_w = 8;
                sub_h = 8;
            }
            if (REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
            {
                sub_w = 8;
                sub_h = 8;
            }
        }
#endif
        int affine_dmvr_poc_c = ctx->ptr;
        com_affine_mc(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, bit_depth);

#if RDO_DBK
        com_set_affine_mvf(&ctx->info, mod_info_curr, ctx->refp, &ctx->map);
        is_from_mv_field = 1;
#endif
#if OBMC
        if (ctx->info.sqh.obmc_enable_flag)
        {
            pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_L) ? TRUE : FALSE), ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_C) ? TRUE : FALSE), ctx->info.bit_depth_internal
                , sub_w, sub_h
                , ctx->ptr
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#if OBMC_TEMP
                , ctx->pic[PIC_IDX_REC]
#endif
            );
        }
#endif
    }
#if AWP
    else if (mod_info_curr->awp_flag)
    {
        com_awp_mc(&ctx->info, mod_info_curr, pi->refp, ctx->tree_status, bit_depth);
#if RDO_DBK
        com_set_awp_mvf(&ctx->info, mod_info_curr, ctx->refp, &ctx->map);
        is_from_mv_field = 1;
#endif
    }
#endif
    else
    {
#if DMVR
        COM_DMVR dmvr;
        dmvr.poc_c = ctx->ptr; 
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = apply_dmvr && ctx->info.sqh.dmvr_enable_flag;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
#if OBMC
        int obmc_blk_width = cu_width;
        int obmc_blk_height = cu_height;
#endif
#if BGC_TM
        u8 tm_bgc_flag = mod_info_curr->bgc_flag;
        u8 tm_bgc_idx = mod_info_curr->bgc_idx;
        if ((!(core->sbTmvp_flag || core->mvap_flag || mod_info_curr->etmvp_flag || mod_info_curr->ipc_flag)) && (mod_info_curr->cu_mode == MODE_SKIP || mod_info_curr->cu_mode == MODE_DIR) && ctx->info.sqh.bgc_tm_enable_flag)
        {
            derive_bgc(&ctx->info, ctx->pinter.refp, ctx->pic[PIC_IDX_REC], mod_info_curr, ctx->map.map_scu, bit_depth, &tm_bgc_flag, &tm_bgc_idx);
        }
#endif
#if SUB_TMVP
        if (core->sbTmvp_flag)
        {
#if OBMC
            if (ctx->info.sqh.obmc_enable_flag)
            {
                obmc_blk_width = cu_width / SBTMVP_NUM_1D;
                obmc_blk_height = cu_height / SBTMVP_NUM_1D;
                store_sbtmvp_mvfield(core->sbTmvp, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
            }
#endif
            com_sbTmvp_mc(&ctx->info, mod_info_curr, (cu_width / SBTMVP_NUM_1D), (cu_height / SBTMVP_NUM_1D), core->sbTmvp, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 0, pi->curr_mvr
#endif
            );
#if IPC_SB8
            ipc_size = 8;
#endif
        }
        else
        {
#endif
#if MVAP
            if (core->mvap_flag)
            {
#if OBMC
                obmc_blk_width = MIN_SUB_BLOCK_SIZE;
                obmc_blk_height = MIN_SUB_BLOCK_SIZE;
#endif
                set_mvap_mvfield(cu_width >> 2, cu_height >> 2, core->valid_mvap_index[core->mod_info_curr.skip_idx - TRADITIONAL_SKIP_NUM], core->neighbor_motions, core->tmp_cu_mvfield);
#if OBMC
                if (ctx->info.sqh.obmc_enable_flag)
                {
                    store_subblk_mvfield(core->tmp_cu_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                }
#endif
                com_mvap_mc(&ctx->info, mod_info_curr, (void*)core->tmp_cu_mvfield, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, 0, pi->curr_mvr
#endif
                );
#if IPC_SB8
                ipc_size = 8;
#endif
            }
            else
            {
#endif
#if ETMVP
                if (mod_info_curr->etmvp_flag)
                {
#if OBMC
                    if (ctx->info.sqh.obmc_enable_flag)
                    {
                        obmc_blk_width = MIN_ETMVP_MC_SIZE;
                        obmc_blk_height = MIN_ETMVP_MC_SIZE;
                        store_subblk_mvfield(core->tmp_etmvp_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                    }
#endif
                    com_etmvp_mc(&ctx->info, mod_info_curr, (void*)core->tmp_etmvp_mvfield, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                        , &dmvr
#endif
#if BIO
                        , ctx->ptr, 0, pi->curr_mvr
#endif
                    );
                }
                else
                {
#endif
                    assert(mod_info_curr->pb_info.sub_scup[0] == mod_info_curr->scup);
#if OBMC
                    if (ctx->info.sqh.obmc_enable_flag)
                    {
                        store_blk_mvfield(mod_info_curr, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
                    }
#endif
                    com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, ctx->tree_status, bit_depth
#if DMVR
                        , &dmvr
#endif
#if BIO
                        , ctx->ptr, 0, pi->curr_mvr
#endif
#if MVAP
                        , 0
#endif
#if SUB_TMVP
                        , 0
#endif
#if BGC
#if BGC_TM
                        , tm_bgc_flag, tm_bgc_idx
#else
                        , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
                    );
#if ETMVP
                }
#endif
#if MVAP
            }
#endif
#if SUB_TMVP
        }
#endif
#if OBMC
        if (ctx->info.sqh.obmc_enable_flag)
        {
            pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_L) ? TRUE : FALSE), ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_C) ? TRUE : FALSE), ctx->info.bit_depth_internal
                , obmc_blk_width, obmc_blk_height
                , ctx->ptr
#if BGC
#if BGC_TM
                , tm_bgc_flag, tm_bgc_idx
#else
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
#if OBMC_TEMP
                , ctx->pic[PIC_IDX_REC]
#endif
            );
        }
#endif
#if INTERPF
        if(mod_info_curr->inter_filter_flag)
        {
            pred_inter_filter(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                              , x, y, cu_width, cu_height, bit_depth, ctx->tree_status, mod_info_curr->inter_filter_flag);
        }
#endif
#if IPC
        if(mod_info_curr->ipc_flag)
        {
            pred_inter_pred_correction(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                              , x, y, cu_width, cu_height, bit_depth, ctx->tree_status, mod_info_curr->ipc_flag
#if IPC_SB8
                              , ipc_size
#endif
            );
        }
#endif
    }

    /* get residual */
    enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);
    for (i = 0; i < N_C; i++)
    {
        if( !run_comp[i] )
            dist[0][i] = dist_pred[i] = 0;
        else
        {
            dist[0][i] = dist_pred[i] = enc_ssd_16b(log2_w[i], log2_h[i], pred[i], org[i], width[i], pi->stride_org[i], bit_depth);
        }
    }
#if SBT_FAST
    core->dist_no_resi[Y_C] = dist[0][Y_C];
    core->dist_no_resi[U_C] = dist[0][U_C];
    core->dist_no_resi[V_C] = dist[0][V_C];
#endif
#if RDO_DBK
    calc_delta_dist_filter_boundary(ctx, core, PIC_REC(ctx), PIC_ORG(ctx), cu_width, cu_height, pred, cu_width, x, y, 0, 0, refi, mv, is_from_mv_field);
    dist[0][Y_C] += ctx->delta_dist;
#endif

    /* test all zero case */
    memset(cbf_best, 0, sizeof(int) * N_C);
    if (mod_info_curr->cu_mode != MODE_DIR) // do not check forced zero for direct mode
    {
        memset(num_nz_coef, 0, sizeof(int) * MAX_NUM_TB * N_C);
        mod_info_curr->tb_part = SIZE_2Nx2N;
        if (ctx->tree_status == TREE_LC)
        {
            cost_best = (double)dist[0][Y_C] + ((dist[0][U_C] + dist[0][V_C]) * ctx->dist_chroma_weight[0]);
        }
        else
        {
            assert(ctx->tree_status == TREE_L);
            cost_best = (double)dist[0][Y_C];
        }

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        cost_best += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        SBAC_STORE(core->s_temp_best, core->s_temp_run);
#if SBT_FAST
        core->cost_best = min( cost_best, core->cost_best );
#endif
    }

    /* transform and quantization */
    bForceAllZero |= (mod_info_curr->cu_mode == MODE_SKIP);
#if PARTITIONING_OPT
    //force all zero (no residual) for CU size larger than 64x64
    bForceAllZero |= cu_width_log2 > 7 || cu_height_log2 > 7;
#else
    //force all zero (no residual) for CU size larger than 64x64
    bForceAllZero |= cu_width_log2 > 6 || cu_height_log2 > 6;
#endif

#if INTER_CCNPM
    u16 cu_ssd_u16 = 0;
    int try_pbt = 0;
#endif

    if (!bForceAllZero)
    {
#if TR_EARLY_TERMINATE
        core->dist_pred_luma = dist_pred[Y_C];
#endif
#if TR_SAVE_LOAD //load
        s64 cu_ssd_s64 = dist_pred[Y_C] + dist_pred[U_C] + dist_pred[V_C];
#if !INTER_CCNPM
        u16 cu_ssd_u16 = 0;
#endif
        core->best_tb_part_hist = 255;
#if SBT_SAVELOAD
        core->best_sbt_info_hist = 255;
#endif
#if IST
        core->best_ist_hist = 255;
#endif
#if INTER_CCNPM
        core->best_inter_ccnpm_hist = 255;
#endif
#if INTERPF && !INTER_CCNPM
        int try_pbt = 0;
#endif

#if SBT_SAVELOAD
        if( ( ctx->info.sqh.position_based_transform_enable_flag &&
            is_tb_avaliable( ctx->info, mod_info_curr ) && mod_info_curr->pb_part == SIZE_2Nx2N )
            || com_sbt_allow( mod_info_curr, ctx->info.sqh.sbt_enable_flag, ctx->tree_status 
#if IST_SBT_IBC
				, &(ctx->info)
#endif
			)
#if IST
            || ((mod_info_curr->cu_mode == MODE_INTER || mod_info_curr->cu_mode == MODE_DIR) && ctx->tree_status != TREE_C && cu_width_log2 < 5 && cu_height_log2 < 5
#if ISTS
            && !mod_info_curr->ph_ists_enable_flag
#endif
            )
#endif
            )
#else
        if (ctx->info.sqh.position_based_transform_enable_flag &&
                is_tb_avaliable(ctx->info, mod_info_curr) && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
            int shift_val = min(cu_width_log2 + cu_height_log2, 9);
            cu_ssd_u16 = (u16)(cu_ssd_s64 + (s64)(1 << (shift_val - 1))) >> shift_val;
#if SBT_SAVELOAD
            search_inter_tr_info( core, cu_ssd_u16, &core->best_tb_part_hist, &core->best_sbt_info_hist
#if IST
                , &core->best_ist_hist
#endif
#if INTER_CCNPM
                , & core->best_inter_ccnpm_hist
#endif
            );
#else
            core->best_tb_part_hist = search_inter_tr_info(core, cu_ssd_u16);
#endif
            //core->best_tb_part_hist = 255; //enable this line to bypass the save load mechanism
#if INTERPF
            try_pbt = 1;
#endif
        }
#endif

#if TS_INTER
        mod_info_curr->ist_tu_flag = 0;
#endif
        tnnz = enc_tq_yuv_nnz(ctx, core, mod_info_curr, coef, resi_t, pi->slice_type, 0, use_secTrans, use_alt4x4Trans, refi, mv, is_from_mv_field);

#if SBT
        if( (tnnz && !mod_info_curr->sbt_info) || (mod_info_curr->num_nz[TB0][Y_C] && mod_info_curr->sbt_info) )
#else
        if (tnnz)
#endif
        {
            com_itdq_yuv(mod_info_curr, coef, resi_t, ctx->wq, cu_width_log2, cu_height_log2, pi->qp_y, pi->qp_u, pi->qp_v, bit_depth, use_secTrans, use_alt4x4Trans
#if IST_CHROMA
                , ctx->tree_status
#endif            
            );
            for (i = 0; i < N_C; i++)
            {
                if( run_comp[i] )
                {
                    com_recon( i == Y_C ? mod_info_curr->tb_part : SIZE_2Nx2N, resi_t[i], pred[i], num_nz_coef, i, width[i], height[i], width[i], rec[i], bit_depth
#if SBT
                        , mod_info_curr->sbt_info
#endif
                    );
                }

                if (is_cu_plane_nz(num_nz_coef, i))
                {
                    dist[1][i] = enc_ssd_16b(log2_w[i], log2_h[i], rec[i], org[i], width[i], pi->stride_org[i], bit_depth);
                }
                else
                {
                    dist[1][i] = dist_pred[i];
                }
            }

#if RDO_DBK
            //filter rec and calculate ssd
            calc_delta_dist_filter_boundary(ctx, core, PIC_REC(ctx), PIC_ORG(ctx), cu_width, cu_height, rec, cu_width, x, y, 0, 1, refi, mv, is_from_mv_field);
            dist[1][Y_C] += ctx->delta_dist;
#endif

            cbf_y = is_cu_plane_nz(num_nz_coef, Y_C) > 0 ? 1 : 0;
            cbf_u = is_cu_plane_nz(num_nz_coef, U_C) > 0 ? 1 : 0;
            cbf_v = is_cu_plane_nz(num_nz_coef, V_C) > 0 ? 1 : 0;

            if (ctx->tree_status == TREE_LC)
            {
                cost = (double)dist[cbf_y][Y_C] + ((dist[cbf_u][U_C] + dist[cbf_v][V_C]) * ctx->dist_chroma_weight[0]);
            }
            else
            {
                assert(ctx->tree_status == TREE_L);
                cost = (double)dist[cbf_y][Y_C];
            }

            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            //enc_sbac_bit_reset(&core->s_temp_run);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
            if (cost < cost_best)
            {
                cost_best = cost;
                cbf_best[Y_C] = cbf_y;
                cbf_best[U_C] = cbf_u;
                cbf_best[V_C] = cbf_v;
                SBAC_STORE(core->s_temp_best, core->s_temp_run);
            }
            SBAC_LOAD(core->s_temp_prev_comp_best, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);

            /* cbf test for each component */
            com_mcpy(nnz_store, num_nz_coef, sizeof(int) * MAX_NUM_TB * N_C);
            tb_part_store = mod_info_curr->tb_part;
            if (ctx->tree_status == TREE_LC) // do not need to test for luma only
            {
                for (i = 0; i < N_C; i++)
                {
                    if (is_cu_plane_nz(nnz_store, i) > 0)
                    {
#if SBT
                        if( mod_info_curr->sbt_info && i == 0 )
                        {
                            cbf_comps[i] = 1;
                            continue;
                        }
#endif
                        cost_comp_best = MAX_COST;
                        SBAC_LOAD(core->s_temp_prev_comp_run, core->s_temp_prev_comp_best);
                        for (j = 0; j < 2; j++)
                        {
                            cost = dist[j][i] * (i == 0 ? 1 : ctx->dist_chroma_weight[i - 1]);
                            if (j)
                            {
                                cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                                if (i == 0)
                                    mod_info_curr->tb_part = tb_part_store;
                            }
                            else
                            {
                                cu_plane_nz_cln(num_nz_coef, i);
                                if (i == 0)
                                    mod_info_curr->tb_part = SIZE_2Nx2N;
                            }

                            SBAC_LOAD(core->s_temp_run, core->s_temp_prev_comp_run);
                            //enc_sbac_bit_reset(&core->s_temp_run);
                            bit_cnt = enc_get_bit_number(&core->s_temp_run);
                            enc_bit_est_inter_comp(ctx, core, coef[i], i);
                            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
                            cost += RATE_TO_COST_LAMBDA(ctx->lambda[i], bit_cnt);
                            if (cost < cost_comp_best)
                            {
                                cost_comp_best = cost;
                                cbf_comps[i] = j;
                                SBAC_STORE(core->s_temp_prev_comp_best, core->s_temp_run);
                            }
                        }
                    }
                    else
                    {
                        cbf_comps[i] = 0;
                    }
                }

                // do not set if all zero, because the case of all zero has been tested
                if (cbf_comps[Y_C] != 0 || cbf_comps[U_C] != 0 || cbf_comps[V_C] != 0)
                {
                    for (i = 0; i < N_C; i++)
                    {
                        if (cbf_comps[i])
                        {
                            cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                            if (i == 0)
                                mod_info_curr->tb_part = tb_part_store;
                        }
                        else
                        {
                            cu_plane_nz_cln(num_nz_coef, i);
                            if (i == 0)
                                mod_info_curr->tb_part = SIZE_2Nx2N;
                        }
                    }

                    // if the best num_nz_coef is changed
                    if (!is_cu_nz_equ(num_nz_coef, nnz_store))
                    {
                        cbf_y = cbf_comps[Y_C];
                        cbf_u = cbf_comps[U_C];
                        cbf_v = cbf_comps[V_C];
                        cost = dist[cbf_y][Y_C] + ((dist[cbf_u][U_C] + dist[cbf_v][V_C]) * ctx->dist_chroma_weight[0]);

                        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
                        //enc_sbac_bit_reset(&core->s_temp_run);
                        bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
                        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
                        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
                        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
                        cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
                        if (cost < cost_best)
                        {
                            cost_best = cost;
                            cbf_best[Y_C] = cbf_y;
                            cbf_best[U_C] = cbf_u;
                            cbf_best[V_C] = cbf_v;
                            SBAC_STORE(core->s_temp_best, core->s_temp_run);
                        }
                    }
                }
            }

            for (i = 0; i < N_C; i++)
            {
                if (cbf_best[i])
                {
                    cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                    if (i == 0)
                        mod_info_curr->tb_part = tb_part_store;
                }
                else
                {
                    cu_plane_nz_cln(num_nz_coef, i);
                    if (i == 0)
                        mod_info_curr->tb_part = SIZE_2Nx2N;
                }

                if (is_cu_plane_nz(num_nz_coef, i) == 0 && is_cu_plane_nz(nnz_store, i) != 0)
                {
                    com_mset(coef[i], 0, sizeof(s16) * ((cu_width * cu_height) >> (i == 0 ? 0 : 2)));
                }
            }
#if SBT //important
            if( !is_cu_plane_nz( num_nz_coef, Y_C ) )
                mod_info_curr->sbt_info = 0;
#endif
        }
#if TR_SAVE_LOAD && !INTER_CCNPM // inter save
#if INTERPF
        if( core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N && try_pbt )
#else
        if (core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
#if SBT_SAVELOAD
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part, mod_info_curr->sbt_info
#if IST
                , mod_info_curr->ist_tu_flag
#endif
            );
#else
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part);
#endif
        }
#endif
    }

#if INTERPF
#if IPC
    if ((mod_info_curr->cu_mode == MODE_DIR && mod_info_curr->inter_filter_flag == 0 ) && 
        (!(!ctx->info.pic_header.ph_ipc_flag && mod_info_curr->ipc_flag)))
#else
    if (mod_info_curr->cu_mode == MODE_DIR && mod_info_curr->inter_filter_flag == 0)
#endif
#else
    if (mod_info_curr->cu_mode == MODE_DIR)
#endif
    {
        // back info
        com_mcpy(nnz_store, num_nz_coef, sizeof(int)* MAX_NUM_TB* N_C);
        int tb_part = mod_info_curr->tb_part;
#if SBT
        int sbt_info = mod_info_curr->sbt_info;
#endif
        // all zero
        memset(num_nz_coef, 0, sizeof(int) * MAX_NUM_TB * N_C);
        mod_info_curr->tb_part = SIZE_2Nx2N;
#if SBT
        mod_info_curr->sbt_info = 0;
#endif
        if (ctx->tree_status == TREE_LC)
        {
            cost = (double)dist[0][Y_C] + ((dist[0][U_C] + dist[0][V_C]) * ctx->dist_chroma_weight[0]);
        }
        else
        {
            assert(ctx->tree_status == TREE_L);
            cost = (double)dist[0][Y_C];
        }

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);

        mod_info_curr->cu_mode = MODE_SKIP;

#if SBT_FAST
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);

#if SBT_FAST
        core->cost_best = min(cost, core->cost_best);
#endif
        if (cost < cost_best)
        {
            cost_best = cost;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }
        else
        {
            com_mcpy(num_nz_coef, nnz_store, sizeof(int) * MAX_NUM_TB * N_C);
            mod_info_curr->cu_mode = MODE_DIR;
            mod_info_curr->tb_part  = tb_part;
#if SBT
            mod_info_curr->sbt_info = sbt_info;
#endif
        }
    }

#if INTER_CCNPM
    if (ctx->info.sqh.ccnpm_enable_flag && ctx->tree_status == TREE_LC && is_cu_plane_nz(mod_info_curr->num_nz, Y_C) && core->best_inter_ccnpm_hist != 0 && (x > 0 || y > 0))
    {
        int cu_mode = mod_info_curr->cu_mode;
        com_mcpy(nnz_store, num_nz_coef, sizeof(int)* MAX_NUM_TB* N_C);

        static s16 bak_2Nx2N_coef[N_C][MAX_CU_DIM];
        static s16 bak_pred[N_C][MAX_CU_DIM];
        int cu_size = 1 << (cu_width_log2 + cu_height_log2);
        for (i = U_C; i < N_C; i++)
        {
            memcpy(bak_2Nx2N_coef[i], coef[i], sizeof(s16)* (i==Y_C? cu_size : cu_size>>2));
            memcpy(bak_pred[i], pred[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
        }

        mod_info_curr->inter_ccnpm_flag = 1;

        /* get residual */
        for (int j = 0; j < cu_height; j++)
        {
            for (int i = 0; i < cu_width; i++)
            {
                *(PIC_REC(ctx)->y + (y+j) * PIC_REC(ctx)->stride_luma + (x+i)) = rec[Y_C][j * cu_width + i];
            }
        }
        u16 avail_cu = com_get_avail_intra(mod_info_curr->x_scu, mod_info_curr->y_scu, ctx->info.pic_width_in_scu, mod_info_curr->scup, ctx->map.map_scu, ctx->info.pic_height_in_scu, cu_width, cu_height);
        for (int comp_id = U_C; comp_id < N_C; comp_id++)
        {
            ipred_ccnpm(comp_id, PIC_REC(ctx)->y, PIC_REC(ctx)->stride_luma, PIC_REC(ctx)->u, PIC_REC(ctx)->v, PIC_REC(ctx)->stride_chroma, pred[comp_id], x, y, cu_width/2, cu_height/2, bit_depth, avail_cu
#if CCNPM_TEMPLATE_OPT
                , IPD_CCNPM
#endif
#if CCNPM_LINE_BUFFER_REDUCTION
                , (1<<ctx->info.log2_max_cuwh)
#endif
            );
#if INTER_CCNPM_OPT
            ccnpm_blending(bak_pred[comp_id], pred[comp_id], cu_width / 2, cu_height / 2, bit_depth);
#endif
        }
        enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);

        cost = 0;
        for (i = U_C; i < N_C; i++)
        {
            int plane_width_log2 = cu_width_log2 - 1;
            int plane_height_log2 = cu_height_log2 - 1;
            int qp = i == U_C ? pi->qp_u : pi->qp_v;
            double lambda = i == U_C ? ctx->lambda[1] : ctx->lambda[2];
            int secT_VH = use_secTrans[CHANNEL_CHROMA][TB0];
            int alt4x4 = use_alt4x4Trans[CHANNEL_CHROMA];

            mod_info_curr->num_nz[TB0][i] = enc_tq_nnz(ctx, mod_info_curr, i, 0, qp, lambda, coef[i], resi_t[i], plane_width_log2,
                plane_height_log2, pi->slice_type, i, 0, secT_VH, alt4x4);
            com_itdq_plane(mod_info_curr, i, coef[i], pi->resi_cb[i], ctx->wq, cu_width_log2 - 1, cu_height_log2 - 1,
                qp, bit_depth, use_secTrans[CHANNEL_CHROMA], use_alt4x4Trans[CHANNEL_CHROMA]
#if IST_CHROMA
                , ctx->tree_status
#endif            
            );
            com_recon(mod_info_curr->tb_part, pi->resi_cb[i], pred[i], mod_info_curr->num_nz, i, cu_width / 2,
                cu_height / 2, cu_width / 2, rec[i], bit_depth, mod_info_curr->sbt_info);
            cost += ((double)(enc_ssd_16b(cu_width_log2-1, cu_height_log2-1, rec[i], org[i], cu_width/2, pi->stride_org[i], bit_depth))) * ctx->dist_chroma_weight[i - 1];
        }

        if (mod_info_curr->cu_mode == MODE_DIR && !is_cu_nz(mod_info_curr->num_nz))
        {
            cost = MAX_COST;
        }
        else
        {
            cost += dist[1][Y_C];

            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        }

        if (cost < cost_best)
        {
            cost_best = cost;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }
        else
        {
            mod_info_curr->inter_ccnpm_flag = 0;
            mod_info_curr->cu_mode = cu_mode;
            com_mcpy(num_nz_coef, nnz_store, sizeof(int)* MAX_NUM_TB* N_C);
            for (i = U_C; i < N_C; i++)
            {
                memcpy(coef[i], bak_2Nx2N_coef[i], sizeof(s16)* (i==Y_C? cu_size : cu_size>>2));
                memcpy(pred[i], bak_pred[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
            }
        }
    }

#if TR_SAVE_LOAD
#if INTERPF
        if( core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N && try_pbt )
#else
        if (core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
#if SBT_SAVELOAD
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part, mod_info_curr->sbt_info
#if IST
                , mod_info_curr->ist_tu_flag
#endif
#if INTER_CCNPM
                , mod_info_curr->inter_ccnpm_flag
#endif
            );
#else
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part);
#endif
        }
#endif
#endif

    if (!is_cu_plane_nz(num_nz_coef, Y_C))
        mod_info_curr->tb_part = SIZE_2Nx2N; // reset best tb_part if no residual Y
#if SBT //important
    if( !is_cu_plane_nz( num_nz_coef, Y_C ) )
        assert( mod_info_curr->sbt_info == 0);
#if SBT_FAST
    core->cost_best = min( cost_best, core->cost_best );
#endif
#endif
    return cost_best;
}

#if INTER_TM
static double pinter_residue_rdo_no_mc(ENC_CTX *ctx, ENC_CORE *core, int bForceAllZero)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    s16 (*coef)[MAX_CU_DIM] = mod_info_curr->coef;
    s16 (*mv)[MV_D] = mod_info_curr->mv;
    s8 *refi = mod_info_curr->refi;
    ENC_PINTER *pi = &ctx->pinter;
    pel(*pred)[MAX_CU_DIM] = mod_info_curr->pred;
    int bit_depth = ctx->info.bit_depth_internal;
    int(*num_nz_coef)[N_C], tnnz, width[N_C], height[N_C], log2_w[N_C], log2_h[N_C];
    pel(*rec)[MAX_CU_DIM];
    s64    dist[2][N_C], dist_pred[N_C];
    double cost, cost_best = MAX_COST;
    int    cbf_best[N_C], nnz_store[MAX_NUM_TB][N_C], tb_part_store;
    int    bit_cnt;
    int    i, cbf_y, cbf_u, cbf_v;
    pel   *org[N_C];
    double cost_comp_best = MAX_COST;
    int    cbf_comps[N_C] = { 0, };
    int    j;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
#if ST_CHROMA
    int use_secTrans[MAX_NUM_CHANNEL][MAX_NUM_TB] = { { 0, 0, 0, 0 },{ 0, 0, 0, 0 } }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans[MAX_NUM_CHANNEL] = { 0,0 };
#else
    int use_secTrans[MAX_NUM_TB] = { 0, 0, 0, 0 }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans = 0;
#endif
    int run_comp[3] = { 1, ctx->tree_status == TREE_LC, ctx->tree_status == TREE_LC };
#if RDO_DBK
    u8  is_from_mv_field = 0;
#endif
#if SBT // sbt rdo
    mod_info_curr->sbt_info = 0;
#endif

#if INTER_CCNPM
    mod_info_curr->inter_ccnpm_flag = 0;
#endif

    rec = mod_info_curr->rec;
    num_nz_coef = mod_info_curr->num_nz;
    width [Y_C] = 1 << cu_width_log2 ;
    height[Y_C] = 1 << cu_height_log2;
    width [U_C] = width[V_C] = 1 << (cu_width_log2 - 1);
    height[U_C] = height[V_C] = 1 << (cu_height_log2 - 1);
    log2_w[Y_C] = cu_width_log2;
    log2_h[Y_C] = cu_height_log2;
    log2_w[U_C] = log2_w[V_C] = cu_width_log2 - 1;
    log2_h[U_C] = log2_h[V_C] = cu_height_log2 - 1;
    org[Y_C] = pi->Yuv_org[Y_C] + (y * pi->stride_org[Y_C]) + x;
    org[U_C] = pi->Yuv_org[U_C] + ((y >> 1) * pi->stride_org[U_C]) + (x >> 1);
    org[V_C] = pi->Yuv_org[V_C] + ((y >> 1) * pi->stride_org[V_C]) + (x >> 1);
#if IST
    mod_info_curr->slice_type = ctx->slice_type;
#endif
#if ISTS
    mod_info_curr->ph_ists_enable_flag = ctx->info.pic_header.ph_ists_enable_flag;
#endif
#if TS_INTER
    mod_info_curr->ph_ts_inter_enable_flag = ctx->info.pic_header.ph_ts_inter_enable_flag;
#endif

    /* get residual */
    enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);
    for (i = 0; i < N_C; i++)
    {
        if( !run_comp[i] )
            dist[0][i] = dist_pred[i] = 0;
        else
        {
            dist[0][i] = dist_pred[i] = enc_ssd_16b(log2_w[i], log2_h[i], pred[i], org[i], width[i], pi->stride_org[i], bit_depth);
        }
    }
#if SBT_FAST
    core->dist_no_resi[Y_C] = dist[0][Y_C];
    core->dist_no_resi[U_C] = dist[0][U_C];
    core->dist_no_resi[V_C] = dist[0][V_C];
#endif
#if RDO_DBK
    calc_delta_dist_filter_boundary(ctx, core, PIC_REC(ctx), PIC_ORG(ctx), cu_width, cu_height, pred, cu_width, x, y, 0, 0, refi, mv, is_from_mv_field);
    dist[0][Y_C] += ctx->delta_dist;
#endif

    /* test all zero case */
    memset(cbf_best, 0, sizeof(int) * N_C);
    if (mod_info_curr->cu_mode != MODE_DIR) // do not check forced zero for direct mode
    {
        memset(num_nz_coef, 0, sizeof(int) * MAX_NUM_TB * N_C);
        mod_info_curr->tb_part = SIZE_2Nx2N;
        if (ctx->tree_status == TREE_LC)
        {
            cost_best = (double)dist[0][Y_C] + ((dist[0][U_C] + dist[0][V_C]) * ctx->dist_chroma_weight[0]);
        }
        else
        {
            assert(ctx->tree_status == TREE_L);
            cost_best = (double)dist[0][Y_C];
        }

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        cost_best += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        SBAC_STORE(core->s_temp_best, core->s_temp_run);
#if SBT_FAST
        core->cost_best = min( cost_best, core->cost_best );
#endif
    }

    /* transform and quantization */
    bForceAllZero |= (mod_info_curr->cu_mode == MODE_SKIP);
#if PARTITIONING_OPT
    //force all zero (no residual) for CU size larger than 64x64
    bForceAllZero |= cu_width_log2 > 7 || cu_height_log2 > 7;
#else
    //force all zero (no residual) for CU size larger than 64x64
    bForceAllZero |= cu_width_log2 > 6 || cu_height_log2 > 6;
#endif

#if INTER_CCNPM
    u16 cu_ssd_u16 = 0;
    int try_pbt = 0;
#endif

    if (!bForceAllZero)
    {
#if TR_EARLY_TERMINATE
        core->dist_pred_luma = dist_pred[Y_C];
#endif
#if TR_SAVE_LOAD //load
        s64 cu_ssd_s64 = dist_pred[Y_C] + dist_pred[U_C] + dist_pred[V_C];
#if !INTER_CCNPM
        u16 cu_ssd_u16 = 0;
#endif
        core->best_tb_part_hist = 255;
#if SBT_SAVELOAD
        core->best_sbt_info_hist = 255;
#endif
#if IST
        core->best_ist_hist = 255;
#endif
#if INTER_CCNPM
        core->best_inter_ccnpm_hist = 255;
#endif
#if INTERPF && !INTER_CCNPM
        int try_pbt = 0;
#endif

#if SBT_SAVELOAD
        if( ( ctx->info.sqh.position_based_transform_enable_flag &&
            is_tb_avaliable( ctx->info, mod_info_curr ) && mod_info_curr->pb_part == SIZE_2Nx2N )
            || com_sbt_allow( mod_info_curr, ctx->info.sqh.sbt_enable_flag, ctx->tree_status 
#if IST_SBT_IBC
				, &(ctx->info)
#endif
			)
#if IST
            || ((mod_info_curr->cu_mode == MODE_INTER || mod_info_curr->cu_mode == MODE_DIR) && ctx->tree_status != TREE_C && cu_width_log2 < 5 && cu_height_log2 < 5
#if ISTS
            && !mod_info_curr->ph_ists_enable_flag
#endif
            )
#endif
            )
#else
        if (ctx->info.sqh.position_based_transform_enable_flag &&
                is_tb_avaliable(ctx->info, mod_info_curr) && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
            int shift_val = min(cu_width_log2 + cu_height_log2, 9);
            cu_ssd_u16 = (u16)(cu_ssd_s64 + (s64)(1 << (shift_val - 1))) >> shift_val;
#if SBT_SAVELOAD
            search_inter_tr_info( core, cu_ssd_u16, &core->best_tb_part_hist, &core->best_sbt_info_hist
#if IST
                , &core->best_ist_hist
#endif
#if INTER_CCNPM
                , & core->best_inter_ccnpm_hist
#endif
            );
#else
            core->best_tb_part_hist = search_inter_tr_info(core, cu_ssd_u16);
#endif
            //core->best_tb_part_hist = 255; //enable this line to bypass the save load mechanism
#if INTERPF
            try_pbt = 1;
#endif
        }
#endif

#if TS_INTER
        mod_info_curr->ist_tu_flag = 0;
#endif
        tnnz = enc_tq_yuv_nnz(ctx, core, mod_info_curr, coef, resi_t, pi->slice_type, 0, use_secTrans, use_alt4x4Trans, refi, mv, is_from_mv_field);

#if SBT
        if( (tnnz && !mod_info_curr->sbt_info) || (mod_info_curr->num_nz[TB0][Y_C] && mod_info_curr->sbt_info) )
#else
        if (tnnz)
#endif
        {
            com_itdq_yuv(mod_info_curr, coef, resi_t, ctx->wq, cu_width_log2, cu_height_log2, pi->qp_y, pi->qp_u, pi->qp_v, bit_depth, use_secTrans, use_alt4x4Trans
#if IST_CHROMA
                , ctx->tree_status
#endif   
            );
            for (i = 0; i < N_C; i++)
            {
                if( run_comp[i] )
                {
                    com_recon( i == Y_C ? mod_info_curr->tb_part : SIZE_2Nx2N, resi_t[i], pred[i], num_nz_coef, i, width[i], height[i], width[i], rec[i], bit_depth
#if SBT
                        , mod_info_curr->sbt_info
#endif
                    );
                }

                if (is_cu_plane_nz(num_nz_coef, i))
                {
                    dist[1][i] = enc_ssd_16b(log2_w[i], log2_h[i], rec[i], org[i], width[i], pi->stride_org[i], bit_depth);
                }
                else
                {
                    dist[1][i] = dist_pred[i];
                }
            }

#if RDO_DBK
            //filter rec and calculate ssd
            calc_delta_dist_filter_boundary(ctx, core, PIC_REC(ctx), PIC_ORG(ctx), cu_width, cu_height, rec, cu_width, x, y, 0, 1, refi, mv, is_from_mv_field);
            dist[1][Y_C] += ctx->delta_dist;
#endif

            cbf_y = is_cu_plane_nz(num_nz_coef, Y_C) > 0 ? 1 : 0;
            cbf_u = is_cu_plane_nz(num_nz_coef, U_C) > 0 ? 1 : 0;
            cbf_v = is_cu_plane_nz(num_nz_coef, V_C) > 0 ? 1 : 0;

            if (ctx->tree_status == TREE_LC)
            {
                cost = (double)dist[cbf_y][Y_C] + ((dist[cbf_u][U_C] + dist[cbf_v][V_C]) * ctx->dist_chroma_weight[0]);
            }
            else
            {
                assert(ctx->tree_status == TREE_L);
                cost = (double)dist[cbf_y][Y_C];
            }

            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            //enc_sbac_bit_reset(&core->s_temp_run);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
            if (cost < cost_best)
            {
                cost_best = cost;
                cbf_best[Y_C] = cbf_y;
                cbf_best[U_C] = cbf_u;
                cbf_best[V_C] = cbf_v;
                SBAC_STORE(core->s_temp_best, core->s_temp_run);
            }
            SBAC_LOAD(core->s_temp_prev_comp_best, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);

            /* cbf test for each component */
            com_mcpy(nnz_store, num_nz_coef, sizeof(int) * MAX_NUM_TB * N_C);
            tb_part_store = mod_info_curr->tb_part;
            if (ctx->tree_status == TREE_LC) // do not need to test for luma only
            {
                for (i = 0; i < N_C; i++)
                {
                    if (is_cu_plane_nz(nnz_store, i) > 0)
                    {
#if SBT
                        if( mod_info_curr->sbt_info && i == 0 )
                        {
                            cbf_comps[i] = 1;
                            continue;
                        }
#endif
                        cost_comp_best = MAX_COST;
                        SBAC_LOAD(core->s_temp_prev_comp_run, core->s_temp_prev_comp_best);
                        for (j = 0; j < 2; j++)
                        {
                            cost = dist[j][i] * (i == 0 ? 1 : ctx->dist_chroma_weight[i - 1]);
                            if (j)
                            {
                                cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                                if (i == 0)
                                    mod_info_curr->tb_part = tb_part_store;
                            }
                            else
                            {
                                cu_plane_nz_cln(num_nz_coef, i);
                                if (i == 0)
                                    mod_info_curr->tb_part = SIZE_2Nx2N;
                            }

                            SBAC_LOAD(core->s_temp_run, core->s_temp_prev_comp_run);
                            //enc_sbac_bit_reset(&core->s_temp_run);
                            bit_cnt = enc_get_bit_number(&core->s_temp_run);
                            enc_bit_est_inter_comp(ctx, core, coef[i], i);
                            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
                            cost += RATE_TO_COST_LAMBDA(ctx->lambda[i], bit_cnt);
                            if (cost < cost_comp_best)
                            {
                                cost_comp_best = cost;
                                cbf_comps[i] = j;
                                SBAC_STORE(core->s_temp_prev_comp_best, core->s_temp_run);
                            }
                        }
                    }
                    else
                    {
                        cbf_comps[i] = 0;
                    }
                }

                // do not set if all zero, because the case of all zero has been tested
                if (cbf_comps[Y_C] != 0 || cbf_comps[U_C] != 0 || cbf_comps[V_C] != 0)
                {
                    for (i = 0; i < N_C; i++)
                    {
                        if (cbf_comps[i])
                        {
                            cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                            if (i == 0)
                                mod_info_curr->tb_part = tb_part_store;
                        }
                        else
                        {
                            cu_plane_nz_cln(num_nz_coef, i);
                            if (i == 0)
                                mod_info_curr->tb_part = SIZE_2Nx2N;
                        }
                    }

                    // if the best num_nz_coef is changed
                    if (!is_cu_nz_equ(num_nz_coef, nnz_store))
                    {
                        cbf_y = cbf_comps[Y_C];
                        cbf_u = cbf_comps[U_C];
                        cbf_v = cbf_comps[V_C];
                        cost = dist[cbf_y][Y_C] + ((dist[cbf_u][U_C] + dist[cbf_v][V_C]) * ctx->dist_chroma_weight[0]);

                        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
                        //enc_sbac_bit_reset(&core->s_temp_run);
                        bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
                        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
                        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
                        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
                        cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
                        if (cost < cost_best)
                        {
                            cost_best = cost;
                            cbf_best[Y_C] = cbf_y;
                            cbf_best[U_C] = cbf_u;
                            cbf_best[V_C] = cbf_v;
                            SBAC_STORE(core->s_temp_best, core->s_temp_run);
                        }
                    }
                }
            }

            for (i = 0; i < N_C; i++)
            {
                if (cbf_best[i])
                {
                    cu_plane_nz_cpy(num_nz_coef, nnz_store, i);
                    if (i == 0)
                        mod_info_curr->tb_part = tb_part_store;
                }
                else
                {
                    cu_plane_nz_cln(num_nz_coef, i);
                    if (i == 0)
                        mod_info_curr->tb_part = SIZE_2Nx2N;
                }

                if (is_cu_plane_nz(num_nz_coef, i) == 0 && is_cu_plane_nz(nnz_store, i) != 0)
                {
                    com_mset(coef[i], 0, sizeof(s16) * ((cu_width * cu_height) >> (i == 0 ? 0 : 2)));
                }
            }
#if SBT //important
            if( !is_cu_plane_nz( num_nz_coef, Y_C ) )
                mod_info_curr->sbt_info = 0;
#endif
        }
#if TR_SAVE_LOAD && !INTER_CCNPM // inter save
#if INTERPF
        if( core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N && try_pbt )
#else
        if (core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
#if SBT_SAVELOAD
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part, mod_info_curr->sbt_info
#if IST
                , mod_info_curr->ist_tu_flag
#endif
            );
#else
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part);
#endif
        }
#endif
    }

#if INTERPF
#if IPC
    if ((mod_info_curr->cu_mode == MODE_DIR && mod_info_curr->inter_filter_flag == 0 ) && 
        (!(!ctx->info.pic_header.ph_ipc_flag && mod_info_curr->ipc_flag)))
#else
    if (mod_info_curr->cu_mode == MODE_DIR && mod_info_curr->inter_filter_flag == 0)
#endif
#else
    if (mod_info_curr->cu_mode == MODE_DIR)
#endif
    {
        // back info
        com_mcpy(nnz_store, num_nz_coef, sizeof(int)* MAX_NUM_TB* N_C);
        int tb_part = mod_info_curr->tb_part;
#if SBT
        int sbt_info = mod_info_curr->sbt_info;
#endif
        // all zero
        memset(num_nz_coef, 0, sizeof(int) * MAX_NUM_TB * N_C);
        mod_info_curr->tb_part = SIZE_2Nx2N;
#if SBT
        mod_info_curr->sbt_info = 0;
#endif
        if (ctx->tree_status == TREE_LC)
        {
            cost = (double)dist[0][Y_C] + ((dist[0][U_C] + dist[0][V_C]) * ctx->dist_chroma_weight[0]);
        }
        else
        {
            assert(ctx->tree_status == TREE_L);
            cost = (double)dist[0][Y_C];
        }

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);

        mod_info_curr->cu_mode = MODE_SKIP;

#if SBT_FAST
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
        enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);

#if SBT_FAST
        core->cost_best = min(cost, core->cost_best);
#endif
        if (cost < cost_best)
        {
            cost_best = cost;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }
        else
        {
            com_mcpy(num_nz_coef, nnz_store, sizeof(int) * MAX_NUM_TB * N_C);
            mod_info_curr->cu_mode = MODE_DIR;
            mod_info_curr->tb_part  = tb_part;
#if SBT
            mod_info_curr->sbt_info = sbt_info;
#endif
        }
    }

#if INTER_CCNPM
    if (ctx->info.sqh.ccnpm_enable_flag && ctx->tree_status == TREE_LC && is_cu_plane_nz(mod_info_curr->num_nz, Y_C) && core->best_inter_ccnpm_hist != 0 && (x > 0 || y > 0))
    {
        int cu_mode = mod_info_curr->cu_mode;
        com_mcpy(nnz_store, num_nz_coef, sizeof(int)* MAX_NUM_TB* N_C);

        static s16 bak_2Nx2N_coef[N_C][MAX_CU_DIM];
        static s16 bak_pred[N_C][MAX_CU_DIM];
        int cu_size = 1 << (cu_width_log2 + cu_height_log2);
        for (i = U_C; i < N_C; i++)
        {
            memcpy(bak_2Nx2N_coef[i], coef[i], sizeof(s16)* (i==Y_C? cu_size : cu_size>>2));
            memcpy(bak_pred[i], pred[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
        }

        mod_info_curr->inter_ccnpm_flag = 1;

        /* get residual */
        for (int j = 0; j < cu_height; j++)
        {
            for (int i = 0; i < cu_width; i++)
            {
                *(PIC_REC(ctx)->y + (y+j) * PIC_REC(ctx)->stride_luma + (x+i)) = rec[Y_C][j * cu_width + i];
            }
        }
        u16 avail_cu = com_get_avail_intra(mod_info_curr->x_scu, mod_info_curr->y_scu, ctx->info.pic_width_in_scu, mod_info_curr->scup, ctx->map.map_scu, ctx->info.pic_height_in_scu, cu_width, cu_height);
        for (int comp_id = U_C; comp_id < N_C; comp_id++)
        {
            ipred_ccnpm(comp_id, PIC_REC(ctx)->y, PIC_REC(ctx)->stride_luma, PIC_REC(ctx)->u, PIC_REC(ctx)->v, PIC_REC(ctx)->stride_chroma, pred[comp_id], x, y, cu_width/2, cu_height/2, bit_depth, avail_cu
#if CCNPM_TEMPLATE_OPT
                , IPD_CCNPM
#endif
#if CCNPM_LINE_BUFFER_REDUCTION
                , (1<<ctx->info.log2_max_cuwh)
#endif
            );
#if INTER_CCNPM_OPT
            ccnpm_blending(bak_pred[comp_id], pred[comp_id], cu_width / 2, cu_height / 2, bit_depth);
#endif
        }
        enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);

        cost = 0;
        for (i = U_C; i < N_C; i++)
        {
            int plane_width_log2 = cu_width_log2 - 1;
            int plane_height_log2 = cu_height_log2 - 1;
            int qp = i == U_C ? pi->qp_u : pi->qp_v;
            double lambda = i == U_C ? ctx->lambda[1] : ctx->lambda[2];
            int secT_VH = use_secTrans[CHANNEL_CHROMA][TB0];
            int alt4x4 = use_alt4x4Trans[CHANNEL_CHROMA];

            mod_info_curr->num_nz[TB0][i] = enc_tq_nnz(ctx, mod_info_curr, i, 0, qp, lambda, coef[i], resi_t[i], plane_width_log2,
                plane_height_log2, pi->slice_type, i, 0, secT_VH, alt4x4);
            com_itdq_plane(mod_info_curr, i, coef[i], pi->resi_cb[i], ctx->wq, cu_width_log2 - 1, cu_height_log2 - 1, qp, bit_depth, use_secTrans[CHANNEL_CHROMA], use_alt4x4Trans[CHANNEL_CHROMA]
#if IST_CHROMA
                , ctx->tree_status
#endif
            );
            com_recon(mod_info_curr->tb_part, pi->resi_cb[i], pred[i], mod_info_curr->num_nz, i, cu_width / 2,
                cu_height / 2, cu_width / 2, rec[i], bit_depth, mod_info_curr->sbt_info);
            cost += ((double)(enc_ssd_16b(cu_width_log2-1, cu_height_log2-1, rec[i], org[i], cu_width/2, pi->stride_org[i], bit_depth))) * ctx->dist_chroma_weight[i - 1];
        }

        if (mod_info_curr->cu_mode == MODE_DIR && !is_cu_nz(mod_info_curr->num_nz))
        {
            cost = MAX_COST;
        }
        else
        {
            cost += dist[1][Y_C];

            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
            enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        }

        if (cost < cost_best)
        {
            cost_best = cost;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }
        else
        {
            mod_info_curr->inter_ccnpm_flag = 0;
            mod_info_curr->cu_mode = cu_mode;
            com_mcpy(num_nz_coef, nnz_store, sizeof(int)* MAX_NUM_TB* N_C);
            for (i = U_C; i < N_C; i++)
            {
                memcpy(coef[i], bak_2Nx2N_coef[i], sizeof(s16)* (i==Y_C? cu_size : cu_size>>2));
                memcpy(pred[i], bak_pred[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
            }
        }
    }

#if TR_SAVE_LOAD
#if INTERPF
        if( core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N && try_pbt )
#else
        if (core->best_tb_part_hist == 255 && mod_info_curr->pb_part == SIZE_2Nx2N)
#endif
        {
#if SBT_SAVELOAD
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part, mod_info_curr->sbt_info
#if IST
                , mod_info_curr->ist_tu_flag
#endif
#if INTER_CCNPM
                , mod_info_curr->inter_ccnpm_flag
#endif
            );
#else
            save_inter_tr_info(core, cu_ssd_u16, (u8)mod_info_curr->tb_part);
#endif
        }
#endif
#endif

    if (!is_cu_plane_nz(num_nz_coef, Y_C))
        mod_info_curr->tb_part = SIZE_2Nx2N; // reset best tb_part if no residual Y
#if SBT //important
    if( !is_cu_plane_nz( num_nz_coef, Y_C ) )
        assert( mod_info_curr->sbt_info == 0);
#if SBT_FAST
    core->cost_best = min( cost_best, core->cost_best );
#endif
#endif
    return cost_best;
}
#endif

#if AWP || SAWP
#if FIX_372
void enc_derive_awp_weights(ENC_CTX* ctx, int width_idx, int height_idx, int awp_idx, int bawp_flag)
#else
void enc_derive_awp_weights(ENC_CTX * ctx, int width_idx, int height_idx, int awp_idx)
#endif
{
    int   count[2] = { 0 };
    int   step_idx, angle_idx, angle_area;
    s32   cu_width  = (1 << (width_idx + MIN_AWP_SIZE_LOG2));
    s32   cu_height = (1 << (height_idx + MIN_AWP_SIZE_LOG2));
    pel  *weight0      = ctx->awp_weight0[width_idx][height_idx][awp_idx];
    pel  *weight1      = ctx->awp_weight1[width_idx][height_idx][awp_idx];
    pel  *weight0_scc   = ctx->awp_weight0_scc[width_idx][height_idx][awp_idx];
    pel  *weight1_scc   = ctx->awp_weight1_scc[width_idx][height_idx][awp_idx];
#if SAWP_WEIGHT_OPT && !AWP_ENH
    const int awp_idx_offset = AWP_MODE_NUM;
    pel* weight0_sawp = ctx->awp_weight0[width_idx][height_idx][awp_idx + awp_idx_offset];
    pel* weight1_sawp = ctx->awp_weight1[width_idx][height_idx][awp_idx + awp_idx_offset];
#endif
    pel  *bin_weight0  = ctx->awp_bin_weight0[width_idx][height_idx][awp_idx];
    pel  *bin_weight1  = ctx->awp_bin_weight1[width_idx][height_idx][awp_idx];
    BOOL *larger_area = &ctx->awp_larger_area[width_idx][height_idx][awp_idx];
#if FIX_372
    if (bawp_flag)
    {
        weight0_scc = ctx->awp_weight0_bawp[width_idx][height_idx][awp_idx];
        weight1_scc = ctx->awp_weight1_bawp[width_idx][height_idx][awp_idx];
        bin_weight0 = ctx->awp_bin_weight0_bawp[width_idx][height_idx][awp_idx];
        bin_weight1 = ctx->awp_bin_weight1_bawp[width_idx][height_idx][awp_idx];
    }
#endif

#if BAWP
    int x_c = 0;
    int y_c = 0;
#if FIX_372
#if AWP_ENH
    const int awp_weight_th = 1 << 4;
    int blend_idx = awp_idx / AWP_MODE_NUM;
    awp_idx = awp_idx % AWP_MODE_NUM;
    com_calculate_awp_para(awp_idx, &blend_idx, &step_idx, &angle_idx, &angle_area, cu_width, cu_height, bawp_flag);
    int blend_size_lut[TOTAL_BLEND_NUM] = { 0 };
    int blend_shift_lut[TOTAL_BLEND_NUM] = { 0 };
    derive_awp_blend_lut(blend_size_lut, blend_shift_lut);
    const int offset = 4 - blend_size_lut[blend_idx];
    const int shift = blend_shift_lut[blend_idx];
    const int max_weight = 32;
#else

#if SAWP_WEIGHT_OPT
    int sawp_blend_idx = 1;
    const int max_sawp_weight = SAWP_WEIGHT_PREC;
#endif
    com_calculate_awp_para(awp_idx, &step_idx, &angle_idx, &angle_area, cu_width, cu_height, bawp_flag);
#endif
#else
    int is_p_slice = ctx->info.pic_header.slice_type == SLICE_P;
    com_calculate_awp_para(awp_idx, &step_idx, &angle_idx, &angle_area, cu_width, cu_height, is_p_slice);
#endif
#else
    com_calculate_awp_para(awp_idx, &step_idx, &angle_idx, &angle_area);
#endif

    // Derive weights for luma
    int first_pos = 0;
    int first_pos_scc = 0;
    int delta_pos_w = 0;
    int delta_pos_h = 0;

    // Set half pixel length
    int valid_length_w = (cu_width + (cu_height >> angle_idx)) << 1;
    int valid_length_h = (cu_height + (cu_width >> angle_idx)) << 1;

    // Reference weight array
    int* final_reference_weights = NULL;
    int* final_reference_weights_scc = NULL;
#if SAWP_WEIGHT_OPT && !AWP_ENH
    int* final_reference_weights_sawp = NULL;
#endif
    const int weight_stride = cu_width;
    int temp_w = ((cu_height << 1) >> angle_idx);
    int temp_h = ((cu_width << 1) >> angle_idx);
    delta_pos_w = (valid_length_w >> 3) - 1;
    delta_pos_h = (valid_length_h >> 3) - 1;
    delta_pos_w = delta_pos_w == 0 ? 1 : delta_pos_w;
    delta_pos_h = delta_pos_h == 0 ? 1 : delta_pos_h;
    delta_pos_w = step_idx * delta_pos_w;
    delta_pos_h = step_idx * delta_pos_h;

    int reference_weights[MAX_AWP_SIZE << 2] = { 0 };
    int reference_weights_scc[MAX_AWP_SIZE << 2] = { 0 };
#if SAWP_WEIGHT_OPT && !AWP_ENH
    int reference_weights_sawp[MAX_AWP_SIZE << 2] = { 0 };
#endif

    switch (angle_area)
    {
    case 0:
        //Calculate first_pos & reference weights [per block]
        first_pos = (valid_length_h >> 1) - 6 + delta_pos_h;
        first_pos_scc = (valid_length_h >> 1) - 3 + delta_pos_h;
#if AWP_ENH
        first_pos = first_pos + offset;
#endif
        for (int i = 0; i < valid_length_h; i++)
        {
#if SAWP_WEIGHT_OPT && !AWP_ENH
            reference_weights_sawp[i] = get_awp_weight_by_blend_idx(i - first_pos, sawp_blend_idx);
#endif
#if AWP_ENH
            reference_weights[i] = get_awp_weight_by_blend_idx(i - first_pos, blend_idx);
#else
            reference_weights[i] = COM_CLIP3(0, 8, i - first_pos);
#endif
#if BAWP
#if FIX_372
            if (bawp_flag)
#else
            if (is_p_slice)
#endif
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 3);
            }
            else
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
            }
#else
            reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
#endif
        }
        //set Delta to align calculate [per block]
        final_reference_weights = reference_weights;
        final_reference_weights_scc = reference_weights_scc;
#if SAWP_WEIGHT_OPT && !AWP_ENH
        final_reference_weights_sawp = reference_weights_sawp;
#endif
        // Calculate Weight [per pixel]
        for (int y = 0; y < cu_height; y++)
        {
            for (int x = 0; x < cu_width; x++)
            {
#if BAWP
#if FIX_372
                if (bawp_flag)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                    weight0_scc[x] = final_reference_weights_scc[(y_c << 1) + ((x_c << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
                }
                else
                {
                    weight0[x] = final_reference_weights[(y << 1) + ((x << 1) >> angle_idx)];
#if AWP_ENH
                    weight1[x] = max_weight - weight0[x];
#else
                    weight1[x] = 8 - weight0[x];
#endif
                    weight0_scc[x] = final_reference_weights_scc[(y << 1) + ((x << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];

#if SAWP_WEIGHT_OPT && !AWP_ENH
                    weight0_sawp[x] = final_reference_weights_sawp[(y << 1) + ((x << 1) >> angle_idx)];
                    weight1_sawp[x] = max_sawp_weight - weight0_sawp[x];
#endif
                }
#else
                if (is_p_slice)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                }
                else
                {
                    x_c = x;
                    y_c = y;
                }
                weight0[x] = final_reference_weights[(y_c << 1) + ((x_c << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(y_c << 1) + ((x_c << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#else
                weight0[x] = final_reference_weights[(y << 1) + ((x << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(y << 1) + ((x << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#if FIX_372
#if AWP_ENH
                if ((bawp_flag == 0 && weight0[x] >= awp_weight_th) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#else
                if ((bawp_flag == 0 && weight0[x] >= 4) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#endif
#else
                if (weight0[x] >= 4)
#endif
                {
                    bin_weight0[x] = 1;
                    bin_weight1[x] = 0;
                    count[0]++;
                }
                else
                {
                    bin_weight0[x] = 0;
                    bin_weight1[x] = 1;
                    count[1]++;
                }
            }
            weight0 += weight_stride;
            weight1 += weight_stride;
            weight0_scc += weight_stride;
            weight1_scc += weight_stride;
#if SAWP_WEIGHT_OPT && !AWP_ENH
            weight0_sawp += weight_stride;
            weight1_sawp += weight_stride;
#endif
            bin_weight0 += weight_stride;
            bin_weight1 += weight_stride;
        }
        break;
    case 1:
        //Calculate first_pos & reference weights [per block]
        first_pos = (valid_length_h >> 1) - 4 + delta_pos_h;
        first_pos_scc = (valid_length_h >> 1) - 1 + delta_pos_h;
#if AWP_ENH
        first_pos = first_pos + offset;
#endif
        for (int i = 0; i < valid_length_h; i++)
        {
#if SAWP_WEIGHT_OPT && !AWP_ENH
            reference_weights_sawp[i] = get_awp_weight_by_blend_idx(i - first_pos, sawp_blend_idx);
#endif
#if AWP_ENH
            reference_weights[i] = get_awp_weight_by_blend_idx(i - first_pos, blend_idx);
#else
            reference_weights[i] = COM_CLIP3(0, 8, i - first_pos);
#endif
#if BAWP
#if FIX_372
            if (bawp_flag)
#else
            if (is_p_slice)
#endif
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 3);
            }
            else
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
            }
#else
            reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
#endif
        }
        //set Delta to align calculate [per block]
        final_reference_weights = reference_weights + temp_h;
        final_reference_weights_scc = reference_weights_scc + temp_h;
#if SAWP_WEIGHT_OPT && !AWP_ENH
        final_reference_weights_sawp = reference_weights_sawp + temp_h;
#endif
        // Calculate Weight [per pixel]
        for (int y = 0; y < cu_height; y++)
        {
            for (int x = 0; x < cu_width; x++)
            {
#if BAWP
#if FIX_372
                if (bawp_flag)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                    weight0_scc[x] = final_reference_weights_scc[(y_c << 1) - ((x_c << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
                }
                else
                {
                    weight0[x] = final_reference_weights[(y << 1) - ((x << 1) >> angle_idx)];
#if AWP_ENH
                    weight1[x] = max_weight - weight0[x];
#else
                    weight1[x] = 8 - weight0[x];
#endif
                    weight0_scc[x] = final_reference_weights_scc[(y << 1) - ((x << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
#if SAWP_WEIGHT_OPT && !AWP_ENH
                    weight0_sawp[x] = final_reference_weights_sawp[(y << 1) - ((x << 1) >> angle_idx)];
                    weight1_sawp[x] = max_sawp_weight - weight0_sawp[x];
#endif
                }
#else
                if (is_p_slice)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                }
                else
                {
                    x_c = x;
                    y_c = y;
                }
                weight0[x] = final_reference_weights[(y_c << 1) - ((x_c << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(y_c << 1) - ((x_c << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#else
                weight0[x] = final_reference_weights[(y << 1) - ((x << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(y << 1) - ((x << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#if FIX_372
#if AWP_ENH 
                if ((bawp_flag == 0 && weight0[x] >= awp_weight_th) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#else
                if ((bawp_flag == 0 && weight0[x] >= 4) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#endif
#else
                if (weight0[x] >= 4)
#endif
                {
                    bin_weight0[x] = 1;
                    bin_weight1[x] = 0;
                    count[0]++;
                }
                else
                {
                    bin_weight0[x] = 0;
                    bin_weight1[x] = 1;
                    count[1]++;
                }
            }
            weight0 += weight_stride;
            weight1 += weight_stride;
            weight0_scc += weight_stride;
            weight1_scc += weight_stride;
#if SAWP_WEIGHT_OPT && !AWP_ENH
            weight0_sawp += weight_stride;
            weight1_sawp += weight_stride;
#endif
            bin_weight0 += weight_stride;
            bin_weight1 += weight_stride;
        }
        break;
    case 2:
        //Calculate first_pos & reference weights [per block]
        first_pos = (valid_length_w >> 1) - 4 + delta_pos_w;
        first_pos_scc = (valid_length_w >> 1) - 1 + delta_pos_w;
#if AWP_ENH
        first_pos = first_pos + offset;
#endif
        for (int i = 0; i < valid_length_w; i++)
        {
#if SAWP_WEIGHT_OPT && !AWP_ENH
            reference_weights_sawp[i] = get_awp_weight_by_blend_idx(i - first_pos, sawp_blend_idx);
#endif
#if AWP_ENH
            reference_weights[i] = get_awp_weight_by_blend_idx(i - first_pos, blend_idx);
#else
            reference_weights[i] = COM_CLIP3(0, 8, i - first_pos);
#endif
#if BAWP
#if FIX_372
            if (bawp_flag)
#else
            if (is_p_slice)
#endif
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 3);
            }
            else
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
            }
#else
            reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
#endif
        }
        //set Delta to align calculate [per block]
        final_reference_weights = reference_weights + temp_w;
        final_reference_weights_scc = reference_weights_scc + temp_w;
#if SAWP_WEIGHT_OPT && !AWP_ENH
        final_reference_weights_sawp = reference_weights_sawp + temp_w;
#endif
        // Calculate Weight [per pixel]
        for (int y = 0; y < cu_height; y++)
        {
            for (int x = 0; x < cu_width; x++)
            {
#if BAWP
#if FIX_372
                if (bawp_flag)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                    weight0_scc[x] = final_reference_weights_scc[(x_c << 1) - ((y_c << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
                }
                else
                {
                    weight0[x] = final_reference_weights[(x << 1) - ((y << 1) >> angle_idx)];
#if AWP_ENH
                    weight1[x] = max_weight - weight0[x];
#else
                    weight1[x] = 8 - weight0[x];
#endif
                    weight0_scc[x] = final_reference_weights_scc[(x << 1) - ((y << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
#if SAWP_WEIGHT_OPT && !AWP_ENH
                    weight0_sawp[x] = final_reference_weights_sawp[(x << 1) - ((y << 1) >> angle_idx)];
                    weight1_sawp[x] = max_sawp_weight - weight0_sawp[x];
#endif
                }
#else
                if (is_p_slice)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                }
                else
                {
                    x_c = x;
                    y_c = y;
                }
                weight0[x] = final_reference_weights[(x_c << 1) - ((y_c << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(x_c << 1) - ((y_c << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#else
                weight0[x] = final_reference_weights[(x << 1) - ((y << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(x << 1) - ((y << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#if FIX_372
#if AWP_ENH 
                if ((bawp_flag == 0 && weight0[x] >= awp_weight_th) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#else
                if ((bawp_flag == 0 && weight0[x] >= 4) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#endif
#else
                if (weight0[x] >= 4)
#endif
                {
                    bin_weight0[x] = 1;
                    bin_weight1[x] = 0;
                    count[0]++;
                }
                else
                {
                    bin_weight0[x] = 0;
                    bin_weight1[x] = 1;
                    count[1]++;
                }
            }
            weight0 += weight_stride;
            weight1 += weight_stride;
            weight0_scc += weight_stride;
            weight1_scc += weight_stride;
#if SAWP_WEIGHT_OPT && !AWP_ENH
            weight0_sawp += weight_stride;
            weight1_sawp += weight_stride;
#endif
            bin_weight0 += weight_stride;
            bin_weight1 += weight_stride;
        }
        break;
    case 3:
        //Calculate first_pos & reference weights [per block]
        first_pos = (valid_length_w >> 1) - 6 + delta_pos_w;
        first_pos_scc = (valid_length_w >> 1) - 3 + delta_pos_w;
#if AWP_ENH
        first_pos = first_pos + offset;
#endif
        for (int i = 0; i < valid_length_w; i++)
        {
#if SAWP_WEIGHT_OPT && !AWP_ENH
            reference_weights_sawp[i] = get_awp_weight_by_blend_idx(i - first_pos, sawp_blend_idx);
#endif
#if AWP_ENH
            reference_weights[i] = get_awp_weight_by_blend_idx(i - first_pos, blend_idx);
#else
            reference_weights[i] = COM_CLIP3(0, 8, i - first_pos);
#endif

#if BAWP
#if FIX_372
            if (bawp_flag)
#else
            if (is_p_slice)
#endif
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 3);
            }
            else
            {
                reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
            }
#else
            reference_weights_scc[i] = COM_CLIP3(0, 8, (i - first_pos_scc) << 2);
#endif
        }
        //set Delta to align calculate [per block]
        final_reference_weights = reference_weights;
        final_reference_weights_scc = reference_weights_scc;
#if SAWP_WEIGHT_OPT && !AWP_ENH
        final_reference_weights_sawp = reference_weights_sawp;
#endif
        // Calculate Weight [per pixel]
        for (int y = 0; y < cu_height; y++)
        {
            for (int x = 0; x < cu_width; x++)
            {
#if BAWP
#if FIX_372
                if (bawp_flag)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                    weight0_scc[x] = final_reference_weights_scc[(x_c << 1) + ((y_c << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
                }
                else
                {
                    weight0[x] = final_reference_weights[(x << 1) + ((y << 1) >> angle_idx)];
#if AWP_ENH
                    weight1[x] = max_weight - weight0[x];
#else
                    weight1[x] = 8 - weight0[x];
#endif
                    weight0_scc[x] = final_reference_weights_scc[(x << 1) + ((y << 1) >> angle_idx)];
                    weight1_scc[x] = 8 - weight0_scc[x];
#if SAWP_WEIGHT_OPT && !AWP_ENH
                    weight0_sawp[x] = final_reference_weights_sawp[(x << 1) + ((y << 1) >> angle_idx)];
                    weight1_sawp[x] = max_sawp_weight - weight0_sawp[x];
#endif
                }
#else
                if (is_p_slice)
                {
                    x_c = ((x >> 2) << 2) + 2;
                    y_c = ((y >> 2) << 2) + 2;
                }
                else
                {
                    x_c = x;
                    y_c = y;
                }
                weight0[x] = final_reference_weights[(x_c << 1) + ((y_c << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(x_c << 1) + ((y_c << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#else
                weight0[x] = final_reference_weights[(x << 1) + ((y << 1) >> angle_idx)];
                weight1[x] = 8 - weight0[x];
                weight0_scc[x] = final_reference_weights_scc[(x << 1) + ((y << 1) >> angle_idx)];
                weight1_scc[x] = 8 - weight0_scc[x];
#endif
#if FIX_372
#if AWP_ENH 
                if ((bawp_flag == 0 && weight0[x] >= awp_weight_th) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#else
                if ((bawp_flag == 0 && weight0[x] >= 4) || (bawp_flag == 1 && weight0_scc[x] >= 4))
#endif
#else
                if (weight0[x] >= 4)
#endif
                {
                    bin_weight0[x] = 1;
                    bin_weight1[x] = 0;
                    count[0]++;
                }
                else
                {
                    bin_weight0[x] = 0;
                    bin_weight1[x] = 1;
                    count[1]++;
                }
            }
            weight0 += weight_stride;
            weight1 += weight_stride;
            weight0_scc += weight_stride;
            weight1_scc += weight_stride;
#if SAWP_WEIGHT_OPT && !AWP_ENH
            weight0_sawp += weight_stride;
            weight1_sawp += weight_stride;
#endif
            bin_weight0 += weight_stride;
            bin_weight1 += weight_stride;
        }
        break;
    default:
        printf("\nError: awp parameter not expected\n");
        assert(0);
    }

    if (count[0] >= count[1])
    {
        *larger_area = 0;
    }
    else
    {
        *larger_area = 1;
    }
}
#endif

#if CHROMA_NOT_SPLIT
double pinter_residue_rdo_chroma(ENC_CTX *ctx, ENC_CORE *core
#if DMVR
                                 , int apply_dmvr
#endif
)
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    s16(*coef)[MAX_CU_DIM] = mod_info_curr->coef;
    s16(*mv)[MV_D] = mod_info_curr->mv;
    s8 *refi = mod_info_curr->refi;
    pel(*pred)[MAX_CU_DIM] = mod_info_curr->pred;
    int bit_depth = ctx->info.bit_depth_internal;
    int(*num_nz_coef)[N_C], tnnz, width[N_C], height[N_C], log2_w[N_C], log2_h[N_C];;
    pel(*rec)[MAX_CU_DIM];
    s64    dist[2][N_C];
    double cost, cost_best = MAX_COST;
    int    cbf_best[N_C];
#if INTER_CCNPM_OPT
    int nnz_store[MAX_NUM_TB][N_C];
#endif
    int    bit_cnt;
    int    i, cbf_y, cbf_u, cbf_v;
    pel   *org[N_C];
    int scup = mod_info_curr->scup;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
#if ST_CHROMA
    int use_secTrans[MAX_NUM_CHANNEL][MAX_NUM_TB] = { { 0, 0, 0, 0 },{ 0, 0, 0, 0 } }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans[MAX_NUM_CHANNEL] = { 0,0 };
#else
    int use_secTrans[MAX_NUM_TB] = { 0, 0, 0, 0 }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans = 0;
#endif
#if SBT
    mod_info_curr->sbt_info = 0;
#endif
#if INTER_CCNPM_OPT
    mod_info_curr->inter_ccnpm_flag = 0;
#endif
#if DMVR
    COM_DMVR dmvr;
#endif

    rec = mod_info_curr->rec;
    num_nz_coef = mod_info_curr->num_nz;
    width[Y_C] = 1 << cu_width_log2;
    height[Y_C] = 1 << cu_height_log2;
    width[U_C] = width[V_C] = 1 << (cu_width_log2 - 1);
    height[U_C] = height[V_C] = 1 << (cu_height_log2 - 1);
    log2_w[Y_C] = cu_width_log2;
    log2_h[Y_C] = cu_height_log2;
    log2_w[U_C] = log2_w[V_C] = cu_width_log2 - 1;
    log2_h[U_C] = log2_h[V_C] = cu_height_log2 - 1;
    org[Y_C] = pi->Yuv_org[Y_C] + (y * pi->stride_org[Y_C]) + x;
    org[U_C] = pi->Yuv_org[U_C] + ((y >> 1) * pi->stride_org[U_C]) + (x >> 1);
    org[V_C] = pi->Yuv_org[V_C] + ((y >> 1) * pi->stride_org[V_C]) + (x >> 1);
#if IST
    mod_info_curr->slice_type = ctx->slice_type;
#endif
#if ISTS
    mod_info_curr->ph_ists_enable_flag = ctx->info.pic_header.ph_ists_enable_flag;
#endif
#if TS_INTER
    mod_info_curr->ph_ts_inter_enable_flag = ctx->info.pic_header.ph_ts_inter_enable_flag;
#endif
#if DEST_PH
    mod_info_curr->ph_dest_enable_flag = ctx->info.pic_header.ph_dest_enable_flag;
#endif

#if TB_SPLIT_EXT
    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, core->mod_info_curr.pb_part, &core->mod_info_curr.pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#endif
#endif

    /* prepare MV */
    int luma_scup = mod_info_curr->scup + PEL2SCU(mod_info_curr->cu_width - 1) + PEL2SCU(mod_info_curr->cu_height - 1) * ctx->info.pic_width_in_scu;
    assert(MCU_GET_INTRA_FLAG(ctx->map.map_scu[luma_scup]) == 0);
    for (i = 0; i < REFP_NUM; i++)
    {
        refi[i] = ctx->map.map_refi[luma_scup][i];
        mv[i][MV_X] = ctx->map.map_mv[luma_scup][i][MV_X];
        mv[i][MV_Y] = ctx->map.map_mv[luma_scup][i][MV_Y];

        mod_info_curr->refi[i] = ctx->map.map_refi[luma_scup][i];
        mod_info_curr->mv[i][MV_X] = ctx->map.map_mv[luma_scup][i][MV_X];
        mod_info_curr->mv[i][MV_Y] = ctx->map.map_mv[luma_scup][i][MV_Y];
    }

#if DMVR
    dmvr.poc_c = ctx->ptr; 
    dmvr.dmvr_current_template = pi->dmvr_template;
    dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
    dmvr.apply_DMVR = apply_dmvr && ctx->info.sqh.dmvr_enable_flag;
    dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif

    /* chroma MC */
    assert(mod_info_curr->pb_info.sub_scup[0] == mod_info_curr->scup);
    com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, ctx->tree_status, bit_depth
#if DMVR
           , &dmvr
#endif
#if BIO
           , ctx->ptr, 0, 0
#endif
#if MVAP
           , 0
#endif
#if SUB_TMVP
           , 0
#endif
#if BGC
           , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
    );

#if OBMC && !DISABLE_OBMC_LOCAL_CHROMA_TREE
    if (ctx->info.sqh.obmc_enable_flag)
    {
        buffer_mvfield(mod_info_curr->tmp_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2), TRUE);
        set_col_mvfield(mod_info_curr, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
        pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_L) ? TRUE : FALSE), ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_C) ? TRUE : FALSE), ctx->info.bit_depth_internal
            , cu_width, cu_height
            , ctx->ptr
#if BGC
            , 0, 0
#endif
        );
        buffer_mvfield(mod_info_curr->tmp_mvfield, (cu_width >> MIN_CU_LOG2), &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2), FALSE);
    }
#endif

    /* get residual */
    enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);
    memset(dist, 0, sizeof(s64) * 2 * N_C);
    for (i = 1; i < N_C; i++)
    {
        dist[0][i] = enc_ssd_16b(log2_w[i], log2_h[i], pred[i], org[i], width[i], pi->stride_org[i], bit_depth);
    }

    /* test all zero case */
    memset(cbf_best, 0, sizeof(int) * N_C);
    memset(num_nz_coef, 0, sizeof(int) * MAX_NUM_TB * N_C);
    assert(mod_info_curr->tb_part == SIZE_2Nx2N);
    cost_best = (double)((dist[0][U_C] + dist[0][V_C]) * ctx->dist_chroma_weight[0]);

    SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
    bit_cnt = enc_get_bit_number(&core->s_temp_run);
    encode_coef(&core->bs_temp, coef, mod_info_curr->cu_width_log2, mod_info_curr->cu_height_log2, mod_info_curr->cu_mode, mod_info_curr, ctx->tree_status, ctx
#if CUDQP
        , core->qp_y
#endif
#if INTER_CCNPM
        , x, y
#endif
    ); // only count coeff bits for chroma tree
    bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
    cost_best += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
    SBAC_STORE(core->s_temp_best, core->s_temp_run);

    /* transform and quantization */
    tnnz = enc_tq_yuv_nnz(ctx, core, mod_info_curr, coef, resi_t, pi->slice_type, 0, use_secTrans, use_alt4x4Trans, refi, mv, 0);

    if (tnnz)
    {
        com_itdq_yuv(mod_info_curr, coef, resi_t, ctx->wq, cu_width_log2, cu_height_log2, pi->qp_y, pi->qp_u, pi->qp_v, bit_depth, use_secTrans, use_alt4x4Trans
#if IST_CHROMA
            , ctx->tree_status
#endif        
        );
        for (i = 1; i < N_C; i++)
        {
            com_recon(i == Y_C ? mod_info_curr->tb_part : SIZE_2Nx2N, resi_t[i], pred[i], num_nz_coef, i, width[i], height[i], width[i], rec[i], bit_depth
#if SBT
                , 0
#endif
            );

            if (is_cu_plane_nz(num_nz_coef, i))
            {
                dist[1][i] = enc_ssd_16b(log2_w[i], log2_h[i], rec[i], org[i], width[i], pi->stride_org[i], bit_depth);
            }
            else
            {
                dist[1][i] = dist[0][i];
            }
        }

        cbf_y = is_cu_plane_nz(num_nz_coef, Y_C) > 0 ? 1 : 0;
        cbf_u = is_cu_plane_nz(num_nz_coef, U_C) > 0 ? 1 : 0;
        cbf_v = is_cu_plane_nz(num_nz_coef, V_C) > 0 ? 1 : 0;

        cost = (double)((dist[cbf_u][U_C] + dist[cbf_v][V_C]) * ctx->dist_chroma_weight[0]);

        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        bit_cnt = enc_get_bit_number(&core->s_temp_run);
        encode_coef(&core->bs_temp, coef, mod_info_curr->cu_width_log2, mod_info_curr->cu_height_log2, mod_info_curr->cu_mode, mod_info_curr, ctx->tree_status, ctx
#if CUDQP
            , core->qp_y
#endif
#if INTER_CCNPM
            , x, y
#endif
        ); // only count coeff bits for chroma tree
        bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
        cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        if (cost < cost_best)
        {
            cost_best = cost;
            cbf_best[Y_C] = cbf_y;
            cbf_best[U_C] = cbf_u;
            cbf_best[V_C] = cbf_v;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }

    }

#if INTER_CCNPM_OPT
    if (ctx->info.sqh.ccnpm_enable_flag && (x > 0 || y > 0))
    {
        int cu_mode = mod_info_curr->cu_mode;
        com_mcpy(nnz_store, num_nz_coef, sizeof(int)* MAX_NUM_TB* N_C);

        static s16 bak_2Nx2N_coef[N_C][MAX_CU_DIM];
        static s16 bak_pred[N_C][MAX_CU_DIM];
        static s16 bak_rec[N_C][MAX_CU_DIM];
        int cu_size = 1 << (cu_width_log2 + cu_height_log2);
        for (i = U_C; i < N_C; i++)
        {
            memcpy(bak_2Nx2N_coef[i], coef[i], sizeof(s16)* (i==Y_C? cu_size : cu_size>>2));
            memcpy(bak_pred[i], pred[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
            memcpy(bak_rec[i], rec[i], sizeof(s16)* (i == Y_C ? cu_size : cu_size >> 2));
        }

        mod_info_curr->inter_ccnpm_flag = 1;

        /* get residual */
        u16 avail_cu = com_get_avail_intra(mod_info_curr->x_scu, mod_info_curr->y_scu, ctx->info.pic_width_in_scu, mod_info_curr->scup, ctx->map.map_scu, ctx->info.pic_height_in_scu, cu_width, cu_height);
        for (int comp_id = U_C; comp_id < N_C; comp_id++)
        {
            ipred_ccnpm(comp_id, PIC_REC(ctx)->y, PIC_REC(ctx)->stride_luma, PIC_REC(ctx)->u, PIC_REC(ctx)->v, PIC_REC(ctx)->stride_chroma, pred[comp_id], x, y, cu_width/2, cu_height/2, bit_depth, avail_cu
#if CCNPM_TEMPLATE_OPT
                , IPD_CCNPM
#endif
#if CCNPM_LINE_BUFFER_REDUCTION
                , (1<<ctx->info.log2_max_cuwh)
#endif
            );
            ccnpm_blending(bak_pred[comp_id], pred[comp_id], cu_width / 2, cu_height / 2, bit_depth);
        }
        enc_diff_pred(x, y, cu_width_log2, cu_height_log2, pi->pic_org, pred, resi_t);

        cost = 0;
        for (i = U_C; i < N_C; i++)
        {
            int plane_width_log2 = cu_width_log2 - 1;
            int plane_height_log2 = cu_height_log2 - 1;
            int qp = i == U_C ? pi->qp_u : pi->qp_v;
            double lambda = i == U_C ? ctx->lambda[1] : ctx->lambda[2];
            int secT_VH = use_secTrans[CHANNEL_CHROMA][TB0];
            int alt4x4 = use_alt4x4Trans[CHANNEL_CHROMA];

            mod_info_curr->num_nz[TB0][i] = enc_tq_nnz(ctx, mod_info_curr, i, 0, qp, lambda, coef[i], resi_t[i], plane_width_log2,
                plane_height_log2, pi->slice_type, i, 0, secT_VH, alt4x4);
            com_itdq_plane(mod_info_curr, i, coef[i], pi->resi_cb[i], ctx->wq, cu_width_log2 - 1, cu_height_log2 - 1,
                qp, bit_depth, use_secTrans[CHANNEL_CHROMA], use_alt4x4Trans[CHANNEL_CHROMA]
#if IST_CHROMA
              , ctx->tree_status
#endif
            );
            com_recon(mod_info_curr->tb_part, pi->resi_cb[i], pred[i], mod_info_curr->num_nz, i, cu_width / 2,
                cu_height / 2, cu_width / 2, rec[i], bit_depth, mod_info_curr->sbt_info);
            cost += ((double)(enc_ssd_16b(cu_width_log2-1, cu_height_log2-1, rec[i], org[i], cu_width/2, pi->stride_org[i], bit_depth))) * ctx->dist_chroma_weight[i - 1];
        }

        if (mod_info_curr->cu_mode == MODE_DIR && !is_cu_nz(mod_info_curr->num_nz))
        {
            cost = MAX_COST;
        }
        else
        {
            SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
            bit_cnt = enc_get_bit_number(&core->s_temp_run);
            encode_coef(&core->bs_temp, coef, mod_info_curr->cu_width_log2, mod_info_curr->cu_height_log2, mod_info_curr->cu_mode, mod_info_curr, ctx->tree_status,
                ctx, core->qp_y, x, y); 
            bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
            cost += RATE_TO_COST_LAMBDA(ctx->lambda[0], bit_cnt);
        }

        if (cost < cost_best)
        {
            cost_best = cost;
            SBAC_STORE(core->s_temp_best, core->s_temp_run);
        }
        else
        {
            mod_info_curr->inter_ccnpm_flag = 0;
            mod_info_curr->cu_mode = cu_mode;
            com_mcpy(num_nz_coef, nnz_store, sizeof(int)* MAX_NUM_TB* N_C);
            for (i = U_C; i < N_C; i++)
            {
                memcpy(coef[i], bak_2Nx2N_coef[i], sizeof(s16)* (cu_size>>2));
                memcpy(pred[i], bak_pred[i], sizeof(s16)* (cu_size >> 2));
                memcpy(rec[i], bak_rec[i], sizeof(s16)* (cu_size >> 2));
            }
        }
        for (i = U_C; i < N_C; i++)
        {
            cbf_best[i] = is_cu_plane_nz(num_nz_coef, i) > 0 ? 1 : 0;
        }
    }
    core->mod_info_best.inter_ccnpm_flag = mod_info_curr->inter_ccnpm_flag;
#endif

    /* save */
    for (i = 0; i < N_C; i++)
    {
        int size_tmp = (cu_width * cu_height) >> (i == 0 ? 0 : 2);
        if (cbf_best[i] == 0)
        {
            cu_plane_nz_cln(core->mod_info_best.num_nz, i);
            com_mset(core->mod_info_best.coef[i], 0, sizeof(s16) * size_tmp);
            com_mcpy(core->mod_info_best.rec[i], pred[i], sizeof(s16) * size_tmp);
        }
        else
        {
            cu_plane_nz_cpy(core->mod_info_best.num_nz, num_nz_coef, i);
            com_mcpy(core->mod_info_best.coef[i], coef[i], sizeof(s16) * size_tmp);
            com_mcpy(core->mod_info_best.rec[i], rec[i], sizeof(s16) * size_tmp);
        }
    }

    return cost_best;
}
#endif

static void init_inter_data(ENC_PINTER *pi, ENC_CORE *core)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    mod_info_curr->skip_idx = 0;
#if SMVD
    mod_info_curr->smvd_flag = 0;
#endif
#if BGC
    mod_info_curr->bgc_flag = 0;
    mod_info_curr->bgc_idx = 0;
#endif
#if AWP
    mod_info_curr->awp_flag  = 0;
#endif
#if INTER_TM
    mod_info_curr->tm_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
    get_part_info(pi->pic_width_in_scu, mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
    assert(mod_info_curr->pb_info.sub_scup[0] == mod_info_curr->scup);

    com_mset(mod_info_curr->mv, 0, sizeof(s16) * REFP_NUM * MV_D);
    com_mset(mod_info_curr->mvd, 0, sizeof(s16) * REFP_NUM * MV_D);
    com_mset(mod_info_curr->refi, 0, sizeof(s8)  * REFP_NUM);

    com_mset(mod_info_curr->num_nz, 0, sizeof(int)*N_C*MAX_NUM_TB);
    com_mset(mod_info_curr->coef, 0, sizeof(s16) * N_C * MAX_CU_DIM);

    com_mset(mod_info_curr->affine_mv, 0, sizeof(CPMV) * REFP_NUM * VER_NUM * MV_D);
    com_mset(mod_info_curr->affine_mvd, 0, sizeof(s16) * REFP_NUM * VER_NUM * MV_D);
}

static void derive_inter_cands(ENC_CTX *ctx, ENC_CORE *core, s16(*pmv_cands)[REFP_NUM][MV_D], s8(*refi_cands)[REFP_NUM], int *num_cands_all, int *num_cands_woUMVE
#if INTER_TM
    , u8 is_tm
#endif
)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int num_cands = 0;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    int scup_co = get_colocal_scup(mod_info_curr->scup, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu);
#if SUB_TMVP 
    int i;
    int sb_scup[SBTMVP_NUM];
    int sb_scup_co[SBTMVP_NUM];
#endif
    COM_MOTION motion_cands_curr[MAX_SKIP_NUM];
#if BGC
    s8 bgc_flag_cands_curr[MAX_SKIP_NUM];
    s8 bgc_idx_cands_curr[MAX_SKIP_NUM];
#endif
    s8 cnt_hmvp_cands_curr = 0;

    int umve_idx;
    s16 pmv_base_cands[UMVE_BASE_NUM][REFP_NUM][MV_D];
    s8 refi_base_cands[UMVE_BASE_NUM][REFP_NUM];

    mod_info_curr->affine_flag = 0;

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);
#if BGC
#if UMVE_ENH
    memset(mod_info_curr->bgc_flag_cands, 0, (MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM_SEC_SET * UMVE_BASE_NUM) * sizeof(s8));
    memset(mod_info_curr->bgc_idx_cands, 0, (MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM_SEC_SET * UMVE_BASE_NUM) * sizeof(s8));
#else
    memset(mod_info_curr->bgc_flag_cands, 0, (MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM * UMVE_BASE_NUM) * sizeof(s8));
    memset(mod_info_curr->bgc_idx_cands, 0, (MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM * UMVE_BASE_NUM) * sizeof(s8));
#endif
#endif
#if UNIFIED_HMVP_2
    int skipCheckTmvpRedundancy = 0;
    if (ctx->info.sqh.profile_id == 0x30 || ctx->info.sqh.profile_id == 0x32)
    {
        skipCheckTmvpRedundancy = 1;
    }
#endif
#if SUB_TMVP 
    if (ctx->info.sqh.sbtmvp_enable_flag)
    {
        for (i = 0; i < SBTMVP_NUM; i++)
        {
            sb_scup[i] = mod_info_curr->scup + ctx->info.pic_width_in_scu* ((cu_height >> 2) - 1)*(i / 2) + ((cu_width >> 2) - 1)*(i % 2);

            sb_scup_co[i] = get_colocal_scup(sb_scup[i], ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu);
        }
    }
#endif
    // insert TMVP
    num_cands = 0;
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        refi_cands[num_cands][REFP_0] = 0;
        refi_cands[num_cands][REFP_1] = -1;
        if (REFI_IS_VALID(ctx->refp[0][REFP_0].map_refi[scup_co][REFP_0]))
        {
            get_col_mv_from_list0(pi->refp[0], ctx->ptr, scup_co, pmv_cands[num_cands]);
        }
        else
        {
            pmv_cands[num_cands][REFP_0][MV_X] = 0;
            pmv_cands[num_cands][REFP_0][MV_Y] = 0;
        }
        pmv_cands[num_cands][REFP_1][MV_X] = 0;
        pmv_cands[num_cands][REFP_1][MV_Y] = 0;
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag)
        {
            for (i = 0; i < SBTMVP_NUM; i++)
            {
                if (REFI_IS_VALID(ctx->refp[0][REFP_0].map_refi[sb_scup_co[i]][REFP_0]) || REFI_IS_VALID(ctx->refp[0][REFP_0].map_refi[sb_scup_co[i]][REFP_1]))
                {
                    get_col_mv_from_list0_ext(pi->refp[0], ctx->ptr, sb_scup_co[i], core->sbTmvp[i].mv, core->sbTmvp[i].ref_idx);
                }
                else
                {
                    copy_mv(core->sbTmvp[i].mv[REFP_0], pmv_cands[num_cands][REFP_0]);
                }
                core->sbTmvp[i].mv[REFP_1][MV_X] = 0;
                core->sbTmvp[i].mv[REFP_1][MV_Y] = 0;
                core->sbTmvp[i].ref_idx[REFP_0] = 0;
                core->sbTmvp[i].ref_idx[REFP_1] = -1;
            }
        }
#endif
    }
    else
    {
        if (!REFI_IS_VALID(pi->refp[0][REFP_1].map_refi[scup_co][REFP_0]))
        {
            com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_0, 0, 0, pmv_cands[num_cands][REFP_0]);

            com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_1, 0, 0, pmv_cands[num_cands][REFP_1]);
        }
        else
        {
            get_col_mv(pi->refp[0], ctx->ptr, scup_co, pmv_cands[num_cands]);
        }
        SET_REFI(refi_cands[num_cands], 0, 0);
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag)
        {
            for (i = 0; i < SBTMVP_NUM; i++)
            {
                if (!REFI_IS_VALID(pi->refp[0][REFP_1].map_refi[sb_scup_co[i]][REFP_0]) && !REFI_IS_VALID(pi->refp[0][REFP_1].map_refi[sb_scup_co[i]][REFP_1]))
                {
                    copy_mv(core->sbTmvp[i].mv[REFP_0], pmv_cands[num_cands][REFP_0]);
                    copy_mv(core->sbTmvp[i].mv[REFP_1], pmv_cands[num_cands][REFP_1]);
                    SET_REFI(core->sbTmvp[i].ref_idx, 0, 0);
                }
                else
                {
                    get_col_mv_ext(pi->refp[0], ctx->ptr, sb_scup_co[i], core->sbTmvp[i].mv, core->sbTmvp[i].ref_idx);
                }
            }
        }
#endif
    }
    num_cands++;

    // insert list_01, list_1, list_0
#if BGC
    derive_MHBskip_spatial_motions(&ctx->info, mod_info_curr, &ctx->map, &pmv_cands[num_cands], &refi_cands[num_cands], &(mod_info_curr->bgc_flag_cands[num_cands]), &(mod_info_curr->bgc_idx_cands[num_cands]));
#else
    derive_MHBskip_spatial_motions(&ctx->info, mod_info_curr, &ctx->map, &pmv_cands[num_cands], &refi_cands[num_cands]);
#endif
    num_cands += PRED_DIR_NUM;

#if INTER_TM
    if (is_tm)
    {
        *num_cands_all = num_cands;
        *num_cands_woUMVE = num_cands;
        return;
    }
#endif

#if MVAP
    if (ctx->info.sqh.mvap_enable_flag)
    {
#if USE_SP
        derive_mvap_motions(ctx->info.pic_header.ibc_flag || ctx->info.pic_header.sp_pic_flag || ctx->info.pic_header.evs_ubvs_pic_flag, num_cands, mod_info_curr->scup, cu_width, cu_height, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, ctx->map.map_scu, ctx->map.map_mv, ctx->map.map_refi, core->neighbor_motions, &core->valid_mvap_num, core->valid_mvap_index, refi_cands);
#else
        derive_mvap_motions(ctx->info.pic_header.ibc_flag, num_cands, mod_info_curr->scup, cu_width, cu_height, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, ctx->map.map_scu, ctx->map.map_mv, ctx->map.map_refi, core->neighbor_motions, &core->valid_mvap_num, core->valid_mvap_index, refi_cands);
#endif
        num_cands += core->valid_mvap_num;
    }
#endif

    // insert HMVP with pruning
    if (ctx->info.sqh.num_of_hmvp_cand || ctx->info.sqh.num_of_mvap_cand)
    {
        int skip_idx;
        for (skip_idx = 0; skip_idx < num_cands; skip_idx++)
        {
            fill_skip_candidates(motion_cands_curr, &cnt_hmvp_cands_curr, ctx->info.sqh.num_of_hmvp_cand, 
#if MVAP
                ctx->info.sqh.num_of_mvap_cand,
#endif
#if BGC
                bgc_flag_cands_curr, bgc_idx_cands_curr, mod_info_curr->bgc_flag_cands[skip_idx], mod_info_curr->bgc_idx_cands[skip_idx],
#endif
                pmv_cands[skip_idx], refi_cands[skip_idx], 0
#if UNIFIED_HMVP_2
                , skipCheckTmvpRedundancy
#endif
            );
        }
        for (skip_idx = core->cnt_hmvp_cands; skip_idx > 0; skip_idx--) // fill the HMVP skip candidates
        {
            COM_MOTION motion = core->motion_cands[skip_idx - 1];
#if BGC
            s8 bgc_flag = core->bgc_flag_cands[skip_idx - 1];
            s8 bgc_idx = core->bgc_idx_cands[skip_idx - 1];
#endif
            fill_skip_candidates(motion_cands_curr, &cnt_hmvp_cands_curr, ctx->info.sqh.num_of_hmvp_cand, 
#if MVAP
                ctx->info.sqh.num_of_mvap_cand,
#endif
#if BGC
                bgc_flag_cands_curr, bgc_idx_cands_curr, bgc_flag, bgc_idx,
#endif
                motion.mv, motion.ref_idx, 1
#if UNIFIED_HMVP_2
                , skipCheckTmvpRedundancy
#endif
            );
        }

        s8 cnt_hmvp_extend = cnt_hmvp_cands_curr;
        COM_MOTION motion = core->cnt_hmvp_cands ? core->motion_cands[core->cnt_hmvp_cands - 1] : motion_cands_curr[TRADITIONAL_SKIP_NUM - 1]; // use last HMVP candidate or last spatial candidate to fill the rest
#if BGC
        s8 bgc_flag_ext = core->cnt_hmvp_cands ? core->bgc_flag_cands[core->cnt_hmvp_cands - 1] : bgc_flag_cands_curr[TRADITIONAL_SKIP_NUM - 1];
        s8 bgc_idx_ext = core->cnt_hmvp_cands ? core->bgc_idx_cands[core->cnt_hmvp_cands - 1] : bgc_idx_cands_curr[TRADITIONAL_SKIP_NUM - 1];
#endif
        for (skip_idx = cnt_hmvp_cands_curr; skip_idx < (TRADITIONAL_SKIP_NUM + 
#if MVAP
            max(ctx->info.sqh.num_of_hmvp_cand, ctx->info.sqh.num_of_mvap_cand)) // fill skip candidates when hmvp not enough
#else
            ctx->info.sqh.num_of_hmvp_cand) // fill skip candidates when hmvp not enough
#endif
            ; skip_idx++)
        {
            // cnt_hmvp_cands_curr not changed
            fill_skip_candidates(motion_cands_curr, &cnt_hmvp_extend, ctx->info.sqh.num_of_hmvp_cand, 
#if MVAP
                ctx->info.sqh.num_of_mvap_cand,
#endif
#if BGC
                bgc_flag_cands_curr, bgc_idx_cands_curr, bgc_flag_ext, bgc_idx_ext,
#endif
                motion.mv, motion.ref_idx, 0
#if UNIFIED_HMVP_2
                , skipCheckTmvpRedundancy
#endif
            );
        }
#if MVAP
        assert(cnt_hmvp_extend == (TRADITIONAL_SKIP_NUM + max(ctx->info.sqh.num_of_hmvp_cand, ctx->info.sqh.num_of_mvap_cand)));
#else
        assert(cnt_hmvp_extend == (TRADITIONAL_SKIP_NUM + ctx->info.sqh.num_of_hmvp_cand));
#endif

        get_hmvp_skip_cands(motion_cands_curr, cnt_hmvp_extend, 
#if BGC
            bgc_flag_cands_curr, bgc_idx_cands_curr, mod_info_curr->bgc_flag_cands, mod_info_curr->bgc_idx_cands,
#endif
            pmv_cands, refi_cands);
        num_cands = cnt_hmvp_cands_curr;
    }

    // insert UMVE
    *num_cands_woUMVE = num_cands;
    if (ctx->info.sqh.umve_enable_flag)
    {
#if BGC
        s8 base_bgc_flag[UMVE_BASE_NUM];
        s8 base_bgc_idx[UMVE_BASE_NUM];
#endif
#if UMVE_TM
        u8 ibc_flag = 0;
        s8 num_hmvp_cands_curr = 0;
#if BGC
        s8 hmvp_bgc_idx_cands_curr[ALLOWED_HMVP_NUM];
        s8 hmvp_bgc_flag_cands_curr[ALLOWED_HMVP_NUM];
#endif
        COM_MOTION hmvp_motion_cands_curr[ALLOWED_HMVP_NUM];
        if (ctx->info.sqh.umve_tm_enable_flag)
        {
            ready_hmvp_mvap_candidates(&ibc_flag, core->neb_motions, &ctx->map, mod_info_curr,
                &num_hmvp_cands_curr, hmvp_motion_cands_curr, core->cnt_hmvp_cands, core->motion_cands, &ctx->info,
#if BGC
                core->bgc_flag_cands, core->bgc_idx_cands, hmvp_bgc_flag_cands_curr, hmvp_bgc_idx_cands_curr
#endif
            );

#if BGC
            derive_umve_base_motions(ctx->info.pic_header.slice_type, ibc_flag, core->neb_motions, &ctx->info, ctx->pinter.refp, ctx->pic[PIC_IDX_REC], mod_info_curr, &ctx->map, pmv_cands[0], refi_cands[0], hmvp_motion_cands_curr, hmvp_bgc_flag_cands_curr, hmvp_bgc_idx_cands_curr, &num_hmvp_cands_curr, core->valid_mvap_num, 1, pmv_base_cands, refi_base_cands, base_bgc_flag, base_bgc_idx, ctx->info.bit_depth_internal);
#else
            derive_umve_base_motions(ctx->info.pic_header.slice_type, ibc_flag, core->neb_motions, &ctx->info, ctx->pinter.refp, ctx->pic[PIC_IDX_REC], mod_info_curr, &ctx->map, pmv_cands[0], refi_cands[0], hmvp_motion_cands_curr, &num_hmvp_cands_curr, core->valid_mvap_num, 1, pmv_base_cands, refi_base_cands, ctx->info.bit_depth_internal);
#endif
        }
#else
#if BGC
        derive_umve_base_motions(&ctx->info, mod_info_curr, &ctx->map, pmv_cands[0], refi_cands[0], pmv_base_cands, refi_base_cands, base_bgc_flag, base_bgc_idx);
#else
        derive_umve_base_motions(&ctx->info, mod_info_curr, &ctx->map, pmv_cands[0], refi_cands[0], pmv_base_cands, refi_base_cands);
#endif
#endif

#if UMVE_ENH 
        int numUMVEIndices = (ctx->info.pic_header.umve_set_flag == 0) ? UMVE_MAX_REFINE_NUM : UMVE_MAX_REFINE_NUM_SEC_SET;
#else
        int numUMVEIndices = UMVE_MAX_REFINE_NUM;
#endif
        int basemv_single = 0;
        check_umve_base(pmv_base_cands, refi_base_cands, &basemv_single);
        if (!basemv_single)
        {
            numUMVEIndices *= UMVE_BASE_NUM;
        }
        for (umve_idx = 0; umve_idx < numUMVEIndices; umve_idx++)
        {
            derive_umve_final_motions(umve_idx, pi->refp, pi->ptr, pmv_base_cands, refi_base_cands, &pmv_cands[*num_cands_woUMVE], &refi_cands[*num_cands_woUMVE]
#if UMVE_ENH 
                , (BOOL)ctx->info.pic_header.umve_set_flag
#endif
#if BGC
                , base_bgc_flag, base_bgc_idx, &(mod_info_curr->bgc_flag_cands[*num_cands_woUMVE]), &(mod_info_curr->bgc_idx_cands[*num_cands_woUMVE])
#endif
            );
        }
        num_cands += numUMVEIndices;
    }
    *num_cands_all = num_cands;
}

static void analyze_direct_skip(ENC_CTX *ctx, ENC_CORE *core, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    double       cost;
    int          skip_idx;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;

    int num_cands_woUMVE = 0;
#if UMVE_ENH
    s16 pmv_cands[MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM_SEC_SET * UMVE_BASE_NUM][REFP_NUM][MV_D];
    s8  refi_cands[MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM_SEC_SET * UMVE_BASE_NUM][REFP_NUM];
#else
    s16 pmv_cands[MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM * UMVE_BASE_NUM][REFP_NUM][MV_D];
    s8 refi_cands[MAX_SKIP_NUM + UMVE_MAX_REFINE_NUM * UMVE_BASE_NUM][REFP_NUM];
#endif
    int num_cands_all, num_rdo;
    double cost_list[MAX_INTER_SKIP_RDO];
    int mode_list[MAX_INTER_SKIP_RDO];
#if SBT
    mod_info_curr->sbt_info = 0;
#endif
#if INTERPF
    int inter_filter_list[MAX_INTER_SKIP_RDO] = { 0 };
    mod_info_curr->inter_filter_flag = 0;
#endif
#if IPC
    int ipc_list[MAX_INTER_SKIP_RDO] = { 0 };
    mod_info_curr->ipc_flag = 0;
#if IPC_FAST
    int ipc_selected = !((core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup].visit) && (core->bef_data[mod_info_curr->cu_width_log2 - 2][mod_info_curr->cu_height_log2 - 2][core->cup].lic_flag_history == 0));
#endif
#endif

    derive_inter_cands(ctx, core, pmv_cands, refi_cands, &num_cands_all, &num_cands_woUMVE
#if INTER_TM
        , 0
#endif
    );
    num_rdo = num_cands_woUMVE;
#if MVAP
    assert(num_rdo <= min(MAX_INTER_SKIP_RDO, TRADITIONAL_SKIP_NUM + max(ctx->info.sqh.num_of_hmvp_cand, ctx->info.sqh.num_of_mvap_cand)));
#else
    assert(num_rdo <= min(MAX_INTER_SKIP_RDO, TRADITIONAL_SKIP_NUM + ctx->info.sqh.num_of_hmvp_cand));
#endif

    make_cand_list(core, ctx, mode_list, cost_list, num_cands_woUMVE, num_cands_all, num_rdo, pmv_cands, refi_cands);

#if IPC
    int no_interpf_rdo = num_rdo;
#endif
#if INTERPF
    if (ctx->info.sqh.interpf_enable_flag)
    {
        num_rdo = make_cand_inter_filter_list(core, ctx, mode_list, inter_filter_list, cost_list, num_cands_woUMVE, num_cands_all, num_rdo, pmv_cands, refi_cands);
    }
#endif

#if IPC
#if IPC_FAST
    if (ipc_selected && ctx->info.sqh.ipc_enable_flag)
#else
    if(ctx->info.sqh.ipc_enable_flag)
#endif
    {
        num_rdo = make_ipc_rdo_list(core, ctx, mode_list, ipc_list, cost_list, num_cands_woUMVE, num_cands_all, num_rdo, pmv_cands, refi_cands, no_interpf_rdo);
    }
#endif
    for (skip_idx = 0; skip_idx < num_rdo; skip_idx++)
    {
        int skip_idx_true = mode_list[skip_idx];

#if INTERPF
        mod_info_curr->inter_filter_flag = inter_filter_list[skip_idx];
#endif
#if IPC
        mod_info_curr->ipc_flag = ipc_list[skip_idx];
        if (!ctx->info.sqh.ipc_enable_flag)
            assert(mod_info_curr->ipc_flag == 0);
#if IPC_FAST
        if (!ipc_selected)
            assert(mod_info_curr->ipc_flag == 0);
#endif
#endif

        if (skip_idx_true < num_cands_woUMVE)
        {
            mod_info_curr->umve_flag = 0;
            mod_info_curr->skip_idx = skip_idx_true;
        }
        else
        {
            mod_info_curr->umve_flag = 1;
            mod_info_curr->umve_idx = skip_idx_true - num_cands_woUMVE;
#if INTERPF
#if UMVE_ENH
            if (!ctx->info.sqh.umve_enh_enable_flag)
#endif
            assert( mod_info_curr->inter_filter_flag == 0 );
#endif
#if IPC
#if UMVE_ENH
            if (!ctx->info.sqh.umve_enh_enable_flag)
#endif
            assert( mod_info_curr->ipc_flag == 0 );
#endif
        }

        mod_info_curr->mv[REFP_0][MV_X] = pmv_cands[skip_idx_true][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = pmv_cands[skip_idx_true][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = pmv_cands[skip_idx_true][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = pmv_cands[skip_idx_true][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = refi_cands[skip_idx_true][REFP_0];
        mod_info_curr->refi[REFP_1] = refi_cands[skip_idx_true][REFP_1];
#if BGC
        mod_info_curr->bgc_flag = mod_info_curr->bgc_flag_cands[skip_idx_true];
        mod_info_curr->bgc_idx  = mod_info_curr->bgc_idx_cands[skip_idx_true];
#if IPC
        if (mod_info_curr->ipc_flag)
            mod_info_curr->bgc_flag = FALSE;
#endif
#endif
#if SUB_TMVP
        if (ctx->info.sqh.sbtmvp_enable_flag && skip_idx_true == 0 && cu_width >= SBTMVP_MIN_SIZE && cu_height >= SBTMVP_MIN_SIZE)
        {
            core->sbTmvp_flag = 1;
#if UNIFIED_HMVP_1
            mod_info_curr->sub_tmvp_flag = 1;
#endif
        }
        else
        {
            core->sbTmvp_flag = 0;
#if UNIFIED_HMVP_1
            mod_info_curr->sub_tmvp_flag = 0;
#endif
        }
#endif
#if MVAP
        if (ctx->info.sqh.mvap_enable_flag)
        {
            if ((skip_idx_true < (core->valid_mvap_num + TRADITIONAL_SKIP_NUM)) && (skip_idx_true >= TRADITIONAL_SKIP_NUM))
            {
                core->mvap_flag = 1;
#if UNIFIED_HMVP_1
                mod_info_curr->mvap_flag = 1;
#endif
            }
            else
            {
                core->mvap_flag = 0;
#if UNIFIED_HMVP_1
                mod_info_curr->mvap_flag = 0;
#endif
            }
        }
#endif

        if (!REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            continue;
        }

        // skip index 1 and 2 for P slice
        if ((ctx->info.pic_header.slice_type == SLICE_P) && (mod_info_curr->skip_idx == 1 || mod_info_curr->skip_idx == 2) && (mod_info_curr->umve_flag == 0))
        {
            continue;
        }


        mod_info_curr->cu_mode = MODE_DIR;

        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                  , (mod_info_curr->umve_flag == 0) && ctx->info.sqh.dmvr_enable_flag
#endif
        );
        check_best_mode( core, pi, cost, cost_best );
    }
#if SUB_TMVP
    core->sbTmvp_flag = 0;
#endif
#if MVAP
    core->mvap_flag = 0;
#endif
}

static void analyze_affine_merge( ENC_CTX *ctx, ENC_CORE *core, double *cost_best )
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    pel          *y_org, *u_org, *v_org;
    s8           mrg_list_refi[AFF_MAX_NUM_MRG][REFP_NUM];
    int          mrg_list_cp_num[AFF_MAX_NUM_MRG];
    CPMV         mrg_list_cp_mv[AFF_MAX_NUM_MRG][REFP_NUM][VER_NUM][MV_D];
#if BGC
    s8           mrg_list_bgc_flag[AFF_MAX_NUM_MRG];
    s8           mrg_list_bgc_idx[AFF_MAX_NUM_MRG];
#endif
    double       cost = MAX_COST;
    int          mrg_idx, num_cands = 0;
    int          ver = 0;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int log2_cuw = mod_info_curr->cu_width_log2;
    int log2_cuh = mod_info_curr->cu_height_log2;
    int cu_width = 1 << log2_cuw;
    int cu_height = 1 << log2_cuh;
    y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    u_org = pi->Yuv_org[U_C] + (x >> 1) + ((y >> 1) * pi->stride_org[U_C]);
    v_org = pi->Yuv_org[V_C] + (x >> 1) + ((y >> 1) * pi->stride_org[V_C]);

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);

    pi->curr_mvr = 0;
    mod_info_curr->mvr_idx = 0;
    mod_info_curr->affine_flag = 1;
#if AFFINE_UMVE
    mod_info_curr->affine_umve_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if HACD
#if BGC
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, mrg_list_bgc_flag, mrg_list_bgc_idx, ctx->ptr, core->history_affine_mv);
#else
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, ctx->ptr, core->history_affine_mv);
#endif
#else
#if BGC
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, mrg_list_bgc_flag, mrg_list_bgc_idx, ctx->ptr);
#else
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, ctx->ptr);
#endif
#endif
  

    if ( num_cands == 0 )
        return;

#if AFFINE_UMVE
    double cost_best_affine_merge = MAX_COST;
    core->mod_info_best.best_affine_merge_index = -1;
#endif
    for ( mrg_idx = 0; mrg_idx < num_cands; mrg_idx++ )
    {
        // affine memory access constraints
        int memory_access[REFP_NUM];
        int allowed = 1;
        int i;
        for (i = 0; i < REFP_NUM; i++)
        {
            if (REFI_IS_VALID(mrg_list_refi[mrg_idx][i]))
            {
#if AFFINE_MEMORY_CONSTRAINT
                memory_access[i] = com_get_affine_memory_access(mrg_list_cp_mv[mrg_idx][i], cu_width, cu_height, mrg_list_cp_num[mrg_idx]);
#else
                if (mrg_list_cp_num[mrg_idx] == 3) // derive RB
                {
                    mrg_list_cp_mv[mrg_idx][i][3][MV_X] = mrg_list_cp_mv[mrg_idx][i][1][MV_X] + mrg_list_cp_mv[mrg_idx][i][2][MV_X] - mrg_list_cp_mv[mrg_idx][i][0][MV_X];
                    mrg_list_cp_mv[mrg_idx][i][3][MV_Y] = mrg_list_cp_mv[mrg_idx][i][1][MV_Y] + mrg_list_cp_mv[mrg_idx][i][2][MV_Y] - mrg_list_cp_mv[mrg_idx][i][0][MV_Y];
                }
                else // derive LB, RB
                {
                    mrg_list_cp_mv[mrg_idx][i][2][MV_X] = mrg_list_cp_mv[mrg_idx][i][0][MV_X] - (mrg_list_cp_mv[mrg_idx][i][1][MV_Y] - mrg_list_cp_mv[mrg_idx][i][0][MV_Y]) * (s16)cu_height / (s16)cu_width;
                    mrg_list_cp_mv[mrg_idx][i][2][MV_Y] = mrg_list_cp_mv[mrg_idx][i][0][MV_Y] + (mrg_list_cp_mv[mrg_idx][i][1][MV_X] - mrg_list_cp_mv[mrg_idx][i][0][MV_X]) * (s16)cu_height / (s16)cu_width;
                    mrg_list_cp_mv[mrg_idx][i][3][MV_X] = mrg_list_cp_mv[mrg_idx][i][1][MV_X] - (mrg_list_cp_mv[mrg_idx][i][1][MV_Y] - mrg_list_cp_mv[mrg_idx][i][0][MV_Y]) * (s16)cu_height / (s16)cu_width;
                    mrg_list_cp_mv[mrg_idx][i][3][MV_Y] = mrg_list_cp_mv[mrg_idx][i][1][MV_Y] + (mrg_list_cp_mv[mrg_idx][i][1][MV_X] - mrg_list_cp_mv[mrg_idx][i][0][MV_X]) * (s16)cu_height / (s16)cu_width;
                }
                memory_access[i] = com_get_affine_memory_access(mrg_list_cp_mv[mrg_idx][i], cu_width, cu_height);
#endif
            }
        }

        if ( REFI_IS_VALID( mrg_list_refi[mrg_idx][0] ) && REFI_IS_VALID( mrg_list_refi[mrg_idx][1] ) )
        {
#if AFFINE_MEMORY_CONSTRAINT
            int mem = (cu_width + 7 + cu_width / 4) * (cu_height + 7 + cu_height / 4);
#else
            int mem = MAX_MEMORY_ACCESS_BI * cu_width * cu_height;
#endif
            if ( memory_access[0] > mem || memory_access[1] > mem )
            {
                allowed = 0;
            }
        }
        else
        {
            int valid_idx = REFI_IS_VALID( mrg_list_refi[mrg_idx][0] ) ? 0 : 1;
#if AFFINE_MEMORY_CONSTRAINT
            int mem = (cu_width + 7 + cu_width / 4) * (cu_height + 7 + cu_height / 4);
#else
            int mem = MAX_MEMORY_ACCESS_UNI * cu_width * cu_height;
#endif
            if ( memory_access[valid_idx] > mem )
            {
                allowed = 0;
            }
        }
        if ( !allowed )
        {
            continue;
        }

        // set motion information
        mod_info_curr->umve_flag = 0;
#if INTERPF
        mod_info_curr->inter_filter_flag = 0;
#endif
#if IPC
        mod_info_curr->ipc_flag = 0;
#endif
        mod_info_curr->affine_flag = (u8)mrg_list_cp_num[mrg_idx] - 1;
        mod_info_curr->skip_idx = (u8)mrg_idx;
        for ( ver = 0; ver < mrg_list_cp_num[mrg_idx]; ver++ )
        {
            mod_info_curr->affine_mv[REFP_0][ver][MV_X] = mrg_list_cp_mv[mrg_idx][REFP_0][ver][MV_X];
            mod_info_curr->affine_mv[REFP_0][ver][MV_Y] = mrg_list_cp_mv[mrg_idx][REFP_0][ver][MV_Y];
            mod_info_curr->affine_mv[REFP_1][ver][MV_X] = mrg_list_cp_mv[mrg_idx][REFP_1][ver][MV_X];
            mod_info_curr->affine_mv[REFP_1][ver][MV_Y] = mrg_list_cp_mv[mrg_idx][REFP_1][ver][MV_Y];
        }
        mod_info_curr->refi[REFP_0] = mrg_list_refi[mrg_idx][REFP_0];
        mod_info_curr->refi[REFP_1] = mrg_list_refi[mrg_idx][REFP_1];
#if AFFINE_DMVR
        int affine_dmvr_poc_c = ctx->ptr;
        s8* refi = mod_info_curr->refi;
        int poc0 = ctx->refp[refi[REFP_0]][REFP_0].ptr;
        int poc1 = ctx->refp[refi[REFP_1]][REFP_1].ptr;
        int bit_depth = ctx->info.bit_depth_internal;
        CPMV(*mv)[VER_NUM][MV_D] = mod_info_curr->affine_mv;
        BOOL affine_dmvr_poc_condition = ((BOOL)((affine_dmvr_poc_c - poc0) * (affine_dmvr_poc_c - poc1) < 0)) && (abs(affine_dmvr_poc_c - poc0) == abs(affine_dmvr_poc_c - poc1));//前后参考帧与当前帧时域上间隔一样
        BOOL affine_dmvr_flag = 0;
        int sub_w;
        int sub_h;
        if (affine_dmvr_poc_condition && REFI_IS_VALID(refi[REFP_0]) && REFI_IS_VALID(refi[REFP_1]))
        {
            affine_dmvr_flag = 1;
            sub_w = 8;
            sub_h = 8;
        }
        if (affine_dmvr_flag)
        {
            process_AFFINEDMVR(&ctx->info, mod_info_curr, ctx->refp, bit_depth, sub_w, sub_h, mv);
#if AFFINE_PARA
            process_AFFINEPARA(&ctx->info, mod_info_curr, ctx->refp, bit_depth, sub_w, sub_h, mv);
#endif
        }
#endif
#if BGC
        mod_info_curr->bgc_flag = mrg_list_bgc_flag[mrg_idx];
        mod_info_curr->bgc_idx = mrg_list_bgc_idx[mrg_idx];
#endif
        mod_info_curr->cu_mode = MODE_DIR;
        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                  , 0
#endif
        );
#if AFFINE_UMVE
        if (cost < cost_best_affine_merge)
        {
            cost_best_affine_merge = cost;
            core->mod_info_best.best_affine_merge_index = mrg_idx;
        }
#endif
        check_best_mode( core, pi, cost, cost_best );
    }
}

#if AFFINE_UMVE
static double GetCost_SATD(ENC_CTX *ctx, ENC_CORE *core)
{

    ENC_PINTER *pi = &ctx->pinter;
    pel          *y_org, *u_org, *v_org;

    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    u_org = pi->Yuv_org[U_C] + (x >> 1) + ((y >> 1) * pi->stride_org[U_C]);
    v_org = pi->Yuv_org[V_C] + (x >> 1) + ((y >> 1) * pi->stride_org[V_C]);

    pel(*pred)[MAX_CU_DIM] = mod_info_curr->pred;
    int bit_depth = ctx->info.bit_depth_internal;
    int    bit_cnt;
    pel   *org[N_C];
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;

#if !BD_AFFINE_AMVR
    if (mod_info_curr->affine_flag)
    {
        pi->curr_mvr = 0;
    }
#endif
    org[Y_C] = pi->Yuv_org[Y_C] + (y * pi->stride_org[Y_C]) + x;
    org[U_C] = pi->Yuv_org[U_C] + ((y >> 1) * pi->stride_org[U_C]) + (x >> 1);
    org[V_C] = pi->Yuv_org[V_C] + ((y >> 1) * pi->stride_org[V_C]) + (x >> 1);
#if IST
    mod_info_curr->slice_type = ctx->slice_type;
#endif
    /* prediction */
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        assert(REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]));
    }

    if (mod_info_curr->affine_flag)
    {
#if ASP
        ctx->info.skip_umve_asp = TRUE;
#endif
        COM_INFO* info = &ctx->info;

        int scup = mod_info_curr->scup;
        int x = mod_info_curr->x_pos;
        int y = mod_info_curr->y_pos;
        int w = mod_info_curr->cu_width;
        int h = mod_info_curr->cu_height;

        int pic_w = info->pic_width;
        int pic_h = info->pic_height;
        int pic_width_in_scu = info->pic_width_in_scu;
        COM_PIC_HEADER * sh = &info->pic_header;

        int vertex_num = mod_info_curr->affine_flag + 1;
        s8 *refi = mod_info_curr->refi;
        CPMV(*mv)[VER_NUM][MV_D] = mod_info_curr->affine_mv;
        pel(*pred_buf)[MAX_CU_DIM] = mod_info_curr->pred;
#if BGC
        u8  bgc_flag = mod_info_curr->bgc_flag;
        u8  bgc_idx = mod_info_curr->bgc_idx;
        pel *pred_fir = info->pred_tmp;
        pel *p0, *p1, *dest;
#endif 
        
        static pel pred_snd[N_C][MAX_CU_DIM];
        pel(*pred)[MAX_CU_DIM] = pred_buf;
#if SIMD_MC
        int bidx = 0;
#else
        int i, j, bidx = 0;
#endif

        int sub_w = 4;
        int sub_h = 4;
        if (sh->affine_subblock_size_idx == 1)
        {
            sub_w = 8;
            sub_h = 8;
        }
        if (REFI_IS_VALID(refi[REFP_0]) && REFI_IS_VALID(refi[REFP_1]))
        {
            sub_w = 8;
            sub_h = 8;
        }

        if (REFI_IS_VALID(refi[REFP_0]))
        {
            /* forward */
#if ASP
            com_affine_mc_l(info, x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[REFP_0], pi->refp[refi[REFP_0]][REFP_0].pic, pred[0], vertex_num, sub_w, sub_h, bit_depth);
#else
            com_affine_mc_l(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[REFP_0], pi->refp[refi[REFP_0]][REFP_0].pic, pred[0], vertex_num, sub_w, sub_h, bit_depth);
#endif
            bidx++;
#if BGC
            if (bgc_flag)
            {
                p0 = pred_buf[Y_C];
                dest = pred_fir;
                for (int j = 0; j < h; j++)
                {
                    for (int i = 0; i < w; i++)
                    {
                        dest[i] = p0[i];
                    }
                    p0 += w;
                    dest += w;
                }
            }
#endif
        }

        if (REFI_IS_VALID(refi[REFP_1]))
        {
            /* backward */
            if (bidx)
            {
                pred = pred_snd;
            }
#if ASP
            com_affine_mc_l(info, x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[REFP_1], pi->refp[refi[REFP_1]][REFP_1].pic, pred[0], vertex_num, sub_w, sub_h, bit_depth);
#else
            com_affine_mc_l(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[REFP_1], pi->refp[refi[REFP_1]][REFP_1].pic, pred[0], vertex_num, sub_w, sub_h, bit_depth);
#endif
            bidx++;
        }

        if (bidx == 2)
        {
#if SIMD_MC
            average_16b_no_clip_sse(pred_buf[Y_C], pred_snd[Y_C], pred_buf[Y_C], w, w, w, w, h);
#else
            p0 = pred_buf[Y_C];
            p1 = pred_snd[Y_C];
            for (j = 0; j < h; j++)
            {
                for (i = 0; i < w; i++)
                {
                    p0[i] = (p0[i] + p1[i] + 1) >> 1;
                }
                p0 += w;
                p1 += w;
            }
#endif
#if BGC
            if (bgc_flag && info->sqh.bgc_enable_flag)
            {
                dest = pred_buf[Y_C];
                p0 = pred_fir;
                p1 = pred_snd[Y_C];
                for (int j = 0; j < h; j++)
                {
                    for (int i = 0; i < w; i++)
                    {
                        if (bgc_idx)
                        {
                            dest[i] += (p0[i] - p1[i]) >> 3;
                        }
                        else
                        {
                            dest[i] += (p1[i] - p0[i]) >> 3;
                        }
                        dest[i] = (pel)COM_CLIP3(0, (1 << bit_depth) - 1, dest[i]);
                    }
                    p0 += w;
                    p1 += w;
                    dest += w;
                }
            }
#endif
        }
#if OBMC
        if (ctx->info.sqh.obmc_enable_flag)
        {
            com_set_affine_mvf(&ctx->info, mod_info_curr, ctx->refp, &ctx->map);
            pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, TRUE, FALSE, bit_depth
                , sub_w, sub_h
                , ctx->ptr
#if BGC
                , bgc_flag, bgc_idx
#endif
#if OBMC_TEMP
                , ctx->pic[PIC_IDX_REC]
#endif
            );
        }
#endif
#if ASP
        ctx->info.skip_umve_asp = FALSE;
#endif
    }
    //dist
    u32 cy = calc_satd_16b(cu_width, cu_height, y_org, mod_info_curr->pred[Y_C], pi->stride_org[Y_C], cu_width, bit_depth);
    double cost_y = (double)cy;
    //bitrate
    SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
    bit_cnt = enc_get_bit_number(&core->s_temp_run);
#if SBT_FAST
    enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 0); // not include coeff bits
#else
    enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type); // not include coeff bits
#endif
    bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
    SBAC_STORE(core->s_temp_best, core->s_temp_run);
    //cost
    cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);

    return cost_y;
}

static void analyze_affine_umve(ENC_CTX *ctx, ENC_CORE *core, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    COM_MODE *mod_info_best = &core->mod_info_best;
    ENC_PINTER *pi = &ctx->pinter;
    s8           mrg_list_refi[AFF_MAX_NUM_MRG][REFP_NUM];
    int          mrg_list_cp_num[AFF_MAX_NUM_MRG];
    CPMV         mrg_list_cp_mv[AFF_MAX_NUM_MRG][REFP_NUM][VER_NUM][MV_D];
#if BGC 
    s8           mrg_list_bgc_flag[AFF_MAX_NUM_MRG];
    s8           mrg_list_bgc_idx[AFF_MAX_NUM_MRG];
#endif
    double       cost = MAX_COST;
    int          num_cands = 0;
    int          ver = 0;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int log2_cuw = mod_info_curr->cu_width_log2;
    int log2_cuh = mod_info_curr->cu_height_log2;
    int cu_width = 1 << log2_cuw;
    int cu_height = 1 << log2_cuh;

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);

    pi->curr_mvr = 0;
    mod_info_curr->mvr_idx = 0;
    mod_info_curr->affine_flag = 1;

#if HACD
#if BGC
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, mrg_list_bgc_flag, mrg_list_bgc_idx, ctx->ptr, core->history_affine_mv);
#else
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, ctx->ptr, core->history_affine_mv);
#endif
#else
#if BGC
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, mrg_list_bgc_flag, mrg_list_bgc_idx, ctx->ptr);
#else
    num_cands = com_get_affine_merge_candidate(&ctx->info, mod_info_curr, pi->refp, &ctx->map, mrg_list_refi, mrg_list_cp_mv, mrg_list_cp_num, ctx->ptr);
#endif
#endif


    if (num_cands == 0)
        return;

    double cost_best_satd = MAX_COST;
    int skip_idx_best = -1;
    int affine_umve_idx0_best = 0;
    int affine_umve_idx1_best = 0;

    //1. base_index
    skip_idx_best = mod_info_best->best_affine_merge_index;
    if (skip_idx_best == -1)
    {
        return;
    }
    // set motion information
    mod_info_curr->umve_flag = 0; 
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if INTERPF
    mod_info_curr->inter_filter_flag = 0;
#endif
#if IPC
    mod_info_curr->ipc_flag = 0;
#endif
    mod_info_curr->affine_flag = (u8)mrg_list_cp_num[skip_idx_best] - 1;
    mod_info_curr->affine_umve_flag = 1;
    mod_info_curr->skip_idx = (u8)skip_idx_best;
    for (ver = 0; ver < mrg_list_cp_num[skip_idx_best]; ver++)
    {
        mod_info_curr->affine_mv[REFP_0][ver][MV_X] = mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_X];
        mod_info_curr->affine_mv[REFP_0][ver][MV_Y] = mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_Y];
        mod_info_curr->affine_mv[REFP_1][ver][MV_X] = mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_X];
        mod_info_curr->affine_mv[REFP_1][ver][MV_Y] = mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_Y];
    }
    mod_info_curr->refi[REFP_0] = mrg_list_refi[skip_idx_best][REFP_0];
    mod_info_curr->refi[REFP_1] = mrg_list_refi[skip_idx_best][REFP_1];
#if AFFINE_DMVR
    int affine_dmvr_poc_c = ctx->ptr;
    s8* refi = mod_info_curr->refi;
    int poc0 = ctx->refp[refi[REFP_0]][REFP_0].ptr;
    int poc1 = ctx->refp[refi[REFP_1]][REFP_1].ptr;
    int bit_depth = ctx->info.bit_depth_internal;
    CPMV(*mv)[VER_NUM][MV_D] = mod_info_curr->affine_mv;
    BOOL affine_dmvr_poc_condition = ((BOOL)((affine_dmvr_poc_c - poc0) * (affine_dmvr_poc_c - poc1) < 0)) && (abs(affine_dmvr_poc_c - poc0) == abs(affine_dmvr_poc_c - poc1));//前后参考帧与当前帧时域上间隔一样
    BOOL affine_dmvr_flag = 0;
    int sub_w;
    int sub_h;
    if (affine_dmvr_poc_condition && REFI_IS_VALID(refi[REFP_0]) && REFI_IS_VALID(refi[REFP_1]))
    {
        affine_dmvr_flag = 1;
        sub_w = 8;
        sub_h = 8;
    }
    if (affine_dmvr_flag)
    {
        process_AFFINEDMVR(&ctx->info, mod_info_curr, ctx->refp, bit_depth, sub_w, sub_h, mv);
#if AFFINE_PARA
        process_AFFINEPARA(&ctx->info, mod_info_curr, ctx->refp, bit_depth, sub_w, sub_h, mv);
#endif
    }
    for (ver = 0; ver < mrg_list_cp_num[skip_idx_best]; ver++)
    {
        mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_X] = mod_info_curr->affine_mv[REFP_0][ver][MV_X];
        mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_Y] = mod_info_curr->affine_mv[REFP_0][ver][MV_Y];
        mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_X] = mod_info_curr->affine_mv[REFP_1][ver][MV_X];
        mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_Y] = mod_info_curr->affine_mv[REFP_1][ver][MV_Y];
    }
#endif
#if BGC
    mod_info_curr->bgc_flag = mrg_list_bgc_flag[skip_idx_best];
    mod_info_curr->bgc_idx = mrg_list_bgc_idx[skip_idx_best];
#endif
    int affine_umve_idx0_change = 0;
    int affine_umve_idx1_change = 0;
    for (int iter = 0; iter < 2; iter++)
    {
        //2. cpmv0
        for (int affine_umve_idx0 = 0; affine_umve_idx0 < AFFINE_UMVE_MAX_REFINE_NUM; affine_umve_idx0++)
        {
            s32 affine_mv_offset[REFP_NUM][MV_D];
            derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx0, affine_mv_offset);
            mod_info_curr->affine_mv[REFP_0][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_X] + affine_mv_offset[REFP_0][MV_X]);
            mod_info_curr->affine_mv[REFP_0][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
            mod_info_curr->affine_mv[REFP_1][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_X] + affine_mv_offset[REFP_1][MV_X]);
            mod_info_curr->affine_mv[REFP_1][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
            mod_info_curr->affine_umve_idx[0] = affine_umve_idx0;
            derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx1_best, affine_mv_offset);
            mod_info_curr->affine_mv[REFP_0][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_X] + affine_mv_offset[REFP_0][MV_X]);
            mod_info_curr->affine_mv[REFP_0][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
            mod_info_curr->affine_mv[REFP_1][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_X] + affine_mv_offset[REFP_1][MV_X]);
            mod_info_curr->affine_mv[REFP_1][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
            mod_info_curr->affine_umve_idx[1] = affine_umve_idx1_best;
            
            double cost_y = GetCost_SATD(ctx, core);
            if (cost_y < cost_best_satd)
            {
                cost_best_satd = cost_y;
                affine_umve_idx0_best = mod_info_curr->affine_umve_idx[0];
                if (iter > 0)
                {
                    affine_umve_idx0_change = 1;
                }
            }
        }
        if (affine_umve_idx0_best == -1)
        {
            return;
        }

        if (iter > 0 && !affine_umve_idx0_change)
        {
            break;
        }
        affine_umve_idx0_change = 0;
        //3. cpmv1
        s32 affine_mv_offset[REFP_NUM][MV_D];
        derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx0_best, affine_mv_offset);
        mod_info_curr->affine_mv[REFP_0][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_X] + affine_mv_offset[REFP_0][MV_X]);
        mod_info_curr->affine_mv[REFP_0][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
        mod_info_curr->affine_mv[REFP_1][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_X] + affine_mv_offset[REFP_1][MV_X]);
        mod_info_curr->affine_mv[REFP_1][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
        mod_info_curr->affine_umve_idx[0] = affine_umve_idx0_best;
        for (int affine_umve_idx1 = 0; affine_umve_idx1 < AFFINE_UMVE_MAX_REFINE_NUM; affine_umve_idx1++)
        {
            derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx1, affine_mv_offset);
            mod_info_curr->affine_mv[REFP_0][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_X] + affine_mv_offset[REFP_0][MV_X]);
            mod_info_curr->affine_mv[REFP_0][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
            mod_info_curr->affine_mv[REFP_1][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_X] + affine_mv_offset[REFP_1][MV_X]);
            mod_info_curr->affine_mv[REFP_1][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
            mod_info_curr->affine_umve_idx[1] = affine_umve_idx1;

            double cost_y = GetCost_SATD(ctx, core);
            if (cost_y < cost_best_satd)
            {
                cost_best_satd = cost_y;
                affine_umve_idx1_best = mod_info_curr->affine_umve_idx[1];
                if (iter > 0)
                {
                    affine_umve_idx1_change = 1;
                }
            }
            
        }

        if (affine_umve_idx1_best == -1)
        {
            return;
        }

        if (iter > 0 && !affine_umve_idx1_change)
        {
            break;
        }
        affine_umve_idx1_change = 0;
    }
    mod_info_curr->skip_idx = skip_idx_best;
    mod_info_curr->affine_umve_idx[0] = affine_umve_idx0_best;
    mod_info_curr->affine_umve_idx[1] = affine_umve_idx1_best;
    {
        mod_info_curr->affine_flag = (u8)mrg_list_cp_num[skip_idx_best] - 1;
        s32 affine_mv_offset[REFP_NUM][MV_D];
        for (ver = 0; ver < mrg_list_cp_num[skip_idx_best]; ver++)
        {
            mod_info_curr->affine_mv[REFP_0][ver][MV_X] = mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_X];
            mod_info_curr->affine_mv[REFP_0][ver][MV_Y] = mrg_list_cp_mv[skip_idx_best][REFP_0][ver][MV_Y];
            mod_info_curr->affine_mv[REFP_1][ver][MV_X] = mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_X];
            mod_info_curr->affine_mv[REFP_1][ver][MV_Y] = mrg_list_cp_mv[skip_idx_best][REFP_1][ver][MV_Y];
        }
        mod_info_curr->refi[REFP_0] = mrg_list_refi[skip_idx_best][REFP_0];
        mod_info_curr->refi[REFP_1] = mrg_list_refi[skip_idx_best][REFP_1];
        derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx0_best, affine_mv_offset);
        mod_info_curr->affine_mv[REFP_0][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_X] + affine_mv_offset[REFP_0][MV_X]);
        mod_info_curr->affine_mv[REFP_0][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][0][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
        mod_info_curr->affine_mv[REFP_1][0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_X] + affine_mv_offset[REFP_1][MV_X]);
        mod_info_curr->affine_mv[REFP_1][0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][0][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
        derive_affine_umve_final_motion(mod_info_curr->refi, affine_umve_idx1_best, affine_mv_offset);
        mod_info_curr->affine_mv[REFP_0][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_X] + affine_mv_offset[REFP_0][MV_X]);
        mod_info_curr->affine_mv[REFP_0][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_0][1][MV_Y] + affine_mv_offset[REFP_0][MV_Y]);
        mod_info_curr->affine_mv[REFP_1][1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_X] + affine_mv_offset[REFP_1][MV_X]);
        mod_info_curr->affine_mv[REFP_1][1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mrg_list_cp_mv[skip_idx_best][REFP_1][1][MV_Y] + affine_mv_offset[REFP_1][MV_Y]);
    }

#if AFFINE_MEMORY_CONSTRAINT
    // affine memory access constraints
    int mem = (cu_width + 7 + cu_width / 4) * (cu_height + 7 + cu_height / 4);
    for (int i = 0; i < REFP_NUM; i++)
    {
        if (REFI_IS_VALID(mod_info_curr->refi[i]))
        {
            int memory_access = com_get_affine_memory_access(mod_info_curr->affine_mv[i], cu_width, cu_height, mod_info_curr->affine_flag + 1);
            if (memory_access > mem)
            {
                return;
            }
        }
    }
#endif
    mod_info_curr->cu_mode = MODE_DIR;
    cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                              , 0
#endif
    );
    check_best_mode( core, pi, cost, cost_best );
}
#endif

#if AWP
void sort_awp_partition_list(int awp_idx, int candidate_idx, double sad_cost, u8 bestCandX_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION], double bestCandX_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION])
{
    int shift = 0;
    while (shift < CADIDATES_PER_PARTITION && sad_cost < bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            bestCandX_idx [awp_idx][CADIDATES_PER_PARTITION - i] = bestCandX_idx [awp_idx][CADIDATES_PER_PARTITION - 1 - i];
            bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - i] = bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - 1 - i];
        }
        bestCandX_idx [awp_idx][CADIDATES_PER_PARTITION - shift] = candidate_idx;
        bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - shift] = sad_cost;
    }
}

void sort_awp_satd_list(int awp_idx, double satd_cost, u8 candidate0, u8 candidate1, u8 best_satd_cand_idx[AWP_RDO_NUM], double best_satd_cost[AWP_RDO_NUM], u8 best_cand0_idx[AWP_RDO_NUM], u8 best_cand1_idx[AWP_RDO_NUM])
{
    int shift = 0;
    while (shift < AWP_RDO_NUM && satd_cost < best_satd_cost[AWP_RDO_NUM - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            best_satd_cand_idx[AWP_RDO_NUM - i] = best_satd_cand_idx[AWP_RDO_NUM - 1 - i];
            best_satd_cost[AWP_RDO_NUM - i] = best_satd_cost[AWP_RDO_NUM - 1 - i];
            best_cand0_idx[AWP_RDO_NUM - i] = best_cand0_idx[AWP_RDO_NUM - 1 - i];
            best_cand1_idx[AWP_RDO_NUM - i] = best_cand1_idx[AWP_RDO_NUM - 1 - i];
        }
        best_satd_cand_idx[AWP_RDO_NUM - shift] = awp_idx;
        best_satd_cost[AWP_RDO_NUM - shift] = satd_cost;
        best_cand0_idx[AWP_RDO_NUM - shift] = candidate0;
        best_cand1_idx[AWP_RDO_NUM - shift] = candidate1;
    }
}

static void analyze_awp_merge(ENC_CTX *ctx, ENC_CORE *core, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int num_cands = 0;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    int scup_co = get_rb_scup(mod_info_curr->scup, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, cu_width, cu_height, ctx->info.max_cuwh);
    int bit_depth = ctx->info.bit_depth_internal;

    // for sad/satd
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int y_org_s = pi->stride_org[Y_C];
    pel* y_org = pi->Yuv_org[Y_C] + x + y * y_org_s;
    int y_pred_s = cu_width;
    pel* y_pred = mod_info_curr->pred[Y_C];

    s32 awp_rdo_idx = 0;

    double cost = 0;
    int max_sad_num = cu_width* cu_height * (1 << 16);

    mod_info_curr->affine_flag = 0;

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);

    mod_info_curr->umve_flag = 0;
    mod_info_curr->affine_flag = 0;
#if MVAP
    core->mvap_flag = 0;
#endif
#if INTERPF
    mod_info_curr->inter_filter_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if IPC
    mod_info_curr->ipc_flag = 0;
#endif
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if EXT_AMVR_HMVP
    mod_info_curr->mvp_from_hmvp_flag = 0;
#endif
    pi->curr_mvr = 0;
    mod_info_curr->ph_awp_refine_flag = ctx->info.pic_header.ph_awp_refine_flag;

    /****************************** get awp list ******************************/
    s16 pmv_temp[REFP_NUM][MV_D];
    s8 refi_temp[REFP_NUM];
    refi_temp[REFP_0] = 0;
    refi_temp[REFP_1] = 0;
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        refi_temp[REFP_0] = 0;
        refi_temp[REFP_1] = -1;
        if (REFI_IS_VALID(ctx->refp[0][REFP_0].map_refi[scup_co][REFP_0]))
        {
            get_col_mv_from_list0(ctx->refp[0], ctx->ptr, scup_co, pmv_temp);
        }
        else
        {
            pmv_temp[REFP_0][MV_X] = 0;
            pmv_temp[REFP_0][MV_Y] = 0;
        }
        pmv_temp[REFP_1][MV_X] = 0;
        pmv_temp[REFP_1][MV_Y] = 0;
    }
    else
    {
        if (!REFI_IS_VALID(ctx->refp[0][REFP_1].map_refi[scup_co][REFP_0]))
        {
            if (REFI_IS_VALID(ctx->refp[0][REFP_1].map_refi[scup_co][REFP_1]))
            {
                get_col_mv_ext(ctx->refp[0], ctx->ptr, scup_co, pmv_temp, refi_temp);
            }
            else
            {
                com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_0, 0, 0, pmv_temp[REFP_0]);

                com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_1, 0, 0, pmv_temp[REFP_1]);
            }
        }
        else
        {
            get_col_mv_ext(ctx->refp[0], ctx->ptr, scup_co, pmv_temp, refi_temp);
        }
    }

    s16 awp_uni_cands[AWP_MV_LIST_LENGTH][REFP_NUM][MV_D];
    s8 awp_uni_refi[AWP_MV_LIST_LENGTH][REFP_NUM];

    u8 valid_cand_num = com_derive_awp_base_motions(&ctx->info, mod_info_curr, &ctx->map, pmv_temp, refi_temp, awp_uni_cands, awp_uni_refi, ctx->rpm.num_refp, pi->refp);
    /************************ get SAD for all candidates ************************/
    enc_init_awp_template(ctx); // only once per sequence

    u64 sad_whole_blk[AWP_MV_LIST_LENGTH] = { 0 };
    u64 best_whole_blk_sad = max_sad_num;
    double best_whole_blk_cost = 0;
    double cand_cost[AWP_MV_LIST_LENGTH];

    //calc_sad_16b
    for (int cand_idx = 0; cand_idx < COM_MIN(AWP_MV_LIST_LENGTH, valid_cand_num); cand_idx++)
    {
        /* get two pred buf */
        mod_info_curr->awp_flag = 0;
        mod_info_curr->mv[REFP_0][MV_X] = awp_uni_cands[cand_idx][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = awp_uni_cands[cand_idx][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = awp_uni_cands[cand_idx][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = awp_uni_cands[cand_idx][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = awp_uni_refi[cand_idx][REFP_0];
        mod_info_curr->refi[REFP_1] = awp_uni_refi[cand_idx][REFP_1];

        //disable DMVR
#if DMVR
        COM_DMVR dmvr;
        dmvr.poc_c = 0;
        dmvr.apply_DMVR = 0;
#endif

        com_mc(x, y, cu_width, cu_height, cu_width, ctx->cand_buff[cand_idx], &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
            , &dmvr
#endif
#if BIO
            , -1, 0, mod_info_curr->mvr_idx
#endif
#if MVAP
            , 0
#endif
#if SUB_TMVP
            , 0
#endif
#if BGC
            , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
        );
        sad_whole_blk[cand_idx] = calc_sad_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], y_org_s, cu_width, bit_depth);

        cand_cost[cand_idx] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], cand_idx + 1);

        if (sad_whole_blk[cand_idx] < best_whole_blk_sad)
        {
            best_whole_blk_sad = sad_whole_blk[cand_idx];
            best_whole_blk_cost = (double)best_whole_blk_sad + (double)cand_cost[cand_idx];
        }
    }
    //clear mv
    mod_info_curr->mv[REFP_0][MV_X] = 0;
    mod_info_curr->mv[REFP_0][MV_Y] = 0;
    mod_info_curr->mv[REFP_1][MV_X] = 0;
    mod_info_curr->mv[REFP_1][MV_Y] = 0;
    mod_info_curr->refi[REFP_0] = REFI_INVALID;
    mod_info_curr->refi[REFP_1] = REFI_INVALID;


    /***************** get best combination for each awp mode ******************/
    u8 best_cand0_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    u8 best_cand1_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand0_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand1_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION];

    //set largest sad
#if BAWP
    s8 awp_mode_num = ctx->info.pic_header.slice_type == SLICE_P ? com_tbl_bawp_num[mod_info_curr->cu_width_log2 - MIN_AWP_SIZE_LOG2][mod_info_curr->cu_height_log2 - MIN_AWP_SIZE_LOG2] : AWP_MODE_NUM;

    for (int awp_idx = 0; awp_idx < awp_mode_num; awp_idx++)
#else
    for (int awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)
#endif
    {
        for (int candidate_idx = 0; candidate_idx < CADIDATES_PER_PARTITION; candidate_idx++)
        {
            best_cand0_idx[awp_idx][candidate_idx] = -1;
            best_cand1_idx[awp_idx][candidate_idx] = -1;
            best_cand0_cost[awp_idx][candidate_idx] = MAX_COST;
            best_cand1_cost[awp_idx][candidate_idx] = MAX_COST;
        }
    }

#if BAWP
    for (int awp_idx = 0; awp_idx < awp_mode_num; awp_idx++)
#else
    for (int awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)
#endif
    {
        u64 sad_small = 0;
        u64 sad_large = 0;
        double awp_cost0 = 0;
        double awp_cost1 = 0;

        for (u8 cand_idx = 0; cand_idx < COM_MIN(AWP_MV_LIST_LENGTH, valid_cand_num); cand_idx++)
        {
            if (ctx->awp_larger_area[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_width_log2 - MIN_AWP_SIZE_LOG2][awp_idx] == 0)
            {
#if FIX_372
                if (ctx->info.pic_header.slice_type == SLICE_P)
                {
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight1_bawp[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
                }
                else
                {
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
                }
#else
                sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
#endif
                awp_cost1 = (double)sad_small + cand_cost[cand_idx];
                sad_large = sad_whole_blk[cand_idx] - sad_small;
                awp_cost0 = (double)sad_large + cand_cost[cand_idx];

            }
            else
            {
#if FIX_372
                if (ctx->info.pic_header.slice_type == SLICE_P)
                {
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight0_bawp[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
                }
                else
                {
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
                }
#else
                sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);
#endif
                awp_cost0 = (double)sad_small + cand_cost[cand_idx];
                sad_large = sad_whole_blk[cand_idx] - sad_small;
                awp_cost1 = (double)sad_large + cand_cost[cand_idx];
            }
            sort_awp_partition_list(awp_idx, cand_idx, awp_cost0, best_cand0_idx, best_cand0_cost);

            sort_awp_partition_list(awp_idx, cand_idx, awp_cost1, best_cand1_idx, best_cand1_cost);
        }
    }

    /**************** use SATD to get best 3 candidates for RDO ****************/
    double satd_cost_y = 0;
    int bit_cnt = 0;
    u8 best_satd_awp_idx[AWP_RDO_NUM];
    double best_satd_cost[AWP_RDO_NUM];
    u8 best_cand0[AWP_RDO_NUM];
    u8 best_cand1[AWP_RDO_NUM];

    for (int i = 0; i < AWP_RDO_NUM; i++)
    {
        best_satd_awp_idx[i] = 0;
        best_satd_cost[i] = MAX_COST;
        best_cand0[i] = -1;
        best_cand1[i] = -1;
    }

    int awp_stad_num = 0;
    u8 temp_cand0;
    u8 temp_cand1;
#if BAWP
    for (int awp_idx = 0; awp_idx < awp_mode_num; awp_idx++)
#else
    for (int awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)
#endif
    {
        for (int cand_per_mode0 = 0; cand_per_mode0 < CADIDATES_PER_PARTITION; cand_per_mode0++)
        {
            for (int cand_per_mode1 = 0; cand_per_mode1 < CADIDATES_PER_PARTITION; cand_per_mode1++)
            {
                mod_info_curr->awp_flag = 1;
                mod_info_curr->skip_idx = awp_idx;
                temp_cand0 = (u8)best_cand0_idx[awp_idx][cand_per_mode0];
                temp_cand1 = (u8)best_cand1_idx[awp_idx][cand_per_mode1];
                mod_info_curr->awp_idx0 = temp_cand0;
                mod_info_curr->awp_idx1 = temp_cand1;

                if (mod_info_curr->awp_idx0 == mod_info_curr->awp_idx1)
                    continue;

                double temp_cost = best_cand0_cost[awp_idx][cand_per_mode0] + best_cand1_cost[awp_idx][cand_per_mode1];

                if (temp_cost > best_whole_blk_cost)
                    continue;

                awp_stad_num++;

                com_set_awp_mv_para(mod_info_curr, awp_uni_cands, awp_uni_refi);

                /* combine two pred buf */
#if BAWP
#if FIX_372
                if (ctx->info.pic_header.slice_type == SLICE_P)
                {
                    com_derive_awp_pred(mod_info_curr, Y_C, ctx->cand_buff[temp_cand0], ctx->cand_buff[temp_cand1], ctx->awp_weight0_bawp[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1_bawp[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                }
                else if (mod_info_curr->ph_awp_refine_flag)
#else
                if (mod_info_curr->ph_awp_refine_flag || ctx->info.pic_header.slice_type == SLICE_P)
#endif
#else
                if (mod_info_curr->ph_awp_refine_flag)
#endif
                {
                    com_derive_awp_pred(mod_info_curr, Y_C, ctx->cand_buff[temp_cand0], ctx->cand_buff[temp_cand1], ctx->awp_weight0_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                }
                else
                {
                    com_derive_awp_pred(mod_info_curr, Y_C, ctx->cand_buff[temp_cand0], ctx->cand_buff[temp_cand1], ctx->awp_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                }
                satd_cost_y = (double)calc_satd_16b(cu_width, cu_height, y_org, y_pred, y_org_s, y_pred_s, bit_depth);

#if BAWP
                bit_cnt = 0;
                if (awp_mode_num > 1)
                {
                    s32 val = 1 << com_tbl_logmap[awp_mode_num];
                    if (awp_idx >= val - (awp_mode_num - val))
                    {
                        bit_cnt++;
                    }
                }
#else
                bit_cnt = 5;
                if (awp_idx > 7)
                {
                    bit_cnt++;
                }
#endif

                int mvBits = 2;
                // cost for mvs
                temp_cand0 -= temp_cand0 < temp_cand1 ? 0 : 1;

                mvBits += temp_cand0;
                mvBits += temp_cand1;

                bit_cnt += mvBits;
                satd_cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);

                sort_awp_satd_list(awp_idx, satd_cost_y, best_cand0_idx[awp_idx][cand_per_mode0], best_cand1_idx[awp_idx][cand_per_mode1], best_satd_awp_idx, best_satd_cost, best_cand0, best_cand1);
            }
        }
    }

    /****************** RDO process for SATD seleted candidates ****************/
#if BAWP
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        awp_stad_num = COM_MIN(2, awp_stad_num); // Only 2 rdo process was allowed for P picture
    }
#endif
    for (awp_rdo_idx = 0; awp_rdo_idx < COM_MIN(AWP_RDO_NUM, awp_stad_num); awp_rdo_idx++)
    {
        mod_info_curr->awp_flag = 1;
        mod_info_curr->skip_idx = best_satd_awp_idx[awp_rdo_idx];
        mod_info_curr->awp_idx0 = best_cand0[awp_rdo_idx];
        mod_info_curr->awp_idx1 = best_cand1[awp_rdo_idx];

        com_set_awp_mv_para(mod_info_curr, awp_uni_cands, awp_uni_refi);

        mod_info_curr->cu_mode = MODE_DIR;

        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
            , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);
    }
    mod_info_curr->awp_flag = 0;
}
#endif

#if INTER_TM
static void make_cand_list_tm(ENC_CTX* ctx, ENC_CORE* core, BOOL apply_dmvr, double* cost_list, int* mode_list)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    s16 (*coef)[MAX_CU_DIM] = mod_info_curr->coef;
    s16 (*mv)[MV_D] = mod_info_curr->mv;
    s8 *refi = mod_info_curr->refi;
    ENC_PINTER *pi = &ctx->pinter;
    pel(*pred)[MAX_CU_DIM] = mod_info_curr->pred;
    int bit_depth = ctx->info.bit_depth_internal;
    int(*num_nz_coef)[N_C], width[N_C], height[N_C], log2_w[N_C], log2_h[N_C];
    int    bit_cnt;
    int    i;
    pel   *org[N_C];
    double cost_comp_best = MAX_COST;
    int    cbf_comps[N_C] = { 0, };
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
#if ST_CHROMA
    int use_secTrans[MAX_NUM_CHANNEL][MAX_NUM_TB] = { { 0, 0, 0, 0 },{ 0, 0, 0, 0 } }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans[MAX_NUM_CHANNEL] = { 0,0 };
#else
    int use_secTrans[MAX_NUM_TB] = { 0, 0, 0, 0 }; // secondary transform is disabled for inter coding
    int use_alt4x4Trans = 0;
#endif
    int run_comp[3] = { 1, ctx->tree_status == TREE_LC, ctx->tree_status == TREE_LC };
#if RDO_DBK
    u8  is_from_mv_field = 0;
#endif
#if IPC_SB8
    int ipc_size = 16;
#endif
#if SBT // sbt rdo
    mod_info_curr->sbt_info = 0;
#endif

#if INTER_CCNPM
    mod_info_curr->inter_ccnpm_flag = 0;
#endif

#if !BD_AFFINE_AMVR
    if (mod_info_curr->affine_flag)
    {
        pi->curr_mvr = 0;
    }
#endif

    num_nz_coef = mod_info_curr->num_nz;
    width [Y_C] = 1 << cu_width_log2 ;
    height[Y_C] = 1 << cu_height_log2;
    width [U_C] = width[V_C] = 1 << (cu_width_log2 - 1);
    height[U_C] = height[V_C] = 1 << (cu_height_log2 - 1);
    log2_w[Y_C] = cu_width_log2;
    log2_h[Y_C] = cu_height_log2;
    log2_w[U_C] = log2_w[V_C] = cu_width_log2 - 1;
    log2_h[U_C] = log2_h[V_C] = cu_height_log2 - 1;
    org[Y_C] = pi->Yuv_org[Y_C] + (y * pi->stride_org[Y_C]) + x;
    org[U_C] = pi->Yuv_org[U_C] + ((y >> 1) * pi->stride_org[U_C]) + (x >> 1);
    org[V_C] = pi->Yuv_org[V_C] + ((y >> 1) * pi->stride_org[V_C]) + (x >> 1);
#if IST
    mod_info_curr->slice_type = ctx->slice_type;
#endif
#if ISTS
    mod_info_curr->ph_ists_enable_flag = ctx->info.pic_header.ph_ists_enable_flag;
#endif
#if TS_INTER
    mod_info_curr->ph_ts_inter_enable_flag = ctx->info.pic_header.ph_ts_inter_enable_flag;
#endif
    /* prediction */
#if BAWP
    if (ctx->info.pic_header.slice_type == SLICE_P && mod_info_curr->awp_flag != 1)
#else
    if (ctx->info.pic_header.slice_type == SLICE_P)
#endif
    {
        assert(REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && !REFI_IS_VALID(mod_info_curr->refi[REFP_1]));
    }

#if DMVR
    COM_DMVR dmvr;
    dmvr.poc_c = ctx->ptr;
    dmvr.dmvr_current_template = pi->dmvr_template;
    dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
    dmvr.apply_DMVR = apply_dmvr && ctx->info.sqh.dmvr_enable_flag;
    dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
#if OBMC
    int obmc_blk_width = cu_width;
    int obmc_blk_height = cu_height;
#endif
#if BGC_TM
    u8 tm_bgc_flag = mod_info_curr->bgc_flag;
    u8 tm_bgc_idx = mod_info_curr->bgc_idx;
    if ((!(core->sbTmvp_flag || core->mvap_flag || mod_info_curr->etmvp_flag || mod_info_curr->ipc_flag)) && (mod_info_curr->cu_mode == MODE_SKIP || mod_info_curr->cu_mode == MODE_DIR) && ctx->info.sqh.bgc_tm_enable_flag)
    {
        derive_bgc(&ctx->info, ctx->pinter.refp, ctx->pic[PIC_IDX_REC], mod_info_curr, ctx->map.map_scu, bit_depth, &tm_bgc_flag, &tm_bgc_idx);
    }
#endif
    assert(mod_info_curr->pb_info.sub_scup[0] == mod_info_curr->scup);
#if OBMC
    if (ctx->info.sqh.obmc_enable_flag)
    {
        store_blk_mvfield(mod_info_curr, &ctx->map.map_mv[mod_info_curr->scup], &ctx->map.map_refi[mod_info_curr->scup], ctx->info.pic_width_in_scu, (cu_width >> MIN_CU_LOG2), (cu_height >> MIN_CU_LOG2));
    }
#endif
    com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, ctx->tree_status, bit_depth
#if DMVR
        , &dmvr
#endif
#if BIO
        , ctx->ptr, 0, pi->curr_mvr
#endif
#if MVAP
        , 0
#endif
#if SUB_TMVP
        , 0
#endif
#if BGC
#if BGC_TM
        , tm_bgc_flag, tm_bgc_idx
#else
        , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
    );
#if OBMC
    if (ctx->info.sqh.obmc_enable_flag)
    {
        pred_obmc(&core->mod_info_curr, &ctx->info, &ctx->map, ctx->pinter.refp, ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_L) ? TRUE : FALSE), ((ctx->tree_status == CHANNEL_LC || ctx->tree_status == CHANNEL_C) ? TRUE : FALSE), ctx->info.bit_depth_internal
            , obmc_blk_width, obmc_blk_height
            , ctx->ptr
#if BGC
#if BGC_TM
            , tm_bgc_flag, tm_bgc_idx
#else
            , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
#endif
#if OBMC_TEMP
            , ctx->pic[PIC_IDX_REC]
#endif
        );
    }
#endif
#if INTERPF
    if(mod_info_curr->inter_filter_flag)
    {
        pred_inter_filter(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                            , x, y, cu_width, cu_height, bit_depth, ctx->tree_status, mod_info_curr->inter_filter_flag);
    }
#endif
#if IPC
    if(mod_info_curr->ipc_flag)
    {
        pred_inter_pred_correction(PIC_REC(ctx), ctx->map.map_scu, ctx->map.map_ipm, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, core->nb, mod_info_curr
                            , x, y, cu_width, cu_height, bit_depth, ctx->tree_status, mod_info_curr->ipc_flag
#if IPC_SB8
                            , ipc_size
#endif
        );
    }
#endif

    mod_info_curr->cu_mode = MODE_SKIP;
    pel* y_org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    u32 cy = calc_satd_16b(cu_width, cu_height, y_org, mod_info_curr->pred[Y_C], pi->stride_org[Y_C], cu_width, bit_depth);
    double cost_y = (double)cy;

    SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
    bit_cnt = enc_get_bit_number(&core->s_temp_run);

#if SBT_FAST
    enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type, 1);
#else
    enc_bit_est_inter(ctx, core, ctx->info.pic_header.slice_type);
#endif

    bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt;
    SBAC_STORE(core->s_temp_best, core->s_temp_run);
    cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);

    int num_rdo = MAX_TM_RDO, shift = 0;
    while (shift < num_rdo && cost_y < cost_list[num_rdo - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (i = 1; i < shift; i++)
        {
            cost_list[num_rdo - i] = cost_list[num_rdo - 1 - i];
            mode_list[num_rdo - i] = mode_list[num_rdo - 1 - i];

            memcpy(pi->pred_cache[num_rdo - i][Y_C], pi->pred_cache[num_rdo - i - 1][Y_C], sizeof(pel) * (cu_width*cu_height));
            memcpy(pi->pred_cache[num_rdo - i][U_C], pi->pred_cache[num_rdo - i - 1][U_C], sizeof(pel) * ((cu_width*cu_height)>>2));
            memcpy(pi->pred_cache[num_rdo - i][V_C], pi->pred_cache[num_rdo - i - 1][V_C], sizeof(pel) * ((cu_width*cu_height)>>2));
        }
        cost_list[num_rdo - shift] = cost_y;
        mode_list[num_rdo - shift] = mod_info_curr->tm_idx;

        memcpy(pi->pred_cache[num_rdo - shift][Y_C], mod_info_curr->pred[Y_C], sizeof(pel)* (cu_width*cu_height));
        memcpy(pi->pred_cache[num_rdo - shift][U_C], mod_info_curr->pred[U_C], sizeof(pel)* ((cu_width*cu_height)>>2));
        memcpy(pi->pred_cache[num_rdo - shift][V_C], mod_info_curr->pred[V_C], sizeof(pel)* ((cu_width*cu_height)>>2));
    }
}

static void analyze_tm(ENC_CTX* ctx, ENC_CORE* core, double* cost_best)
{
    COM_MODE* mod_info_curr = &core->mod_info_curr;
    ENC_PINTER* pi = &ctx->pinter;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << mod_info_curr->cu_width_log2;
    int cu_height = 1 << mod_info_curr->cu_height_log2;
    const int bit_depth = ctx->info.bit_depth_internal;
    BOOL is_simplified = ctx->info.sqh.tm_enable_flag == 1 ? 0 : 1;

    if (x < TM_WIDTH || y < TM_WIDTH)
    {
        return;
    }

    if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].tm_flag_history == 0)
        {
            return;
        }
    }

    s16 init_pmv[TM_CANDS][REFP_NUM][MV_D];
    s16 pmv_cands[TM_CANDS][REFP_NUM][MV_D];
    s8  refi_cands[TM_CANDS][REFP_NUM];

    int num_cands_all = 0, num_cands_woUMVE = 0;
    derive_inter_cands(ctx, core, pmv_cands, refi_cands, &num_cands_all, &num_cands_woUMVE, 1);
    assert(num_cands_all == 1 + PRED_DIR_NUM && num_cands_all == TM_CANDS);
    u8 reduced_flag = 0;
    if (is_simplified)
    {
        cands_adjustment(pmv_cands, refi_cands, &num_cands_all); 
        remove_duplicate_cands(pmv_cands, refi_cands, &num_cands_all); 
        reduced_flag = num_cands_all < TM_CANDS ? 1 : 0;
    }
    else
    {
        remove_duplicate_cands(pmv_cands, refi_cands, &num_cands_all); 
        reduced_flag = num_cands_all < TM_CANDS ? 1 : 0;
        cands_adjustment(pmv_cands, refi_cands, &num_cands_all);
    }
    pre_evaluation_cands(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, PIC_REC(ctx)->y, PIC_REC(ctx)->stride_luma, pi->refp, pmv_cands, refi_cands, &num_cands_all, bit_depth, is_simplified); 
    for (u8 cand_idx = 0; cand_idx < num_cands_all; cand_idx++)
    {
        for (u8 refp_idx = 0; refp_idx < REFP_NUM; refp_idx++)
        {
            init_pmv[cand_idx][refp_idx][MV_X] = pmv_cands[cand_idx][refp_idx][MV_X];
            init_pmv[cand_idx][refp_idx][MV_Y] = pmv_cands[cand_idx][refp_idx][MV_Y];
            single_mv_clip(x-TM_WIDTH, y-TM_WIDTH, ctx->info.pic_width, ctx->info.pic_height, cu_width+TM_WIDTH, cu_height+TM_WIDTH, init_pmv[cand_idx][refp_idx]);
        }
        com_tm_process(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, pi->refp, refi_cands[cand_idx], init_pmv[cand_idx], pmv_cands[cand_idx], PIC_REC(ctx)->y, PIC_REC(ctx)->stride_luma, bit_depth, is_simplified);
    }

    double cost_list[MAX_TM_RDO];
    for (u8 i = 0; i < MAX_TM_RDO; i++)
    {
        cost_list[i] = MAX_COST;
    }
    int mode_list[MAX_TM_RDO];
    int rdo_num = num_cands_all;
    for (int rdo_idx = 0; rdo_idx < rdo_num; rdo_idx++)
    {
        u8 cand_idx = rdo_idx;
        mod_info_curr->tm_idx = reduced_flag ? (cand_idx | 0x10) : cand_idx;

        mod_info_curr->tm_flag = 1;
        mod_info_curr->cu_mode = MODE_DIR;
        mod_info_curr->inter_filter_flag = 0;
        mod_info_curr->umve_flag = 0;
        mod_info_curr->awp_flag = 0;
        mod_info_curr->affine_flag = 0;
        mod_info_curr->ipc_flag = 0;
        mod_info_curr->etmvp_flag = 0;
        core->sbTmvp_flag = 0;
        mod_info_curr->sub_tmvp_flag = 0;
        core->mvap_flag = 0;
        mod_info_curr->mvap_flag = 0;
        pi->curr_mvr = 0;

        for (u8 refp_idx = 0; refp_idx < REFP_NUM; refp_idx++)
        {
            mod_info_curr->init_mv[refp_idx][MV_X] = init_pmv[cand_idx][refp_idx][MV_X];
            mod_info_curr->init_mv[refp_idx][MV_Y] = init_pmv[cand_idx][refp_idx][MV_Y];
            mod_info_curr->mv[refp_idx][MV_X] = pmv_cands[cand_idx][refp_idx][MV_X];
            mod_info_curr->mv[refp_idx][MV_Y] = pmv_cands[cand_idx][refp_idx][MV_Y];
            mod_info_curr->refi[refp_idx] = refi_cands[cand_idx][refp_idx];
        }

        make_cand_list_tm(ctx, core, 0, cost_list, mode_list);
    }

    rdo_num = num_cands_all == TM_CANDS ? MAX_TM_RDO : MAX_TM_RDO-1;
    for (int rdo_idx = 0; rdo_idx < rdo_num; rdo_idx++)
    {
        u8 cand_idx = mode_list[rdo_idx] & 0x0f;
        mod_info_curr->tm_idx = mode_list[rdo_idx];

        mod_info_curr->tm_flag = 1;
        mod_info_curr->cu_mode = MODE_DIR;
        mod_info_curr->inter_filter_flag = 0;
        mod_info_curr->umve_flag = 0;
        mod_info_curr->awp_flag = 0;
        mod_info_curr->affine_flag = 0;
        mod_info_curr->ipc_flag = 0;
        mod_info_curr->etmvp_flag = 0;
        core->sbTmvp_flag = 0;
        mod_info_curr->sub_tmvp_flag = 0;
        core->mvap_flag = 0;
        mod_info_curr->mvap_flag = 0;
        pi->curr_mvr = 0;

        mod_info_curr->mv[REFP_0][MV_X] = pmv_cands[cand_idx][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = pmv_cands[cand_idx][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = pmv_cands[cand_idx][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = pmv_cands[cand_idx][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = refi_cands[cand_idx][REFP_0];
        mod_info_curr->refi[REFP_1] = refi_cands[cand_idx][REFP_1];

        memcpy(mod_info_curr->pred[Y_C], pi->pred_cache[rdo_idx][Y_C], sizeof(pel)* (cu_width* cu_height));
        memcpy(mod_info_curr->pred[U_C], pi->pred_cache[rdo_idx][U_C], sizeof(pel)* ((cu_width*cu_height)>>2));
        memcpy(mod_info_curr->pred[V_C], pi->pred_cache[rdo_idx][V_C], sizeof(pel)* ((cu_width*cu_height)>>2));

        double cost = pinter_residue_rdo_no_mc(ctx, core, 0);
        check_best_mode(core, pi, cost, cost_best);
    }
}
#endif

#if AWP_MVR
void sort_awp_mvr_partition_list(u8 awp_idx, u8 candidate_idx, u8 umveIdx, double sad_cost, u8 bestCandX_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION], u8 bestUMVE_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION], double bestCandX_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION])
{
    int shift = 0;
    while (shift < CADIDATES_PER_PARTITION && sad_cost < bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            bestCandX_idx[awp_idx][CADIDATES_PER_PARTITION - i] = bestCandX_idx[awp_idx][CADIDATES_PER_PARTITION - 1 - i];
            bestUMVE_idx[awp_idx][CADIDATES_PER_PARTITION - i] = bestUMVE_idx[awp_idx][CADIDATES_PER_PARTITION - 1 - i];
            bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - i] = bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - 1 - i];
        }
        bestCandX_idx[awp_idx][CADIDATES_PER_PARTITION - shift] = candidate_idx;
        bestUMVE_idx[awp_idx][CADIDATES_PER_PARTITION - shift] = umveIdx;
        bestCandX_cost[awp_idx][CADIDATES_PER_PARTITION - shift] = sad_cost;
    }
}

void sort_awp_partition_rd_cost_list(u8 awp_idx, double temp_cost, u8 best_awp_modes[AWP_RDO_NUM], double best_awp_mode_cost[AWP_RDO_NUM])
{
    int shift = 0;
    while (shift < AWP_RDO_NUM && temp_cost < best_awp_mode_cost[AWP_RDO_NUM - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            best_awp_modes[AWP_RDO_NUM - i] = best_awp_modes[AWP_RDO_NUM - 1 - i];
            best_awp_mode_cost[AWP_RDO_NUM - i] = best_awp_mode_cost[AWP_RDO_NUM - 1 - i];
        }
        best_awp_modes[AWP_RDO_NUM - shift] = awp_idx;
        best_awp_mode_cost[AWP_RDO_NUM - shift] = temp_cost;
    }
}

void sort_awp_partition_sad_list(u8 awp_idx, double temp_cost, u8 best_awp_modes[NUM_PARTITION_FOR_AWP_MVR_RD], double best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD])
{
    int shift = 0;
    while (shift < NUM_PARTITION_FOR_AWP_MVR_RD && temp_cost < best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            best_awp_modes[NUM_PARTITION_FOR_AWP_MVR_RD - i] = best_awp_modes[NUM_PARTITION_FOR_AWP_MVR_RD - 1 - i];
            best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD - i] = best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD - 1 - i];
        }
        best_awp_modes[NUM_PARTITION_FOR_AWP_MVR_RD - shift] = awp_idx;
        best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD - shift] = temp_cost;
    }
}

#if AWP_ENH
void sort_awp_satd_list1(int awp_idx, double satd_cost, u8 candidate0, u8 candidate1, u8 umve0, u8 umve1, int best_satd_cand_idx[AWP_RDO_NUM], double best_satd_cost[AWP_RDO_NUM], u8 best_cand0_idx[AWP_RDO_NUM], u8 best_cand1_idx[AWP_RDO_NUM], u8 best_umve0_idx[AWP_RDO_NUM], u8 best_umve1_idx[AWP_RDO_NUM])
#else
void sort_awp_satd_list1(int awp_idx, double satd_cost, u8 candidate0, u8 candidate1, u8 umve0, u8 umve1, u8 best_satd_cand_idx[AWP_RDO_NUM], double best_satd_cost[AWP_RDO_NUM], u8 best_cand0_idx[AWP_RDO_NUM], u8 best_cand1_idx[AWP_RDO_NUM], u8 best_umve0_idx[AWP_RDO_NUM], u8 best_umve1_idx[AWP_RDO_NUM])
#endif
{
    int shift = 0;
    while (shift < AWP_RDO_NUM && satd_cost < best_satd_cost[AWP_RDO_NUM - 1 - shift])
    {
        shift++;
    }
    if (shift != 0)
    {
        for (int i = 1; i < shift; i++)
        {
            best_satd_cand_idx[AWP_RDO_NUM - i] = best_satd_cand_idx[AWP_RDO_NUM - 1 - i];
            best_satd_cost[AWP_RDO_NUM - i] = best_satd_cost[AWP_RDO_NUM - 1 - i];
            best_cand0_idx[AWP_RDO_NUM - i] = best_cand0_idx[AWP_RDO_NUM - 1 - i];
            best_cand1_idx[AWP_RDO_NUM - i] = best_cand1_idx[AWP_RDO_NUM - 1 - i];
            best_umve0_idx[AWP_RDO_NUM - i] = best_umve0_idx[AWP_RDO_NUM - 1 - i];
            best_umve1_idx[AWP_RDO_NUM - i] = best_umve1_idx[AWP_RDO_NUM - 1 - i];
        }
        best_satd_cand_idx[AWP_RDO_NUM - shift] = awp_idx;
        best_satd_cost[AWP_RDO_NUM - shift] = satd_cost;
        best_cand0_idx[AWP_RDO_NUM - shift] = candidate0;
        best_cand1_idx[AWP_RDO_NUM - shift] = candidate1;
        best_umve0_idx[AWP_RDO_NUM - shift] = umve0;
        best_umve1_idx[AWP_RDO_NUM - shift] = umve1;
    }
}

#if AWP_ENH
static inline u32 sort_awp_mode(double* cost_list, int cost_len, int* idx_list, int idx_len)
{
    int num_valid = 1;
    double sorted_list[AWP_MODE_NUM];
    sorted_list[0] = cost_list[0];
    idx_list[0] = (int)0;

    for (int idx = 1; idx < cost_len; ++idx)
    {
        int insert_idx = 0;
        double* sub_list = sorted_list;
        u32 sub_list_size = num_valid;
        while (sub_list_size > 1)
        {
            int middle_idx = sub_list_size >> 1;
            if (cost_list[idx] < sub_list[middle_idx])
            {
                sub_list_size = middle_idx;
            }
            else
            {
                sub_list += middle_idx;
                sub_list_size -= middle_idx;
                insert_idx += middle_idx;
            }
        }
        insert_idx += (cost_list[idx] < sub_list[0] ? 0 : 1);
        if (insert_idx < idx_len)
        {
            int start_idx = idx_len - 1;
            if (num_valid < idx_len)
            {
                start_idx = num_valid;
                ++num_valid;
            }
            for (int i = start_idx; i > insert_idx; --i)
            {
                sorted_list[i] = sorted_list[i - 1];
                idx_list[i] = idx_list[i - 1];
            }
            sorted_list[insert_idx] = cost_list[idx];
            idx_list[insert_idx] = (int)idx;
        }
    }
    return num_valid;
}
#endif

static void analyze_awp_merge_comb(ENC_CTX *ctx, ENC_CORE *core, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int num_cands = 0;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    int scup_co = get_rb_scup(mod_info_curr->scup, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, cu_width, cu_height, ctx->info.max_cuwh);
    int bit_depth = ctx->info.bit_depth_internal;

    // for sad/satd
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int y_org_s = pi->stride_org[Y_C];
    pel* y_org = pi->Yuv_org[Y_C] + x + y * y_org_s;
    int y_pred_s = cu_width;
    pel* y_pred = mod_info_curr->pred[Y_C];

    s32 awp_rdo_idx = 0;

    double cost = 0;
    const int num_iter = 2;

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);

#if AWP_ENH
    int use_dawp = ctx->info.sqh.dawp_enable_flag && ctx->slice_type != SLICE_P ? 1 : 0;
    if (use_dawp)
    {
        init_awp_tpl(mod_info_curr, ctx->awp_weight_tpl);
        com_get_tpl_cur(PIC_REC(ctx), ctx->map.map_scu, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, mod_info_curr);
    }
#endif

    mod_info_curr->umve_flag = 0;
    mod_info_curr->affine_flag = 0;
#if MVAP
    core->mvap_flag = 0;
#endif
#if INTERPF
    mod_info_curr->inter_filter_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag     = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if IPC
    mod_info_curr->ipc_flag = 0;
#endif
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if EXT_AMVR_HMVP
    mod_info_curr->mvp_from_hmvp_flag = 0;
#endif
    pi->curr_mvr = 0;
    mod_info_curr->ph_awp_refine_flag = ctx->info.pic_header.ph_awp_refine_flag;

    /****************************** calc signaling cost ******************************/
    SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
    int bit_cnt_org = enc_get_bit_number(&core->s_temp_run);

#if AWP_ENH  
    double awp_blend_idx_cost[TOTAL_BLEND_NUM];
    for (int i = 0; i < TOTAL_BLEND_NUM; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_blend_idx(&core->bs_temp, i, ctx);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_blend_idx_cost[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }
#endif

#if AWP_ENH
    double awp_mode_idx_cost[56];
    for (int i = 0; i < 56; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_mode_idx(&core->bs_temp, i, ctx);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_mode_idx_cost[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }
#endif

    double awp_cand_idx_cost[AWP_MV_LIST_LENGTH];
    for (int i = 0; i < AWP_MV_LIST_LENGTH; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_cand_idx0(&core->bs_temp, i);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_cand_idx_cost[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }

    double awp_cand_idx_cost1[AWP_MV_LIST_LENGTH - 1];
    for (int i = 0; i < AWP_MV_LIST_LENGTH - 1; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_cand_idx1(&core->bs_temp, i);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_cand_idx_cost1[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }

    double awp_umve_flag[2];
    for (int i = 0; i < 2; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_mvr_flag(&core->bs_temp, i, ctx);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_umve_flag[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }

    double awp_umve_idx_cost[AWP_MVR_MAX_REFINE_NUM];
    for (int i = 0; i < AWP_MVR_MAX_REFINE_NUM; i++)
    {
        SBAC_LOAD(core->s_temp_run, core->s_curr_best[cu_width_log2 - 2][cu_height_log2 - 2]);
        encode_awp_mvr_idx(&core->bs_temp, i);
        int bit_cnt = enc_get_bit_number(&core->s_temp_run) - bit_cnt_org;
        awp_umve_idx_cost[i] = RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], bit_cnt);
    }

    /****************************** get awp list ******************************/
    s16 pmv_temp[REFP_NUM][MV_D];
    s8 refi_temp[REFP_NUM];
    refi_temp[REFP_0] = 0;
    refi_temp[REFP_1] = 0;
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        refi_temp[REFP_0] = 0;
        refi_temp[REFP_1] = -1;
        if (REFI_IS_VALID(ctx->refp[0][REFP_0].map_refi[scup_co][REFP_0]))
        {
            get_col_mv_from_list0(ctx->refp[0], ctx->ptr, scup_co, pmv_temp);
        }
        else
        {
            pmv_temp[REFP_0][MV_X] = 0;
            pmv_temp[REFP_0][MV_Y] = 0;
        }
        pmv_temp[REFP_1][MV_X] = 0;
        pmv_temp[REFP_1][MV_Y] = 0;
    }
    else
    {
        if (!REFI_IS_VALID(ctx->refp[0][REFP_1].map_refi[scup_co][REFP_0]))
        {
            if (REFI_IS_VALID(ctx->refp[0][REFP_1].map_refi[scup_co][REFP_1]))
            {
                get_col_mv_ext(ctx->refp[0], ctx->ptr, scup_co, pmv_temp, refi_temp);
            }
            else
            {
                com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_0, 0, 0, pmv_temp[REFP_0]);

                com_get_mvp_default(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, REFP_1, 0, 0, pmv_temp[REFP_1]);
            }
        }
        else
        {
            get_col_mv_ext(ctx->refp[0], ctx->ptr, scup_co, pmv_temp, refi_temp);
        }
    }

    s16 awp_uni_cands[AWP_MV_LIST_LENGTH][REFP_NUM][MV_D];
    s8  awp_uni_refi[AWP_MV_LIST_LENGTH][REFP_NUM];
    s16 awp_mvr_uni_cands[AWP_MV_LIST_LENGTH][AWP_MVR_MAX_REFINE_NUM][REFP_NUM][MV_D];
    s8  awp_mvr_uni_refi[AWP_MV_LIST_LENGTH][AWP_MVR_MAX_REFINE_NUM][REFP_NUM];
    u8 valid_cand_num = com_derive_awp_base_motions(&ctx->info, mod_info_curr, &ctx->map, pmv_temp, refi_temp, awp_uni_cands, awp_uni_refi, ctx->rpm.num_refp, pi->refp);
    /************************ get SAD for all candidates ************************/
    enc_init_awp_template(ctx); // only once per sequence

    u64 sad_whole_blk[AWP_MV_LIST_LENGTH] = { 0 };
    u64 sad_umve_whole_blk[AWP_MV_LIST_LENGTH][AWP_MVR_MAX_REFINE_NUM];
    s32 awp_mvr_offset[REFP_NUM][MV_D];

#if DMVR
    COM_DMVR dmvr;
    dmvr.poc_c = 0;
#endif

    for (u8 cand_idx = 0; cand_idx < COM_MIN(AWP_MV_LIST_LENGTH, valid_cand_num); cand_idx++)
    {
        //calc sad of non-UMVE candidates

#if AWP_ENH
        if (use_dawp && mod_info_curr->tpl_cur_avail[0] == 1 && mod_info_curr->tpl_cur_avail[1] == 1)
        {
            mod_info_curr->awp_flag = 0;
            s16 tpl_mv_offset[2][MV_D] = { 0 };
            tpl_mv_offset[0][MV_X] = 0;
            tpl_mv_offset[0][MV_Y] = -(1 << 2);
            tpl_mv_offset[1][MV_X] = -(1 << 2);
            tpl_mv_offset[1][MV_Y] = 0;
            int dir;
            const int tpl_size = 1; 
            int tpl_height[2], tpl_width[2];
            tpl_width[0] = cu_width;
            tpl_height[0] = tpl_size;
            tpl_width[1] = tpl_size;
            tpl_height[1] = cu_height;
            int tpl_stride = 1;
            int offset[2] = { 0, AWP_TPL_SIZE };
            for (dir = 0; dir < 2; dir++)
            {
                mod_info_curr->mv[REFP_0][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_X] + tpl_mv_offset[dir][MV_X]);
                mod_info_curr->mv[REFP_0][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_Y] + tpl_mv_offset[dir][MV_Y]);
                mod_info_curr->mv[REFP_1][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_X] + tpl_mv_offset[dir][MV_X]);
                mod_info_curr->mv[REFP_1][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_Y] + tpl_mv_offset[dir][MV_Y]);
                mod_info_curr->refi[REFP_0] = awp_uni_refi[cand_idx][REFP_0];
                mod_info_curr->refi[REFP_1] = awp_uni_refi[cand_idx][REFP_1];
#if DMVR
                dmvr.apply_DMVR = 0;
#endif 
                com_mc(x, y, (tpl_width[dir] == 1 ? 4 : tpl_width[dir]), (tpl_height[dir] == 1 ? 4 : tpl_height[dir]), cu_width, ctx->cand_buff[cand_idx], &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , -1, 0, mod_info_curr->mvr_idx
#endif
#if MVAP
                    , 0
#endif
#if SUB_TMVP
                    , 0
#endif
#if BGC
                    , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
                );
                for (int i = 0; i < tpl_height[dir]; i++)
                {
                    for (int j = 0; j < tpl_width[dir]; j++)
                    {
                        ctx->cand_buff_tpl[cand_idx][0][i * tpl_stride + j + offset[dir]] = ctx->cand_buff[cand_idx][Y_C][i * cu_width + j];
                    }
                }

            }
        }
#endif

        mod_info_curr->awp_flag = 0;
        mod_info_curr->mv[REFP_0][MV_X] = awp_uni_cands[cand_idx][REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = awp_uni_cands[cand_idx][REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = awp_uni_cands[cand_idx][REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = awp_uni_cands[cand_idx][REFP_1][MV_Y];
        mod_info_curr->refi[REFP_0] = awp_uni_refi[cand_idx][REFP_0];
        mod_info_curr->refi[REFP_1] = awp_uni_refi[cand_idx][REFP_1];

#if DMVR
        dmvr.apply_DMVR = 0;
#endif 
        com_mc(x, y, cu_width, cu_height, cu_width, ctx->cand_buff[cand_idx], &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
            , &dmvr
#endif
#if BIO
            , -1, 0, mod_info_curr->mvr_idx
#endif
#if MVAP
            , 0
#endif
#if SUB_TMVP
            , 0
#endif
#if BGC
            , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
        );
        sad_whole_blk[cand_idx] = calc_sad_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], y_org_s, cu_width, bit_depth);  // sad 

        // calc sad of UMVE candidates

        for (u8 awp_mvr_idx = 0; awp_mvr_idx < AWP_MVR_MAX_REFINE_NUM; awp_mvr_idx++)
        {
            derive_awp_mvr_final_motion(awp_mvr_idx, pi->refp, awp_uni_refi[cand_idx], awp_mvr_offset);

#if AWP_ENH
            if (use_dawp && mod_info_curr->tpl_cur_avail[0] == 1 && mod_info_curr->tpl_cur_avail[1] == 1)
            {
                s16 tpl_mv_offset[2][MV_D] = { 0 };
                tpl_mv_offset[0][MV_X] = 0;
                tpl_mv_offset[0][MV_Y] = -(1 << 2);
                tpl_mv_offset[1][MV_X] = -(1 << 2);
                tpl_mv_offset[1][MV_Y] = 0;
                int dir;
                const int tpl_size = 1; 
                int tpl_height[2], tpl_width[2];
                tpl_width[0] = cu_width;
                tpl_height[0] = tpl_size;
                tpl_width[1] = tpl_size;
                tpl_height[1] = cu_height;
                int tpl_stride = 1;
                int offset[2] = { 0, AWP_TPL_SIZE };
                for (dir = 0; dir < 2; dir++)
                {
                    mod_info_curr->mv[REFP_0][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_X] + awp_mvr_offset[REFP_0][MV_X] + tpl_mv_offset[dir][MV_X]);
                    mod_info_curr->mv[REFP_0][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_Y] + awp_mvr_offset[REFP_0][MV_Y] + tpl_mv_offset[dir][MV_Y]);
                    mod_info_curr->mv[REFP_1][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_X] + awp_mvr_offset[REFP_1][MV_X] + tpl_mv_offset[dir][MV_X]);
                    mod_info_curr->mv[REFP_1][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_Y] + awp_mvr_offset[REFP_1][MV_Y] + tpl_mv_offset[dir][MV_Y]);
                    mod_info_curr->refi[REFP_0] = awp_uni_refi[cand_idx][REFP_0];
                    mod_info_curr->refi[REFP_1] = awp_uni_refi[cand_idx][REFP_1];
#if DMVR
                    dmvr.apply_DMVR = 0;
#endif 
                    com_mc(x, y, (tpl_width[dir] == 1 ? 4 : tpl_width[dir]), (tpl_height[dir] == 1 ? 4 : tpl_height[dir]), cu_width, ctx->candBuffUMVE[cand_idx][awp_mvr_idx], &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                        , &dmvr
#endif
#if BIO
                        , -1, 0, mod_info_curr->mvr_idx
#endif
#if MVAP
                        , 0
#endif
#if SUB_TMVP
                        , 0
#endif
#if BGC
                        , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
                    );
                    for (int i = 0; i < tpl_height[dir]; i++)
                    {
                        for (int j = 0; j < tpl_width[dir]; j++)
                        {
                            ctx->cand_buff_umve_tpl[cand_idx][awp_mvr_idx][0][i * tpl_stride + j + offset[dir]] = ctx->candBuffUMVE[cand_idx][awp_mvr_idx][Y_C][i * cu_width + j];
                        }
                    }

                }
            }
#endif

            mod_info_curr->mv[REFP_0][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_X] + awp_mvr_offset[REFP_0][MV_X]);
            mod_info_curr->mv[REFP_0][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_0][MV_Y] + awp_mvr_offset[REFP_0][MV_Y]);
            mod_info_curr->mv[REFP_1][MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_X] + awp_mvr_offset[REFP_1][MV_X]);
            mod_info_curr->mv[REFP_1][MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, awp_uni_cands[cand_idx][REFP_1][MV_Y] + awp_mvr_offset[REFP_1][MV_Y]);
            mod_info_curr->refi[REFP_0] = awp_uni_refi[cand_idx][REFP_0];
            mod_info_curr->refi[REFP_1] = awp_uni_refi[cand_idx][REFP_1];

            awp_mvr_uni_cands[cand_idx][awp_mvr_idx][REFP_0][MV_X] = mod_info_curr->mv[REFP_0][MV_X];
            awp_mvr_uni_cands[cand_idx][awp_mvr_idx][REFP_0][MV_Y] = mod_info_curr->mv[REFP_0][MV_Y];
            awp_mvr_uni_cands[cand_idx][awp_mvr_idx][REFP_1][MV_X] = mod_info_curr->mv[REFP_1][MV_X];
            awp_mvr_uni_cands[cand_idx][awp_mvr_idx][REFP_1][MV_Y] = mod_info_curr->mv[REFP_1][MV_Y];
            awp_mvr_uni_refi[cand_idx][awp_mvr_idx][REFP_0] = mod_info_curr->refi[REFP_0];
            awp_mvr_uni_refi[cand_idx][awp_mvr_idx][REFP_1] = mod_info_curr->refi[REFP_1];

#if DMVR
            dmvr.apply_DMVR = 0;
#endif 
            com_mc(x, y, cu_width, cu_height, cu_width, ctx->candBuffUMVE[cand_idx][awp_mvr_idx], &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , -1, 0, mod_info_curr->mvr_idx
#endif
#if MVAP
                , 0
#endif
#if SUB_TMVP
                , 0
#endif
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
            );

            sad_umve_whole_blk[cand_idx][awp_mvr_idx] = calc_sad_16b(cu_width, cu_height, y_org, ctx->candBuffUMVE[cand_idx][awp_mvr_idx][Y_C], y_org_s, cu_width, bit_depth);  // sad 
        }
    }

    /***************** get best combination for each partition ******************/

    u8 best_cand0_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    u8 best_cand1_idx[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand0_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand1_cost[AWP_MODE_NUM][CADIDATES_PER_PARTITION];

    u8 best_cand0_idx_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    u8 best_cand1_idx_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    u8 best_umve_idx0_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    u8 best_umve_idx1_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand0_cost_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];
    double best_cand1_cost_umve[AWP_MODE_NUM][CADIDATES_PER_PARTITION];

    for (u8 awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)  // 56
    {
        for (u8 candidate_idx = 0; candidate_idx < CADIDATES_PER_PARTITION; candidate_idx++)  // 2
        {
            best_cand0_idx[awp_idx][candidate_idx] = MAX_U8;
            best_cand1_idx[awp_idx][candidate_idx] = MAX_U8;
            best_cand0_cost[awp_idx][candidate_idx] = MAX_COST;
            best_cand1_cost[awp_idx][candidate_idx] = MAX_COST;

            best_cand0_idx_umve[awp_idx][candidate_idx] = MAX_U8;
            best_cand1_idx_umve[awp_idx][candidate_idx] = MAX_U8;
            best_umve_idx0_umve[awp_idx][candidate_idx] = MAX_U8;
            best_umve_idx1_umve[awp_idx][candidate_idx] = MAX_U8;
            best_cand0_cost_umve[awp_idx][candidate_idx] = MAX_COST;
            best_cand1_cost_umve[awp_idx][candidate_idx] = MAX_COST;
        }
    }

    u64 sad_small = 0;
    u64 sad_large = 0;
    double awp_cost0 = 0;
    double awp_cost1 = 0;

    for (u8 awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)  // 56
    {
        for (u8 cand_idx = 0; cand_idx < COM_MIN(AWP_MV_LIST_LENGTH, valid_cand_num); cand_idx++)  // 5
        {
            // non-UMVE candidates

            if (ctx->awp_larger_area[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_width_log2 - MIN_AWP_SIZE_LOG2][awp_idx] == 0)  // P0 dominate
            {
                // P1 sad
                sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);

                awp_cost1 = (double)sad_small + awp_cand_idx_cost[cand_idx];  // P1 cost
                sad_large = sad_whole_blk[cand_idx] - sad_small;             // P0 sad
                awp_cost0 = (double)sad_large + awp_cand_idx_cost[cand_idx];  // P0 cost
            }
            else  // P1 dominate
            {
                // P0 sad
                sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->cand_buff[cand_idx][Y_C], ctx->awp_bin_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);

                awp_cost0 = (double)sad_small + awp_cand_idx_cost[cand_idx];  // P0 cost
                sad_large = sad_whole_blk[cand_idx] - sad_small;             // P1 sad 
                awp_cost1 = (double)sad_large + awp_cand_idx_cost[cand_idx];  // P1 cost
            }

            sort_awp_partition_list(awp_idx, cand_idx, awp_cost0, best_cand0_idx, best_cand0_cost);
            sort_awp_partition_list(awp_idx, cand_idx, awp_cost1, best_cand1_idx, best_cand1_cost);
        }
    }

    _Bool include_awp_umve[AWP_MODE_NUM];
    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        memset(include_awp_umve, FALSE, AWP_MODE_NUM * sizeof(_Bool));

        u8     best_awp_modes[NUM_PARTITION_FOR_AWP_MVR_RD];
        double best_awp_mode_cost[NUM_PARTITION_FOR_AWP_MVR_RD];
        for (int i = 0; i < NUM_PARTITION_FOR_AWP_MVR_RD; i++)
        {
            best_awp_modes[i] = MAX_U8;
            best_awp_mode_cost[i] = MAX_COST;
        }
        for (u8 awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)  // 56
        {
            double temp_cost = best_cand0_cost[awp_idx][0] + best_cand1_cost[awp_idx][0];
            sort_awp_partition_sad_list(awp_idx, temp_cost, best_awp_modes, best_awp_mode_cost);
        }
        for (int i = 0; i < NUM_PARTITION_FOR_AWP_MVR_RD; i++)
        {
            include_awp_umve[best_awp_modes[i]] = TRUE;
        }
    }
    else
    {
        memset(include_awp_umve, FALSE, AWP_MODE_NUM * sizeof(_Bool));
        for (int it = 0; it < AWP_RDO_NUM; it++)
        {
            u8 awp_idx = core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].awp_mode_history[it];
            include_awp_umve[awp_idx] = TRUE;
        }
    }

    for (u8 awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)  // 56
    {
        _Bool test_awp_mode = include_awp_umve[awp_idx];

        if (!test_awp_mode)
        {
            continue;
        }

        for (u8 cand_idx = 0; cand_idx < COM_MIN(AWP_MV_LIST_LENGTH, valid_cand_num); cand_idx++)  // 5
        {
            // UMVE candidates

            for (u8 awp_mvr_idx = 0; awp_mvr_idx < AWP_MVR_MAX_REFINE_NUM; awp_mvr_idx++)  // 20
            {
                if (ctx->awp_larger_area[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_width_log2 - MIN_AWP_SIZE_LOG2][awp_idx] == 0)  // P0 dominate
                {
                    // P1 sad
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->candBuffUMVE[cand_idx][awp_mvr_idx][Y_C], ctx->awp_bin_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);

                    awp_cost1 = (double)sad_small + awp_umve_idx_cost[awp_mvr_idx];   // P1 cost
                    sad_large = sad_umve_whole_blk[cand_idx][awp_mvr_idx] - sad_small; // P0 sad  
                    awp_cost0 = (double)sad_large + awp_umve_idx_cost[awp_mvr_idx];   // P0 cost
                }
                else  // P1 dominate
                {
                    // P0 sad
                    sad_small = calc_sad_mask_16b(cu_width, cu_height, y_org, ctx->candBuffUMVE[cand_idx][awp_mvr_idx][Y_C], ctx->awp_bin_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], y_org_s, cu_width, cu_width, bit_depth);

                    awp_cost0 = (double)sad_small + awp_umve_idx_cost[awp_mvr_idx];   // P0 cost
                    sad_large = sad_umve_whole_blk[cand_idx][awp_mvr_idx] - sad_small; // P1 sad 
                    awp_cost1 = (double)sad_large + awp_umve_idx_cost[awp_mvr_idx];   // P1 cost
                }

                sort_awp_mvr_partition_list(awp_idx, cand_idx, awp_mvr_idx, awp_cost0, best_cand0_idx_umve, best_umve_idx0_umve, best_cand0_cost_umve);
                sort_awp_mvr_partition_list(awp_idx, cand_idx, awp_mvr_idx, awp_cost1, best_cand1_idx_umve, best_umve_idx1_umve, best_cand1_cost_umve);
            }
        }
    }

    /**************** use SATD to get best candidates for RDO ****************/
    double satd_cost_y = 0;
#if AWP_ENH
    double awp_best_cost[AWP_MODE_NUM];
    int awp_best_cost_idx[AWP_MODE_NUM];
    for (int i = 0; i < AWP_MODE_NUM; i++)
    {
        awp_best_cost[i] = MAX_COST;
        awp_best_cost_idx[i] = i;
    }
    int use_aawp = ctx->info.sqh.aawp_enable_flag && !mod_info_curr->ph_awp_refine_flag && ctx->info.pic_header.slice_type != SLICE_P ? 1 : 0;
    int best_satd_awp_idx[AWP_RDO_NUM];   // AWP mode
#else
    u8 best_satd_awp_idx[AWP_RDO_NUM];   // AWP mode
#endif
    u8 best_cand0[AWP_RDO_NUM];           // cand0Idx
    u8 best_cand1[AWP_RDO_NUM];           // cand1Idx
    u8 best_umve0_idx[AWP_RDO_NUM];           // umve0Idx
    u8 best_umve1_idx[AWP_RDO_NUM];           // umve1Idx
    double best_satd_cost[AWP_RDO_NUM];

    for (int i = 0; i < AWP_RDO_NUM; i++)
    {
        best_satd_awp_idx[i] = MAX_U8;
        best_cand0[i] = MAX_U8;
        best_cand1[i] = MAX_U8;
        best_umve0_idx[i] = MAX_U8;
        best_umve1_idx[i] = MAX_U8;
        best_satd_cost[i] = MAX_COST;
    }

    int awp_satd_num = 0;
    u8 temp_cand0, temp_cand1;
    u8 temp_umve_cand0, temp_umve_cand1;
    for (u8 awp_idx = 0; awp_idx < AWP_MODE_NUM; awp_idx++)
    {
        u8 numCandsPartition0 = include_awp_umve[awp_idx] ? (2 * CADIDATES_PER_PARTITION) : CADIDATES_PER_PARTITION;
        u8 numCandsPartition1 = include_awp_umve[awp_idx] ? (2 * CADIDATES_PER_PARTITION) : CADIDATES_PER_PARTITION;
        for (u8 cand_per_partition0 = 0; cand_per_partition0 < numCandsPartition0; cand_per_partition0++)
        {
            for (u8 cand_per_partition1 = 0; cand_per_partition1 < numCandsPartition1; cand_per_partition1++)
            {
                BOOL is_cand0_umve = (cand_per_partition0 >= CADIDATES_PER_PARTITION);
                BOOL is_cand1_umve = (cand_per_partition1 >= CADIDATES_PER_PARTITION);

                if (!is_cand0_umve)
                {
                    temp_cand0 = best_cand0_idx[awp_idx][cand_per_partition0];
                    temp_umve_cand0 = MAX_U8;
                }
                else
                {
                    temp_cand0 = best_cand0_idx_umve[awp_idx][cand_per_partition0 - CADIDATES_PER_PARTITION];
                    temp_umve_cand0 = best_umve_idx0_umve[awp_idx][cand_per_partition0 - CADIDATES_PER_PARTITION];
                }

                if (!is_cand1_umve)
                {
                    temp_cand1 = best_cand1_idx[awp_idx][cand_per_partition1];
                    temp_umve_cand1 = MAX_U8;
                }
                else
                {
                    temp_cand1 = best_cand1_idx_umve[awp_idx][cand_per_partition1 - CADIDATES_PER_PARTITION];
                    temp_umve_cand1 = best_umve_idx1_umve[awp_idx][cand_per_partition1 - CADIDATES_PER_PARTITION];
                }

                if (temp_cand0 == MAX_U8 || temp_cand1 == MAX_U8)
                {
                    continue;
                }

                if (!is_cand0_umve && !is_cand1_umve)
                {
                    if (temp_cand0 == temp_cand1)
                    {
                        continue;
                    }
                }

                if (is_cand0_umve && is_cand1_umve)
                {
                    if ((temp_cand0 == temp_cand1) && (temp_umve_cand0 == temp_umve_cand1))
                    {
                        continue;
                    }
                }

                awp_satd_num++;

                /* combine two pred buf */
                if (mod_info_curr->ph_awp_refine_flag)
                {
                    com_derive_awp_pred(mod_info_curr, Y_C,
                        (is_cand0_umve ? ctx->candBuffUMVE[temp_cand0][temp_umve_cand0] : ctx->cand_buff[temp_cand0]),
                        (is_cand1_umve ? ctx->candBuffUMVE[temp_cand1][temp_umve_cand1] : ctx->cand_buff[temp_cand1]),
                        ctx->awp_weight0_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                }
                else
                {
                    com_derive_awp_pred(mod_info_curr, Y_C,
                        (is_cand0_umve ? ctx->candBuffUMVE[temp_cand0][temp_umve_cand0] : ctx->cand_buff[temp_cand0]),
                        (is_cand1_umve ? ctx->candBuffUMVE[temp_cand1][temp_umve_cand1] : ctx->cand_buff[temp_cand1]),
                        ctx->awp_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                }
                satd_cost_y = (double)calc_satd_16b(cu_width, cu_height, y_org, y_pred, y_org_s, y_pred_s, bit_depth);

                int par_bit = 5;
                if (awp_idx > 7)
                {
                    par_bit++;
                }
                satd_cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], par_bit);

#if AWP_ENH
                if (use_aawp)
                {
                    satd_cost_y += awp_blend_idx_cost[0];
                }
#endif
                if ((is_cand0_umve == 0 && is_cand1_umve == 0) || (is_cand0_umve == 1 && is_cand1_umve == 1 && temp_umve_cand0 == temp_umve_cand1))
                {
                    satd_cost_y += awp_cand_idx_cost[temp_cand1];
                    satd_cost_y += awp_cand_idx_cost1[temp_cand0 < temp_cand1 ? temp_cand0 : temp_cand0 - 1];
                }
                else
                {
                    satd_cost_y += awp_cand_idx_cost[temp_cand0];
                    satd_cost_y += awp_cand_idx_cost[temp_cand1];
                }

                satd_cost_y += (is_cand0_umve ? awp_umve_flag[1] : awp_umve_flag[0]);
                if (is_cand0_umve)
                {
                    satd_cost_y += awp_umve_idx_cost[temp_umve_cand0];
                }

                satd_cost_y += (is_cand1_umve ? awp_umve_flag[1] : awp_umve_flag[0]);
                if (is_cand1_umve)
                {
                    satd_cost_y += awp_umve_idx_cost[temp_umve_cand1];
                }
                sort_awp_satd_list1(awp_idx, satd_cost_y, temp_cand0, temp_cand1, temp_umve_cand0, temp_umve_cand1, best_satd_awp_idx, best_satd_cost, best_cand0, best_cand1, best_umve0_idx, best_umve1_idx);

#if AWP_ENH
                if (satd_cost_y < awp_best_cost[awp_idx])
                {
                    awp_best_cost[awp_idx] = satd_cost_y;
                }
#endif
            }
        }
    }

#if AWP_ENH
    if (use_aawp)
    {
        const int total_awp_mode = TOTAL_BLEND_NUM;
        const int test_awp_mode_num = 8;
        sort_awp_mode(awp_best_cost, AWP_MODE_NUM, awp_best_cost_idx, test_awp_mode_num);
        for (int blend_idx = 1; blend_idx < total_awp_mode; blend_idx++)
        {
            for (int test_awp_idx = 0; test_awp_idx < test_awp_mode_num; test_awp_idx++)
            {
                int awp_idx = awp_best_cost_idx[test_awp_idx];
                u8 numCandsPartition0 = include_awp_umve[awp_idx] ? (2 * CADIDATES_PER_PARTITION) : CADIDATES_PER_PARTITION;
                u8 numCandsPartition1 = include_awp_umve[awp_idx] ? (2 * CADIDATES_PER_PARTITION) : CADIDATES_PER_PARTITION;
                for (u8 cand_per_partition0 = 0; cand_per_partition0 < numCandsPartition0; cand_per_partition0++)
                {
                    for (u8 cand_per_partition1 = 0; cand_per_partition1 < numCandsPartition1; cand_per_partition1++)
                    {
                        BOOL is_cand0_umve = (cand_per_partition0 >= CADIDATES_PER_PARTITION);
                        BOOL is_cand1_umve = (cand_per_partition1 >= CADIDATES_PER_PARTITION);
                        if (!is_cand0_umve)
                        {
                            temp_cand0 = best_cand0_idx[awp_idx][cand_per_partition0];
                            temp_umve_cand0 = MAX_U8;
                        }
                        else
                        {
                            temp_cand0 = best_cand0_idx_umve[awp_idx][cand_per_partition0 - CADIDATES_PER_PARTITION];
                            temp_umve_cand0 = best_umve_idx0_umve[awp_idx][cand_per_partition0 - CADIDATES_PER_PARTITION];
                        }
                        if (!is_cand1_umve)
                        {
                            temp_cand1 = best_cand1_idx[awp_idx][cand_per_partition1];
                            temp_umve_cand1 = MAX_U8;
                        }
                        else
                        {
                            temp_cand1 = best_cand1_idx_umve[awp_idx][cand_per_partition1 - CADIDATES_PER_PARTITION];
                            temp_umve_cand1 = best_umve_idx1_umve[awp_idx][cand_per_partition1 - CADIDATES_PER_PARTITION];
                        }
                        if (temp_cand0 == MAX_U8 || temp_cand1 == MAX_U8)
                        {
                            continue;
                        }
                        if (!is_cand0_umve && !is_cand1_umve)
                        {
                            if (temp_cand0 == temp_cand1)
                            {
                                continue;
                            }
                        }
                        if (is_cand0_umve && is_cand1_umve)
                        {
                            if ((temp_cand0 == temp_cand1) && (temp_umve_cand0 == temp_umve_cand1))
                            {
                                continue;
                            }
                        }
                        awp_satd_num++;
                        int blend_awp_idx = awp_idx + blend_idx * AWP_MODE_NUM;
                        /* combine two pred buf */
                        if (mod_info_curr->ph_awp_refine_flag)
                        {
                            com_derive_awp_pred(mod_info_curr, Y_C,
                                (is_cand0_umve ? ctx->candBuffUMVE[temp_cand0][temp_umve_cand0] : ctx->cand_buff[temp_cand0]),
                                (is_cand1_umve ? ctx->candBuffUMVE[temp_cand1][temp_umve_cand1] : ctx->cand_buff[temp_cand1]),
                                ctx->awp_weight0_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx], ctx->awp_weight1_scc[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][awp_idx]);
                        }
                        else
                        {
                            com_derive_awp_pred(mod_info_curr, Y_C,
                                (is_cand0_umve ? ctx->candBuffUMVE[temp_cand0][temp_umve_cand0] : ctx->cand_buff[temp_cand0]),
                                (is_cand1_umve ? ctx->candBuffUMVE[temp_cand1][temp_umve_cand1] : ctx->cand_buff[temp_cand1]),
                                ctx->awp_weight0[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][blend_awp_idx], ctx->awp_weight1[cu_width_log2 - MIN_AWP_SIZE_LOG2][cu_height_log2 - MIN_AWP_SIZE_LOG2][blend_awp_idx]);
                        }
                        satd_cost_y = (double)calc_satd_16b(cu_width, cu_height, y_org, y_pred, y_org_s, y_pred_s, bit_depth);
                        int par_bit = 5;
                        if (awp_idx > 7)
                        {
                            par_bit++;
                        }
                        satd_cost_y += RATE_TO_COST_SQRT_LAMBDA(ctx->sqrt_lambda[0], par_bit);
                        satd_cost_y += awp_blend_idx_cost[blend_idx];
                        if ((is_cand0_umve == 0 && is_cand1_umve == 0) || (is_cand0_umve == 1 && is_cand1_umve == 1 && temp_umve_cand0 == temp_umve_cand1))
                        {
                            satd_cost_y += awp_cand_idx_cost[temp_cand1];
                            satd_cost_y += awp_cand_idx_cost1[temp_cand0 < temp_cand1 ? temp_cand0 : temp_cand0 - 1];
                        }
                        else
                        {
                            satd_cost_y += awp_cand_idx_cost[temp_cand0];
                            satd_cost_y += awp_cand_idx_cost[temp_cand1];
                        }
                        satd_cost_y += (is_cand0_umve ? awp_umve_flag[1] : awp_umve_flag[0]);
                        if (is_cand0_umve)
                        {
                            satd_cost_y += awp_umve_idx_cost[temp_umve_cand0];
                        }
                        satd_cost_y += (is_cand1_umve ? awp_umve_flag[1] : awp_umve_flag[0]);
                        if (is_cand1_umve)
                        {
                            satd_cost_y += awp_umve_idx_cost[temp_umve_cand1];
                        }
                        sort_awp_satd_list1(blend_awp_idx, satd_cost_y, temp_cand0, temp_cand1, temp_umve_cand0, temp_umve_cand1, best_satd_awp_idx, best_satd_cost, best_cand0, best_cand1, best_umve0_idx, best_umve1_idx);
                    }
                }
            }
        }
    }
#endif

    /****************** RDO process for SATD seleted candidates ****************/
    s16 awp_final_mv0[REFP_NUM][MV_D], awp_final_mv1[REFP_NUM][MV_D];
    s8  awp_final_refi0[REFP_NUM], awp_final_refi1[REFP_NUM];

    double bestAWPRDCost[AWP_RDO_NUM];
    for (int i = 0; i < AWP_RDO_NUM; i++)
    {
        bestAWPRDCost[i] = MAX_COST;
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].awp_mode_history[i] = MAX_U8;
    }

#if AWP_ENH
    static u8 dawp_check[AWP_MVR_MAX_REFINE_NUM + 1][AWP_MVR_MAX_REFINE_NUM + 1][AWP_MV_LIST_LENGTH][AWP_MV_LIST_LENGTH] = { 0 };
    const int check_len = (AWP_MVR_MAX_REFINE_NUM + 1) * (AWP_MVR_MAX_REFINE_NUM + 1)* AWP_MV_LIST_LENGTH * AWP_MV_LIST_LENGTH;
    memset(dawp_check, 0, check_len * sizeof(u8));
    static int dawp_inv_mode_list[AWP_MVR_MAX_REFINE_NUM + 1][AWP_MVR_MAX_REFINE_NUM + 1][AWP_MV_LIST_LENGTH][AWP_MV_LIST_LENGTH][AWP_MODE_NUM] = { 0 };
#endif

    for (awp_rdo_idx = 0; awp_rdo_idx < COM_MIN(AWP_RDO_NUM, awp_satd_num); awp_rdo_idx++)  // 7 candidates
    {
        mod_info_curr->awp_flag = 1;
#if AWP_ENH
        int blend_awp_idx = best_satd_awp_idx[awp_rdo_idx];
        mod_info_curr->awp_blend_idx = blend_awp_idx / AWP_MODE_NUM;  // blend index
        mod_info_curr->skip_idx = blend_awp_idx % AWP_MODE_NUM;  // partition index
#else
        mod_info_curr->skip_idx = best_satd_awp_idx[awp_rdo_idx];  // partition index
#endif
        mod_info_curr->awp_idx0 = best_cand0[awp_rdo_idx];          // candIdx0 
        mod_info_curr->awp_idx1 = best_cand1[awp_rdo_idx];          // candIdx1
        mod_info_curr->awp_mvr_idx0 = best_umve0_idx[awp_rdo_idx];    // umveIdx0
        mod_info_curr->awp_mvr_idx1 = best_umve1_idx[awp_rdo_idx];    // umveIdx1
        mod_info_curr->awp_mvr_flag0 = (mod_info_curr->awp_mvr_idx0 == MAX_U8) ? 0 : 1;
        mod_info_curr->awp_mvr_flag1 = (mod_info_curr->awp_mvr_idx1 == MAX_U8) ? 0 : 1;

#if AWP_ENH
        int umve_idx0 = mod_info_curr->awp_mvr_flag0 ? (mod_info_curr->awp_mvr_idx0 + 1) : 0;
        int umve_idx1 = mod_info_curr->awp_mvr_flag1 ? (mod_info_curr->awp_mvr_idx1 + 1) : 0;
        int temp_cand0 = mod_info_curr->awp_idx0;
        int temp_cand1 = mod_info_curr->awp_idx1;
        int temp_umve_cand0 = mod_info_curr->awp_mvr_idx0;
        int temp_umve_cand1 = mod_info_curr->awp_mvr_idx1;
        int is_cand0_umve = mod_info_curr->awp_mvr_flag0;
        int is_cand1_umve = mod_info_curr->awp_mvr_flag1;
        int awp_idx = mod_info_curr->skip_idx;
        if (use_dawp)
        {
            if (dawp_check[umve_idx0][umve_idx1][temp_cand0][temp_cand1] == 0)
            {
                pel* pred_ref0 = is_cand0_umve ? ctx->cand_buff_umve_tpl[temp_cand0][temp_umve_cand0][Y_C] : ctx->cand_buff_tpl[temp_cand0][Y_C];
                pel* pred_ref1 = is_cand1_umve ? ctx->cand_buff_umve_tpl[temp_cand1][temp_umve_cand1][Y_C] : ctx->cand_buff_tpl[temp_cand1][Y_C];
                com_get_tpl_ref(mod_info_curr, pred_ref0, pred_ref1/*, weight_ref0, weight_ref1*/);

                int mode_list[AWP_MODE_NUM];

                int* inv_mode_list = &dawp_inv_mode_list[umve_idx0][umve_idx1][temp_cand0][temp_cand1][0];
                dawp_check[umve_idx0][umve_idx1][temp_cand0][temp_cand1] = 1;

                com_tpl_reorder_awp_mode(mod_info_curr, mode_list, inv_mode_list);

                int dawp_idx = inv_mode_list[awp_idx];
                if (dawp_idx >= 56)
                {
                    continue;
                }
                mod_info_curr->dawp_idx = dawp_idx;
            }
            else
            {
                int* inv_mode_list = &dawp_inv_mode_list[umve_idx0][umve_idx1][temp_cand0][temp_cand1][0];
                int dawp_idx = inv_mode_list[awp_idx];
                if (dawp_idx >= 56)
                {
                    continue;
                }
                mod_info_curr->dawp_idx = dawp_idx;
            }
        }
        else
        {
            mod_info_curr->dawp_idx = mod_info_curr->skip_idx;
        }
#endif

        if (!mod_info_curr->awp_mvr_flag0)
        {
            awp_final_mv0[REFP_0][MV_X] = awp_uni_cands[mod_info_curr->awp_idx0][REFP_0][MV_X];
            awp_final_mv0[REFP_0][MV_Y] = awp_uni_cands[mod_info_curr->awp_idx0][REFP_0][MV_Y];
            awp_final_mv0[REFP_1][MV_X] = awp_uni_cands[mod_info_curr->awp_idx0][REFP_1][MV_X];
            awp_final_mv0[REFP_1][MV_Y] = awp_uni_cands[mod_info_curr->awp_idx0][REFP_1][MV_Y];
            awp_final_refi0[REFP_0] = awp_uni_refi[mod_info_curr->awp_idx0][REFP_0];
            awp_final_refi0[REFP_1] = awp_uni_refi[mod_info_curr->awp_idx0][REFP_1];
        }
        else
        {
            awp_final_mv0[REFP_0][MV_X] = awp_mvr_uni_cands[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_0][MV_X];
            awp_final_mv0[REFP_0][MV_Y] = awp_mvr_uni_cands[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_0][MV_Y];
            awp_final_mv0[REFP_1][MV_X] = awp_mvr_uni_cands[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_1][MV_X];
            awp_final_mv0[REFP_1][MV_Y] = awp_mvr_uni_cands[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_1][MV_Y];
            awp_final_refi0[REFP_0] = awp_mvr_uni_refi[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_0];
            awp_final_refi0[REFP_1] = awp_mvr_uni_refi[mod_info_curr->awp_idx0][mod_info_curr->awp_mvr_idx0][REFP_1];
        }

        if (!mod_info_curr->awp_mvr_flag1)
        {
            awp_final_mv1[REFP_0][MV_X] = awp_uni_cands[mod_info_curr->awp_idx1][REFP_0][MV_X];
            awp_final_mv1[REFP_0][MV_Y] = awp_uni_cands[mod_info_curr->awp_idx1][REFP_0][MV_Y];
            awp_final_mv1[REFP_1][MV_X] = awp_uni_cands[mod_info_curr->awp_idx1][REFP_1][MV_X];
            awp_final_mv1[REFP_1][MV_Y] = awp_uni_cands[mod_info_curr->awp_idx1][REFP_1][MV_Y];
            awp_final_refi1[REFP_0] = awp_uni_refi[mod_info_curr->awp_idx1][REFP_0];
            awp_final_refi1[REFP_1] = awp_uni_refi[mod_info_curr->awp_idx1][REFP_1];
        }
        else
        {
            awp_final_mv1[REFP_0][MV_X] = awp_mvr_uni_cands[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_0][MV_X];
            awp_final_mv1[REFP_0][MV_Y] = awp_mvr_uni_cands[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_0][MV_Y];
            awp_final_mv1[REFP_1][MV_X] = awp_mvr_uni_cands[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_1][MV_X];
            awp_final_mv1[REFP_1][MV_Y] = awp_mvr_uni_cands[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_1][MV_Y];
            awp_final_refi1[REFP_0] = awp_mvr_uni_refi[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_0];
            awp_final_refi1[REFP_1] = awp_mvr_uni_refi[mod_info_curr->awp_idx1][mod_info_curr->awp_mvr_idx1][REFP_1];
        }
        com_set_awp_mvr_mv_para(mod_info_curr, awp_final_mv0, awp_final_refi0, awp_final_mv1, awp_final_refi1);

        mod_info_curr->cu_mode = MODE_DIR;
        double cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
            , 0
#endif
        );
        sort_awp_partition_rd_cost_list(mod_info_curr->skip_idx, cost, core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].awp_mode_history, bestAWPRDCost);
        check_best_mode(core, pi, cost, cost_best);
    }
    mod_info_curr->awp_flag = 0;
    mod_info_curr->awp_mvr_flag0 = 0;
    mod_info_curr->awp_mvr_flag1 = 0;
}
#endif

#if ETMVP
static void analyze_etmvp_merge(ENC_CTX *ctx, ENC_CORE *core, double *cost_best)
{
    COM_MODE   *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    COM_MOTION  first_stage_motion;
    s32 ref_block_x = 0;
    s32 ref_block_y = 0;
    s32 tmp_ref_block_x = 0;
    s32 tmp_ref_block_y = 0;
    s32 etmvp_idx = 0;
    s32 num_rdo = 0;
    s32 cu_width = 1 << mod_info_curr->cu_width_log2;
    s32 cu_height = 1 << mod_info_curr->cu_height_log2;

    s32 offset_x[MAX_ETMVP_NUM] = { 0, 8, -8, 0, 0 };
    s32 offset_y[MAX_ETMVP_NUM] = { 0, 0, 0, 8, -8 };

    s32 etmvp_cand_num = MAX_ETMVP_NUM;
    double cost = 0;
    s32 is_valid_etmvp = 0;
    s32 tmp_index = 0;
    s32 valid_etmvp_offset[MAX_ETMVP_NUM] = { 0 };
    COM_MOTION base_motion;

    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    init_inter_data(pi, core);
    
#if USE_SP
    derive_first_stage_motion(ctx->info.pic_header.ibc_flag || ctx->info.pic_header.sp_pic_flag || ctx->info.pic_header.evs_ubvs_pic_flag, mod_info_curr->scup, cu_width, cu_height, ctx->info.pic_width_in_scu, ctx->map.map_scu, ctx->map.map_mv, ctx->map.map_refi, &first_stage_motion);
#else
    derive_first_stage_motion(mod_info_curr->ibc_flag, mod_info_curr->scup, cu_width, cu_height, ctx->info.pic_width_in_scu, ctx->map.map_scu, ctx->map.map_mv, ctx->map.map_refi, &first_stage_motion);
#endif

    base_motion.mv[REFP_0][MV_X] = first_stage_motion.mv[REFP_0][MV_X];
    base_motion.mv[REFP_0][MV_Y] = first_stage_motion.mv[REFP_0][MV_Y];
    base_motion.ref_idx[REFP_0] = first_stage_motion.ref_idx[REFP_0];
    base_motion.mv[REFP_1][MV_X] = first_stage_motion.mv[REFP_1][MV_X];
    base_motion.mv[REFP_1][MV_Y] = first_stage_motion.mv[REFP_1][MV_Y];
    base_motion.ref_idx[REFP_1] = first_stage_motion.ref_idx[REFP_1];

    derive_scaled_base_motion(ctx->info.pic_header.slice_type, ctx->ptr, pi->refp, &base_motion, &first_stage_motion);
    derive_ref_block_position(ctx->info.pic_header.slice_type, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, core->x_pel, core->y_pel, mod_info_curr->x_pos, mod_info_curr->y_pos, ctx->ptr, pi->refp, &ref_block_x, &ref_block_y, ctx->info.max_cuwh);

    etmvp_cand_num = get_valid_etmvp_motion(ctx->info.pic_header.slice_type, ctx->ptr, cu_width, cu_height, core->x_pel, core->y_pel, ctx->info.pic_width, ctx->info.pic_height, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, ref_block_x, ref_block_y, pi->refp, valid_etmvp_offset, first_stage_motion, ctx->info.max_cuwh);

    mod_info_curr->umve_flag = 0;
    mod_info_curr->affine_flag = 0;
#if MVAP
    core->mvap_flag = 0;
#endif
#if INTERPF
    mod_info_curr->inter_filter_flag = 0;
#endif
#if IPC
    mod_info_curr->ipc_flag = 0;
#endif
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if EXT_AMVR_HMVP
    mod_info_curr->mvp_from_hmvp_flag = 0;
#endif
#if AWP
    mod_info_curr->awp_flag = 0;
#endif
#if INTER_TM
    mod_info_curr->tm_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if AFFINE_UMVE
    mod_info_curr->affine_umve_flag = 0;
#endif
    pi->curr_mvr = 0;
    mod_info_curr->etmvp_flag = 1;
    mod_info_curr->refi[REFP_0] = 0;
    if (ctx->info.pic_header.slice_type == SLICE_P)
    {
        mod_info_curr->refi[REFP_1] = REFI_INVALID;
    }
    else
    {
        mod_info_curr->refi[REFP_1] = 0;
    }

    for (etmvp_idx = 0; etmvp_idx < etmvp_cand_num; etmvp_idx++)
    {
        tmp_ref_block_x = ref_block_x + offset_x[valid_etmvp_offset[etmvp_idx]];
        tmp_ref_block_y = ref_block_y + offset_y[valid_etmvp_offset[etmvp_idx]];
        mod_info_curr->skip_idx = etmvp_idx;

        set_etmvp_mvfield(ctx->info.pic_header.slice_type, ctx->ptr, tmp_ref_block_x, tmp_ref_block_y, cu_width, cu_height, ctx->info.pic_width, ctx->info.pic_height, core->x_pel, core->y_pel, ctx->info.pic_width_in_scu, ctx->info.pic_height_in_scu, pi->refp, core->tmp_etmvp_mvfield, first_stage_motion, ctx->info.max_cuwh);

        mod_info_curr->cu_mode = MODE_DIR;

        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
            , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);
    }
    mod_info_curr->etmvp_flag = 0;
}
#endif

#if INTER_ME_MVLIB
void resetUniMvList(ENC_CORE *core, BOOL isLowdelay)
{
    for (int i = 0; i < MAX_NUM_MVR; i++)
    {
        core->s_enc_me_mvlib[i].uni_mv_list_idx = 0;
        core->s_enc_me_mvlib[i].uni_mv_list_size = 0;
        if (!isLowdelay)
        {
            core->s_enc_me_mvlib[i].uni_mv_list_max_size = 64;
        }
        else
        {
            core->s_enc_me_mvlib[i].uni_mv_list_max_size = 16;
        }
    }
}

void insertUniMvCands(ENC_ME_MVLIB *mvlib, int x, int y, int w, int h, s16 mvTemp[REFP_NUM][MAX_NUM_ACTIVE_REF_FRAME][MV_D])
{
    ENC_ME_MVLIB *core = mvlib;
    BLK_UNI_MV_INFO* curMvInfo = core->uni_mv_list + core->uni_mv_list_idx;
    int j = 0;
    for (; j < core->uni_mv_list_size; j++)
    {
        BLK_UNI_MV_INFO* prevMvInfo = core->uni_mv_list + ((core->uni_mv_list_idx - 1 - j + core->uni_mv_list_max_size) % (core->uni_mv_list_max_size));
        if ((x == prevMvInfo->x) && (y == prevMvInfo->y) && (w == prevMvInfo->w) && (h == prevMvInfo->h))
        {
            break;
        }
    }

    if (j < core->uni_mv_list_size)
    {
        curMvInfo = core->uni_mv_list + ((core->uni_mv_list_idx - 1 - j + core->uni_mv_list_max_size) % (core->uni_mv_list_max_size));
    }

    memcpy(curMvInfo->uniMvs, mvTemp, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D * sizeof(s16));
    if (j == core->uni_mv_list_size)  // new element
    {
        curMvInfo->x = x;
        curMvInfo->y = y;
        curMvInfo->w = w;
        curMvInfo->h = h;
        core->uni_mv_list_size = min(core->uni_mv_list_size + 1, core->uni_mv_list_max_size);
        core->uni_mv_list_idx = (core->uni_mv_list_idx + 1) % (core->uni_mv_list_max_size);
    }
}

void savePrevUniMvInfo(ENC_ME_MVLIB *mvlib, int x, int y, int w, int h, BLK_UNI_MV_INFO *tmpUniMvInfo, BOOL *isUniMvInfoSaved)
{
    ENC_ME_MVLIB *core = mvlib;
    int j = 0;
    BLK_UNI_MV_INFO* curUniMvInfo = NULL;
    for (; j < core->uni_mv_list_size; j++)
    {
        curUniMvInfo = core->uni_mv_list + ((core->uni_mv_list_idx - 1 - j + core->uni_mv_list_max_size) % (core->uni_mv_list_max_size));
        if ((x == curUniMvInfo->x) && (y == curUniMvInfo->y) && (w == curUniMvInfo->w) && (h == curUniMvInfo->h))
        {
            break;
        }
    }

    if (j < core->uni_mv_list_size)
    {
        *isUniMvInfoSaved = TRUE;
        *tmpUniMvInfo = *curUniMvInfo;
    }
}

void addUniMvInfo(ENC_ME_MVLIB *mvlib, BLK_UNI_MV_INFO *tmpUniMVInfo)
{
    ENC_ME_MVLIB *core = mvlib;
    int j = 0;
    BLK_UNI_MV_INFO* prevUniMvInfo = NULL;
    for (; j < core->uni_mv_list_size; j++)
    {
        prevUniMvInfo = core->uni_mv_list + ((core->uni_mv_list_idx - 1 - j + core->uni_mv_list_max_size) % (core->uni_mv_list_max_size));
        if ((tmpUniMVInfo->x == prevUniMvInfo->x) && (tmpUniMVInfo->y == prevUniMvInfo->y) && (tmpUniMVInfo->w == prevUniMvInfo->w) && (tmpUniMVInfo->h == prevUniMvInfo->h))
        {
            break;
        }
    }
    if (j < core->uni_mv_list_size)
    {
        *prevUniMvInfo = *tmpUniMVInfo;
    }
    else
    {
        core->uni_mv_list[core->uni_mv_list_idx] = *tmpUniMVInfo;
        core->uni_mv_list_idx = (core->uni_mv_list_idx + 1) % core->uni_mv_list_max_size;
        core->uni_mv_list_size = min(core->uni_mv_list_size + 1, core->uni_mv_list_max_size);
    }
}
#endif

#if ENC_ME_IMP
void resetAffUniMvList(ENC_CORE *core, BOOL isLD)
{
    core->affMVListIdx = 0;
    core->affMVListSize = 0;
    core->affMVListMaxSize = isLD ? 4 : 8;
}

void insertAffUniMvCands(ENC_CORE *core, int x, int y, int w, int h, CPMV affMvTemp[REFP_NUM][MAX_NUM_ACTIVE_REF_FRAME][VER_NUM][MV_D])
{
    if (!core->affMVListMaxSize)
    {
        return;
    }

    AffineMvInfo* curAffUniMvInfo = core->affMVList + core->affMVListIdx;
    int j = 0;
    for (; j < core->affMVListSize; j++)
    {
        AffineMvInfo* prevAffUniMvInfo = core->affMVList + ((core->affMVListIdx - 1 - j + core->affMVListMaxSize) % (core->affMVListMaxSize));
        if ((x == prevAffUniMvInfo->x) && (y == prevAffUniMvInfo->y) && (w == prevAffUniMvInfo->w) && (h == prevAffUniMvInfo->h))
        {
            break;
        }
    }

    if (j < core->affMVListSize)
    {
        curAffUniMvInfo = core->affMVList + ((core->affMVListIdx - 1 - j + core->affMVListMaxSize) % (core->affMVListMaxSize));
    }

    memcpy(curAffUniMvInfo->affMVs, affMvTemp, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * VER_NUM * MV_D * sizeof(CPMV));
    if (j == core->affMVListSize)  // new element
    {
        curAffUniMvInfo->x = x;
        curAffUniMvInfo->y = y;
        curAffUniMvInfo->w = w;
        curAffUniMvInfo->h = h;
        core->affMVListSize = min(core->affMVListSize + 1, core->affMVListMaxSize);
        core->affMVListIdx = (core->affMVListIdx + 1) % (core->affMVListMaxSize);
    }
}

void savePrevAffUniMvInfo(ENC_CORE *core, int x, int y, int w, int h, AffineMvInfo *tmpAffUniMvInfo, BOOL *isAffUniMvInfoSaved)
{
    int j = 0;
    AffineMvInfo* curAffUniMvInfo = NULL;
    for (; j < core->affMVListSize; j++)
    {
        curAffUniMvInfo = core->affMVList + ((core->affMVListIdx - 1 - j + core->affMVListMaxSize) % (core->affMVListMaxSize));
        if ((x == curAffUniMvInfo->x) && (y == curAffUniMvInfo->y) && (w == curAffUniMvInfo->w) && (h == curAffUniMvInfo->h))
        {
            break;
        }
    }

    if (j < core->affMVListSize)
    {
        *isAffUniMvInfoSaved = TRUE;
        *tmpAffUniMvInfo = *curAffUniMvInfo;
    }
}

void addAffUniMvInfo(ENC_CORE *core, AffineMvInfo *tmpAffUniMvInfo)
{
    if (!core->affMVListMaxSize)
    {
        return;
    }

    int j = 0;
    AffineMvInfo *prevAffUniMvInfo = NULL;
    for (; j < core->affMVListSize; j++)
    {
        prevAffUniMvInfo = core->affMVList + ((core->affMVListIdx - j - 1 + core->affMVListMaxSize) % (core->affMVListMaxSize));
        if ((tmpAffUniMvInfo->x == prevAffUniMvInfo->x) && (tmpAffUniMvInfo->y == prevAffUniMvInfo->y) && (tmpAffUniMvInfo->w == prevAffUniMvInfo->w) && (tmpAffUniMvInfo->h == prevAffUniMvInfo->h))
        {
            break;
        }
    }
    if (j < core->affMVListSize)
    {
        *prevAffUniMvInfo = *tmpAffUniMvInfo;
    }
    else
    {
        core->affMVList[core->affMVListIdx] = *tmpAffUniMvInfo;
        core->affMVListIdx = (core->affMVListIdx + 1) % core->affMVListMaxSize;
        core->affMVListSize = min(core->affMVListSize + 1, core->affMVListMaxSize);
    }
}
#endif

#if FAST_EXT_AMVR_HMVP
static void derive_mvp_info(ENC_CTX *ctx, ENC_CORE *core, int lidx, int refi)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;

    s16 x = mod_info_curr->x_pos;
    s16 y = mod_info_curr->y_pos;

    pel *ref;
    COM_PIC *refpic = pi->refp[refi][lidx].pic;
    pel *org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
    int bit_depth = pi->bit_depth;

    s16* mvp;
    s16 mvc[MV_D];
    u32 cost_best = COM_UINT32_MAX;

    for (int mvp_from_hmvp_flag = 0; mvp_from_hmvp_flag < 2; mvp_from_hmvp_flag++)
    {
        mod_info_curr->mvp_from_hmvp_flag = mvp_from_hmvp_flag;
        mvp = pi->mvps_uni[mvp_from_hmvp_flag][lidx][refi];

        com_derive_mvp(ctx->info, mod_info_curr, ctx->ptr, lidx, refi, core->cnt_hmvp_cands,
            core->motion_cands, ctx->map, ctx->refp, pi->curr_mvr, mvp);

        mvc[MV_X] = (s16)x + (mvp[MV_X] >> 2);
        mvc[MV_Y] = (s16)y + (mvp[MV_Y] >> 2);
        mvc[MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mvc[MV_X]);
        mvc[MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mvc[MV_Y]);

        if (pi->curr_mvr > 2)
        {
            com_mv_rounding_s16(mvc[MV_X], mvc[MV_Y], &mvc[MV_X], &mvc[MV_Y], pi->curr_mvr - 2, pi->curr_mvr - 2);
        }

        ref = refpic->y + mvc[MV_X] + mvc[MV_Y] * refpic->stride_luma;
        
        u32 cost = calc_sad_16b(cu_width, cu_height, org, ref, pi->stride_org[Y_C], refpic->stride_luma, pi->bit_depth);

        if (cost < cost_best)
        {
            cost_best = cost;

            pi->mvp_scale[lidx][refi][MV_X] = mvp[MV_X];
            pi->mvp_scale[lidx][refi][MV_Y] = mvp[MV_Y];
            pi->mvp_flag[lidx][refi] = mvp_from_hmvp_flag;
        }
    }

    pi->mvp_from_hmvp_flag = pi->mvp_flag[lidx][refi];
    mod_info_curr->mvp_from_hmvp_flag = pi->mvp_flag[lidx][refi];
}

static void check_best_uni_mvp(ENC_CTX *ctx, ENC_CORE *core, int lidx, int refi, s16 mv[MV_D], u32 *cost)
{
    if (!ctx->info.sqh.emvr_enable_flag)
        return;

    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    u8 emvr_flag = mod_info_curr->mvp_from_hmvp_flag;

    s16 *mvp[2];
    int mv_bits[2];

    mvp[0] = pi->mvps_uni[0][lidx][refi];
    mvp[1] = pi->mvps_uni[1][lidx][refi];
    
    mv_bits[0] = get_mv_bits_with_mvr(mv[MV_X] - mvp[0][MV_X], mv[MV_Y] - mvp[0][MV_Y], pi->num_refp, refi, pi->curr_mvr);
    mv_bits[1] = get_mv_bits_with_mvr(mv[MV_X] - mvp[1][MV_X], mv[MV_Y] - mvp[1][MV_Y], pi->num_refp, refi, pi->curr_mvr);

    if (mv_bits[emvr_flag] > mv_bits[1 - emvr_flag])
    {
        pi->mvp_scale[lidx][refi][MV_X] = pi->mvps_uni[1 - emvr_flag][lidx][refi][MV_X];
        pi->mvp_scale[lidx][refi][MV_Y] = pi->mvps_uni[1 - emvr_flag][lidx][refi][MV_Y];
        pi->mvp_flag[lidx][refi] = 1 - emvr_flag;

        *cost = *cost - MV_COST(pi, mv_bits[emvr_flag]) + MV_COST(pi, mv_bits[1 - emvr_flag]);
    }

    s16 x = mod_info_curr->x_pos;
    s16 y = mod_info_curr->y_pos;

    pel *ref;
    COM_PIC *refpic = pi->refp[refi][lidx].pic;
    pel *org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;
    int bit_depth = pi->bit_depth;

    u32 cost_temp;
    s16 mv_temp[MV_D];

    s16 gmvp[2][MV_D];
    gmvp[0][MV_X] = mvp[0][MV_X] + ((s16)x << 2);
    gmvp[0][MV_Y] = mvp[0][MV_Y] + ((s16)y << 2);
    gmvp[1][MV_X] = mvp[1][MV_X] + ((s16)x << 2);
    gmvp[1][MV_Y] = mvp[1][MV_Y] + ((s16)y << 2);

    for (int i = 0; i < pi->mv_cands_uni_size[lidx][refi]; i++)
    {
        mv_temp[MV_X] = pi->mv_cands_uni[lidx][refi][i][MV_X];
        mv_temp[MV_Y] = pi->mv_cands_uni[lidx][refi][i][MV_Y];
        mv_temp[MV_X] = COM_CLIP3(pi->min_mv_offset[MV_X], pi->max_mv_offset[MV_X], mv_temp[MV_X]);
        mv_temp[MV_Y] = COM_CLIP3(pi->min_mv_offset[MV_Y], pi->max_mv_offset[MV_Y], mv_temp[MV_Y]);

        if (pi->curr_mvr > 2)
        {
            com_mv_rounding_s16(mv_temp[MV_X], mv_temp[MV_Y], &mv_temp[MV_X], &mv_temp[MV_Y], pi->curr_mvr - 2, pi->curr_mvr - 2);
        }

        mv_bits[0] = get_mv_bits_with_mvr((mv_temp[MV_X] << 2) - gmvp[0][MV_X], (mv_temp[MV_Y] << 2) - gmvp[0][MV_Y], pi->num_refp, refi, pi->curr_mvr);
        mv_bits[1] = get_mv_bits_with_mvr((mv_temp[MV_X] << 2) - gmvp[1][MV_X], (mv_temp[MV_Y] << 2) - gmvp[1][MV_Y], pi->num_refp, refi, pi->curr_mvr);

        emvr_flag = (mv_bits[0] > mv_bits[1]) ? 1 : 0;
        mv_bits[emvr_flag] += 2;
        cost_temp = MV_COST(pi, mv_bits[emvr_flag]);

        ref = refpic->y + mv_temp[MV_X] + mv_temp[MV_Y] * refpic->stride_luma;

        if (pi->me_level > ME_LEV_IPEL && (pi->curr_mvr == 0 || pi->curr_mvr == 1))
            cost_temp += calc_satd_16b(cu_width, cu_height, org, ref, pi->stride_org[Y_C], refpic->stride_luma, pi->bit_depth);
        else
            cost_temp += calc_sad_16b(cu_width, cu_height, org, ref, pi->stride_org[Y_C], refpic->stride_luma, pi->bit_depth);

        if (cost_temp < *cost)
        {
            *cost = cost_temp;

            mv[MV_X] = (mv_temp[MV_X] - (s16)x) << 2;
            mv[MV_Y] = (mv_temp[MV_Y] - (s16)y) << 2;
            pi->mvp_scale[lidx][refi][MV_X] = mvp[emvr_flag][MV_X];
            pi->mvp_scale[lidx][refi][MV_Y] = mvp[emvr_flag][MV_Y];
            pi->mvp_flag[lidx][refi] = emvr_flag;
        }
    }
}

static void check_best_bi_mvp(ENC_CTX *ctx, ENC_CORE *core)
{
    if (!ctx->info.sqh.emvr_enable_flag)
        return;

    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;

    int mv_bits;
    int mv_bits_best = COM_INT_MAX;

    for (int mvp_from_hmvp_flag = 0; mvp_from_hmvp_flag < 2; mvp_from_hmvp_flag++)
    {
        s16 *mv = mod_info_curr->mv[REFP_0];
        s16 *mvp = pi->mvps_uni[mvp_from_hmvp_flag][REFP_0][mod_info_curr->refi[REFP_0]];
        mv_bits = get_mv_bits_with_mvr(mv[MV_X] - mvp[MV_X], mv[MV_Y] - mvp[MV_Y], pi->num_refp, mod_info_curr->refi[REFP_0], pi->curr_mvr);

        mv = mod_info_curr->mv[REFP_1];
        mvp = pi->mvps_uni[mvp_from_hmvp_flag][REFP_1][mod_info_curr->refi[REFP_1]];
        mv_bits += get_mv_bits_with_mvr(mv[MV_X] - mvp[MV_X], mv[MV_Y] - mvp[MV_Y], pi->num_refp, mod_info_curr->refi[REFP_1], pi->curr_mvr);

        if (mv_bits < mv_bits_best)
        {
            mv_bits_best = mv_bits;

            pi->mvp_from_hmvp_flag = mvp_from_hmvp_flag;
            mod_info_curr->mvp_from_hmvp_flag = mvp_from_hmvp_flag;
            memcpy(pi->mvp_scale, pi->mvps_uni[mvp_from_hmvp_flag], REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D * sizeof(s16));
        }
    }
}
#endif

static void analyze_uni_pred(ENC_CTX *ctx, ENC_CORE *core, double *cost_L0L1, s16 mv_L0L1[REFP_NUM][MV_D], s8 *refi_L0L1, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int lidx;
    s16 *mvp, *mv, *mvd;
#if FAST_LD
    u32 me_cost_l0[MAX_NUM_ACTIVE_REF_FRAME];      // Store temp L0 ME value
    int satd_decision[MAX_NUM_ACTIVE_REF_FRAME];  // Store temp LO and L1 SATD decision
#endif
    u32 mecost, best_mecost;
    s8 refi_cur = 0;
    s8 best_refi = 0;
    s8 t0, t1;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;

    mod_info_curr->cu_mode = MODE_INTER;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
    mod_info_curr->cs2_flag = 0;
#if RSD_OPT 
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
#if FAST_EXT_AMVR_HMVP
    mod_info_curr->mvp_from_hmvp_flag = 0;
    pi->mvp_from_hmvp_flag = 0;
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
    for (lidx = 0; lidx <= ((pi->slice_type == SLICE_P) ? PRED_L0 : PRED_L1); lidx++) // uni-prediction (L0 or L1)
    {
        init_inter_data(pi, core);
        mv = mod_info_curr->mv[lidx];
        mvd = mod_info_curr->mvd[lidx];
        pi->num_refp = (u8)ctx->rpm.num_refp[lidx];
        best_mecost = COM_UINT32_MAX;
        for (refi_cur = 0; refi_cur < pi->num_refp; refi_cur++)
        {
            mvp = pi->mvp_scale[lidx][refi_cur];

#if FAST_EXT_AMVR_HMVP
            if (ctx->info.sqh.emvr_enable_flag)
            {
                derive_mvp_info(ctx, core, lidx, refi_cur);
            }
            else
#endif
            com_derive_mvp(ctx->info, mod_info_curr, ctx->ptr, lidx, refi_cur, core->cnt_hmvp_cands,
                core->motion_cands, ctx->map, ctx->refp, pi->curr_mvr, mvp);

            // motion search
#if FAST_LD
            if (ctx->param.fast_ld_me && (lidx == PRED_L1 && ctx->info.pic_header.l1idx_to_l0idx[refi_cur] >= 0))
            {
                mv[MV_X] = pi->mv_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][MV_X];
                mv[MV_Y] = pi->mv_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][MV_Y];

                mecost = me_cost_l0[ctx->info.pic_header.l1idx_to_l0idx[refi_cur]];
                // refine mvd cost
                int mv_bits1 = get_mv_bits_with_mvr(mv[MV_X] - pi->mvp_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][MV_X],
                    mv[MV_Y] - pi->mvp_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][MV_Y],
                    ctx->rpm.num_refp[REFP_0], ctx->info.pic_header.l1idx_to_l0idx[refi_cur], pi->curr_mvr);

                mecost -= MV_COST(pi, mv_bits1);

                int mv_bits2 = get_mv_bits_with_mvr(mv[MV_X] - pi->mvp_scale[lidx][refi_cur][MV_X],
                    mv[MV_Y] - pi->mvp_scale[lidx][refi_cur][MV_Y],
                    pi->num_refp, refi_cur, pi->curr_mvr);

                mecost += MV_COST(pi, mv_bits2);

                if (mv_bits2 < mv_bits1)
                {
                    satd_decision[refi_cur] = 1;
                }
                else
                {
                    satd_decision[refi_cur] = 0;
                }
            }
            else
            {
#if INTER_ME_MVLIB
#if ENC_ME_IMP
                mecost = pi->fn_me(ctx, pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#else
                mecost = pi->fn_me(pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#endif
#else
#if ENC_ME_IMP
                mecost = pi->fn_me(ctx, pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#else
                mecost = pi->fn_me(pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#endif
#endif
#if FAST_EXT_AMVR_HMVP
                check_best_uni_mvp(ctx, core, lidx, refi_cur, mv, &mecost);
#endif
                if (ctx->param.fast_ld_me && lidx == PRED_L0)
                {
                    me_cost_l0[refi_cur] = mecost;
                }
            }
#else
#if INTER_ME_MVLIB
#if ENC_ME_IMP
            mecost = pi->fn_me(ctx, pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#else
            mecost = pi->fn_me(pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#endif
#else
#if ENC_ME_IMP
            mecost = pi->fn_me(ctx, pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#else
            mecost = pi->fn_me(pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx, mvp, mv, 0);
#endif
#endif
#endif
            pi->mv_scale[lidx][refi_cur][MV_X] = mv[MV_X];
            pi->mv_scale[lidx][refi_cur][MV_Y] = mv[MV_Y];
            if (mecost < best_mecost)
            {
                best_mecost = mecost;
                best_refi = refi_cur;
            }

#if BD_AFFINE_AMVR
            if (pi->curr_mvr < MAX_NUM_AFFINE_MVR)
#else
            if (pi->curr_mvr == 0)
#endif
            {
                pi->best_mv_uni[lidx][refi_cur][MV_X] = mv[MV_X];
                pi->best_mv_uni[lidx][refi_cur][MV_Y] = mv[MV_Y];
            }
        }
        mv[MV_X] = pi->mv_scale[lidx][best_refi][MV_X];
        mv[MV_Y] = pi->mv_scale[lidx][best_refi][MV_Y];
        mvp = pi->mvp_scale[lidx][best_refi];
#if FAST_EXT_AMVR_HMVP
        pi->mvp_from_hmvp_flag = pi->mvp_flag[lidx][best_refi];
        mod_info_curr->mvp_from_hmvp_flag = pi->mvp_flag[lidx][best_refi];
#endif
        t0 = (lidx == 0) ? best_refi : REFI_INVALID;
        t1 = (lidx == 1) ? best_refi : REFI_INVALID;
        SET_REFI(mod_info_curr->refi, t0, t1);
        refi_L0L1[lidx] = best_refi;

        mv[MV_X] = (mv[MV_X] >> pi->curr_mvr) << pi->curr_mvr;
        mv[MV_Y] = (mv[MV_Y] >> pi->curr_mvr) << pi->curr_mvr;

        mvd[MV_X] = mv[MV_X] - mvp[MV_X];
        mvd[MV_Y] = mv[MV_Y] - mvp[MV_Y];

        /* important: reset mv/mvd */
        {
            // note: reset mvd, after clipping, mvd might not align with amvr index
            int amvr_shift = pi->curr_mvr;
            mvd[MV_X] = mvd[MV_X] >> amvr_shift << amvr_shift;
            mvd[MV_Y] = mvd[MV_Y] >> amvr_shift << amvr_shift;

            // note: reset mv, after clipping, mv might not equal to mvp + mvd
            int mv_x = (s32)mvd[MV_X] + mvp[MV_X];
            int mv_y = (s32)mvd[MV_Y] + mvp[MV_Y];
            mv[MV_X] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_x);
            mv[MV_Y] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_y);
        }

        mv_L0L1[lidx][MV_X] = mv[MV_X];
        mv_L0L1[lidx][MV_Y] = mv[MV_Y];

        // get mot_bits for bi search: mvd + refi
        pi->mot_bits[lidx] = get_mv_bits_with_mvr(mvd[MV_X], mvd[MV_Y], pi->num_refp, best_refi, pi->curr_mvr);
        pi->mot_bits[lidx] -= (pi->curr_mvr == MAX_NUM_MVR - 1) ? pi->curr_mvr : (pi->curr_mvr + 1); // minus amvr index
#if INTERPF
        core->mod_info_curr.inter_filter_flag = 0;
#endif
#if IPC
        core->mod_info_curr.ipc_flag = 0;
#endif

#if FAST_LD
        if (ctx->param.fast_ld_me && lidx == PRED_L1 && refi_L0L1[REFP_0] == ctx->info.pic_header.l1idx_to_l0idx[refi_L0L1[REFP_1]] && satd_decision[refi_L0L1[REFP_1]] == 0)
        {
            continue;
        }
        else
        {
            cost_L0L1[lidx] = pinter_residue_rdo(ctx, core, 0
#if DMVR
                , 0
#endif
            );

            check_best_mode(core, pi, cost_L0L1[lidx], cost_best);
        }
#else
        cost_L0L1[lidx] = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                             , 0
#endif
        );
        check_best_mode(core, pi, cost_L0L1[lidx], cost_best);
#endif
    }
#if FAST_EXT_AMVR_HMVP
    if (ctx->info.sqh.emvr_enable_flag)
    {
        pi->mvp_from_hmvp_flag = core->mod_info_best.mvp_from_hmvp_flag;
        mod_info_curr->mvp_from_hmvp_flag = core->mod_info_best.mvp_from_hmvp_flag;
        memcpy(pi->mvp_scale, pi->mvps_uni[core->mod_info_best.mvp_from_hmvp_flag], REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D * sizeof(s16));
    }
#endif
#if INTER_ME_MVLIB
    if (pi->mvp_from_hmvp_flag == 0)
    {
        ENC_ME_MVLIB *mvlib = &core->s_enc_me_mvlib[pi->curr_mvr];
        insertUniMvCands(mvlib, x, y, cu_width, cu_height, pi->mv_scale);
        memcpy(mvlib->uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].reused_uni_mv, pi->mv_scale, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D * sizeof(s16));
        mvlib->uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit = 1;
    }
#endif
}

static void analyze_bi(ENC_CTX *ctx, ENC_CORE *core, s16 mv_L0L1[REFP_NUM][MV_D], const s8 *refi_L0L1, const double *cost_L0L1, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int bit_depth = ctx->info.bit_depth_internal;
    s8         refi[REFP_NUM] = { REFI_INVALID, REFI_INVALID };
    u32        best_mecost = COM_UINT32_MAX;
    s8        refi_best = 0, refi_cur;
    int        changed = 0;
    u32        mecost;
    pel        *org;
    pel(*pred)[MAX_CU_DIM];
    s8         t0, t1;
    double      cost;
    s8         lidx_ref, lidx_cnd;
    int         i;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    mod_info_curr->cu_mode = MODE_INTER;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
    mod_info_curr->cs2_flag = 0;
#if RSD_OPT
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
#if DMVR
    COM_DMVR dmvr;
#endif

    init_inter_data(pi, core);
    if (cost_L0L1[PRED_L0] <= cost_L0L1[PRED_L1])
    {
        lidx_ref = REFP_0;
        lidx_cnd = REFP_1;
    }
    else
    {
        lidx_ref = REFP_1;
        lidx_cnd = REFP_0;
    }
    mod_info_curr->refi[REFP_0] = refi_L0L1[REFP_0];
    mod_info_curr->refi[REFP_1] = refi_L0L1[REFP_1];
    t0 = (lidx_ref == REFP_0) ? refi_L0L1[lidx_ref] : REFI_INVALID;
    t1 = (lidx_ref == REFP_1) ? refi_L0L1[lidx_ref] : REFI_INVALID;
    SET_REFI(refi, t0, t1);
    mod_info_curr->mv[lidx_ref][MV_X] = mv_L0L1[lidx_ref][MV_X];
    mod_info_curr->mv[lidx_ref][MV_Y] = mv_L0L1[lidx_ref][MV_Y];
    mod_info_curr->mv[lidx_cnd][MV_X] = mv_L0L1[lidx_cnd][MV_X];
    mod_info_curr->mv[lidx_cnd][MV_Y] = mv_L0L1[lidx_cnd][MV_Y];
    /* get MVP lidx_cnd */
    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    pred = mod_info_curr->pred;
    for (i = 0; i < BI_ITER; i++)
    {
        /* predict reference */
        s8 temp_refi[REFP_NUM];
        temp_refi[REFP_0] = mod_info_curr->refi[REFP_0];
        temp_refi[REFP_1] = mod_info_curr->refi[REFP_1];
        mod_info_curr->refi[REFP_0] = refi[REFP_0];
        mod_info_curr->refi[REFP_1] = refi[REFP_1];

#if DMVR
        dmvr.poc_c = ctx->ptr; 
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = 0;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif

        com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 0, pi->curr_mvr
#endif
#if MVAP
                , 0
#endif
#if SUB_TMVP
                , 0
#endif
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
        );
        mod_info_curr->refi[REFP_0] = temp_refi[REFP_0];
        mod_info_curr->refi[REFP_1] = temp_refi[REFP_1];

        get_org_bi(org, pred[Y_C], pi->stride_org[Y_C], cu_width, cu_height, pi->org_bi, cu_width, 0, 0);
        SWAP(refi[lidx_ref], refi[lidx_cnd], t0);
        SWAP(lidx_ref, lidx_cnd, t0);
        changed = 0;
        for (refi_cur = 0; refi_cur < pi->num_refp; refi_cur++)
        {
#if INTER_ME_MVLIB
#if ENC_ME_IMP
            mecost = pi->fn_me(ctx, pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx_ref, pi->mvp_scale[lidx_ref][refi_cur], pi->mv_scale[lidx_ref][refi_cur], 1);
#else
            mecost = pi->fn_me(pi, core, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx_ref, pi->mvp_scale[lidx_ref][refi_cur], pi->mv_scale[lidx_ref][refi_cur], 1);
#endif
#else
#if ENC_ME_IMP
            mecost = pi->fn_me(ctx, pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx_ref, pi->mvp_scale[lidx_ref][refi_cur], pi->mv_scale[lidx_ref][refi_cur], 1);
#else
            mecost = pi->fn_me(pi, x, y, cu_width, cu_height, mod_info_curr->x_pos, mod_info_curr->y_pos, cu_width, &refi_cur, lidx_ref, pi->mvp_scale[lidx_ref][refi_cur], pi->mv_scale[lidx_ref][refi_cur], 1);
#endif
#endif
            if (mecost < best_mecost)
            {
                refi_best = refi_cur;
                best_mecost = mecost;
                changed = 1;
                t0 = (lidx_ref == REFP_0) ? refi_best : mod_info_curr->refi[lidx_cnd];
                t1 = (lidx_ref == REFP_1) ? refi_best : mod_info_curr->refi[lidx_cnd];
                SET_REFI(mod_info_curr->refi, t0, t1);
                mod_info_curr->mv[lidx_ref][MV_X] = pi->mv_scale[lidx_ref][refi_cur][MV_X];
                mod_info_curr->mv[lidx_ref][MV_Y] = pi->mv_scale[lidx_ref][refi_cur][MV_Y];

                pi->mot_bits[lidx_ref] = get_mv_bits_with_mvr(mod_info_curr->mv[lidx_ref][MV_X] - pi->mvp_scale[lidx_ref][refi_cur][MV_X], mod_info_curr->mv[lidx_ref][MV_Y] - pi->mvp_scale[lidx_ref][refi_cur][MV_Y], pi->num_refp, refi_cur, pi->curr_mvr);
                pi->mot_bits[lidx_ref] -= (pi->curr_mvr == MAX_NUM_MVR - 1) ? pi->curr_mvr : (pi->curr_mvr + 1); // minus amvr index
            }
        }
        t0 = (lidx_ref == REFP_0) ? refi_best : REFI_INVALID;
        t1 = (lidx_ref == REFP_1) ? refi_best : REFI_INVALID;
        SET_REFI(refi, t0, t1);
        if (!changed)
        {
            break;
        }
    }

    mod_info_curr->mv[REFP_0][MV_X] = (mod_info_curr->mv[REFP_0][MV_X] >> pi->curr_mvr) << pi->curr_mvr;
    mod_info_curr->mv[REFP_0][MV_Y] = (mod_info_curr->mv[REFP_0][MV_Y] >> pi->curr_mvr) << pi->curr_mvr;
    mod_info_curr->mv[REFP_1][MV_X] = (mod_info_curr->mv[REFP_1][MV_X] >> pi->curr_mvr) << pi->curr_mvr;
    mod_info_curr->mv[REFP_1][MV_Y] = (mod_info_curr->mv[REFP_1][MV_Y] >> pi->curr_mvr) << pi->curr_mvr;

#if FAST_EXT_AMVR_HMVP
    check_best_bi_mvp(ctx, core);
#endif

    mod_info_curr->mvd[REFP_0][MV_X] = mod_info_curr->mv[REFP_0][MV_X] - pi->mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][MV_X];
    mod_info_curr->mvd[REFP_0][MV_Y] = mod_info_curr->mv[REFP_0][MV_Y] - pi->mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][MV_Y];
    mod_info_curr->mvd[REFP_1][MV_X] = mod_info_curr->mv[REFP_1][MV_X] - pi->mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][MV_X];
    mod_info_curr->mvd[REFP_1][MV_Y] = mod_info_curr->mv[REFP_1][MV_Y] - pi->mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][MV_Y];

    /* important: reset mv/mvd */
    for (i=0; i<REFP_NUM; i++)
    {
        // note: reset mvd, after clipping, mvd might not align with amvr index
        int amvr_shift = pi->curr_mvr;
        mod_info_curr->mvd[i][MV_X] = mod_info_curr->mvd[i][MV_X] >> amvr_shift << amvr_shift;
        mod_info_curr->mvd[i][MV_Y] = mod_info_curr->mvd[i][MV_Y] >> amvr_shift << amvr_shift;

        // note: mvd = mv - mvp, but because of clipping, mv might not equal to mvp + mvd
        int mv_x = (s32)mod_info_curr->mvd[i][MV_X] + pi->mvp_scale[i][mod_info_curr->refi[i]][MV_X];
        int mv_y = (s32)mod_info_curr->mvd[i][MV_Y] + pi->mvp_scale[i][mod_info_curr->refi[i]][MV_Y];
        mod_info_curr->mv[i][MV_X] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_x);
        mod_info_curr->mv[i][MV_Y] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_y);
    }
#if INTERPF
    core->mod_info_curr.inter_filter_flag = 0;
#endif
#if IPC
    core->mod_info_curr.ipc_flag = 0;
#endif
    cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                              , 0
#endif
    );
    check_best_mode(core, pi, cost, cost_best);
#if BGC
    if (cost <= 1.2 * (*cost_best) && ctx->info.sqh.bgc_enable_flag && cu_width * cu_height >= 256 && REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
    {
        mod_info_curr->bgc_flag = 1;
        mod_info_curr->bgc_idx = 0;
        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
            , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);

        mod_info_curr->bgc_idx = 1;
        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
            , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);

        mod_info_curr->bgc_flag = 0;
        mod_info_curr->bgc_idx = 0;
    }
#endif
}

#if SMVD
static u32 smvd_refine( ENC_CTX *ctx, ENC_CORE *core, int x, int y, int log2_cuw, int log2_cuh, s16 mv[REFP_NUM][MV_D], s16 mvp[REFP_NUM][MV_D], s8 refi[REFP_NUM], s32 lidx_cur, s32 lidx_tar, u32 mecost, s32 search_pattern, s32 search_round, s32 search_shift )
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int bit_depth = ctx->info.bit_depth_internal;

    int         cu_width, cu_height;
    u8          mv_bits = 0;
    pel        *org;

    s32 search_offset_cross[4][MV_D] = { { 0, 1 },{ 1, 0 },{ 0, -1 },{ -1, 0 } };
    s32 search_offset_square[8][MV_D] = { { -1, 1 },{ 0, 1 },{ 1, 1 },{ 1, 0 },{ 1, -1 },{ 0, -1 },{ -1, -1 },{ -1, 0 } };
    s32 search_offset_diamond[8][MV_D] = { { 0, 2 },{ 1, 1 },{ 2, 0 },{ 1, -1 },{ 0, -2 },{ -1, -1 },{ -2, 0 },{ -1, 1 } };
    s32 search_offset_hexagon[6][MV_D] = { { 2, 0 },{ 1, 2 },{ -1, 2 },{ -2, 0 },{ -1, -2 },{ 1, -2 } };

    s32 direct_start = 0;
    s32 direct_end = 0;
    s32 direct_rounding = 0;
    s32 direct_mask = 0;
    s32( *search_offset )[MV_D] = search_offset_diamond;

    s32 round;
    s32 best_direct = -1;
    s32 step = 1;
#if DMVR
    COM_DMVR dmvr;
#endif

    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];

    cu_width = (1 << log2_cuw);
    cu_height = (1 << log2_cuh);

    if ( search_pattern == 0 )
    {
        direct_end = 3;
        direct_rounding = 4;
        direct_mask = 0x03;
        search_offset = search_offset_cross;
    }
    else if ( search_pattern == 1 )
    {
        direct_end = 7;
        direct_rounding = 8;
        direct_mask = 0x07;
        search_offset = search_offset_square;
    }
    else if ( search_pattern == 2 )
    {
        direct_end = 7;
        direct_rounding = 8;
        direct_mask = 0x07;
        search_offset = search_offset_diamond;
    }
    else if ( search_pattern == 3 )
    {
        direct_end = 5;
        search_offset = search_offset_hexagon;
    }

    for ( round = 0; round < search_round; round++ )
    {
        s16 mv_cur_center[MV_D];
        s32 index;

        best_direct = -1;
        mv_cur_center[MV_X] = mv[lidx_cur][MV_X];
        mv_cur_center[MV_Y] = mv[lidx_cur][MV_Y];

        for ( index = direct_start; index <= direct_end; index++ )
        {
            s32 direct;
            u32 mecost_tmp;
            s16 mv_cand[REFP_NUM][MV_D], mvd_cand[REFP_NUM][MV_D];

            if ( search_pattern == 3 )
            {
                direct = index < 0 ? index + 6 : index >= 6 ? index - 6 : index;
            }
            else
            {
                direct = (index + direct_rounding) & direct_mask;
            }

            // update list cur
            mv_cand[lidx_cur][MV_X] = mv_cur_center[MV_X] + (s16)(search_offset[direct][MV_X] << search_shift);
            mv_cand[lidx_cur][MV_Y] = mv_cur_center[MV_Y] + (s16)(search_offset[direct][MV_Y] << search_shift);
            mvd_cand[lidx_cur][MV_X] = mv_cand[lidx_cur][MV_X] - mvp[lidx_cur][MV_X];
            mvd_cand[lidx_cur][MV_Y] = mv_cand[lidx_cur][MV_Y] - mvp[lidx_cur][MV_Y];

            // update list tar
            mv_cand[lidx_tar][MV_X] = mvp[lidx_tar][MV_X] - mvd_cand[lidx_cur][MV_X];
            mv_cand[lidx_tar][MV_Y] = mvp[lidx_tar][MV_Y] - mvd_cand[lidx_cur][MV_Y];

            mod_info_curr->mv[REFP_0][MV_X] = mv_cand[REFP_0][MV_X];
            mod_info_curr->mv[REFP_0][MV_Y] = mv_cand[REFP_0][MV_Y];
            mod_info_curr->mv[REFP_1][MV_X] = mv_cand[REFP_1][MV_X];
            mod_info_curr->mv[REFP_1][MV_Y] = mv_cand[REFP_1][MV_Y];
            
#if DMVR
            dmvr.poc_c = ctx->ptr; 
            dmvr.dmvr_current_template = pi->dmvr_template;
            dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
            dmvr.apply_DMVR = 0;
            dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif
            
            // get cost
            int enc_fast = search_pattern == 2;
            com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                    , &dmvr
#endif
#if BIO
                    , ctx->ptr, enc_fast, pi->curr_mvr
#endif
#if MVAP
                    , 0
#endif
#if SUB_TMVP
                    , 0
#endif
#if BGC
                    , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
            );

            mv_bits = (u8)get_mv_bits_with_mvr( mvd_cand[lidx_cur][MV_X], mvd_cand[lidx_cur][MV_Y], 0, mod_info_curr->refi[lidx_cur], pi->curr_mvr );
            mecost_tmp = enc_satd_16b( log2_cuw, log2_cuh, org, mod_info_curr->pred[0], pi->stride_org[Y_C], cu_width, bit_depth);
            mecost_tmp += MV_COST( pi, mv_bits );

            // save best
            if ( mecost_tmp < mecost )
            {
                mecost = mecost_tmp;
                mv[lidx_cur][MV_X] = mv_cand[lidx_cur][MV_X];
                mv[lidx_cur][MV_Y] = mv_cand[lidx_cur][MV_Y];

                mv[lidx_tar][MV_X] = mv_cand[lidx_tar][MV_X];
                mv[lidx_tar][MV_Y] = mv_cand[lidx_tar][MV_Y];

                best_direct = direct;
            }
        }

        if ( best_direct == -1 )
        {
            break;
        }
        step = 1;
        if ( (search_pattern == 1) || (search_pattern == 2) )
        {
            step = 2 - (best_direct & 0x01);
        }
        direct_start = best_direct - step;
        direct_end = best_direct + step;
    }

    return mecost;
};

static void analyze_smvd( ENC_CTX *ctx, ENC_CORE *core, double *cost_best )
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int bit_depth = ctx->info.bit_depth_internal;
    u32        mecost;
    pel        *org;
    double      cost = MAX_COST;
    int         lidx_ref, lidx_cnd;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int log2_cuw = mod_info_curr->cu_width_log2;
    int log2_cuh = mod_info_curr->cu_height_log2;
    int cu_width = 1 << log2_cuw;
    int cu_height = 1 << log2_cuh;
    mod_info_curr->cu_mode = MODE_INTER;
#if USE_IBC
    mod_info_curr->ibc_flag = 0;
#endif
#if CIBC
    mod_info_curr->cibc_flag = 0;
#endif
#if USE_SP
    mod_info_curr->sp_flag = 0;
    mod_info_curr->cs2_flag = 0;
#if RSD_OPT 
    mod_info_curr->sp_rsd_flag = 0;
#endif
#endif
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
    init_inter_data(pi, core);

    u8          mv_bits = 0;
    s16         mv[REFP_NUM][MV_D], mvp[REFP_NUM][MV_D], mvd[REFP_NUM][MV_D];
    int         max_round;
#if DMVR
    COM_DMVR dmvr;
#endif

    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];

    {
        mod_info_curr->smvd_flag = 1;
        lidx_ref = 0;
        lidx_cnd = 1;

        mod_info_curr->refi[REFP_0] = 0;
        mod_info_curr->refi[REFP_1] = 0;

        mvp[REFP_0][MV_X] = pi->mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][MV_X];
        mvp[REFP_0][MV_Y] = pi->mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][MV_Y];
        mvp[REFP_1][MV_X] = pi->mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][MV_X];
        mvp[REFP_1][MV_Y] = pi->mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][MV_Y];

        mv[0][MV_X] = mvp[0][MV_X];
        mv[0][MV_Y] = mvp[0][MV_Y];
        mv[1][MV_X] = mvp[1][MV_X];
        mv[1][MV_Y] = mvp[1][MV_Y];

        mvd[REFP_0][MV_X] = mv[REFP_0][MV_X] - mvp[REFP_0][MV_X];
        mvd[REFP_0][MV_Y] = mv[REFP_0][MV_Y] - mvp[REFP_0][MV_Y];
        mvd[REFP_1][MV_X] = mv[REFP_1][MV_X] - mvp[REFP_1][MV_X];
        mvd[REFP_1][MV_Y] = mv[REFP_1][MV_Y] - mvp[REFP_1][MV_Y];

        mod_info_curr->mv[REFP_0][MV_X] = mv[REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = mv[REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = mv[REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = mv[REFP_1][MV_Y];

#if DMVR
        dmvr.poc_c = ctx->ptr; 
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = 0;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif

        com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 1, pi->curr_mvr
#endif
#if MVAP
                , 0
#endif
#if SUB_TMVP
                , 0
#endif
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
        );

        mv_bits = (u8)get_mv_bits_with_mvr( mvd[lidx_ref][MV_X], mvd[lidx_ref][MV_Y], 0, mod_info_curr->refi[lidx_ref], pi->curr_mvr );
        mecost = enc_satd_16b( log2_cuw, log2_cuh, org, mod_info_curr->pred[0], pi->stride_org[Y_C], cu_width, bit_depth);
        mecost += MV_COST( pi, mv_bits );

        s16 mv_bi[REFP_NUM][MV_D];
        u32 mecost_bi;
        mv_bi[REFP_0][MV_X] = pi->mv_scale[REFP_0][0][MV_X];
        mv_bi[REFP_0][MV_Y] = pi->mv_scale[REFP_0][0][MV_Y];
        mvd[REFP_0][MV_X] = mv_bi[REFP_0][MV_X] - mvp[REFP_0][MV_X];
        mvd[REFP_0][MV_Y] = mv_bi[REFP_0][MV_Y] - mvp[REFP_0][MV_Y];
        mv_bi[REFP_1][MV_X] = mvp[REFP_1][MV_X] - mvd[REFP_0][MV_X];
        mv_bi[REFP_1][MV_Y] = mvp[REFP_1][MV_Y] - mvd[REFP_0][MV_Y];

        mod_info_curr->mv[REFP_0][MV_X] = mv_bi[REFP_0][MV_X];
        mod_info_curr->mv[REFP_0][MV_Y] = mv_bi[REFP_0][MV_Y];
        mod_info_curr->mv[REFP_1][MV_X] = mv_bi[REFP_1][MV_X];
        mod_info_curr->mv[REFP_1][MV_Y] = mv_bi[REFP_1][MV_Y];

#if DMVR
        dmvr.poc_c = ctx->ptr; 
        dmvr.dmvr_current_template = pi->dmvr_template;
        dmvr.dmvr_ref_pred_interpolated = pi->dmvr_ref_pred_interpolated;
        dmvr.apply_DMVR = 0;
        dmvr.dmvr_padding_buf = mod_info_curr->dmvr_padding_buf;
#endif

        com_mc(mod_info_curr->x_pos, mod_info_curr->y_pos, mod_info_curr->cu_width, mod_info_curr->cu_height, mod_info_curr->cu_width, mod_info_curr->pred, &ctx->info, mod_info_curr, pi->refp, CHANNEL_L, bit_depth
#if DMVR
                , &dmvr
#endif
#if BIO
                , ctx->ptr, 1, pi->curr_mvr
#endif
#if MVAP
                , 0
#endif
#if SUB_TMVP
                , 0
#endif
#if BGC
                , mod_info_curr->bgc_flag, mod_info_curr->bgc_idx
#endif
        );

        mv_bits = (u8)get_mv_bits_with_mvr(mvd[0][MV_X], mvd[0][MV_Y], 0, mod_info_curr->refi[0], pi->curr_mvr);
        mecost_bi = enc_satd_16b(log2_cuw, log2_cuh, org, mod_info_curr->pred[0], pi->stride_org[Y_C], cu_width, bit_depth);
        mecost_bi += MV_COST(pi, mv_bits);

        if (mecost_bi < mecost)
        {
            mecost = mecost_bi;
            mv[0][MV_X] = mv_bi[0][MV_X];
            mv[0][MV_Y] = mv_bi[0][MV_Y];
            mv[1][MV_X] = mv_bi[1][MV_X];
            mv[1][MV_Y] = mv_bi[1][MV_Y];
        }

        // refine
        max_round = 8;
        mecost = smvd_refine( ctx, core, x, y, log2_cuw, log2_cuh, mv, mvp, mod_info_curr->refi, lidx_ref, lidx_cnd, mecost, 2, max_round, pi->curr_mvr );
        mecost = smvd_refine( ctx, core, x, y, log2_cuw, log2_cuh, mv, mvp, mod_info_curr->refi, lidx_ref, lidx_cnd, mecost, 0, 1, pi->curr_mvr );

        mod_info_curr->mv[REFP_0][MV_X] = (mv[REFP_0][MV_X] >> pi->curr_mvr) << pi->curr_mvr;
        mod_info_curr->mv[REFP_0][MV_Y] = (mv[REFP_0][MV_Y] >> pi->curr_mvr) << pi->curr_mvr;
        mod_info_curr->mv[REFP_1][MV_X] = (mv[REFP_1][MV_X] >> pi->curr_mvr) << pi->curr_mvr;
        mod_info_curr->mv[REFP_1][MV_Y] = (mv[REFP_1][MV_Y] >> pi->curr_mvr) << pi->curr_mvr;

        mod_info_curr->mvd[REFP_0][MV_X] = mod_info_curr->mv[REFP_0][MV_X] - mvp[REFP_0][MV_X];
        mod_info_curr->mvd[REFP_0][MV_Y] = mod_info_curr->mv[REFP_0][MV_Y] - mvp[REFP_0][MV_Y];
        mod_info_curr->mvd[REFP_1][MV_X] = mod_info_curr->mv[REFP_1][MV_X] - mvp[REFP_1][MV_X];
        mod_info_curr->mvd[REFP_1][MV_Y] = mod_info_curr->mv[REFP_1][MV_Y] - mvp[REFP_1][MV_Y];

        /* important: reset mv/mvd */
        // note: after clipping, mvd might not align with amvr index
        int amvr_shift = pi->curr_mvr;
        mod_info_curr->mvd[REFP_0][MV_X] = mod_info_curr->mvd[REFP_0][MV_X] >> amvr_shift;
        mod_info_curr->mvd[REFP_0][MV_Y] = mod_info_curr->mvd[REFP_0][MV_Y] >> amvr_shift;

        mod_info_curr->mvd[REFP_1][MV_X] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, -mod_info_curr->mvd[REFP_0][MV_X]);
        mod_info_curr->mvd[REFP_1][MV_Y] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, -mod_info_curr->mvd[REFP_0][MV_Y]);

        // note: mvd = mv - mvp, but because of clipping, mv might not equal to mvp + mvd
        for (int i = 0; i < REFP_NUM; i++)
        {
            int mv_x = ((s32)mod_info_curr->mvd[i][MV_X] << amvr_shift) + mvp[i][MV_X];
            int mv_y = ((s32)mod_info_curr->mvd[i][MV_Y] << amvr_shift) + mvp[i][MV_Y];
            mod_info_curr->mv[i][MV_X] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_x);
            mod_info_curr->mv[i][MV_Y] = COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, mv_y);
        }
        mod_info_curr->mvd[REFP_0][MV_X] = mod_info_curr->mvd[REFP_0][MV_X] << amvr_shift;
        mod_info_curr->mvd[REFP_0][MV_Y] = mod_info_curr->mvd[REFP_0][MV_Y] << amvr_shift;
#if INTERPF
        core->mod_info_curr.inter_filter_flag = 0;
#endif
#if IPC
        core->mod_info_curr.ipc_flag = 0;
#endif
        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                  , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);
#if BGC
        if (cost <= 1.2 * (*cost_best) && ctx->info.sqh.bgc_enable_flag && cu_width * cu_height >= 256 && REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            mod_info_curr->bgc_flag = 1;
            mod_info_curr->bgc_idx = 0;
            cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                , 0
#endif
            );
            check_best_mode(core, pi, cost, cost_best);

            mod_info_curr->bgc_idx = 1;
            cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                , 0
#endif
            );
            check_best_mode(core, pi, cost, cost_best);

            mod_info_curr->bgc_flag = 0;
            mod_info_curr->bgc_idx = 0;
        }
#endif
    }
}
#endif

int pinter_init_frame(ENC_CTX *ctx)
{
    ENC_PINTER *pi;
    COM_PIC     *pic;
    int size;
    pi = &ctx->pinter;
    pic = pi->pic_org = PIC_ORG(ctx);
    pi->Yuv_org[Y_C] = pic->y;
    pi->Yuv_org[U_C] = pic->u;
    pi->Yuv_org[V_C] = pic->v;
    pi->stride_org[Y_C] = pic->stride_luma;
    pi->stride_org[U_C] = pic->stride_chroma;
    pi->stride_org[V_C] = pic->stride_chroma;

    pic = PIC_REC(ctx);
    pi->refp = ctx->refp;
    pi->slice_type = ctx->slice_type;
    pi->bit_depth = ctx->info.bit_depth_internal;
    pi->map_mv = ctx->map.map_mv;
    pi->pic_width_in_scu = ctx->info.pic_width_in_scu;
    size = sizeof(pel) * MAX_CU_DIM;
    com_mset(pi->pred_buf, 0, size);
    size = sizeof(pel) * N_C * MAX_CU_DIM;
    com_mset(pi->rec_buf, 0, size);
    size = sizeof(s16) * REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D;
    com_mset(pi->mvp_scale, 0, size);
    size = sizeof(s16) * REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * MV_D;
    com_mset(pi->mv_scale, 0, size);
    size = sizeof(int) * MV_D;
    com_mset(pi->max_imv, 0, size);

    size = sizeof(CPMV) * REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * VER_NUM * MV_D;
    com_mset(pi->affine_mvp_scale, 0, size);
    size = sizeof(CPMV) * REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * VER_NUM * MV_D;
    com_mset(pi->affine_mv_scale, 0, size);
    size = sizeof(pel) * MAX_CU_DIM;
    com_mset(pi->p_error, 0, size);
    size = sizeof(int) * 2 * MAX_CU_DIM;
    com_mset(pi->i_gradient, 0, size);

    size = sizeof(s16) * MAX_CU_DIM;
    com_mset(pi->org_bi, 0, size);
    size = sizeof(s32) * REFP_NUM;
    com_mset(pi->mot_bits, 0, size);

#if DMVR
    size = sizeof(pel) * MAX_CU_DIM;
    com_mset(pi->dmvr_template, 0, size);

    size = sizeof(pel) * REFP_NUM * (MAX_CU_SIZE + ((DMVR_NEW_VERSION_ITER_COUNT + 1) * REF_PRED_EXTENTION_PEL_COUNT)) *
        (MAX_CU_SIZE + ((DMVR_NEW_VERSION_ITER_COUNT + 1) * REF_PRED_EXTENTION_PEL_COUNT));
    com_mset(pi->dmvr_ref_pred_interpolated, 0, size);
#endif

    return COM_OK;
}

int pinter_init_lcu(ENC_CTX *ctx, ENC_CORE *core)
{
    ENC_PINTER *pi;
    pi = &ctx->pinter;
    pi->lambda_mv = (u32)floor(65536.0 * ctx->sqrt_lambda[0]);
    pi->qp_y = core->qp_y;
    pi->qp_u = core->qp_u;
    pi->qp_v = core->qp_v;
    pi->ptr = ctx->ptr;
    pi->gop_size = ctx->param.gop_size;
    return COM_OK;
}


static void scaled_horizontal_sobel_filter(pel *pred, int pred_stride, int *derivate, int derivate_buf_stride, int width, int height)
{
#if SIMD_AFFINE || SIMD_GRAD_ME
    int j, col, row;
    __m128i mm_pred[4];
    __m128i mm2x_pred[2];
    __m128i mm_intermediates[4];
    __m128i mm_derivate[2];
    assert(!(height % 2));
    assert(!(width % 4));
    /* Derivates of the rows and columns at the boundary are done at the end of this function */
    /* The value of col and row indicate the columns and rows for which the derivates have already been computed */
    for (col = 1; (col + 2) < width; col += 2)
    {
        mm_pred[0] = _mm_loadl_epi64((const __m128i *)(&pred[0 * pred_stride + col - 1]));
        mm_pred[1] = _mm_loadl_epi64((const __m128i *)(&pred[1 * pred_stride + col - 1]));
        mm_pred[0] = _mm_cvtepi16_epi32(mm_pred[0]);
        mm_pred[1] = _mm_cvtepi16_epi32(mm_pred[1]);
        for (row = 1; row < (height - 1); row += 2)
        {
            mm_pred[2] = _mm_loadl_epi64((const __m128i *)(&pred[(row + 1) * pred_stride + col - 1]));
            mm_pred[3] = _mm_loadl_epi64((const __m128i *)(&pred[(row + 2) * pred_stride + col - 1]));
            mm_pred[2] = _mm_cvtepi16_epi32(mm_pred[2]);
            mm_pred[3] = _mm_cvtepi16_epi32(mm_pred[3]);
            mm2x_pred[0] = _mm_slli_epi32(mm_pred[1], 1);
            mm2x_pred[1] = _mm_slli_epi32(mm_pred[2], 1);
            mm_intermediates[0] = _mm_add_epi32(mm2x_pred[0], mm_pred[0]);
            mm_intermediates[2] = _mm_add_epi32(mm2x_pred[1], mm_pred[1]);
            mm_intermediates[0] = _mm_add_epi32(mm_intermediates[0], mm_pred[2]);
            mm_intermediates[2] = _mm_add_epi32(mm_intermediates[2], mm_pred[3]);
            mm_pred[0] = mm_pred[2];
            mm_pred[1] = mm_pred[3];
            mm_intermediates[1] = _mm_srli_si128(mm_intermediates[0], 8);
            mm_intermediates[3] = _mm_srli_si128(mm_intermediates[2], 8);
            mm_derivate[0] = _mm_sub_epi32(mm_intermediates[1], mm_intermediates[0]);
            mm_derivate[1] = _mm_sub_epi32(mm_intermediates[3], mm_intermediates[2]);
            _mm_storel_epi64((__m128i *)(&derivate[col + (row + 0) * derivate_buf_stride]), mm_derivate[0]);
            _mm_storel_epi64((__m128i *)(&derivate[col + (row + 1) * derivate_buf_stride]), mm_derivate[1]);
        }
    }
    for (j = 1; j < (height - 1); j++)
    {
        derivate[j * derivate_buf_stride] = derivate[j * derivate_buf_stride + 1];
        derivate[j * derivate_buf_stride + (width - 1)] = derivate[j * derivate_buf_stride + (width - 2)];
    }
    com_mcpy
    (
        derivate,
        derivate + derivate_buf_stride,
        width * sizeof(derivate[0])
    );
    com_mcpy
    (
        derivate + (height - 1) * derivate_buf_stride,
        derivate + (height - 2) * derivate_buf_stride,
        width * sizeof(derivate[0])
    );
#else
    int j, k;
    for (j = 1; j < height - 1; j++)
    {
        for (k = 1; k < width - 1; k++)
        {
            int center = j * pred_stride + k;
            derivate[j * derivate_buf_stride + k] =
                pred[center + 1 - pred_stride] -
                pred[center - 1 - pred_stride] +
                (pred[center + 1] * 2) -
                (pred[center - 1] * 2) +
                pred[center + 1 + pred_stride] -
                pred[center - 1 + pred_stride];
        }
        derivate[j * derivate_buf_stride] = derivate[j * derivate_buf_stride + 1];
        derivate[j * derivate_buf_stride + width - 1] = derivate[j * derivate_buf_stride + width - 2];
    }
    derivate[0] = derivate[derivate_buf_stride + 1];
    derivate[width - 1] = derivate[derivate_buf_stride + width - 2];
    derivate[(height - 1) * derivate_buf_stride] = derivate[(height - 2) * derivate_buf_stride + 1];
    derivate[(height - 1) * derivate_buf_stride + width - 1] = derivate[(height - 2) * derivate_buf_stride + (width - 2)];
    for (j = 1; j < width - 1; j++)
    {
        derivate[j] = derivate[derivate_buf_stride + j];
        derivate[(height - 1) * derivate_buf_stride + j] = derivate[(height - 2) * derivate_buf_stride + j];
    }
#endif
}

static void scaled_vertical_sobel_filter(pel *pred, int pred_stride, int *derivate, int derivate_buf_stride, int width, int height)
{
#if SIMD_AFFINE || SIMD_GRAD_ME
    int j, col, row;
    __m128i mm_pred[4];
    __m128i mm_intermediates[6];
    __m128i mm_derivate[2];
    assert(!(height % 2));
    assert(!(width % 4));
    /* Derivates of the rows and columns at the boundary are done at the end of this function */
    /* The value of col and row indicate the columns and rows for which the derivates have already been computed */
    for (col = 1; col < (width - 1); col += 2)
    {
        mm_pred[0] = _mm_loadl_epi64((const __m128i *)(&pred[0 * pred_stride + col - 1]));
        mm_pred[1] = _mm_loadl_epi64((const __m128i *)(&pred[1 * pred_stride + col - 1]));
        mm_pred[0] = _mm_cvtepi16_epi32(mm_pred[0]);
        mm_pred[1] = _mm_cvtepi16_epi32(mm_pred[1]);
        for (row = 1; row < (height - 1); row += 2)
        {
            mm_pred[2] = _mm_loadl_epi64((const __m128i *)(&pred[(row + 1) * pred_stride + col - 1]));
            mm_pred[3] = _mm_loadl_epi64((const __m128i *)(&pred[(row + 2) * pred_stride + col - 1]));
            mm_pred[2] = _mm_cvtepi16_epi32(mm_pred[2]);
            mm_pred[3] = _mm_cvtepi16_epi32(mm_pred[3]);
            mm_intermediates[0] = _mm_sub_epi32(mm_pred[2], mm_pred[0]);
            mm_intermediates[3] = _mm_sub_epi32(mm_pred[3], mm_pred[1]);
            mm_pred[0] = mm_pred[2];
            mm_pred[1] = mm_pred[3];
            mm_intermediates[1] = _mm_srli_si128(mm_intermediates[0], 4);
            mm_intermediates[4] = _mm_srli_si128(mm_intermediates[3], 4);
            mm_intermediates[2] = _mm_srli_si128(mm_intermediates[0], 8);
            mm_intermediates[5] = _mm_srli_si128(mm_intermediates[3], 8);
            mm_intermediates[1] = _mm_slli_epi32(mm_intermediates[1], 1);
            mm_intermediates[4] = _mm_slli_epi32(mm_intermediates[4], 1);
            mm_intermediates[0] = _mm_add_epi32(mm_intermediates[0], mm_intermediates[2]);
            mm_intermediates[3] = _mm_add_epi32(mm_intermediates[3], mm_intermediates[5]);
            mm_derivate[0] = _mm_add_epi32(mm_intermediates[0], mm_intermediates[1]);
            mm_derivate[1] = _mm_add_epi32(mm_intermediates[3], mm_intermediates[4]);
            _mm_storel_epi64((__m128i *) (&derivate[col + (row + 0) * derivate_buf_stride]), mm_derivate[0]);
            _mm_storel_epi64((__m128i *) (&derivate[col + (row + 1) * derivate_buf_stride]), mm_derivate[1]);
        }
    }
    for (j = 1; j < (height - 1); j++)
    {
        derivate[j * derivate_buf_stride] = derivate[j * derivate_buf_stride + 1];
        derivate[j * derivate_buf_stride + (width - 1)] = derivate[j * derivate_buf_stride + (width - 2)];
    }
    com_mcpy
    (
        derivate,
        derivate + derivate_buf_stride,
        width * sizeof(derivate[0])
    );
    com_mcpy
    (
        derivate + (height - 1) * derivate_buf_stride,
        derivate + (height - 2) * derivate_buf_stride,
        width * sizeof(derivate[0])
    );
#else
    int k, j;
    for (k = 1; k < width - 1; k++)
    {
        for (j = 1; j < height - 1; j++)
        {
            int center = j * pred_stride + k;
            derivate[j * derivate_buf_stride + k] =
                pred[center + pred_stride - 1] -
                pred[center - pred_stride - 1] +
                (pred[center + pred_stride] * 2) -
                (pred[center - pred_stride] * 2) +
                pred[center + pred_stride + 1] -
                pred[center - pred_stride + 1];
        }
        derivate[k] = derivate[derivate_buf_stride + k];
        derivate[(height - 1) * derivate_buf_stride + k] = derivate[(height - 2) * derivate_buf_stride + k];
    }
    derivate[0] = derivate[derivate_buf_stride + 1];
    derivate[width - 1] = derivate[derivate_buf_stride + width - 2];
    derivate[(height - 1) * derivate_buf_stride] = derivate[(height - 2) * derivate_buf_stride + 1];
    derivate[(height - 1) * derivate_buf_stride + width - 1] = derivate[(height - 2) * derivate_buf_stride + (width - 2)];
    for (j = 1; j < height - 1; j++)
    {
        derivate[j * derivate_buf_stride] = derivate[j * derivate_buf_stride + 1];
        derivate[j * derivate_buf_stride + width - 1] = derivate[j * derivate_buf_stride + width - 2];
    }
#endif
}

static void equal_coeff_computer(pel *residue, int residue_stride, int **derivate, int derivate_buf_stride, s64(*equal_coeff)[7], int width, int height, int vertex_num)
{
#if SIMD_AFFINE
    int j, k;
    int idx1 = 0, idx2 = 0;
    __m128i mm_two, mm_four;
    __m128i mm_tmp[4];
    __m128i mm_intermediate[4];
    __m128i mm_idx_k, mm_idx_j[2];
    __m128i mm_residue[2];
    // Add directly to indexes to get new index
    mm_two = _mm_set1_epi32(2);
    mm_four = _mm_set1_epi32(4);
    if (vertex_num == 3)
    {
        __m128i mm_c[12];
        //  mm_c - map
        //  C for 1st row of pixels
        //  mm_c[0] = iC[0][i] | iC[0][i+1] | iC[0][i+2] | iC[0][i+3]
        //  mm_c[1] = iC[1][i] | iC[1][i+1] | iC[1][i+2] | iC[1][i+3]
        //  mm_c[2] = iC[2][i] | iC[2][i+1] | iC[2][i+2] | iC[2][i+3]
        //  mm_c[3] = iC[3][i] | iC[3][i+1] | iC[3][i+2] | iC[3][i+3]
        //  mm_c[4] = iC[4][i] | iC[4][i+1] | iC[4][i+2] | iC[4][i+3]
        //  mm_c[5] = iC[5][i] | iC[5][i+1] | iC[5][i+2] | iC[5][i+3]
        //  C for 2nd row of pixels
        //  mm_c[6] = iC[6][i] | iC[6][i+1] | iC[6][i+2] | iC[6][i+3]
        //  mm_c[7] = iC[7][i] | iC[7][i+1] | iC[7][i+2] | iC[7][i+3]
        //  mm_c[8] = iC[8][i] | iC[8][i+1] | iC[8][i+2] | iC[8][i+3]
        //  mm_c[9] = iC[9][i] | iC[9][i+1] | iC[9][i+2] | iC[9][i+3]
        //  mm_c[10] = iC[10][i] | iC[10][i+1] | iC[10][i+2] | iC[10][i+3]
        //  mm_c[11] = iC[11][i] | iC[11][i+1] | iC[11][i+2] | iC[11][i+3]
        idx1 = -2 * derivate_buf_stride - 4;
        idx2 = -derivate_buf_stride - 4;
        mm_idx_j[0] = _mm_set1_epi32(-2);
        mm_idx_j[1] = _mm_set1_epi32(-1);
        for (j = 0; j < height; j += 2)
        {
            mm_idx_j[0] = _mm_add_epi32(mm_idx_j[0], mm_two);
            mm_idx_j[1] = _mm_add_epi32(mm_idx_j[1], mm_two);
            mm_idx_k = _mm_set_epi32(-1, -2, -3, -4);
            idx1 += (derivate_buf_stride << 1);
            idx2 += (derivate_buf_stride << 1);
            for (k = 0; k < width; k += 4)
            {
                idx1 += 4;
                idx2 += 4;
                mm_idx_k = _mm_add_epi32(mm_idx_k, mm_four);
                // 1st row
                mm_c[0] = _mm_loadu_si128((const __m128i*)&derivate[0][idx1]);
                mm_c[2] = _mm_loadu_si128((const __m128i*)&derivate[1][idx1]);
                // 2nd row
                mm_c[6] = _mm_loadu_si128((const __m128i*)&derivate[0][idx2]);
                mm_c[8] = _mm_loadu_si128((const __m128i*)&derivate[1][idx2]);
                // 1st row
                mm_c[1] = _mm_mullo_epi32(mm_idx_k, mm_c[0]);
                mm_c[3] = _mm_mullo_epi32(mm_idx_k, mm_c[2]);
                mm_c[4] = _mm_mullo_epi32(mm_idx_j[0], mm_c[0]);
                mm_c[5] = _mm_mullo_epi32(mm_idx_j[0], mm_c[2]);
                // 2nd row
                mm_c[7] = _mm_mullo_epi32(mm_idx_k, mm_c[6]);
                mm_c[9] = _mm_mullo_epi32(mm_idx_k, mm_c[8]);
                mm_c[10] = _mm_mullo_epi32(mm_idx_j[1], mm_c[6]);
                mm_c[11] = _mm_mullo_epi32(mm_idx_j[1], mm_c[8]);
                // Residue
                mm_residue[0] = _mm_loadl_epi64((const __m128i*)&residue[idx1]);
                mm_residue[1] = _mm_loadl_epi64((const __m128i*)&residue[idx2]);
                mm_residue[0] = _mm_cvtepi16_epi32(mm_residue[0]);
                mm_residue[1] = _mm_cvtepi16_epi32(mm_residue[1]);
                mm_residue[0] = _mm_slli_epi32(mm_residue[0], 3);
                mm_residue[1] = _mm_slli_epi32(mm_residue[1], 3);
                // Calculate residue coefficients first
                mm_tmp[2] = _mm_srli_si128(mm_residue[0], 4);
                mm_tmp[3] = _mm_srli_si128(mm_residue[1], 4);
                // 1st row
                mm_tmp[0] = _mm_srli_si128(mm_c[0], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[6], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][6], mm_intermediate[3]);
                // 2nd row
                mm_tmp[0] = _mm_srli_si128(mm_c[1], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[7], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][6], mm_intermediate[3]);
                // 3rd row
                mm_tmp[0] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[8], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[8], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][6], mm_intermediate[3]);
                // 4th row
                mm_tmp[0] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[9], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[9], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][6], mm_intermediate[3]);
                // 5th row
                mm_tmp[0] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[10], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[4], mm_c[10], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[5][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][6], mm_intermediate[3]);
                // 6th row
                mm_tmp[0] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[11], 4);
                // 7th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[5], mm_c[11], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[6][6]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][6], mm_intermediate[3]);
                //Start calculation of coefficient matrix
                // 1st row
                mm_tmp[0] = _mm_srli_si128(mm_c[0], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[6], 4);
                // 1st col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[0], mm_c[6], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][0]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][0], mm_intermediate[3]);
                // 2nd col of row and 1st col of 2nd row
                mm_tmp[2] = _mm_srli_si128(mm_c[1], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[7], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[1], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][1]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][1], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][0], mm_intermediate[3]);
                // 3rd col of row and 1st col of 3rd row
                mm_tmp[2] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[8], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[2], mm_c[8], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][2], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][0], mm_intermediate[3]);
                // 4th col of row and 1st col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[9], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[3], mm_c[9], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][0], mm_intermediate[3]);
                // 5th col of row and 1st col of the 5th row
                mm_tmp[2] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[10], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[4], mm_c[10], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][4], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][0], mm_intermediate[3]);
                // 6th col of row and 1st col of the 6th row
                mm_tmp[2] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[11], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[6], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][5], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][0], mm_intermediate[3]);
                // 2nd row
                mm_tmp[0] = _mm_srli_si128(mm_c[1], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[7], 4);
                // 2nd col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_c[1], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][1]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][1], mm_intermediate[3]);
                // 3rd col of row and 2nd col of 3rd row
                mm_tmp[2] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[8], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_c[2], mm_c[8], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][2], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][1], mm_intermediate[3]);
                // 4th col of row and 2nd col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[9], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_c[3], mm_c[9], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][1], mm_intermediate[3]);
                // 5th col of row and 1st col of the 5th row
                mm_tmp[2] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[10], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_c[4], mm_c[10], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][4], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][1], mm_intermediate[3]);
                // 6th col of row and 1st col of the 6th row
                mm_tmp[2] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[11], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[7], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][5], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][1], mm_intermediate[3]);
                // 3rd row
                mm_tmp[0] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[8], 4);
                //3rd Col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[8], mm_c[2], mm_c[8], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][2], mm_intermediate[3]);
                // 4th col of row and 3rd col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[9], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[8], mm_c[3], mm_c[9], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][2], mm_intermediate[3]);
                // 5th col of row and 1st col of the 5th row
                mm_tmp[2] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[10], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[8], mm_c[4], mm_c[10], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][4], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][2], mm_intermediate[3]);
                // 6th col of row and 1st col of the 6th row
                mm_tmp[2] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[11], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[8], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][5], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][2], mm_intermediate[3]);
                // 4th row
                mm_tmp[0] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[9], 4);
                // 4th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[9], mm_c[3], mm_c[9], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][3], mm_intermediate[3]);
                // 5th col of row and 1st col of the 5th row
                mm_tmp[2] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[10], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[9], mm_c[4], mm_c[10], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][4], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][3], mm_intermediate[3]);
                // 6th col of row and 1st col of the 6th row
                mm_tmp[2] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[11], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[9], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][5], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][3], mm_intermediate[3]);
                // 5th row
                mm_tmp[0] = _mm_srli_si128(mm_c[4], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[10], 4);
                // 5th col of row and 1st col of the 5th row
                CALC_EQUAL_COEFF_8PXLS(mm_c[4], mm_c[10], mm_c[4], mm_c[10], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[5][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][4], mm_intermediate[3]);
                // 6th col of row and 1st col of the 6th row
                mm_tmp[2] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[11], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[4], mm_c[10], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[5][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[5][5], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][4], mm_intermediate[3]);
                // 6th row
                mm_tmp[0] = _mm_srli_si128(mm_c[5], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[11], 4);
                // 5th col of row and 1st col of the 5th row
                CALC_EQUAL_COEFF_8PXLS(mm_c[5], mm_c[11], mm_c[5], mm_c[11], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[6][5]);
                _mm_storel_epi64((__m128i*)&equal_coeff[6][5], mm_intermediate[3]);
            }
            idx1 -= (width);
            idx2 -= (width);
        }
    }
    else
    {
        __m128i mm_c[8];
        //  mm_c - map
        //  C for 1st row of pixels
        //  mm_c[0] = iC[0][i] | iC[0][i+1] | iC[0][i+2] | iC[0][i+3]
        //  mm_c[1] = iC[1][i] | iC[1][i+1] | iC[1][i+2] | iC[1][i+3]
        //  mm_c[2] = iC[2][i] | iC[2][i+1] | iC[2][i+2] | iC[2][i+3]
        //  mm_c[3] = iC[3][i] | iC[3][i+1] | iC[3][i+2] | iC[3][i+3]
        //  C for 2nd row of pixels
        //  mm_c[4] = iC[0][i] | iC[0][i+1] | iC[0][i+2] | iC[0][i+3]
        //  mm_c[5] = iC[1][i] | iC[1][i+1] | iC[1][i+2] | iC[1][i+3]
        //  mm_c[6] = iC[2][i] | iC[2][i+1] | iC[2][i+2] | iC[2][i+3]
        //  mm_c[7] = iC[3][i] | iC[3][i+1] | iC[3][i+2] | iC[3][i+3]
        idx1 = -2 * derivate_buf_stride - 4;
        idx2 = -derivate_buf_stride - 4;
        mm_idx_j[0] = _mm_set1_epi32(-2);
        mm_idx_j[1] = _mm_set1_epi32(-1);
        for (j = 0; j < height; j += 2)
        {
            mm_idx_j[0] = _mm_add_epi32(mm_idx_j[0], mm_two);
            mm_idx_j[1] = _mm_add_epi32(mm_idx_j[1], mm_two);
            mm_idx_k = _mm_set_epi32(-1, -2, -3, -4);
            idx1 += (derivate_buf_stride << 1);
            idx2 += (derivate_buf_stride << 1);
            for (k = 0; k < width; k += 4)
            {
                idx1 += 4;
                idx2 += 4;
                mm_idx_k = _mm_add_epi32(mm_idx_k, mm_four);
                mm_c[0] = _mm_loadu_si128((const __m128i*)&derivate[0][idx1]);
                mm_c[2] = _mm_loadu_si128((const __m128i*)&derivate[1][idx1]);
                mm_c[4] = _mm_loadu_si128((const __m128i*)&derivate[0][idx2]);
                mm_c[6] = _mm_loadu_si128((const __m128i*)&derivate[1][idx2]);
                mm_c[1] = _mm_mullo_epi32(mm_idx_k, mm_c[0]);
                mm_c[3] = _mm_mullo_epi32(mm_idx_j[0], mm_c[0]);
                mm_c[5] = _mm_mullo_epi32(mm_idx_k, mm_c[4]);
                mm_c[7] = _mm_mullo_epi32(mm_idx_j[1], mm_c[4]);
                mm_residue[0] = _mm_loadl_epi64((const __m128i*)&residue[idx1]);
                mm_residue[1] = _mm_loadl_epi64((const __m128i*)&residue[idx2]);
                mm_tmp[0] = _mm_mullo_epi32(mm_idx_j[0], mm_c[2]);
                mm_tmp[1] = _mm_mullo_epi32(mm_idx_k, mm_c[2]);
                mm_tmp[2] = _mm_mullo_epi32(mm_idx_j[1], mm_c[6]);
                mm_tmp[3] = _mm_mullo_epi32(mm_idx_k, mm_c[6]);
                mm_residue[0] = _mm_cvtepi16_epi32(mm_residue[0]);
                mm_residue[1] = _mm_cvtepi16_epi32(mm_residue[1]);
                mm_c[1] = _mm_add_epi32(mm_c[1], mm_tmp[0]);
                mm_c[3] = _mm_sub_epi32(mm_c[3], mm_tmp[1]);
                mm_c[5] = _mm_add_epi32(mm_c[5], mm_tmp[2]);
                mm_c[7] = _mm_sub_epi32(mm_c[7], mm_tmp[3]);
                mm_residue[0] = _mm_slli_epi32(mm_residue[0], 3);
                mm_residue[1] = _mm_slli_epi32(mm_residue[1], 3);
                //Start calculation of coefficient matrix
                // 1st row
                mm_tmp[0] = _mm_srli_si128(mm_c[0], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[4], 4);
                // 1st col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[4], mm_c[0], mm_c[4], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][0]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][0], mm_intermediate[3]);
                // 2nd col of row and 1st col of 2nd row
                mm_tmp[2] = _mm_srli_si128(mm_c[1], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[5], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[4], mm_c[1], mm_c[5], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][1]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][1], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][0], mm_intermediate[3]);
                // 3rd col of row and 1st col of 3rd row
                mm_tmp[2] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[6], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[4], mm_c[2], mm_c[6], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][2], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][0], mm_intermediate[3]);
                // 4th col of row and 1st col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[7], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[4], mm_c[3], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][0], mm_intermediate[3]);
                // 5th col of row
                mm_tmp[2] = _mm_srli_si128(mm_residue[0], 4);
                mm_tmp[3] = _mm_srli_si128(mm_residue[1], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[0], mm_c[4], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[1][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[1][4], mm_intermediate[3]);
                // 2nd row
                mm_tmp[0] = _mm_srli_si128(mm_c[1], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[5], 4);
                // 2nd col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[5], mm_c[1], mm_c[5], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][1]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][1], mm_intermediate[3]);
                // 3rd col of row and 2nd col of 3rd row
                mm_tmp[2] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[6], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[5], mm_c[2], mm_c[6], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][2], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][1], mm_intermediate[3]);
                // 4th col of row and 2nd col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[7], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[5], mm_c[3], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][1], mm_intermediate[3]);
                // 5th col of row
                mm_tmp[2] = _mm_srli_si128(mm_residue[0], 4);
                mm_tmp[3] = _mm_srli_si128(mm_residue[1], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[1], mm_c[5], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[2][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[2][4], mm_intermediate[3]);
                // 3rd row
                mm_tmp[0] = _mm_srli_si128(mm_c[2], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[6], 4);
                //3rd Col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[6], mm_c[2], mm_c[6], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][2]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][2], mm_intermediate[3]);
                // 4th col of row and 3rd col of 4th row
                mm_tmp[2] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[3] = _mm_srli_si128(mm_c[7], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[6], mm_c[3], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][3], mm_intermediate[3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][2], mm_intermediate[3]);
                // 5th col of row
                mm_tmp[2] = _mm_srli_si128(mm_residue[0], 4);
                mm_tmp[3] = _mm_srli_si128(mm_residue[1], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[2], mm_c[6], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[3][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[3][4], mm_intermediate[3]);
                // 4th row
                mm_tmp[0] = _mm_srli_si128(mm_c[3], 4);
                mm_tmp[1] = _mm_srli_si128(mm_c[7], 4);
                // 4th col of row
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[7], mm_c[3], mm_c[7], mm_tmp[0], mm_tmp[1], mm_tmp[0], mm_tmp[1], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][3]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][3], mm_intermediate[3]);
                // 5th col of row
                mm_tmp[2] = _mm_srli_si128(mm_residue[0], 4);
                mm_tmp[3] = _mm_srli_si128(mm_residue[1], 4);
                CALC_EQUAL_COEFF_8PXLS(mm_c[3], mm_c[7], mm_residue[0], mm_residue[1], mm_tmp[0], mm_tmp[1], mm_tmp[2], mm_tmp[3], mm_intermediate[0], mm_intermediate[1], mm_intermediate[2], mm_intermediate[3], (const __m128i*)&equal_coeff[4][4]);
                _mm_storel_epi64((__m128i*)&equal_coeff[4][4], mm_intermediate[3]);
            }
            idx1 -= (width);
            idx2 -= (width);
        }
    }
#else
    int affine_param_num = (vertex_num << 1);
    int j, k, col, row;
    for (j = 0; j != height; j++)
    {
        for (k = 0; k != width; k++)
        {
            s64 intermediates[2];
            int iC[6];
            int iIdx = j * derivate_buf_stride + k;
            if (vertex_num == 2)
            {
                iC[0] = derivate[0][iIdx];
                iC[1] = k * derivate[0][iIdx];
                iC[1] += j * derivate[1][iIdx];
                iC[2] = derivate[1][iIdx];
                iC[3] = j * derivate[0][iIdx];
                iC[3] -= k * derivate[1][iIdx];
            }
            else
            {
                iC[0] = derivate[0][iIdx];
                iC[1] = k * derivate[0][iIdx];
                iC[2] = derivate[1][iIdx];
                iC[3] = k * derivate[1][iIdx];
                iC[4] = j * derivate[0][iIdx];
                iC[5] = j * derivate[1][iIdx];
            }
            for (col = 0; col < affine_param_num; col++)
            {
                intermediates[0] = iC[col];
                for (row = 0; row < affine_param_num; row++)
                {
                    intermediates[1] = intermediates[0] * iC[row];
                    equal_coeff[col + 1][row] += intermediates[1];
                }
                intermediates[1] = intermediates[0] * residue[iIdx];
                equal_coeff[col + 1][affine_param_num] += intermediates[1] * 8;
            }
        }
    }
#endif
}

void solve_equal(double(*equal_coeff)[7], int order, double* affine_para)
{
    int i, j, k;
    // row echelon
    for (i = 1; i < order; i++)
    {
        // find column max
        double temp = fabs(equal_coeff[i][i - 1]);
        int temp_idx = i;
        for (j = i + 1; j < order + 1; j++)
        {
            if (fabs(equal_coeff[j][i - 1]) > temp)
            {
                temp = fabs(equal_coeff[j][i - 1]);
                temp_idx = j;
            }
        }
        // swap line
        if (temp_idx != i)
        {
            for (j = 0; j < order + 1; j++)
            {
                equal_coeff[0][j] = equal_coeff[i][j];
                equal_coeff[i][j] = equal_coeff[temp_idx][j];
                equal_coeff[temp_idx][j] = equal_coeff[0][j];
            }
        }
        // elimination first column
        for (j = i + 1; j < order + 1; j++)
        {
            for (k = i; k < order + 1; k++)
            {
                equal_coeff[j][k] = equal_coeff[j][k] - equal_coeff[i][k] * equal_coeff[j][i - 1] / equal_coeff[i][i - 1];
            }
        }
    }
    affine_para[order - 1] = equal_coeff[order][order] / equal_coeff[order][order - 1];
    for (i = order - 2; i >= 0; i--)
    {
        double temp = 0;
        for (j = i + 1; j < order; j++)
        {
            temp += equal_coeff[i + 1][j] * affine_para[j];
        }
        affine_para[i] = (equal_coeff[i + 1][order] - temp) / equal_coeff[i + 1][i];
    }
}

static int get_affine_mv_bits(CPMV mv[VER_NUM][MV_D], CPMV mvp[VER_NUM][MV_D], int num_refp, int refi, int vertex_num
#if BD_AFFINE_AMVR
                              , u8 curr_mvr
#endif
                             )
{
    int bits = 0;
    int vertex;
    for (vertex = 0; vertex < vertex_num; vertex++)
    {
        int mvd_x = mv[vertex][MV_X] - mvp[vertex][MV_X];
        int mvd_y = mv[vertex][MV_Y] - mvp[vertex][MV_Y];
#if BD_AFFINE_AMVR
        u8 amvr_shift = Tab_Affine_AMVR(curr_mvr);
        // note: if clipped to MAX value, the mv/mvp might not align with amvr shift
        if (mv[vertex][MV_X] != COM_CPMV_MAX && mvp[vertex][MV_X] != COM_CPMV_MAX)
            assert(mvd_x == ((mvd_x >> amvr_shift) << amvr_shift));
        if (mv[vertex][MV_Y] != COM_CPMV_MAX && mvp[vertex][MV_Y] != COM_CPMV_MAX)
            assert(mvd_y == ((mvd_y >> amvr_shift) << amvr_shift));
        mvd_x >>= amvr_shift;
        mvd_y >>= amvr_shift;
#endif
        bits += (mvd_x > 2048 || mvd_x <= -2048) ? get_exp_golomb_bits(COM_ABS(mvd_x)) : enc_tbl_mv_bits[mvd_x];
        bits += (mvd_y > 2048 || mvd_y <= -2048) ? get_exp_golomb_bits(COM_ABS(mvd_y)) : enc_tbl_mv_bits[mvd_y];
    }
    bits += enc_tbl_refi_bits[num_refp][refi];
    return bits;
}

#if ASP
static u32 pinter_affine_me_gradient(COM_INFO* info, ENC_PINTER* pi, int x, int y, int cu_width_log2, int cu_height_log2, s8* refi, int lidx, CPMV mvp[VER_NUM][MV_D], CPMV mv[VER_NUM][MV_D], int bi, int vertex_num, int sub_w, int sub_h)
#else
static u32 pinter_affine_me_gradient(ENC_PINTER * pi, int x, int y, int cu_width_log2, int cu_height_log2, s8 * refi, int lidx, CPMV mvp[VER_NUM][MV_D], CPMV mv[VER_NUM][MV_D], int bi, int vertex_num, int sub_w, int sub_h)
#endif
{
    int bit_depth = pi->bit_depth;
    CPMV mvt[VER_NUM][MV_D];
    s16 mvd[VER_NUM][MV_D];
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    u32 cost, cost_best = COM_UINT32_MAX;
    s8 ri = *refi;
    COM_PIC* refp = pi->refp[ri][lidx].pic;
    pel *pred = pi->pred_buf;
#if OBMC
    pel *org = bi ? pi->org_bi : pi->org_obmc_affine;
    int s_org = cu_width;
#else
    pel *org = bi ? pi->org_bi : (pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C]);
    int s_org = bi ? cu_width : pi->stride_org[Y_C];
#endif
    int mv_bits, best_bits;
    int vertex, iter;
    int iter_num = bi ? AF_ITER_BI : AF_ITER_UNI;
    int para_num = (vertex_num << 1) + 1;
    int affine_param_num = para_num - 1;
    double affine_para[6];
    double delta_mv[6];
    s64    equal_coeff_t[7][7];
    double equal_coeff[7][7];
    pel    *error = pi->p_error;
    int    *derivate[2];
    derivate[0] = pi->i_gradient[0];
    derivate[1] = pi->i_gradient[1];
    cu_width = 1 << cu_width_log2;
    cu_height = 1 << cu_height_log2;
#if ENC_ME_IMP
    u8 amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
#endif

    /* set start mv */
    for (vertex = 0; vertex < vertex_num; vertex++)
    {
        mvt[vertex][MV_X] = mv[vertex][MV_X];
        mvt[vertex][MV_Y] = mv[vertex][MV_Y];
        mvd[vertex][MV_X] = 0;
        mvd[vertex][MV_Y] = 0;
    }
    /* do motion compensation with start mv */
#if ASP
    com_affine_mc_l(info, x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#else
    com_affine_mc_l(x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#endif

    /* get mvd bits*/
    best_bits = get_affine_mv_bits(mvt, mvp, pi->num_refp, ri, vertex_num
#if BD_AFFINE_AMVR
                                   , pi->curr_mvr
#endif
                                  );
    if (bi)
    {
        best_bits += pi->mot_bits[1 - lidx];
    }
    cost_best = MV_COST(pi, best_bits);

    /* get satd */
    cost_best += calc_satd_16b(cu_width, cu_height, org, pred, s_org, cu_width, bit_depth) >> bi;
    if (vertex_num == 3)
    {
        iter_num = bi ? (AF_ITER_BI - 2) : (AF_ITER_UNI - 2);
    }
    for (iter = 0; iter < iter_num; iter++)
    {
        int row, col;
        int all_zero = 0;
        enc_diff_16b(cu_width_log2, cu_height_log2, org, pred, s_org, cu_width, cu_width, error);
        // sobel x direction
        // -1 0 1
        // -2 0 2
        // -1 0 1
        scaled_horizontal_sobel_filter(pred, cu_width, derivate[0], cu_width, cu_width, cu_height);
        // sobel y direction
        // -1 -2 -1
        //  0  0  0
        //  1  2  1
        scaled_vertical_sobel_filter(pred, cu_width, derivate[1], cu_width, cu_width, cu_height);
        // solve delta x and y
        for (row = 0; row < para_num; row++)
        {
            com_mset(&equal_coeff_t[row][0], 0, para_num * sizeof(s64));
        }
        equal_coeff_computer(error, cu_width, derivate, cu_width, equal_coeff_t, cu_width, cu_height, vertex_num);
        for (row = 0; row < para_num; row++)
        {
            for (col = 0; col < para_num; col++)
            {
                equal_coeff[row][col] = (double)equal_coeff_t[row][col];
            }
        }
        solve_equal(equal_coeff, affine_param_num, affine_para);
        // convert to delta mv
        if (vertex_num == 3)
        {
            // for MV0
            delta_mv[0] = affine_para[0];
            delta_mv[2] = affine_para[2];
            // for MV1
            delta_mv[1] = affine_para[1] * cu_width + affine_para[0];
            delta_mv[3] = affine_para[3] * cu_width + affine_para[2];
            // for MV2
            delta_mv[4] = affine_para[4] * cu_height + affine_para[0];
            delta_mv[5] = affine_para[5] * cu_height + affine_para[2];
#if BD_AFFINE_AMVR
#if !ENC_ME_IMP
            u8 amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
#endif
            if (amvr_shift == 0)
            {
                mvd[0][MV_X] = (s16)(delta_mv[0] * 16 + (delta_mv[0] >= 0 ? 0.5 : -0.5)); //  1/16-pixel
                mvd[0][MV_Y] = (s16)(delta_mv[2] * 16 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_X] = (s16)(delta_mv[1] * 16 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_Y] = (s16)(delta_mv[3] * 16 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
                mvd[2][MV_X] = (s16)(delta_mv[4] * 16 + (delta_mv[4] >= 0 ? 0.5 : -0.5));
                mvd[2][MV_Y] = (s16)(delta_mv[5] * 16 + (delta_mv[5] >= 0 ? 0.5 : -0.5));
            }
            else
            {
#if ENC_ME_IMP
                CPMV tmp_mvd[VER_NUM][MV_D];
                tmp_mvd[0][MV_X] = (CPMV)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
                tmp_mvd[0][MV_Y] = (CPMV)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                tmp_mvd[1][MV_X] = (CPMV)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                tmp_mvd[1][MV_Y] = (CPMV)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
                tmp_mvd[2][MV_X] = (CPMV)(delta_mv[1] * 4 + (delta_mv[4] >= 0 ? 0.5 : -0.5));
                tmp_mvd[2][MV_Y] = (CPMV)(delta_mv[3] * 4 + (delta_mv[5] >= 0 ? 0.5 : -0.5));
                tmp_mvd[0][MV_X] <<= 2; // 1/16-pixel
                tmp_mvd[0][MV_Y] <<= 2;
                tmp_mvd[1][MV_X] <<= 2;
                tmp_mvd[1][MV_Y] <<= 2;
                tmp_mvd[2][MV_X] <<= 2;
                tmp_mvd[2][MV_Y] <<= 2;
                if (amvr_shift > 0)
                {
                    com_mv_rounding_s32(tmp_mvd[0][MV_X], tmp_mvd[0][MV_Y], &tmp_mvd[0][MV_X], &tmp_mvd[0][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s32(tmp_mvd[1][MV_X], tmp_mvd[1][MV_Y], &tmp_mvd[1][MV_X], &tmp_mvd[1][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s32(tmp_mvd[2][MV_X], tmp_mvd[2][MV_Y], &tmp_mvd[2][MV_X], &tmp_mvd[2][MV_Y], amvr_shift, amvr_shift);
                }

                mvd[0][MV_X] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[0][MV_X]);
                mvd[0][MV_Y] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[0][MV_Y]);
                if (mvd[0][MV_X] == COM_INT16_MAX)
                {
                    mvd[0][MV_X] = (mvd[0][MV_X] >> amvr_shift) << amvr_shift;
                }
                if (mvd[0][MV_Y] == COM_INT16_MAX)
                {
                    mvd[0][MV_Y] = (mvd[0][MV_Y] >> amvr_shift) << amvr_shift;
                }

                mvd[1][MV_X] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[1][MV_X]);
                mvd[1][MV_Y] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[1][MV_Y]);
                if (mvd[1][MV_X] == COM_INT16_MAX)
                {
                    mvd[1][MV_X] = (mvd[1][MV_X] >> amvr_shift) << amvr_shift;
                }
                if (mvd[1][MV_Y] == COM_INT16_MAX)
                {
                    mvd[1][MV_Y] = (mvd[1][MV_Y] >> amvr_shift) << amvr_shift;
                }

                mvd[2][MV_X] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[2][MV_X]);
                mvd[2][MV_Y] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[2][MV_Y]);
                if (mvd[2][MV_X] == COM_INT16_MAX)
                {
                    mvd[2][MV_X] = (mvd[2][MV_X] >> amvr_shift) << amvr_shift;
                }
                if (mvd[2][MV_Y] == COM_INT16_MAX)
                {
                    mvd[2][MV_Y] = (mvd[2][MV_Y] >> amvr_shift) << amvr_shift;
                }
#else
                mvd[0][MV_X] = (s16)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
                mvd[0][MV_Y] = (s16)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_X] = (s16)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_Y] = (s16)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
                mvd[2][MV_X] = (s16)(delta_mv[1] * 4 + (delta_mv[4] >= 0 ? 0.5 : -0.5));
                mvd[2][MV_Y] = (s16)(delta_mv[3] * 4 + (delta_mv[5] >= 0 ? 0.5 : -0.5));
                mvd[0][MV_X] <<= 2; // 1/16-pixel
                mvd[0][MV_Y] <<= 2;
                mvd[1][MV_X] <<= 2;
                mvd[1][MV_Y] <<= 2;
                mvd[2][MV_X] <<= 2;
                mvd[2][MV_Y] <<= 2;
                if (amvr_shift > 0)
                {
                    com_mv_rounding_s16(mvd[0][MV_X], mvd[0][MV_Y], &mvd[0][MV_X], &mvd[0][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s16(mvd[1][MV_X], mvd[1][MV_Y], &mvd[1][MV_X], &mvd[1][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s16(mvd[2][MV_X], mvd[2][MV_Y], &mvd[2][MV_X], &mvd[2][MV_Y], amvr_shift, amvr_shift);
                }
#endif
            }
#else
            mvd[0][MV_X] = (s16)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
            mvd[0][MV_Y] = (s16)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
            mvd[1][MV_X] = (s16)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
            mvd[1][MV_Y] = (s16)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
            mvd[2][MV_X] = (s16)(delta_mv[4] * 4 + (delta_mv[4] >= 0 ? 0.5 : -0.5));
            mvd[2][MV_Y] = (s16)(delta_mv[5] * 4 + (delta_mv[5] >= 0 ? 0.5 : -0.5));
#endif
        }
        else
        {
            // for MV0
            delta_mv[0] = affine_para[0];
            delta_mv[2] = affine_para[2];
            // for MV1
            delta_mv[1] = affine_para[1] * cu_width + affine_para[0];
            delta_mv[3] = -affine_para[3] * cu_width + affine_para[2];
#if BD_AFFINE_AMVR
#if !ENC_ME_IMP
            u8 amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
#endif
            if (amvr_shift == 0)
            {
                mvd[0][MV_X] = (s16)(delta_mv[0] * 16 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
                mvd[0][MV_Y] = (s16)(delta_mv[2] * 16 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_X] = (s16)(delta_mv[1] * 16 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_Y] = (s16)(delta_mv[3] * 16 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
            }
            else
            {
#if ENC_ME_IMP
                CPMV tmp_mvd[VER_NUM][MV_D];
                tmp_mvd[0][MV_X] = (CPMV)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
                tmp_mvd[0][MV_Y] = (CPMV)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                tmp_mvd[1][MV_X] = (CPMV)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                tmp_mvd[1][MV_Y] = (CPMV)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
                tmp_mvd[0][MV_X] <<= 2; // 1/16-pixel
                tmp_mvd[0][MV_Y] <<= 2;
                tmp_mvd[1][MV_X] <<= 2;
                tmp_mvd[1][MV_Y] <<= 2;
                if (amvr_shift > 0)
                {
                    com_mv_rounding_s32(tmp_mvd[0][MV_X], tmp_mvd[0][MV_Y], &tmp_mvd[0][MV_X], &tmp_mvd[0][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s32(tmp_mvd[1][MV_X], tmp_mvd[1][MV_Y], &tmp_mvd[1][MV_X], &tmp_mvd[1][MV_Y], amvr_shift, amvr_shift);
                }
                mvd[0][MV_X] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[0][MV_X]);
                mvd[0][MV_Y] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[0][MV_Y]);
                if (mvd[0][MV_X] == COM_INT16_MAX)
                {
                    mvd[0][MV_X] = (mvd[0][MV_X] >> amvr_shift) << amvr_shift;
                }
                if (mvd[0][MV_Y] == COM_INT16_MAX)
                {
                    mvd[0][MV_Y] = (mvd[0][MV_Y] >> amvr_shift) << amvr_shift;
                }

                mvd[1][MV_X] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[1][MV_X]);
                mvd[1][MV_Y] = (s16)COM_CLIP3((s32)COM_INT16_MIN, (s32)COM_INT16_MAX, tmp_mvd[1][MV_Y]);
                if (mvd[1][MV_X] == COM_INT16_MAX)
                {
                    mvd[1][MV_X] = (mvd[1][MV_X] >> amvr_shift) << amvr_shift;
                }
                if (mvd[1][MV_Y] == COM_INT16_MAX)
                {
                    mvd[1][MV_Y] = (mvd[1][MV_Y] >> amvr_shift) << amvr_shift;
                }
#else
                mvd[0][MV_X] = (s16)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
                mvd[0][MV_Y] = (s16)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_X] = (s16)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
                mvd[1][MV_Y] = (s16)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
                mvd[0][MV_X] <<= 2;//  1/16-pixel
                mvd[0][MV_Y] <<= 2;
                mvd[1][MV_X] <<= 2;
                mvd[1][MV_Y] <<= 2;
                if (amvr_shift > 0)
                {
                    com_mv_rounding_s16(mvd[0][MV_X], mvd[0][MV_Y], &mvd[0][MV_X], &mvd[0][MV_Y], amvr_shift, amvr_shift);
                    com_mv_rounding_s16(mvd[1][MV_X], mvd[1][MV_Y], &mvd[1][MV_X], &mvd[1][MV_Y], amvr_shift, amvr_shift);
                }
#endif
            }
#else
            mvd[0][MV_X] = (s16)(delta_mv[0] * 4 + (delta_mv[0] >= 0 ? 0.5 : -0.5));
            mvd[0][MV_Y] = (s16)(delta_mv[2] * 4 + (delta_mv[2] >= 0 ? 0.5 : -0.5));
            mvd[1][MV_X] = (s16)(delta_mv[1] * 4 + (delta_mv[1] >= 0 ? 0.5 : -0.5));
            mvd[1][MV_Y] = (s16)(delta_mv[3] * 4 + (delta_mv[3] >= 0 ? 0.5 : -0.5));
#endif
        }

        // check early terminate
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
            if (mvd[vertex][MV_X] != 0 || mvd[vertex][MV_Y] != 0)
            {
                all_zero = 0;
                break;
            }
            all_zero = 1;
        }
        if (all_zero)
        {
            break;
        }

        /* update mv */
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
#if ENC_ME_IMP
            if (mvt[vertex][MV_X] == COM_CPMV_MAX)
            {
                mvt[vertex][MV_X] = (mvt[vertex][MV_X] >> amvr_shift) << amvr_shift;
            }
            if (mvt[vertex][MV_Y] == COM_CPMV_MAX)
            {
                mvt[vertex][MV_Y] = (mvt[vertex][MV_Y] >> amvr_shift) << amvr_shift;
            }
#endif
            s32 mvx = (s32)mvt[vertex][MV_X] + (s32)mvd[vertex][MV_X];
            s32 mvy = (s32)mvt[vertex][MV_Y] + (s32)mvd[vertex][MV_Y];
            mvt[vertex][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvx);
            mvt[vertex][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvy);
#if BD_AFFINE_AMVR
            // after clipping, last 2/4 bits of mv may not be zero when amvr_shift is 2/4, perform rounding without offset
#if !ENC_ME_IMP
            u8 amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
#endif
            mvt[vertex][MV_X] = mvt[vertex][MV_X] >> amvr_shift << amvr_shift;
            mvt[vertex][MV_Y] = mvt[vertex][MV_Y] >> amvr_shift << amvr_shift;
#endif
        }
        /* do motion compensation with updated mv */
#if ASP
        com_affine_mc_l(info, x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#else
        com_affine_mc_l(x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#endif
        /* get mvd bits*/
        mv_bits = get_affine_mv_bits(mvt, mvp, pi->num_refp, ri, vertex_num
#if BD_AFFINE_AMVR
                                     , pi->curr_mvr
#endif
                                    );
        if (bi)
        {
            mv_bits += pi->mot_bits[1 - lidx];
        }
        cost = MV_COST(pi, mv_bits);
        /* get satd */
        cost += calc_satd_16b(cu_width, cu_height, org, pred, s_org, cu_width, bit_depth) >> bi;
        /* save best mv */
        if (cost < cost_best)
        {
            cost_best = cost;
            best_bits = mv_bits;
            for (vertex = 0; vertex < vertex_num; vertex++)
            {
                mv[vertex][MV_X] = mvt[vertex][MV_X];
                mv[vertex][MV_Y] = mvt[vertex][MV_Y];
            }
        }
    }
    return (cost_best - MV_COST(pi, best_bits));
}

static void analyze_affine_uni(ENC_CTX * ctx, ENC_CORE * core, CPMV aff_mv_L0L1[REFP_NUM][VER_NUM][MV_D], s8 *refi_L0L1, double * cost_L0L1, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int bit_depth = ctx->info.bit_depth_internal;
    int lidx;
    s8* refi;
    s8 refi_cur;
    u32 mecost, best_mecost;
#if FAST_LD
    u32 me_cost_l0[MAX_NUM_ACTIVE_REF_FRAME];  // Store temp L0 ME value (including Mobits cost)
    int mobits_l0[MAX_NUM_ACTIVE_REF_FRAME];
    int mobits_l1[MAX_NUM_ACTIVE_REF_FRAME];
    int best_l0_refi = -1;
#endif
    s8 t0, t1;
    s8 refi_temp = 0;
    CPMV(*affine_mvp)[MV_D], (*affine_mv)[MV_D];
    s16 (*affine_mvd)[MV_D];
    int vertex = 0;
    int vertex_num = 2;
    int mebits, best_bits = 0;
    u32 cost_trans[REFP_NUM][MAX_NUM_ACTIVE_REF_FRAME];
    CPMV tmp_mv_array[VER_NUM][MV_D];
    int memory_access;
    int allowed = 1;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
#if AFFINE_MEMORY_CONSTRAINT
    int mem = (cu_width + 7 + cu_width / 4) * (cu_height + 7 + cu_height / 4);
#else
    int mem = MAX_MEMORY_ACCESS_UNI * (1 << cu_width_log2) * (1 << cu_height_log2);
#endif

    int sub_w = 4;
    int sub_h = 4;
    if (ctx->info.pic_header.affine_subblock_size_idx > 0)
    {
        sub_w = 8;
        sub_h = 8;
    }

    /* AFFINE 4 parameters Motion Search */
    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    mod_info_curr->cu_mode = MODE_INTER;
    mod_info_curr->affine_flag = 1;
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
    for (lidx = 0; lidx <= ((pi->slice_type == SLICE_P) ? PRED_L0 : PRED_L1); lidx++)
    {
        init_inter_data(pi, core);
        refi = mod_info_curr->refi;
        affine_mv = mod_info_curr->affine_mv[lidx];
        affine_mvd = mod_info_curr->affine_mvd[lidx];
        pi->num_refp = (u8)ctx->rpm.num_refp[lidx];
        best_mecost = COM_UINT32_MAX;
        for (refi_cur = 0; refi_cur < pi->num_refp; refi_cur++)
        {
            affine_mvp = pi->affine_mvp_scale[lidx][refi_cur];
            mod_info_curr->refi[lidx] = refi_cur;
            com_get_affine_mvp_scaling(&ctx->info, mod_info_curr, ctx->refp, &ctx->map, ctx->ptr, lidx, affine_mvp, vertex_num
#if BD_AFFINE_AMVR
                                       , pi->curr_mvr
#endif
                                      );
#if ENC_ME_IMP
            if (!(ctx->param.fast_ld_me && lidx == PRED_L1 && ctx->info.pic_header.l1idx_to_l0idx[refi_cur] >= 0))
#endif 
            {
                u32 mvp_best = COM_UINT32_MAX;
                u32 mvp_temp = COM_UINT32_MAX;
                s8  refi_t[REFP_NUM];
                COM_PIC* refp = pi->refp[refi_cur][lidx].pic;
                pel *pred = pi->pred_buf;
#if OBMC
                pel *org = pi->org_obmc_affine;
                int s_org = cu_width;
#else
                pel *org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
                int s_org = pi->stride_org[Y_C];
#endif

                // get cost of mvp
#if ASP
                com_affine_mc_l(&ctx->info, x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, affine_mvp, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#else
                com_affine_mc_l(x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, affine_mvp, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#endif
                mvp_best = calc_satd_16b(cu_width, cu_height, org, pred, s_org, cu_width, bit_depth);
                mebits = get_affine_mv_bits(affine_mvp, affine_mvp, pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                                            , pi->curr_mvr
#endif
                                           );
                mvp_best += MV_COST(pi, mebits);

                // get cost of translational mv
                s16 mv_cliped[REFP_NUM][MV_D];
                mv_cliped[lidx][MV_X] = (s16)(pi->best_mv_uni[lidx][refi_cur][MV_X]);
                mv_cliped[lidx][MV_Y] = (s16)(pi->best_mv_uni[lidx][refi_cur][MV_Y]);

                refi_t[lidx] = refi_cur;
                refi_t[1 - lidx] = -1;
                mv_clip(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, refi_t, mv_cliped, mv_cliped);
                for (vertex = 0; vertex < vertex_num; vertex++)
                {
                    tmp_mv_array[vertex][MV_X] = mv_cliped[lidx][MV_X];
                    tmp_mv_array[vertex][MV_Y] = mv_cliped[lidx][MV_Y];
#if BD_AFFINE_AMVR
                    s32 tmp_mvx, tmp_mvy;
                    tmp_mvx = tmp_mv_array[vertex][MV_X] << 2;
                    tmp_mvy = tmp_mv_array[vertex][MV_Y] << 2;
                    if (pi->curr_mvr == 1)
                    {
                        com_mv_rounding_s32(tmp_mvx, tmp_mvy, &tmp_mvx, &tmp_mvy, 4, 4);
                    }
                    tmp_mv_array[vertex][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, tmp_mvx);
                    tmp_mv_array[vertex][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, tmp_mvy);
#endif
                }
#if ASP
                com_affine_mc_l(&ctx->info, x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, tmp_mv_array, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#else
                com_affine_mc_l(x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, tmp_mv_array, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#endif
                cost_trans[lidx][refi_cur] = calc_satd_16b(cu_width, cu_height, org, pred, s_org, cu_width, bit_depth);
                mebits = get_affine_mv_bits(tmp_mv_array, affine_mvp, pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                                            , pi->curr_mvr
#endif
                                           );
                mvp_temp = cost_trans[lidx][refi_cur] + MV_COST(pi, mebits);
                if (mvp_temp < mvp_best)
                {
                    for (vertex = 0; vertex < vertex_num; vertex++)
                    {
                        affine_mv[vertex][MV_X] = tmp_mv_array[vertex][MV_X];
                        affine_mv[vertex][MV_Y] = tmp_mv_array[vertex][MV_Y];
                    }
                }
                else
                {
                    for (vertex = 0; vertex < vertex_num; vertex++)
                    {
                        affine_mv[vertex][MV_X] = affine_mvp[vertex][MV_X];
                        affine_mv[vertex][MV_Y] = affine_mvp[vertex][MV_Y];
                    }
                }
            }

            /* affine motion search */
#if FAST_LD
            if (ctx->param.fast_ld_me && lidx == PRED_L1 && ctx->info.pic_header.l1idx_to_l0idx[refi_cur] >= 0)
            {
                for (vertex = 0; vertex < vertex_num; vertex++)
                {
                    affine_mv[vertex][MV_X] = pi->affine_mv_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][vertex][MV_X];
                    affine_mv[vertex][MV_Y] = pi->affine_mv_scale[REFP_0][ctx->info.pic_header.l1idx_to_l0idx[refi_cur]][vertex][MV_Y];
                }

                mecost = me_cost_l0[ctx->info.pic_header.l1idx_to_l0idx[refi_cur]];
            }
            else
            {
#if ASP
                mecost = pi->fn_affine_me(&ctx->info, pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, affine_mv, 0, vertex_num, sub_w, sub_h);
#else
                mecost = pi->fn_affine_me(pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, affine_mv, 0, vertex_num, sub_w, sub_h);
#endif
                if (ctx->param.fast_ld_me && lidx == PRED_L0)
                {
                    me_cost_l0[refi_cur] = mecost;
                }
            }
#else
#if ASP
            mecost = pi->fn_affine_me(&ctx->info, pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, affine_mv, 0, vertex_num, sub_w, sub_h);
#else
            mecost = pi->fn_affine_me(pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, affine_mv, 0, vertex_num, sub_w, sub_h);
#endif
#endif

            // update MVP bits
#if !ENC_ME_IMP
            t0 = (lidx == 0) ? refi_cur : REFI_INVALID;
            t1 = (lidx == 1) ? refi_cur : REFI_INVALID;
            SET_REFI(refi, t0, t1);
#endif
            mebits = get_affine_mv_bits(affine_mv, affine_mvp, pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                                        , pi->curr_mvr
#endif
                                       );

#if FAST_LD && !ENC_ME_IMP
            if (ctx->param.fast_ld_me)
            {
                if (lidx == REFP_0)
                {
                    mobits_l0[refi_cur] = mebits;
                }
                else
                {
                    mobits_l1[refi_cur] = mebits;
                }
            }
#endif
            mecost += MV_COST(pi, mebits);

#if ENC_ME_IMP
            if (!(ctx->param.fast_ld_me && lidx == PRED_L1 && ctx->info.pic_header.l1idx_to_l0idx[refi_cur] >= 0))
            {
#if OBMC
                pel *org = pi->org_obmc_affine;
                int s_org = cu_width;
#else
                pel*     org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
                int      s_org = pi->stride_org[Y_C];
#endif
                COM_PIC* refp = pi->refp[refi_cur][lidx].pic;
                pel*     pred = pi->pred_buf;
                assert(vertex_num == 2);

                u32  best_init_cost = COM_UINT32_MAX;
                CPMV best_init_mv[VER_NUM][MV_D];
                for (int i = 0; i < core->affMVListSize; i++)
                {
                    AffineMvInfo *mvInfo = core->affMVList + ((core->affMVListIdx - i - 1 + core->affMVListMaxSize) % (core->affMVListMaxSize));
                    int j = 0;
                    for (; j < i; j++)
                    {
                        AffineMvInfo *prevMvInfo = core->affMVList + ((core->affMVListIdx - j - 1 + core->affMVListMaxSize) % (core->affMVListMaxSize));
                        if ((mvInfo->affMVs[lidx][refi_cur][0][0] == prevMvInfo->affMVs[lidx][refi_cur][0][0]) && (mvInfo->affMVs[lidx][refi_cur][0][1] == prevMvInfo->affMVs[lidx][refi_cur][0][1]) && \
                            (mvInfo->affMVs[lidx][refi_cur][1][0] == prevMvInfo->affMVs[lidx][refi_cur][1][0]) && (mvInfo->affMVs[lidx][refi_cur][1][1] == prevMvInfo->affMVs[lidx][refi_cur][1][1]) && \
                            (mvInfo->x == prevMvInfo->x) && (mvInfo->y == prevMvInfo->y) && (mvInfo->w == prevMvInfo->w))
                        {
                            break;
                        }
                    }
                    if (j < i)
                    {
                        continue;
                    }

                    CPMV(*nbMv)[MV_D] = mvInfo->affMVs[lidx][refi_cur];
                    s32 mv_scale_hor = (s32)nbMv[0][MV_X] << 7;
                    s32 mv_scale_ver = (s32)nbMv[0][MV_Y] << 7;
                    s32 dmv_hor_x = (((s32)nbMv[1][MV_X] - (s32)nbMv[0][MV_X]) << 7) >> com_tbl_log2[mvInfo->w];
                    s32 dmv_hor_y = (((s32)nbMv[1][MV_Y] - (s32)nbMv[0][MV_Y]) << 7) >> com_tbl_log2[mvInfo->w];
                    s32 dmv_ver_x = -dmv_hor_y;
                    s32 dmv_ver_y = dmv_hor_x;

                    int amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
                    CPMV mvt_tmp[VER_NUM][MV_D];

                    mvt_tmp[0][MV_X] = mv_scale_hor + dmv_hor_x * (x - mvInfo->x) + dmv_ver_x * (y - mvInfo->y);
                    mvt_tmp[0][MV_Y] = mv_scale_ver + dmv_hor_y * (x - mvInfo->x) + dmv_ver_y * (y - mvInfo->y);
                    com_mv_rounding_s32(mvt_tmp[0][MV_X], mvt_tmp[0][MV_Y], &mvt_tmp[0][MV_X], &mvt_tmp[0][MV_Y], 7 + amvr_shift, amvr_shift);
                    mvt_tmp[0][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvt_tmp[0][MV_X]);
                    mvt_tmp[0][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvt_tmp[0][MV_Y]);

                    mvt_tmp[1][MV_X] = mv_scale_hor + dmv_hor_x * (x + cu_width - mvInfo->x) + dmv_ver_x * (y - mvInfo->y);
                    mvt_tmp[1][MV_Y] = mv_scale_ver + dmv_hor_y * (x + cu_width - mvInfo->x) + dmv_ver_y * (y - mvInfo->y);
                    com_mv_rounding_s32(mvt_tmp[1][MV_X], mvt_tmp[1][MV_Y], &mvt_tmp[1][MV_X], &mvt_tmp[1][MV_Y], 7 + amvr_shift, amvr_shift);
                    mvt_tmp[1][MV_X] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvt_tmp[1][MV_X]);
                    mvt_tmp[1][MV_Y] = (CPMV)COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mvt_tmp[1][MV_Y]);

                    for (vertex = 0; vertex < vertex_num; vertex++)
                    {
                        if (mvt_tmp[vertex][MV_X] == COM_CPMV_MAX)
                        {
                            mvt_tmp[vertex][MV_X] = mvt_tmp[vertex][MV_X] >> amvr_shift << amvr_shift;
                        }
                        if (mvt_tmp[vertex][MV_Y] == COM_CPMV_MAX)
                        {
                            mvt_tmp[vertex][MV_Y] = mvt_tmp[vertex][MV_Y] >> amvr_shift << amvr_shift;
                        }
                    }

                    if ((i != 0) && (mvt_tmp[0][0] == best_init_mv[0][0]) && (mvt_tmp[0][1] == best_init_mv[0][1]) && (mvt_tmp[1][0] == best_init_mv[1][0]) && (mvt_tmp[1][1] == best_init_mv[1][1]))
                    {
                        continue;
                    }

#if ASP
                    com_affine_mc_l(&ctx->info, x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt_tmp, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#else
                    com_affine_mc_l(x, y, refp->width_luma, refp->height_luma, cu_width, cu_height, mvt_tmp, refp, pred, vertex_num, sub_w, sub_h, bit_depth);
#endif
                    u32 curcost = calc_satd_16b(cu_width, cu_height, org, pred, s_org, cu_width, bit_depth);
                    int curBits = get_affine_mv_bits(mvt_tmp, affine_mvp, pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                        , pi->curr_mvr
#endif
                    );
                    curcost += MV_COST(pi, curBits);
                    if (curcost < best_init_cost)
                    {
                        best_init_cost = curcost;
                        for (vertex = 0; vertex < vertex_num; vertex++)
                        {
                            best_init_mv[vertex][MV_X] = mvt_tmp[vertex][MV_X];
                            best_init_mv[vertex][MV_Y] = mvt_tmp[vertex][MV_Y];
                        }
                    }
                }

                if (core->affMVListSize)
                {
#if ASP
                    u32 altcost = pi->fn_affine_me(&ctx->info, pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, best_init_mv, 0, vertex_num, sub_w, sub_h);
#else
                    u32 altcost = pi->fn_affine_me(pi, x, y, cu_width_log2, cu_height_log2, &refi_cur, lidx, affine_mvp, best_init_mv, 0, vertex_num, sub_w, sub_h);
#endif
                    int altbits = get_affine_mv_bits(best_init_mv, affine_mvp, pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                        , pi->curr_mvr
#endif
                    );
                    altcost += MV_COST(pi, altbits);

                    if (altcost < mecost)
                    {
                        mecost = altcost;
                        mebits = altbits;
                        for (vertex = 0; vertex < vertex_num; vertex++)
                        {
                            affine_mv[vertex][MV_X] = best_init_mv[vertex][MV_X];
                            affine_mv[vertex][MV_Y] = best_init_mv[vertex][MV_Y];
                        }
                    }
                }
            }
#if FAST_LD
            if (ctx->param.fast_ld_me)
            {
                if (lidx == REFP_0)
                {
                    me_cost_l0[refi_cur] = mecost - MV_COST(pi, mebits);
                    mobits_l0[refi_cur] = mebits;
                }
                else
                {
                    mobits_l1[refi_cur] = mebits;
                }
            }
#endif
#endif

            /* save affine per ref me results */
            for (vertex = 0; vertex < vertex_num; vertex++)
            {
                pi->affine_mv_scale[lidx][refi_cur][vertex][MV_X] = affine_mv[vertex][MV_X];
                pi->affine_mv_scale[lidx][refi_cur][vertex][MV_Y] = affine_mv[vertex][MV_Y];
            }
            if (mecost < best_mecost)
            {
                best_mecost = mecost;
                best_bits = mebits;
                refi_temp = refi_cur;
            }
        }
        /* save affine per list me results */
        refi_cur = refi_temp;
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
            affine_mv[vertex][MV_X] = pi->affine_mv_scale[lidx][refi_cur][vertex][MV_X];
            affine_mv[vertex][MV_Y] = pi->affine_mv_scale[lidx][refi_cur][vertex][MV_Y];
        }
        affine_mvp = pi->affine_mvp_scale[lidx][refi_cur];
        t0 = (lidx == 0) ? refi_cur : REFI_INVALID;
        t1 = (lidx == 1) ? refi_cur : REFI_INVALID;
        SET_REFI(refi, t0, t1);
        refi_L0L1[lidx] = refi_cur;

        /* get affine mvd */
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
            affine_mvd[vertex][MV_X] = affine_mv[vertex][MV_X] - affine_mvp[vertex][MV_X];
            affine_mvd[vertex][MV_Y] = affine_mv[vertex][MV_Y] - affine_mvp[vertex][MV_Y];
        }

        /* important: reset affine mv/mvd */
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
#if BD_AFFINE_AMVR
            // note: after clipping, mvd might not align with amvr index
            int amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
            affine_mvd[vertex][MV_X] = affine_mvd[vertex][MV_X] >> amvr_shift << amvr_shift;
            affine_mvd[vertex][MV_Y] = affine_mvd[vertex][MV_Y] >> amvr_shift << amvr_shift;
#endif
            // note: mvd = mv - mvp, but because of clipping, mv might not equal to mvp + mvd
            int mv_x = (s32)affine_mvd[vertex][MV_X] + affine_mvp[vertex][MV_X];
            int mv_y = (s32)affine_mvd[vertex][MV_Y] + affine_mvp[vertex][MV_Y];
            affine_mv[vertex][MV_X] = COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mv_x);
            affine_mv[vertex][MV_Y] = COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mv_y);
        }

        pi->mot_bits[lidx] = best_bits;
        affine_mv[2][MV_X] = affine_mv[0][MV_X] - (affine_mv[1][MV_Y] - affine_mv[0][MV_Y]) * (s16)cu_height / (s16)cu_width;
        affine_mv[2][MV_Y] = affine_mv[0][MV_Y] + (affine_mv[1][MV_X] - affine_mv[0][MV_X]) * (s16)cu_height / (s16)cu_width;
        affine_mv[3][MV_X] = affine_mv[1][MV_X] - (affine_mv[1][MV_Y] - affine_mv[0][MV_Y]) * (s16)cu_height / (s16)cu_width;
        affine_mv[3][MV_Y] = affine_mv[1][MV_Y] + (affine_mv[1][MV_X] - affine_mv[0][MV_X]) * (s16)cu_height / (s16)cu_width;

        for (vertex = 0; vertex < vertex_num; vertex++)
        {
            aff_mv_L0L1[lidx][vertex][MV_X] = affine_mv[vertex][MV_X];
            aff_mv_L0L1[lidx][vertex][MV_Y] = affine_mv[vertex][MV_Y];
        }
        allowed = 1;
#if AFFINE_MEMORY_CONSTRAINT
        memory_access = com_get_affine_memory_access(affine_mv, cu_width, cu_height, 2);
#else
        memory_access = com_get_affine_memory_access(affine_mv, cu_width, cu_height);
#endif
        if (memory_access > mem)
        {
            allowed = 0;
        }
        if (allowed)
        {
            assert(mod_info_curr->mv[REFP_0][MV_X] == 0);
            assert(mod_info_curr->mv[REFP_0][MV_Y] == 0);
#if FAST_LD
            if (ctx->param.fast_ld_me && lidx == PRED_L1 && refi_L0L1[REFP_0] == ctx->info.pic_header.l1idx_to_l0idx[refi_L0L1[REFP_1]]
                && (mobits_l1[refi_L0L1[REFP_1]] > mobits_l0[refi_L0L1[REFP_0]]))
            {
                continue;
            }
            else
            {
                cost_L0L1[lidx] = pinter_residue_rdo(ctx, core, 0
#if DMVR
                    , 0
#endif
                );
                check_best_mode(core, pi, cost_L0L1[lidx], cost_best);
            }
#else
            cost_L0L1[lidx] = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                                 , 0
#endif
            );
            check_best_mode(core, pi, cost_L0L1[lidx], cost_best);
#endif
        }
    }
#if ENC_ME_IMP
    if (pi->curr_mvr == 0)
    {
        insertAffUniMvCands(core, x, y, cu_width, cu_height, pi->affine_mv_scale);
        memcpy(core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].reused_affine_uni_mv, pi->affine_mv_scale, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * VER_NUM * MV_D * sizeof(CPMV));
        core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit = 1;
    }
#endif
}

static void analyze_affine_bi(ENC_CTX * ctx, ENC_CORE * core, CPMV aff_mv_L0L1[REFP_NUM][VER_NUM][MV_D], const s8 *refi_L0L1, const double * cost_L0L1, double *cost_best)
{
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    ENC_PINTER *pi = &ctx->pinter;
    int bit_depth = ctx->info.bit_depth_internal;
    s8         refi[REFP_NUM] = { REFI_INVALID, REFI_INVALID };
    u32        best_mecost = COM_UINT32_MAX;
    s8        refi_best = 0, refi_cur;
    int        changed = 0;
    u32        mecost;
    pel        *org;
    pel(*pred)[MAX_CU_DIM];
    int        vertex_num = 2;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width_log2 = mod_info_curr->cu_width_log2;
    int cu_height_log2 = mod_info_curr->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
    s8         t0, t1;
    double      cost;
    s8         lidx_ref, lidx_cnd;
    u8          mvp_idx = 0;
    int         i;
    int         vertex;
    int         mebits;
    int         memory_access[REFP_NUM];
#if AFFINE_MEMORY_CONSTRAINT
    int         mem = (cu_width + 7 + cu_width / 4) * (cu_height + 7 + cu_height / 4);
#else
    int         mem = MAX_MEMORY_ACCESS_BI * (1 << cu_width_log2) * (1 << cu_height_log2);
#endif
    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->pb_part, &mod_info_curr->pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, mod_info_curr->x_scu << 2, mod_info_curr->y_scu << 2, cu_width, cu_height, mod_info_curr->tb_part, &mod_info_curr->tb_info);
#endif
    mod_info_curr->cu_mode = MODE_INTER;
    init_inter_data(pi, core);
#if UNIFIED_HMVP_1
    mod_info_curr->mvap_flag = 0;
    mod_info_curr->sub_tmvp_flag = 0;
#endif
    if (cost_L0L1[REFP_0] <= cost_L0L1[REFP_1])
    {
        lidx_ref = REFP_0;
        lidx_cnd = REFP_1;
    }
    else
    {
        lidx_ref = REFP_1;
        lidx_cnd = REFP_0;
    }
    mod_info_curr->refi[REFP_0] = refi_L0L1[REFP_0];
    mod_info_curr->refi[REFP_1] = refi_L0L1[REFP_1];
    for (vertex = 0; vertex < vertex_num; vertex++)
    {
        mod_info_curr->affine_mv[lidx_ref][vertex][MV_X] = aff_mv_L0L1[lidx_ref][vertex][MV_X];
        mod_info_curr->affine_mv[lidx_ref][vertex][MV_Y] = aff_mv_L0L1[lidx_ref][vertex][MV_Y];
        mod_info_curr->affine_mv[lidx_cnd][vertex][MV_X] = aff_mv_L0L1[lidx_cnd][vertex][MV_X];
        mod_info_curr->affine_mv[lidx_cnd][vertex][MV_Y] = aff_mv_L0L1[lidx_cnd][vertex][MV_Y];
    }
    /* get MVP lidx_cnd */
    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
    pred = mod_info_curr->pred;
    t0 = (lidx_ref == REFP_0) ? mod_info_curr->refi[lidx_ref] : REFI_INVALID;
    t1 = (lidx_ref == REFP_1) ? mod_info_curr->refi[lidx_ref] : REFI_INVALID;
    SET_REFI(refi, t0, t1);
    for (i = 0; i < AFFINE_BI_ITER; i++)
    {
        /* predict reference */
#if ASP
        com_affine_mc_l(&ctx->info, x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[lidx_ref], pi->refp[refi[lidx_ref]][lidx_ref].pic, pred[0], vertex_num, 8, 8, bit_depth);
#else
        com_affine_mc_l(x, y, ctx->info.pic_width, ctx->info.pic_height, cu_width, cu_height, mod_info_curr->affine_mv[lidx_ref], pi->refp[refi[lidx_ref]][lidx_ref].pic, pred[0], vertex_num, 8, 8, bit_depth);
#endif

        get_org_bi(org, pred[Y_C], pi->stride_org[Y_C], cu_width, cu_height, pi->org_bi, cu_width, 0, 0);
        SWAP(refi[lidx_ref], refi[lidx_cnd], t0);
        SWAP(lidx_ref, lidx_cnd, t0);
        changed = 0;
        for (refi_cur = 0; refi_cur < pi->num_refp; refi_cur++)
        {
            refi[lidx_ref] = refi_cur;
#if ASP
            mecost = pi->fn_affine_me(&ctx->info, pi, x, y, cu_width_log2, cu_height_log2, &refi[lidx_ref], lidx_ref, \
                                      pi->affine_mvp_scale[lidx_ref][refi_cur], pi->affine_mv_scale[lidx_ref][refi_cur], 1, vertex_num, 8, 8);
#else
            mecost = pi->fn_affine_me(pi, x, y, cu_width_log2, cu_height_log2, &refi[lidx_ref], lidx_ref, \
                                      pi->affine_mvp_scale[lidx_ref][refi_cur], pi->affine_mv_scale[lidx_ref][refi_cur], 1, vertex_num, 8, 8);
#endif

            // update MVP bits
            mebits = get_affine_mv_bits(pi->affine_mv_scale[lidx_ref][refi_cur], pi->affine_mvp_scale[lidx_ref][refi_cur], pi->num_refp, refi_cur, vertex_num
#if BD_AFFINE_AMVR
                                        , pi->curr_mvr
#endif
                                       );
            mebits += pi->mot_bits[1 - lidx_ref]; // add opposite bits
            mecost += MV_COST(pi, mebits);

            if (mecost < best_mecost)
            {
                pi->mot_bits[lidx_ref] = mebits - pi->mot_bits[1 - lidx_ref];
                refi_best = refi_cur;
                best_mecost = mecost;
                changed = 1;
                t0 = (lidx_ref == REFP_0) ? refi_best : mod_info_curr->refi[lidx_cnd];
                t1 = (lidx_ref == REFP_1) ? refi_best : mod_info_curr->refi[lidx_cnd];
                SET_REFI(mod_info_curr->refi, t0, t1);
                for (vertex = 0; vertex < vertex_num; vertex++)
                {
                    mod_info_curr->affine_mv[lidx_ref][vertex][MV_X] = pi->affine_mv_scale[lidx_ref][refi_cur][vertex][MV_X];
                    mod_info_curr->affine_mv[lidx_ref][vertex][MV_Y] = pi->affine_mv_scale[lidx_ref][refi_cur][vertex][MV_Y];
                }
            }
        }
        t0 = (lidx_ref == REFP_0) ? refi_best : REFI_INVALID;
        t1 = (lidx_ref == REFP_1) ? refi_best : REFI_INVALID;
        SET_REFI(refi, t0, t1);
        if (!changed) break;
    }

    // get affine mvd
    for (vertex = 0; vertex < vertex_num; vertex++)
    {
        mod_info_curr->affine_mvd[REFP_0][vertex][MV_X] = mod_info_curr->affine_mv[REFP_0][vertex][MV_X] - pi->affine_mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][vertex][MV_X];
        mod_info_curr->affine_mvd[REFP_0][vertex][MV_Y] = mod_info_curr->affine_mv[REFP_0][vertex][MV_Y] - pi->affine_mvp_scale[REFP_0][mod_info_curr->refi[REFP_0]][vertex][MV_Y];
        mod_info_curr->affine_mvd[REFP_1][vertex][MV_X] = mod_info_curr->affine_mv[REFP_1][vertex][MV_X] - pi->affine_mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][vertex][MV_X];
        mod_info_curr->affine_mvd[REFP_1][vertex][MV_Y] = mod_info_curr->affine_mv[REFP_1][vertex][MV_Y] - pi->affine_mvp_scale[REFP_1][mod_info_curr->refi[REFP_1]][vertex][MV_Y];
    }

    /* important: reset affine mv/mvd */
    for (i = 0; i < REFP_NUM; i++)
    {
        for (vertex = 0; vertex < vertex_num; vertex++)
        {
#if BD_AFFINE_AMVR
            // note: after clipping, mvd might not align with amvr index
            int amvr_shift = Tab_Affine_AMVR(pi->curr_mvr);
            mod_info_curr->affine_mvd[i][vertex][MV_X] = mod_info_curr->affine_mvd[i][vertex][MV_X] >> amvr_shift << amvr_shift;
            mod_info_curr->affine_mvd[i][vertex][MV_Y] = mod_info_curr->affine_mvd[i][vertex][MV_Y] >> amvr_shift << amvr_shift;
#endif
            // note: mvd = mv - mvp, but because of clipping, mv might not equal to mvp + mvd
            int mv_x = (s32)mod_info_curr->affine_mvd[i][vertex][MV_X] + pi->affine_mvp_scale[i][mod_info_curr->refi[i]][vertex][MV_X];
            int mv_y = (s32)mod_info_curr->affine_mvd[i][vertex][MV_Y] + pi->affine_mvp_scale[i][mod_info_curr->refi[i]][vertex][MV_Y];
            mod_info_curr->affine_mv[i][vertex][MV_X] = COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mv_x);
            mod_info_curr->affine_mv[i][vertex][MV_Y] = COM_CLIP3(COM_CPMV_MIN, COM_CPMV_MAX, mv_y);
        }
    }

    for (i = 0; i < REFP_NUM; i++)
    {
#if AFFINE_MEMORY_CONSTRAINT
        memory_access[i] = com_get_affine_memory_access(mod_info_curr->affine_mv[i], cu_width, cu_height, vertex_num);
#else
        if (vertex_num == 3)
        {
            mod_info_curr->affine_mv[i][3][MV_X] = mod_info_curr->affine_mv[i][1][MV_X] + mod_info_curr->affine_mv[i][2][MV_X] - mod_info_curr->affine_mv[i][0][MV_X];
            mod_info_curr->affine_mv[i][3][MV_Y] = mod_info_curr->affine_mv[i][1][MV_Y] + mod_info_curr->affine_mv[i][2][MV_Y] - mod_info_curr->affine_mv[i][0][MV_Y];
        }
        else
        {
            mod_info_curr->affine_mv[i][2][MV_X] = mod_info_curr->affine_mv[i][0][MV_X] - (mod_info_curr->affine_mv[i][1][MV_Y] - mod_info_curr->affine_mv[i][0][MV_Y]) * (s16)cu_height / (s16)cu_width;
            mod_info_curr->affine_mv[i][2][MV_Y] = mod_info_curr->affine_mv[i][0][MV_Y] + (mod_info_curr->affine_mv[i][1][MV_X] - mod_info_curr->affine_mv[i][0][MV_X]) * (s16)cu_height / (s16)cu_width;
            mod_info_curr->affine_mv[i][3][MV_X] = mod_info_curr->affine_mv[i][1][MV_X] - (mod_info_curr->affine_mv[i][1][MV_Y] - mod_info_curr->affine_mv[i][0][MV_Y]) * (s16)cu_height / (s16)cu_width;
            mod_info_curr->affine_mv[i][3][MV_Y] = mod_info_curr->affine_mv[i][1][MV_Y] + (mod_info_curr->affine_mv[i][1][MV_X] - mod_info_curr->affine_mv[i][0][MV_X]) * (s16)cu_height / (s16)cu_width;
        }
        memory_access[i] = com_get_affine_memory_access(mod_info_curr->affine_mv[i], cu_width, cu_height);
#endif
    }
    if (memory_access[0] > mem || memory_access[1] > mem)
    {
        return;
    }
    else
    {
        cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                                  , 0
#endif
        );
        check_best_mode(core, pi, cost, cost_best);
#if BGC
        if (cost <= 1.2 * (*cost_best) && ctx->info.sqh.bgc_enable_flag && cu_width * cu_height >= 256 && REFI_IS_VALID(mod_info_curr->refi[REFP_0]) && REFI_IS_VALID(mod_info_curr->refi[REFP_1]))
        {
            mod_info_curr->bgc_flag = 1;
            mod_info_curr->bgc_idx = 0;
             cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                , 0
#endif
            );
            check_best_mode(core, pi, cost, cost_best);

            mod_info_curr->bgc_idx = 1;
            cost = pinter_residue_rdo(ctx, core, 0
#if DMVR
                , 0
#endif
            );
            check_best_mode(core, pi, cost, cost_best);

            mod_info_curr->bgc_flag = 0;
            mod_info_curr->bgc_idx = 0;
        }
#endif
    }
}

// the best motion vector recorded will be used to determine the search range in the next MVR loop
static void update_best_int_mv(COM_MODE *bst_info, ENC_PINTER *pi)
{
    if (abs(bst_info->mv[REFP_0][MV_X]) > abs(bst_info->mv[REFP_1][MV_X]))
    {
        pi->max_imv[MV_X] = max(abs(pi->max_imv[MV_X]), (abs(bst_info->mv[REFP_0][MV_X])) >> 2);
        if (bst_info->mv[REFP_0][MV_X] < 0)
        {
            pi->max_imv[MV_X] = -1 * pi->max_imv[MV_X];
        }
    }
    else
    {
        pi->max_imv[MV_X] = max(abs(pi->max_imv[MV_X]), (abs(bst_info->mv[REFP_1][MV_X])) >> 2);
        if (bst_info->mv[REFP_1][MV_X] < 0)
        {
            pi->max_imv[MV_X] = -1 * pi->max_imv[MV_X];
        }
    }

    if (abs(bst_info->mv[REFP_0][MV_Y]) > abs(bst_info->mv[REFP_1][MV_Y]))
    {
        pi->max_imv[MV_Y] = max(abs(pi->max_imv[MV_Y]), (abs(bst_info->mv[REFP_0][MV_Y])) >> 2);
        if (bst_info->mv[REFP_0][MV_Y] < 0)
        {
            pi->max_imv[MV_Y] = -1 * pi->max_imv[MV_Y];
        }
    }
    else
    {
        pi->max_imv[MV_Y] = max(abs(pi->max_imv[MV_Y]), (abs(bst_info->mv[REFP_1][MV_Y])) >> 2);
        if (bst_info->mv[REFP_1][MV_Y] < 0)
        {
            pi->max_imv[MV_Y] = -1 * pi->max_imv[MV_Y];
        }
    }
}

#if EXT_AMVR_HMVP
static int same_mvp(ENC_CORE *core, ENC_CTX *ctx, COM_MOTION hmvp_motion)
{
    COM_MOTION c_motion;
    COM_MODE *mod_info_curr = &core->mod_info_curr;
    int x_scu = mod_info_curr->x_scu;
    int y_scu = mod_info_curr->y_scu;
    int cu_width_in_scu = mod_info_curr->cu_width >> MIN_CU_LOG2;
    int pic_width_in_scu = ctx->info.pic_width_in_scu;
    u32 *map_scu = ctx->map.map_scu;
    int cnt_hmvp = core->cnt_hmvp_cands;

    int neb_addr = mod_info_curr->scup - ctx->info.pic_width_in_scu + cu_width_in_scu;
    int neb_avaliable = y_scu > 0 && x_scu + cu_width_in_scu < pic_width_in_scu && MCU_GET_CODED_FLAG(map_scu[neb_addr]) && !MCU_GET_INTRA_FLAG(map_scu[neb_addr]);
    neb_avaliable = neb_avaliable && !MCU_GET_IBC(map_scu[neb_addr]);
    if (neb_avaliable && cnt_hmvp)
    {
        c_motion.mv[0][0] = ctx->map.map_mv[neb_addr][0][0];
        c_motion.mv[0][1] = ctx->map.map_mv[neb_addr][0][1];
        c_motion.mv[1][0] = ctx->map.map_mv[neb_addr][1][0];
        c_motion.mv[1][1] = ctx->map.map_mv[neb_addr][1][1];

        c_motion.ref_idx[0] = ctx->map.map_refi[neb_addr][0];
        c_motion.ref_idx[1] = ctx->map.map_refi[neb_addr][1];
        return same_motion(hmvp_motion, c_motion);
    }
    return 0;
}
#endif

#if OBMC
#if OBMC_TEMP
void enc_subtract_obmc(int obmc_template_flag,pel *yuvPredOrg, int predOrgStride, pel *yuvPredOBMC, int predOBMCStride, int width, int height, int dir, pel *weight, int bit_depth)
#else
void enc_subtract_obmc(pel *yuvPredOrg, int predOrgStride, pel *yuvPredOBMC, int predOBMCStride, int width, int height, int dir, pel *weight, int bit_depth)
#endif
{
    assert(dir == 0 || dir == 1);

    pel *pOrg = yuvPredOrg;
    pel *pOBMC = yuvPredOBMC;

    if (dir == 0) //above
    {
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
#if OBMC_TEMP
                if (!obmc_template_flag)
                {
                    pOrg[i] = (pel)((64.0 * (double)pOrg[i] - (double)(64 - weight[j]) * (double)pOBMC[i]) / (double)weight[j] + 0.5);
                }
#else
                pOrg[i] = (pel)((64.0 * (double)pOrg[i] - (double)(64 - weight[j]) * (double)pOBMC[i]) / (double)weight[j] + 0.5);

#endif
            }
            pOrg += predOrgStride;
            pOBMC += predOBMCStride;
        }
    }

    if (dir == 1) //left
    {
        for (int i = 0; i < width; i++)
        {
            for (int j = 0; j < height; j++)
            {
#if OBMC_TEMP
                if (!obmc_template_flag)
                {
                pOrg[i + j * predOrgStride] = (pel)((64.0 * (double)pOrg[i + j * predOrgStride] - (double)(64 - weight[i]) * (double)pOBMC[i + j * predOBMCStride]) / (double)weight[i] + 0.5);
                }
#else
                pOrg[i + j * predOrgStride] = (pel)((64.0 * (double)pOrg[i + j * predOrgStride] - (double)(64 - weight[i]) * (double)pOBMC[i + j * predOBMCStride]) / (double)weight[i] + 0.5);
#endif
            }
        }
    }
}

void enc_exclude_obmc(ENC_CTX *ctx, ENC_CORE *core, BOOL enable_affine)
{
    COM_INFO *info = &ctx->info;
    COM_MAP  *pic_map = &ctx->map;
    ENC_PINTER *pi = &ctx->pinter;
    COM_REFP(*refp)[REFP_NUM] = pi->refp;
    int cur_ptr = ctx->ptr;

    COM_MODE* mod_info_curr = &core->mod_info_curr;
    int x = mod_info_curr->x_pos;
    int y = mod_info_curr->y_pos;
    int cu_width = mod_info_curr->cu_width;
    int cu_height = mod_info_curr->cu_height;

    if (info->pic_header.slice_type == SLICE_P)
    {
        return;
    }

    if ((cu_width * cu_height) < 64)
    {
        return;
    }

    int widthInBlock = (cu_width >> MIN_CU_LOG2);
    int heightInBlock = (cu_height >> MIN_CU_LOG2);
    int bit_depth = info->bit_depth_internal;

    s8  org_refi[REFP_NUM] = { mod_info_curr->refi[REFP_0], mod_info_curr->refi[REFP_1] };
    s16 org_mv[REFP_NUM][MV_D] = { {mod_info_curr->mv[REFP_0][MV_X], mod_info_curr->mv[REFP_0][MV_Y]}, {mod_info_curr->mv[REFP_1][MV_X], mod_info_curr->mv[REFP_1][MV_Y]} };

    int half_ob_width = (cu_width >> 1);
    int half_ob_height = (cu_height >> 1);
    int log2_half_w = com_tbl_log2[half_ob_width];
    int log2_half_h = com_tbl_log2[half_ob_height];

    int affine_sub_w = 4, affine_sub_h = 4, half_ob_affine_width = 2, half_ob_affine_height = 2, log2_half_affine_w = 1, log2_half_affine_h = 1;
    if (enable_affine)
    {
        if (ctx->info.pic_header.affine_subblock_size_idx > 0)
        {
            affine_sub_w = 8;
            affine_sub_h = 8;
        }
        else
        {
            affine_sub_w = 4;
            affine_sub_h = 4;
        }
        half_ob_affine_width = (affine_sub_w >> 1);
        half_ob_affine_height = (affine_sub_h >> 1);
        log2_half_affine_w = com_tbl_log2[half_ob_affine_width];
        log2_half_affine_h = com_tbl_log2[half_ob_affine_height];

        assert(half_ob_affine_width <= half_ob_width);
        assert(half_ob_affine_height <= half_ob_height);
    }


    pel *weight_above = NULL, *weight_left = NULL;
    pel *weight_above_affine = NULL, *weight_left_affine = NULL;
    if (info->pic_header.obmc_blending_flag)
    {
        weight_above = info->obmc_weight[1][log2_half_h];
        weight_left = info->obmc_weight[1][log2_half_w];
        if (enable_affine)
        {
            weight_above_affine = info->obmc_weight[1][log2_half_affine_h];
            weight_left_affine = info->obmc_weight[1][log2_half_affine_w];
        }
    }
    else
    {
        weight_above = info->obmc_weight[0][log2_half_h];
        weight_left = info->obmc_weight[0][log2_half_w];
        if (enable_affine)
        {
            weight_above_affine = info->obmc_weight[0][log2_half_affine_h];
            weight_left_affine = info->obmc_weight[0][log2_half_affine_w];
        }
    }

    for (int blkBoundaryType = 0; blkBoundaryType < 2; blkBoundaryType++)  // 0 - top; 1 - left
    {
        int lengthInBlock = ((blkBoundaryType == 0) ? widthInBlock : heightInBlock);
        int subIdx = 0;
        int offsetX_in_unit = 0, offsetY_in_unit = 0;
        MOTION_MERGE_TYPE state = INVALID_NEIGH;

        BOOL same_blending = enable_affine && (((blkBoundaryType == 0) && (half_ob_height == half_ob_affine_height)) || ((blkBoundaryType == 1) && (half_ob_width == half_ob_affine_width)));
        while (subIdx < lengthInBlock)
        {
            int length = 0;
            if (blkBoundaryType == 0)
            {
                offsetX_in_unit = subIdx;
                offsetY_in_unit = 0;
            }
            else
            {
                offsetX_in_unit = 0;
                offsetY_in_unit = subIdx;
            }

            s16 neigh_mv[REFP_NUM][MV_D];
            s8 neigh_refi[REFP_NUM];

            state = getSameNeigMotion(info, mod_info_curr, pic_map, neigh_mv, neigh_refi, offsetX_in_unit, offsetY_in_unit, blkBoundaryType, &length, lengthInBlock - subIdx, FALSE, cur_ptr, refp);
            if (state == CONSECUTIVE_INTER)
            {
                int sub_blk_x = x + ((blkBoundaryType == 0) ? (subIdx << MIN_CU_LOG2) : 0);
                int sub_blk_y = y + ((blkBoundaryType == 0) ? 0 : (subIdx << MIN_CU_LOG2));
                int sub_blk_w = ((blkBoundaryType == 0) ? (length << MIN_CU_LOG2) : half_ob_width);
                int sub_blk_h = ((blkBoundaryType == 0) ? half_ob_height : (length << MIN_CU_LOG2));

                mod_info_curr->mv[REFP_0][MV_X] = neigh_mv[REFP_0][MV_X];
                mod_info_curr->mv[REFP_0][MV_Y] = neigh_mv[REFP_0][MV_Y];
                mod_info_curr->refi[REFP_0] = neigh_refi[REFP_0];
                mod_info_curr->mv[REFP_1][MV_X] = neigh_mv[REFP_1][MV_X];
                mod_info_curr->mv[REFP_1][MV_Y] = neigh_mv[REFP_1][MV_Y];
                mod_info_curr->refi[REFP_1] = neigh_refi[REFP_1];

                pel *pc_subblk_buffer[N_C] = { &(info->subblk_obmc_buf[Y_C][0]) + (sub_blk_x - x) + (sub_blk_y - y) * cu_width, NULL, NULL };
                com_subblk_obmc(sub_blk_x, sub_blk_y, sub_blk_w, sub_blk_h, cu_width, pc_subblk_buffer, info, mod_info_curr, refp, TRUE, FALSE, bit_depth
#if BGC
                    , 0, 0
#endif
                );

                pel *pc_dst_buffer_luma = pi->org_obmc + (sub_blk_x - x) + (sub_blk_y - y) * cu_width;
#if OBMC_TEMP
                enc_subtract_obmc(info->sqh.obmc_template_enable_flag,pc_dst_buffer_luma, cu_width, pc_subblk_buffer[Y_C], cu_width, sub_blk_w, sub_blk_h, blkBoundaryType, ((blkBoundaryType == 0) ? weight_above : weight_left), bit_depth);
#else
                enc_subtract_obmc(pc_dst_buffer_luma, cu_width, pc_subblk_buffer[Y_C], cu_width, sub_blk_w, sub_blk_h, blkBoundaryType, ((blkBoundaryType == 0) ? weight_above : weight_left), bit_depth);
#endif
                if (enable_affine && !same_blending)
                {
                    int sub_blk_affine_w = ((blkBoundaryType == 0) ? (length << MIN_CU_LOG2) : half_ob_affine_width);
                    int sub_blk_affine_h = ((blkBoundaryType == 0) ? half_ob_affine_height : (length << MIN_CU_LOG2));

                    pel *pc_dst_buffer_affine_luma = pi->org_obmc_affine + (sub_blk_x - x) + (sub_blk_y - y) * cu_width;
#if OBMC_TEMP
                    enc_subtract_obmc(info->sqh.obmc_template_enable_flag,pc_dst_buffer_affine_luma, cu_width, pc_subblk_buffer[Y_C], cu_width, sub_blk_affine_w, sub_blk_affine_h, blkBoundaryType, ((blkBoundaryType == 0) ? weight_above_affine : weight_left_affine), bit_depth);
#else                    
                    enc_subtract_obmc(pc_dst_buffer_affine_luma, cu_width, pc_subblk_buffer[Y_C], cu_width, sub_blk_affine_w, sub_blk_affine_h, blkBoundaryType, ((blkBoundaryType == 0) ? weight_above_affine : weight_left_affine), bit_depth);
#endif
                }

                mod_info_curr->mv[REFP_0][MV_X] = org_mv[REFP_0][MV_X];
                mod_info_curr->mv[REFP_0][MV_Y] = org_mv[REFP_0][MV_Y];
                mod_info_curr->refi[REFP_0] = org_refi[REFP_0];
                mod_info_curr->mv[REFP_1][MV_X] = org_mv[REFP_1][MV_X];
                mod_info_curr->mv[REFP_1][MV_Y] = org_mv[REFP_1][MV_Y];
                mod_info_curr->refi[REFP_1] = org_refi[REFP_1];

                subIdx += length;
            }
            else if (state == CUR_BI_PRED)
            {
                subIdx += length;
            }
            else if ((state == CONSECUTIVE_INTRA) || (state == CONSECUTIVE_IBC))
            {
                subIdx += length;
            }
            else
            {
                subIdx += lengthInBlock;
            }
        }
        if (same_blending)
        {
            pel *pc_dst_buffer_luma = pi->org_obmc;
            pel *pc_dst_buffer_affine_luma = pi->org_obmc_affine;
            int blk_width = ((blkBoundaryType == 0) ? cu_width : half_ob_width);
            int blk_height = ((blkBoundaryType == 0) ? half_ob_height : cu_height);
            for (int h = 0; h < blk_height; h++)
            {
                memcpy(pc_dst_buffer_affine_luma, pc_dst_buffer_luma, blk_width * sizeof(pel));
                pc_dst_buffer_luma += cu_width;
                pc_dst_buffer_affine_luma += cu_width;
            }
        }
        assert(subIdx == lengthInBlock);
    }
}
#endif

double analyze_inter_cu(ENC_CTX *ctx, ENC_CORE *core)
{
    ENC_PINTER *pi = &ctx->pinter;
    COM_MODE *cur_info = &core->mod_info_curr;
    COM_MODE *bst_info = &core->mod_info_best;
    int bit_depth = ctx->info.bit_depth_internal;
    int i, j;
    static s16  coef_blk[N_C][MAX_CU_DIM];
    static s16  resi[N_C][MAX_CU_DIM];
    double cost_best = MAX_COST;
    int cu_width_log2 = cur_info->cu_width_log2;
    int cu_height_log2 = cur_info->cu_height_log2;
    int cu_width = 1 << cu_width_log2;
    int cu_height = 1 << cu_height_log2;
#if BGC
    cur_info->bgc_flag = 0;
    cur_info->bgc_idx = 0;
#endif

#if TB_SPLIT_EXT
    init_pb_part(&core->mod_info_curr);
    init_tb_part(&core->mod_info_curr);
    get_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.pb_part, &core->mod_info_curr.pb_info);
#if DT_TRANSFORM
    get_tb_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#else
    get_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#endif
#endif

#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
    int num_iter_mvp = 2;
    int num_hmvp_inter = MAX_NUM_MVR;
    if (ctx->info.sqh.emvr_enable_flag) // also imply (ctx->info.sqh.num_of_hmvp_cand && ctx->info.sqh.amvr_enable_flag) is true
    {
        if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
        {
            num_hmvp_inter = core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].mvr_hmvp_idx_history + 1;
            if (num_hmvp_inter > MAX_NUM_MVR)
            {
                num_hmvp_inter = MAX_NUM_MVR;
            }
        }
    }
#endif

    int allow_amvr_emvr = 1;
    if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].amvr_emvr_flag_history == 0)
        {
            allow_amvr_emvr = 0;
        }
    }

    int size = sizeof(u8) * REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * 2;
    com_mset(pi->imv_valid, 0, size);

    int num_amvr = MAX_NUM_MVR;
    if (ctx->info.sqh.amvr_enable_flag)
    {
        if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
        {
            num_amvr = core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].mvr_idx_history + 1;
            if (num_amvr > MAX_NUM_MVR)
            {
                num_amvr = MAX_NUM_MVR;
            }
        }
    }
    else
    {
        num_amvr = 1; /* only allow 1/4 pel of resolution */
    }
    pi->curr_mvr = 0;

    int allow_affine = ctx->info.sqh.affine_enable_flag;
    if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].affine_flag_history == 0)
        {
            allow_affine = 0;
        }
    }
    // init translation mv for affine
    for (i = 0; i < REFP_NUM; i++)
    {
        for (j = 0; j < MAX_NUM_ACTIVE_REF_FRAME; j++)
        {
            pi->best_mv_uni[i][j][MV_X] = 0;
            pi->best_mv_uni[i][j][MV_Y] = 0;
        }
    }

#if OBMC
    pel *org_luma = pi->Yuv_org[Y_C] + cur_info->x_pos + cur_info->y_pos * pi->stride_org[Y_C];
    pel *obmc_luma = pi->org_obmc;
    pel *obmc_luma_affine = pi->org_obmc_affine;

#if DISABLE_OBMC_AFFINE
    BOOL enable_affine = FALSE;
#else
    BOOL enable_affine = (allow_affine && cu_width >= AFF_SIZE && cu_height >= AFF_SIZE);
#endif

    for (i = 0; i < cu_height; i++)
    {
        memcpy(obmc_luma, org_luma, cu_width * sizeof(pel));
        obmc_luma += cu_width;
#if !DISABLE_OBMC_AFFINE
        if (enable_affine)
#endif
        {
            memcpy(obmc_luma_affine, org_luma, cu_width * sizeof(pel));
            obmc_luma_affine += cu_width;
        }
        org_luma += pi->stride_org[Y_C];
    }
    if (ctx->info.sqh.obmc_enable_flag)
    {
        enc_exclude_obmc(ctx, core, enable_affine);
    }
#endif

#if INTER_CU_CONSTRAINT
    if (cu_width * cu_height >= 64)
    {
#endif
        analyze_direct_skip(ctx, core, &cost_best);
#if INTER_CU_CONSTRAINT
    }
#endif

#if INTERPF //after this, no inter mode function will use inter filter
    cur_info->inter_filter_flag = 0;
#endif
#if IPC
    cur_info->ipc_flag = 0;
#endif

    if (allow_amvr_emvr)
    {
#if FAST_EXT_AMVR_HMVP
        pi->mv_cands_uni_max_size = 16;
        memset(pi->mv_cands_uni_size, 0, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * sizeof(int));
        memset(pi->mv_cands_uni_cost, COM_UINT32_MAX, REFP_NUM * MAX_NUM_ACTIVE_REF_FRAME * pi->mv_cands_uni_max_size * sizeof(s32));
#endif

#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
        for (cur_info->mvp_from_hmvp_flag = 0; cur_info->mvp_from_hmvp_flag < num_iter_mvp; cur_info->mvp_from_hmvp_flag++)
        {
            pi->mvp_from_hmvp_flag = cur_info->mvp_from_hmvp_flag;
            if (cur_info->mvp_from_hmvp_flag)
            {
                num_amvr = 0;
                if (ctx->info.sqh.emvr_enable_flag) // also imply (ctx->info.sqh.num_of_hmvp_cand && ctx->info.sqh.amvr_enable_flag) is true
                {
                    if ((bst_info->cu_mode == MODE_SKIP && core->skip_mvps_check == 0) || (bst_info->cu_mode != MODE_SKIP))
                    {
                        num_amvr = min(num_hmvp_inter, core->cnt_hmvp_cands);
                        pi->max_imv[0] = 0;
                        pi->max_imv[1] = 0;
                    }
                }
            }
#endif

            for (pi->curr_mvr = 0; pi->curr_mvr < num_amvr; pi->curr_mvr++)
            {
                double cost_L0L1[2] = { MAX_COST, MAX_COST };
                s8 refi_L0L1[2] = { REFI_INVALID, REFI_INVALID };
                s16 mv_L0L1[REFP_NUM][MV_D];
#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
                if (cur_info->mvp_from_hmvp_flag)
                {
                    if (same_mvp(core, ctx, core->motion_cands[core->cnt_hmvp_cands - 1 - pi->curr_mvr]))
                    {
                        continue;
                    }
                    if (bst_info->cu_mode == MODE_SKIP && bst_info->skip_idx >= TRADITIONAL_SKIP_NUM && bst_info->skip_idx - TRADITIONAL_SKIP_NUM == pi->curr_mvr)
                    {
                        continue;
                    }
                }
#endif
#if TB_SPLIT_EXT
                init_pb_part(&core->mod_info_curr);
                init_tb_part(&core->mod_info_curr);
                get_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.pb_part, &core->mod_info_curr.pb_info);
#if DT_TRANSFORM
                get_tb_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#else
                get_part_info(ctx->info.pic_width_in_scu, cur_info->x_scu << 2, cur_info->y_scu << 2, cu_width, cu_height, core->mod_info_curr.tb_part, &core->mod_info_curr.tb_info);
#endif
#endif
                analyze_uni_pred(ctx, core, cost_L0L1, mv_L0L1, refi_L0L1, &cost_best);
#if INTER_CU_CONSTRAINT
                if (pi->slice_type == SLICE_B && cu_width * cu_height >= 64)
#else
                if (pi->slice_type == SLICE_B) // bi-prediction
#endif
                {
                    analyze_bi(ctx, core, mv_L0L1, refi_L0L1, cost_L0L1, &cost_best);
#if SMVD
                    int allow_smvd = ctx->info.sqh.smvd_enable_flag;
                    if( core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit )
                    {
                        if( core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].smvd_flag_history == 0 )
                        {
                            allow_smvd = 0;
                        }
                    }

                    if (ctx->info.sqh.smvd_enable_flag && ctx->ptr - ctx->refp[0][REFP_0].ptr == ctx->refp[0][REFP_1].ptr - ctx->ptr
                    && !cur_info->mvp_from_hmvp_flag
                    && allow_smvd
                   )
                    {
                        analyze_smvd(ctx, core, &cost_best);
                    }
#endif
                }

#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
                if (cur_info->mvp_from_hmvp_flag)
                {
                    if (pi->curr_mvr >= SKIP_MVR_IDX + 1 && (core->mod_info_best.cu_mode == MODE_SKIP))
                    {
                        break;
                    }
                }
                else
#endif
                    if (pi->curr_mvr >= SKIP_MVR_IDX && ((core->mod_info_best.cu_mode == MODE_SKIP || core->mod_info_best.cu_mode == MODE_DIR)))
                    {
                        break;
                    }

                if (pi->curr_mvr >= FAST_MVR_IDX)
                {
                    if (abs(bst_info->mvd[REFP_0][MV_X]) <= 0 &&
                        abs(bst_info->mvd[REFP_0][MV_Y]) <= 0 &&
                        abs(bst_info->mvd[REFP_1][MV_X]) <= 0 &&
                        abs(bst_info->mvd[REFP_1][MV_Y]) <= 0)
                    {
                        break;
                    }
                }
                update_best_int_mv(bst_info, pi);
            }
#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
        }
#endif
    }
#if INTER_ME_MVLIB
    else
    {
        for (int i = 0; i < MAX_NUM_MVR; i++)
        {
            if (core->s_enc_me_mvlib[i].uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
            {
                insertUniMvCands(&core->s_enc_me_mvlib[i], cur_info->x_pos, cur_info->y_pos, cu_width, cu_height, core->s_enc_me_mvlib[i].uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].reused_uni_mv);
            }
        }
    }
#endif
    double cost_L0L1[2] = { MAX_COST, MAX_COST };
    s8 refi_L0L1[2] = { REFI_INVALID, REFI_INVALID };
    CPMV aff_mv_L0L1[REFP_NUM][VER_NUM][MV_D];

    if (allow_affine && cu_width >= AFF_SIZE && cu_height >= AFF_SIZE)
    {
        analyze_affine_merge(ctx, core, &cost_best);
    }

#if ETMVP
    if (ctx->info.sqh.etmvp_enable_flag && cu_width >= MIN_ETMVP_SIZE && cu_height >= MIN_ETMVP_SIZE)
    {
        analyze_etmvp_merge(ctx, core, &cost_best);
    }
#endif

    if (allow_affine && cu_width >= AFF_SIZE && cu_height >= AFF_SIZE)
    {
        if (!(core->mod_info_best.cu_mode == MODE_SKIP && !core->mod_info_best.affine_flag)) //fast skip affine
        {
#if BD_AFFINE_AMVR
            int num_affine_amvr = ctx->info.sqh.amvr_enable_flag ? MAX_NUM_AFFINE_MVR : 1;
            for (pi->curr_mvr = 0; pi->curr_mvr < num_affine_amvr; pi->curr_mvr++)
            {
#endif
                analyze_affine_uni(ctx, core, aff_mv_L0L1, refi_L0L1, cost_L0L1, &cost_best);
                if (pi->slice_type == SLICE_B)
                {
                    analyze_affine_bi(ctx, core, aff_mv_L0L1, refi_L0L1, cost_L0L1, &cost_best);
                }
#if BD_AFFINE_AMVR
            }
#endif
        }
#if ENC_ME_IMP
        else
        {
            if (core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
            {
                insertAffUniMvCands(core, cur_info->x_pos, cur_info->y_pos, cu_width, cu_height, core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].reused_affine_uni_mv);
            }
        }
#endif
    }
#if ENC_ME_IMP
    else
    {
        if (core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
        {
            insertAffUniMvCands(core, cur_info->x_pos, cur_info->y_pos, cu_width, cu_height, core->affine_uni_mv_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].reused_affine_uni_mv);
        }
    }
#endif

#if AFFINE_UMVE
    if (ctx->info.sqh.affine_umve_enable_flag && cu_width >= AFF_SIZE && cu_height >= AFF_SIZE)
    {
        analyze_affine_umve(ctx, core, &cost_best);
    }
#endif

#if AWP
#if BAWP
    int allow_awp = ctx->info.sqh.awp_enable_flag;
#else
    int allow_awp = ctx->info.sqh.awp_enable_flag && pi->slice_type == SLICE_B;
#endif
    if (allow_awp && cu_width >= MIN_AWP_SIZE && cu_height >= MIN_AWP_SIZE &&
                     cu_width <= MAX_AWP_SIZE && cu_height <= MAX_AWP_SIZE)
    {
#if AWP_MVR
#if BAWP
        if (ctx->info.sqh.awp_mvr_enable_flag && pi->slice_type == SLICE_B)
#else
        if (ctx->info.sqh.awp_mvr_enable_flag)
#endif
        {
            BOOL test_awp = TRUE;
            if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
            {
                if (core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].awp_flag_history == 0)
                {
                    test_awp = FALSE;
                }
            }
            if (test_awp)
            {
                analyze_awp_merge_comb(ctx, core, &cost_best);
            }
        }
        else
#endif
        analyze_awp_merge(ctx, core, &cost_best);
    }
#endif

#if INTER_TM
    if (ctx->info.sqh.tm_enable_flag == 1 && cu_width * cu_height >= 64)
    {
        analyze_tm(ctx, core, &cost_best);
    }
    else if (ctx->info.sqh.tm_enable_flag == 2 && cu_width * cu_height >= 128 && cu_width > 4 && cu_height > 4)
    {
        analyze_tm(ctx, core, &cost_best);
    }
#endif

    /* reconstruct */
    int start_comp = (ctx->tree_status == TREE_L || ctx->tree_status == TREE_LC) ? Y_C : U_C;
    int num_comp = ctx->tree_status == TREE_LC ? 3 : (ctx->tree_status == TREE_L ? 1 : 2);
    for (j = start_comp; j < start_comp + num_comp; j++)
    {
        int size_tmp = (cu_width * cu_height) >> (j == 0 ? 0 : 2);
        com_mcpy(coef_blk[j], bst_info->coef[j], sizeof(s16) * size_tmp);
    }
#if ST_CHROMA
    int use_secTrans[MAX_NUM_CHANNEL][MAX_NUM_TB] = { { 0, 0, 0, 0 },{ 0, 0, 0, 0 } };
    int use_alt4x4Trans[MAX_NUM_CHANNEL] = { 0,0 };
#else
    int use_secTrans[MAX_NUM_TB] = { 0, 0, 0, 0 };
#endif
#if IST
    core->mod_info_best.slice_type = ctx->slice_type;
#endif
#if ST_CHROMA
    com_itdq_yuv(&core->mod_info_best, coef_blk, resi, ctx->wq, cu_width_log2, cu_height_log2, pi->qp_y, pi->qp_u, pi->qp_v, bit_depth, use_secTrans, use_alt4x4Trans
#if IST_CHROMA
        , ctx->tree_status
#endif    
    );
#else
    com_itdq_yuv(&core->mod_info_best, coef_blk, resi, ctx->wq, cu_width_log2, cu_height_log2, pi->qp_y, pi->qp_u, pi->qp_v, bit_depth, use_secTrans, 0);
#endif
    for (i = start_comp; i < start_comp + num_comp; i++)
    {
        int stride = (i == 0 ? cu_width : cu_width >> 1);
        com_recon(i == Y_C ? core->mod_info_best.tb_part : SIZE_2Nx2N, resi[i], bst_info->pred[i], core->mod_info_best.num_nz, i, stride, (i == 0 ? cu_height : cu_height >> 1), stride, bst_info->rec[i], bit_depth
#if SBT
            , bst_info->sbt_info
#endif
        );
    }

#if AWP_MVR
    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].awp_flag_history = bst_info->awp_flag;
    }
#endif

    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].mvr_idx_history = bst_info->mvr_idx;
    }

#if EXT_AMVR_HMVP & !FAST_EXT_AMVR_HMVP
    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit && bst_info->mvp_from_hmvp_flag)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].mvr_hmvp_idx_history = bst_info->mvr_idx;
    }
#endif

    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].affine_flag_history = bst_info->affine_flag;
    }

    if( !core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit )
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].smvd_flag_history = bst_info->smvd_flag;
    }

#if INTER_TM
    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].tm_flag_history = bst_info->tm_flag;
    }
#endif

    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        if (bst_info->cu_mode == MODE_INTER && !bst_info->affine_flag)
        {
            core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].amvr_emvr_flag_history = 1;
        }
        else
        {
            core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].amvr_emvr_flag_history = 0;
        }
    }
#if IPC_FAST
    if (!core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].visit)
    {
        core->bef_data[cu_width_log2 - 2][cu_height_log2 - 2][core->cup].lic_flag_history = bst_info->ipc_flag;
    }
#endif
    return cost_best;
}

int pinter_set_complexity(ENC_CTX *ctx, int complexity)
{
    ENC_PINTER *pi;
    pi = &ctx->pinter;
    /* default values *************************************************/
    pi->max_search_range = ctx->param.max_b_frames == 0 ? SEARCH_RANGE_IPEL_LD : SEARCH_RANGE_IPEL_RA;
    pi->search_range_ipel[MV_X] = (s16)pi->max_search_range;
    pi->search_range_ipel[MV_Y] = (s16)pi->max_search_range;
    pi->search_range_spel[MV_X] = SEARCH_RANGE_SPEL;
    pi->search_range_spel[MV_Y] = SEARCH_RANGE_SPEL;
    pi->search_pattern_hpel = tbl_search_pattern_hpel_partial;
    pi->search_pattern_hpel_cnt = 8;
    pi->search_pattern_qpel = tbl_search_pattern_qpel_8point;
    pi->search_pattern_qpel_cnt = 8;
    pi->me_level = ME_LEV_QPEL;
    pi->fn_me = pinter_me_epzs;
    pi->fn_affine_me = pinter_affine_me_gradient;
    pi->complexity = complexity;
    return COM_OK;
}

#if AWP || SAWP
void enc_init_awp_template(ENC_CTX * ctx)
{
#if FIX_372
    if (!ctx->awp_init)
#else
    if ((!ctx->awp_init) || ctx->info.pic_header.slice_type == SLICE_P)
#endif
    {
        unsigned min_width = MIN_AWP_SIZE_LOG2;
        unsigned min_height = MIN_AWP_SIZE_LOG2;
        unsigned max_width = MAX_AWP_SIZE_LOG2;
        unsigned max_height = MAX_AWP_SIZE_LOG2;
        unsigned num_width = max_width - min_width + 1;
        unsigned num_height = max_height - min_height + 1;
#if BAWP
        int awp_mode_num = 0;
#endif
        for (unsigned w = 0; w < num_width; w++)
        {
            for (unsigned h = 0; h < num_height; h++)
            {
#if FIX_372
                // weight matrix for awp&sawp
#if AWP_ENH
                for (int idx = 0; idx < TOTAL_BLEND_MODE_NUM; idx++)
#else
                for (int idx = 0; idx < AWP_MODE_NUM; idx++)
#endif
                {
                    enc_derive_awp_weights(ctx, w, h, idx, 0);
                }

#if AWP_ENH
                for (int idx = 0; idx < AWP_MODE_NUM; idx++)
                {
                    com_derive_awp_tpl_weights(ctx->awp_weight_tpl, w, h, idx);
                }

#endif
                // weight matrix for bawp
                for (int idx = 0; idx < com_tbl_bawp_num[w][h]; idx++)
                {
                    enc_derive_awp_weights(ctx, w, h, idx, 1);
                }
#else
#if BAWP
                awp_mode_num = ctx->info.pic_header.slice_type == SLICE_P ? com_tbl_bawp_num[w][h] : AWP_MODE_NUM;
                for (int idx = 0; idx < awp_mode_num; idx++)
#else
                for (int idx = 0; idx < AWP_MODE_NUM; idx++)
#endif
                {
                    enc_derive_awp_weights(ctx, w, h, idx);
                }
#endif
            }
        }
        ctx->awp_init = TRUE;
    }
}
#endif

#if ENC_ME_IMP
void valid_mv_clip(int x, int y, int pic_w, int pic_h, int w, int h, s16 mv[MV_D], s16 mv_t[MV_D])
{
    int min_clip[MV_D], max_clip[MV_D];
    x <<= 2;
    y <<= 2;
    w <<= 2;
    h <<= 2;
#if CTU_256
    min_clip[MV_X] = (-MAX_CU_SIZE2 - 4) << 2;
    min_clip[MV_Y] = (-MAX_CU_SIZE2 - 4) << 2;
    max_clip[MV_X] = (pic_w - 1 + MAX_CU_SIZE2 + 4) << 2;
    max_clip[MV_Y] = (pic_h - 1 + MAX_CU_SIZE2 + 4) << 2;
#else
    min_clip[MV_X] = (-MAX_CU_SIZE - 4) << 2;
    min_clip[MV_Y] = (-MAX_CU_SIZE - 4) << 2;
    max_clip[MV_X] = (pic_w - 1 + MAX_CU_SIZE + 4) << 2;
    max_clip[MV_Y] = (pic_h - 1 + MAX_CU_SIZE + 4) << 2;
#endif
    mv_t[MV_X] = mv[MV_X];
    mv_t[MV_Y] = mv[MV_Y];

    if (x + mv[MV_X] < min_clip[MV_X]) mv_t[MV_X] = (s16)(min_clip[MV_X] - x);
    if (y + mv[MV_Y] < min_clip[MV_Y]) mv_t[MV_Y] = (s16)(min_clip[MV_Y] - y);
    if (x + mv[MV_X] + w - 4 > max_clip[MV_X]) mv_t[MV_X] = (s16)(max_clip[MV_X] - x - w + 4);
    if (y + mv[MV_Y] + h - 4 > max_clip[MV_Y]) mv_t[MV_Y] = (s16)(max_clip[MV_Y] - y - h + 4);
}

static void fill_equal_coeff(pel *residue, int residue_stride, int **derivate, int derivate_buf_stride, s64* cx2, s64* cy2, s64* cxy, s64* cex, s64* cey, int width, int height)
{
#if SIMD_GRAD_ME
    __m128i mm_cx2 = _mm_set1_epi32(0);
    __m128i mm_cy2 = _mm_set1_epi32(0);
    __m128i mm_cxy = _mm_set1_epi32(0);
    __m128i mm_cex = _mm_set1_epi32(0);
    __m128i mm_cey = _mm_set1_epi32(0);

    __m128i mm_cx, mm_cx1, mm_cy, mm_cy1, mm_residue, mm_residue1;
    __m128i mm_tmp, mm_tmp1;

    int start_idx = 0;
    int start_idx1 = 0;
    for (int j = 0; j < height; j++)
    {
        for (int k = 0; k < width; k += 4)
        {
            int iIdx = start_idx + k;
            int iIdx1 = start_idx1 + k;

            mm_cx = _mm_loadu_si128((const __m128i*)&derivate[0][iIdx]);
            mm_cx1 = _mm_srli_si128(mm_cx, 4);
            mm_cy = _mm_loadu_si128((const __m128i*)&derivate[1][iIdx]);
            mm_cy1 = _mm_srli_si128(mm_cy, 4);
            mm_residue = _mm_loadl_epi64((const __m128i*)&residue[iIdx1]);
            mm_residue = _mm_cvtepi16_epi32(mm_residue);
            mm_residue1 = _mm_srli_si128(mm_residue, 4);
            mm_residue = _mm_slli_epi32(mm_residue, 3);
            mm_residue1 = _mm_slli_epi32(mm_residue1, 3);

            mm_tmp = _mm_mul_epi32(mm_cx, mm_cx);
            mm_tmp1 = _mm_mul_epi32(mm_cx1, mm_cx1);
            mm_cx2 = _mm_add_epi64(mm_cx2, mm_tmp);
            mm_cx2 = _mm_add_epi64(mm_cx2, mm_tmp1);

            mm_tmp = _mm_mul_epi32(mm_cy, mm_cy);
            mm_tmp1 = _mm_mul_epi32(mm_cy1, mm_cy1);
            mm_cy2 = _mm_add_epi64(mm_cy2, mm_tmp);
            mm_cy2 = _mm_add_epi64(mm_cy2, mm_tmp1);

            mm_tmp = _mm_mul_epi32(mm_cx, mm_cy);
            mm_tmp1 = _mm_mul_epi32(mm_cx1, mm_cy1);
            mm_cxy = _mm_add_epi64(mm_cxy, mm_tmp);
            mm_cxy = _mm_add_epi64(mm_cxy, mm_tmp1);

            mm_tmp = _mm_mul_epi32(mm_residue, mm_cx);
            mm_tmp1 = _mm_mul_epi32(mm_residue1, mm_cx1);
            mm_cex = _mm_add_epi64(mm_cex, mm_tmp);
            mm_cex = _mm_add_epi64(mm_cex, mm_tmp1);

            mm_tmp = _mm_mul_epi32(mm_residue, mm_cy);
            mm_tmp1 = _mm_mul_epi32(mm_residue1, mm_cy1);
            mm_cey = _mm_add_epi64(mm_cey, mm_tmp);
            mm_cey = _mm_add_epi64(mm_cey, mm_tmp1);
        }
        start_idx += derivate_buf_stride;
        start_idx1 += residue_stride;
    }

    mm_tmp = _mm_srli_si128(mm_cx2, 8);
    mm_cx2 = _mm_add_epi64(mm_cx2, mm_tmp);
    _mm_storel_epi64((__m128i*)cx2, mm_cx2);

    mm_tmp = _mm_srli_si128(mm_cy2, 8);
    mm_cy2 = _mm_add_epi64(mm_cy2, mm_tmp);
    _mm_storel_epi64((__m128i*)cy2, mm_cy2);

    mm_tmp = _mm_srli_si128(mm_cxy, 8);
    mm_cxy = _mm_add_epi64(mm_cxy, mm_tmp);
    _mm_storel_epi64((__m128i*)cxy, mm_cxy);

    mm_tmp = _mm_srli_si128(mm_cex, 8);
    mm_cex = _mm_add_epi64(mm_cex, mm_tmp);
    _mm_storel_epi64((__m128i*)cex, mm_cex);

    mm_tmp = _mm_srli_si128(mm_cey, 8);
    mm_cey = _mm_add_epi64(mm_cey, mm_tmp);
    _mm_storel_epi64((__m128i*)cey, mm_cey);
#else
    *cx2 = *cy2 = *cxy = *cex = *cey = 0;
    for (int j = 0; j < height; j++)
    {
        for (int k = 0; k < width; k++)
        {
            int iIdx = j * derivate_buf_stride + k;
            int iIdx1 = j * residue_stride + k;

            s64 sx = derivate[0][iIdx];
            s64 sy = derivate[1][iIdx];

            *cx2 += (sx * sx);
            *cy2 += (sy * sy);
            *cxy += (sx * sy);
            *cex += ((residue[iIdx1] << 3) * sx);
            *cey += ((residue[iIdx1] << 3) * sy);
        }
    }
#endif
}

static u32 me_grad_search(ENC_PINTER *pi, int x, int y, int w, int h, int cu_x, int cu_y, int cu_stride, s8 refi, int lidx, s16 gmvp[MV_D], s16 mvi[MV_D], s16 mv[MV_D], int bi)
{
    mvi[MV_X] = (mvi[MV_X] >> pi->curr_mvr) << pi->curr_mvr;
    mvi[MV_Y] = (mvi[MV_Y] >> pi->curr_mvr) << pi->curr_mvr;

    int bit_depth = pi->bit_depth;
    pel *org, *ref, *pred;
    s16 *org_bi;
    u32 cost, cost_best = COM_UINT32_MAX;
    s16 cx, cy;
    int lidx_r = (lidx == REFP_0) ? REFP_1 : REFP_0;
    int mv_bits, s_org, s_ref;

    int pic_w = pi->pic_org->width_luma;
    int pic_h = pi->pic_org->height_luma;
    int w_log2 = com_tbl_log2[w];
    int h_log2 = com_tbl_log2[h];

#if OBMC
    s_org = cu_stride;
    org = pi->org_obmc + (x - cu_x) + (y - cu_y) * cu_stride;
#else
    s_org = pi->stride_org[Y_C];
    org = pi->Yuv_org[Y_C] + x + y * pi->stride_org[Y_C];
#endif
    s_ref = pi->refp[refi][lidx].pic->stride_luma;
    ref = pi->refp[refi][lidx].pic->y;
    org_bi = pi->org_bi + (x - cu_x) + (y - cu_y) * cu_stride;
    pred = pi->pred_buf + (x - cu_x) + (y - cu_y) * cu_stride;

    s64    cx2, cy2, cxy, cex, cey;
    pel    *error = pi->p_error;
    int    *derivate[2];
    derivate[0] = pi->i_gradient[0];
    derivate[1] = pi->i_gradient[1];
    double  dMv[2];

    /* make MV to be global coordinate */
    s16 mv_clip[MV_D];
    valid_mv_clip(x, y, pic_w, pic_h, w, h, mvi, mv_clip);
    cx = mv_clip[MV_X] + ((s16)x << 2);
    cy = mv_clip[MV_Y] + ((s16)y << 2);

    /* intial value */
    mv[MV_X] = mvi[MV_X];
    mv[MV_Y] = mvi[MV_Y];

    // get initial satd cost as cost_best
    mv_bits = get_mv_bits_with_mvr((mvi[MV_X] + ((s16)x << 2)) - gmvp[MV_X], (mvi[MV_Y] + ((s16)y << 2)) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
    mv_bits += bi ? 1 : 2; // add inter_dir bits
    if (bi)
    {
        mv_bits += pi->mot_bits[lidx_r];
    }
    /* get MVD cost_best */
    cost_best = MV_COST(pi, mv_bits);
    com_mc_l(mvi[MV_X], mvi[MV_Y], ref, cx, cy, s_ref, cu_stride, pred, w, h, bit_depth);
    if (bi)
    {
        /* get satd */
        cost_best += calc_satd_16b(w, h, org_bi, pred, cu_stride, cu_stride, bit_depth) >> 1;
        enc_diff_16b(w_log2, h_log2, org_bi, pred, cu_stride, cu_stride, w, error);
    }
    else
    {
        /* get satd */
        cost_best += calc_satd_16b(w, h, org, pred, s_org, cu_stride, bit_depth);
        enc_diff_16b(w_log2, h_log2, org, pred, s_org, cu_stride, w, error);
    }

    // sobel x direction
    scaled_horizontal_sobel_filter(pred, cu_stride, derivate[0], w, w, h);
    // sobel y direction
    scaled_vertical_sobel_filter(pred, cu_stride, derivate[1], w, w, h);

    for (int iter = 0; iter < 3; iter++)
    {
        // solve delta dMv_x and dMv_y
        fill_equal_coeff(error, w, derivate, w, &cx2, &cy2, &cxy, &cex, &cey, w, h);
        double demon = (double)cx2 * (double)cy2 - (double)cxy * (double)cxy;
        if (demon == 0)
        {
            break;
        }
        double numer_x = (double)cex * (double)cy2 - (double)cey * (double)cxy;
        double numer_y = (double)cey * (double)cx2 - (double)cex * (double)cxy;
        dMv[0] = numer_x / demon;
        dMv[1] = numer_y / demon;

        s32 dMvX = (s32)(dMv[0] * 4 + (dMv[0] >= 0 ? 0.5 : -0.5));
        s32 dMvY = (s32)(dMv[1] * 4 + (dMv[1] >= 0 ? 0.5 : -0.5));
        com_mv_rounding_s32(dMvX, dMvY, &dMvX, &dMvY, pi->curr_mvr, pi->curr_mvr);

        if (dMvX == 0 && dMvY == 0)
        {
            break;
        }

        // update Mv and prediction
        s32 newMvX = (s32)mvi[MV_X] + dMvX;
        s32 newMvY = (s32)mvi[MV_Y] + dMvY;
        mvi[MV_X] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, newMvX);
        mvi[MV_Y] = (s16)COM_CLIP3(COM_INT16_MIN, COM_INT16_MAX, newMvY);
        if (mvi[MV_X] == COM_INT16_MAX)
        {
            mvi[MV_X] = (mvi[MV_X] >> pi->curr_mvr) << pi->curr_mvr;
        }
        if (mvi[MV_Y] == COM_INT16_MAX)
        {
            mvi[MV_Y] = (mvi[MV_Y] >> pi->curr_mvr) << pi->curr_mvr;
        }

        s16 mv_clip[MV_D];
        valid_mv_clip(x, y, pic_w, pic_h, w, h, mvi, mv_clip);

        cx = mv_clip[MV_X] + ((s16)x << 2);
        cy = mv_clip[MV_Y] + ((s16)y << 2);

        mv_bits = get_mv_bits_with_mvr((mvi[MV_X] + ((s16)x << 2)) - gmvp[MV_X], (mvi[MV_Y] + ((s16)y << 2)) - gmvp[MV_Y], pi->num_refp, refi, pi->curr_mvr);
        mv_bits += bi ? 1 : 2;
        if (bi)
        {
            mv_bits += pi->mot_bits[lidx_r];
        }
        cost = MV_COST(pi, mv_bits);
        com_mc_l(mvi[MV_X], mvi[MV_Y], ref, cx, cy, s_ref, cu_stride, pred, w, h, bit_depth);
        if (bi)
        {
            /* get sad */
            cost += calc_satd_16b(w, h, org_bi, pred, cu_stride, cu_stride, bit_depth) >> 1;
        }
        else
        {
            /* get sad */
            cost += calc_satd_16b(w, h, org, pred, s_org, cu_stride, bit_depth);
        }

        if (cost < cost_best)
        {
            cost_best = cost;
            mv[MV_X] = mvi[MV_X];
            mv[MV_Y] = mvi[MV_Y];
        }

        if (iter < 2)
        {
            if (bi)
            {
                enc_diff_16b(w_log2, h_log2, org_bi, pred, cu_stride, cu_stride, w, error);
            }
            else
            {
                enc_diff_16b(w_log2, h_log2, org, pred, s_org, cu_stride, w, error);
            }
            // sobel x direction
            scaled_horizontal_sobel_filter(pred, cu_stride, derivate[0], w, w, h);
            // sobel y direction
            scaled_vertical_sobel_filter(pred, cu_stride, derivate[1], w, w, h);
        }
    }
    return cost_best;
}
#endif
