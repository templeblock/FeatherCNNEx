//Tencent is pleased to support the open source community by making FeatherCNN available.

//Copyright (C) 2018 THL A29 Limited, a Tencent company. All rights reserved.

//Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
//in compliance with the License. You may obtain a copy of the License at
//
//https://opensource.org/licenses/BSD-3-Clause
//
//Unless required by applicable law or agreed to in writing, software distributed
//under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
//CONDITIONS OF ANY KIND, either express or implied. See the License for the
//specific language governing permissions and limitations under the License.

#include "winograd_kernels.h"

#include <stdlib.h>
#include <arm_neon.h>
#include <assert.h>
#include <string.h>
#ifdef __APPLE__
#else
#include <omp.h>
#endif

extern "C" void TensorGEMMInnerKernel4x4x4_fp16(float* WTp, const int wstride, const fix16_t* UTp, const fix16_t* vp, const int inChannels);
extern "C" void TensorGEMMInnerKernel4x3x4_fp16(float* WTp, const int wstride, const fix16_t* UTp, const fix16_t* vp, const int inChannels);
extern "C" void TensorGEMMInnerKernel4x2x4_fp16(float* WTp, const int wstride, const fix16_t* UTp, const fix16_t* vp, const int inChannels);
extern "C" void TensorGEMMInnerKernel4x1x4_fp16(float* WTp, const int wstride, const fix16_t* UTp, const fix16_t* vp, const int inChannels);

static inline void neon_transpose4x4_inplace_f32(
    float32x4_t row0[1],
    float32x4_t row1[1],
    float32x4_t row2[1],
    float32x4_t row3[1])
{
    float32x4x2_t row01 = vtrnq_f32(*row0, *row1);
    float32x4x2_t row23 = vtrnq_f32(*row2, *row3);
    *row0 = vcombine_f32(vget_low_f32(row01.val[0]), vget_low_f32(row23.val[0]));
    *row1 = vcombine_f32(vget_low_f32(row01.val[1]), vget_low_f32(row23.val[1]));
    *row2 = vcombine_f32(vget_high_f32(row01.val[0]), vget_high_f32(row23.val[0]));
    *row3 = vcombine_f32(vget_high_f32(row01.val[1]), vget_high_f32(row23.val[1]));
}

static void winogradKernelTransform(float* transKernel, float* kernel)
{
    float32x4_t w0, w1, w2, w3;
    float32x4_t s0, s1, s2, s3;

    w0 = vld1q_f32(kernel);
    w0 = vsetq_lane_f32(0.f, w0, 3);
    w1 = vld1q_f32(kernel + 3);
    w1 = vsetq_lane_f32(0.f, w1, 3);
    w2 = vld1q_f32(kernel + 6);
    w2 = vsetq_lane_f32(0.f, w2, 3);
    w3 = vdupq_n_f32(0.f);

    float32x4_t vhalf = vdupq_n_f32(0.5);

    s0 = vaddq_f32(w0, w2);
    s1 = vmulq_f32(vhalf, vaddq_f32(s0, w1));
    s2 = vmulq_f32(vhalf, vsubq_f32(s0, w1));

    float32x4x2_t row01 = vtrnq_f32(w0, s1);
    float32x4x2_t row23 = vtrnq_f32(s2, w2);
    s0 = vcombine_f32(vget_low_f32(row01.val[0]), vget_low_f32(row23.val[0]));
    s1 = vcombine_f32(vget_low_f32(row01.val[1]), vget_low_f32(row23.val[1]));
    s2 = vcombine_f32(vget_high_f32(row01.val[0]), vget_high_f32(row23.val[0]));
    s3 = vcombine_f32(vget_high_f32(row01.val[1]), vget_high_f32(row23.val[1]));

    w0 = vaddq_f32(s0, s2);
    w1 = vmulq_f32(vhalf, vaddq_f32(w0, s1));
    w2 = vmulq_f32(vhalf, vsubq_f32(w0, s1));

    vst1q_f32(transKernel, s0);
    vst1q_f32(transKernel + 4, w1);
    vst1q_f32(transKernel + 8, w2);
    vst1q_f32(transKernel + 12, s2);
}

void transformKernel(float* UT, float* kernel, int inChannels, int outChannels, float* ST)
{
    for(int j = 0; j < outChannels; ++j)
        for(int i = 0; i < inChannels; ++i)
            winogradKernelTransform(ST + 16 * (j * inChannels + i), kernel + 9 * (j * inChannels + i));

    const int stride = inChannels * outChannels * 4;
    float* utp[4];
    for(int i = 0; i < inChannels; ++i)
    {
        utp[0] = UT + i * 16;
        utp[1] = utp[0] + stride;
        utp[2] = utp[1] + stride;
        utp[3] = utp[2] + stride;
        float *stp = ST + 16 * i;//For i-th input channel
        for(int j = 0; j < outChannels; j += 4)
        {
            float32x4_t k00, k01, k02, k03;
            float32x4_t k10, k11, k12, k13;
            float32x4_t k20, k21, k22, k23;
            float32x4_t k30, k31, k32, k33;

            k00 = vld1q_f32(stp);
            k01 = vld1q_f32(stp + 4);
            k02 = vld1q_f32(stp + 8);
            k03 = vld1q_f32(stp + 12);
            stp += 16 * inChannels;

            k10 = vld1q_f32(stp);
            k11 = vld1q_f32(stp + 4);
            k12 = vld1q_f32(stp + 8);
            k13 = vld1q_f32(stp + 12);
            stp += 16 * inChannels;

            k20 = vld1q_f32(stp);
            k21 = vld1q_f32(stp + 4);
            k22 = vld1q_f32(stp + 8);
            k23 = vld1q_f32(stp + 12);
            stp += 16 * inChannels;

            k30 = vld1q_f32(stp);
            k31 = vld1q_f32(stp + 4);
            k32 = vld1q_f32(stp + 8);
            k33 = vld1q_f32(stp + 12);
            stp += 16 * inChannels;

            vst1q_f32(utp[0], k00);
            vst1q_f32(utp[0] + 4, k10);
            vst1q_f32(utp[0] + 8, k20);
            vst1q_f32(utp[0] + 12, k30);
            utp[0] += 16 * inChannels;
            vst1q_f32(utp[1], k01);
            vst1q_f32(utp[1] + 4, k11);
            vst1q_f32(utp[1] + 8, k21);
            vst1q_f32(utp[1] + 12, k31);
            utp[1] += 16 * inChannels;
            vst1q_f32(utp[2], k02);
            vst1q_f32(utp[2] + 4, k12);
            vst1q_f32(utp[2] + 8, k22);
            vst1q_f32(utp[2] + 12, k32);
            utp[2] += 16 * inChannels;
            vst1q_f32(utp[3], k03);
            vst1q_f32(utp[3] + 4, k13);
            vst1q_f32(utp[3] + 8, k23);
            vst1q_f32(utp[3] + 12, k33);
            utp[3] += 16 * inChannels;
        }
    }
}

static void winogradInputFrameTransform(float* VT, int ldvt, int inChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks)
{
    float32x4_t d0, d1, d2, d3;
    float32x4_t w0, w1, w2, w3;

    for(int ic = 0; ic < inChannels; ++ic)
    {
        float *inputFrame = input + ic * frameStride;
        float *outp = VT + ic * 16;
        for(int j = 0; j < nColBlocks; ++j)
        {
            float* r0 = inputFrame + ldin * j * 2;
            float* r1 = r0 + ldin;
            float* r2 = r1 + ldin;
            float* r3 = r2 + ldin;

            for(int i = 0; i < nRowBlocks; ++i)
            {
                d0 = vld1q_f32(r0);
                d1 = vld1q_f32(r1);
                d2 = vld1q_f32(r2);
                d3 = vld1q_f32(r3);

                w0 = vsubq_f32(d0, d2);
                w1 = vaddq_f32(d1, d2);
                w2 = vsubq_f32(d2, d1);
                w3 = vsubq_f32(d3, d1);
                neon_transpose4x4_inplace_f32(&w0, &w1, &w2, &w3);
                d0 = vsubq_f32(w0, w2);
                d1 = vaddq_f32(w1, w2);
                d2 = vsubq_f32(w2, w1);
                d3 = vsubq_f32(w3, w1);

                vst1q_f32(outp,      d0);
                vst1q_f32(outp +  4, d1);
                vst1q_f32(outp +  8, d2);
                vst1q_f32(outp + 12, d3);
                outp += 16 * inChannels;

                r0 += 2;
                r1 += 2;
                r2 += 2;
                r3 += 2;
            }
        }
    }
}

static inline void inputTransform(float32x4_t &d0, float32x4_t &d1, float32x4_t &d2, float32x4_t &d3)
{
    float32x4_t w0, w1, w2, w3;
    w0 = vsubq_f32(d0, d2);
    w1 = vaddq_f32(d1, d2);
    w2 = vsubq_f32(d2, d1);
    w3 = vsubq_f32(d3, d1);
    neon_transpose4x4_inplace_f32(&w0, &w1, &w2, &w3);
    d0 = vsubq_f32(w0, w2);
    d1 = vaddq_f32(w1, w2);
    d2 = vsubq_f32(w2, w1);
    d3 = vsubq_f32(w3, w1);
}

static void winogradInputFrameTransformStride(float* VT, int ldvt, int inChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, int num_threads)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int outStride = 4 * nBlocks * inChannels;
    float* outp[4];
    outp[0] = VT;
    outp[1] = outp[0] + outStride;
    outp[2] = outp[1] + outStride;
    outp[3] = outp[2] + outStride;

    #pragma omp parallel for num_threads(num_threads) collapse(2) schedule(static)
    for(int ic = 0; ic < inChannels; ++ic)
    {
        for(int i = 0; i < nBlocksAligned; i++)
        {
            float *inputFrame = input + ic * frameStride;
            int fx = i % nRowBlocks;
            int fy = i / nRowBlocks;
            float* r0 = inputFrame + ldin * fy * 2 + fx * 2;
            float* r1 = r0 + ldin;
            float* r2 = r1 + ldin;
            float* r3 = r2 + ldin;
            float32x4_t d0, d1, d2, d3;
            d0 = vld1q_f32(r0);
            d1 = vld1q_f32(r1);
            d2 = vld1q_f32(r2);
            d3 = vld1q_f32(r3);
            inputTransform(d0, d1, d2, d3);
            int yidx, xidx, offset;
            yidx = i / 4;
            xidx = (i % 4 + ic * 4) * 4;
            offset = xidx + yidx * inChannels * 16;
            vst1q_f32(outp[0] + offset, d0);
            vst1q_f32(outp[1] + offset, d1);
            vst1q_f32(outp[2] + offset, d2);
            vst1q_f32(outp[3] + offset, d3);
        }
    }

    #pragma omp parallel for num_threads(num_threads)
    for(int ic = 0; ic < inChannels; ++ic)
    {
        for(int i = nBlocksAligned; i < nBlocks; ++i)
        {
            float *inputFrame = input + ic * frameStride;
            int fx = i % nRowBlocks;
            int fy = i / nRowBlocks;
            float* r0 = inputFrame + ldin * fy * 2 + fx * 2;
            float* r1 = r0 + ldin;
            float* r2 = r1 + ldin;
            float* r3 = r2 + ldin;
            float32x4_t d0, d1, d2, d3;
            d0 = vld1q_f32(r0);
            d1 = vld1q_f32(r1);
            d2 = vld1q_f32(r2);
            d3 = vld1q_f32(r3);
            inputTransform(d0, d1, d2, d3);
            int baseOffset = nBlocksAligned * inChannels * 4;
            int offset = baseOffset + ic * 4 * (nBlocks % 4) + (i - nBlocksAligned) * 4;
            vst1q_f32(outp[0] + offset, d0);
            vst1q_f32(outp[1] + offset, d1);
            vst1q_f32(outp[2] + offset, d2);
            vst1q_f32(outp[3] + offset, d3);
        }
    }
}

static void winogradInputFrameTransformStride_fp16(fix16_t* VT, int ldvt, int inChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, int num_threads)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int outStride = 4 * nBlocks * inChannels;
    fix16_t* outp[4];
    outp[0] = VT;
    outp[1] = outp[0] + outStride;
    outp[2] = outp[1] + outStride;
    outp[3] = outp[2] + outStride;

    #pragma omp parallel for num_threads(num_threads) collapse(2) schedule(static)
    for(int ic = 0; ic < inChannels; ++ic)
    {
        for(int i = 0; i < nBlocksAligned; i++)
        {
            float *inputFrame = input + ic * frameStride;
            int fx = i % nRowBlocks;
            int fy = i / nRowBlocks;
            float* r0 = inputFrame + ldin * fy * 2 + fx * 2;
            float* r1 = r0 + ldin;
            float* r2 = r1 + ldin;
            float* r3 = r2 + ldin;
            float32x4_t d0, d1, d2, d3;
            d0 = vld1q_f32(r0);
            d1 = vld1q_f32(r1);
            d2 = vld1q_f32(r2);
            d3 = vld1q_f32(r3);
            inputTransform(d0, d1, d2, d3);
            int yidx, xidx, offset;
            yidx = i / 4;
            xidx = (i % 4 + ic * 4) * 4;
            offset = xidx + yidx * inChannels * 16;
            vst1q_f16_f32(outp[0] + offset, d0);
            vst1q_f16_f32(outp[1] + offset, d1);
            vst1q_f16_f32(outp[2] + offset, d2);
            vst1q_f16_f32(outp[3] + offset, d3);
        }
    }

    #pragma omp parallel for num_threads(num_threads)
    for(int ic = 0; ic < inChannels; ++ic)
    {
        for(int i = nBlocksAligned; i < nBlocks; ++i)
        {
            float *inputFrame = input + ic * frameStride;
            int fx = i % nRowBlocks;
            int fy = i / nRowBlocks;
            float* r0 = inputFrame + ldin * fy * 2 + fx * 2;
            float* r1 = r0 + ldin;
            float* r2 = r1 + ldin;
            float* r3 = r2 + ldin;
            float32x4_t d0, d1, d2, d3;
            d0 = vld1q_f32(r0);
            d1 = vld1q_f32(r1);
            d2 = vld1q_f32(r2);
            d3 = vld1q_f32(r3);
            inputTransform(d0, d1, d2, d3);
            int baseOffset = nBlocksAligned * inChannels * 4;
            int offset = baseOffset + ic * 4 * (nBlocks % 4) + (i - nBlocksAligned) * 4;
            vst1q_f16_f32(outp[0] + offset, d0);
            vst1q_f16_f32(outp[1] + offset, d1);
            vst1q_f16_f32(outp[2] + offset, d2);
            vst1q_f16_f32(outp[3] + offset, d3);
        }
    }
}

static inline void GEBPInnerKernel4x4x4_fp16(fix16_t* &vp, fix16_t* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    for(int i = beginIdx; i < endIdx; i+=4)
    {
        TensorGEMMInnerKernel4x4x4_fp16(WTp + i * 4, wstride, UTp, vp, inChannels);
        vp += inChannels*16;
    }
}

static inline void GEBPInnerKernel4x3x4_fp16(fix16_t* &vp, fix16_t* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    for(int i = beginIdx; i < endIdx; i+=3)
    {
        TensorGEMMInnerKernel4x3x4_fp16(WTp + i * 4, wstride, UTp, vp, inChannels);
        vp += inChannels*12;
    }
}

static inline void GEBPInnerKernel4x2x4_fp16(fix16_t* &vp, fix16_t* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    for(int i = beginIdx; i < endIdx; i+=2)
    {
        TensorGEMMInnerKernel4x2x4_fp16(WTp + i * 4, wstride, UTp, vp, inChannels);
        vp += inChannels*8;
    }
}

static inline void GEBPInnerKernel4x1x4_fp16(fix16_t* &vp, fix16_t* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    for(int i = beginIdx; i < endIdx; i++)
    {
        TensorGEMMInnerKernel4x1x4_fp16(WTp + i * 4, wstride, UTp, vp, inChannels);
        vp += inChannels*4;
    }
}

static inline void GEBPInnerKernel4x4x4(float* &vp, float* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    float32x4_t vc00, vc01, vc02, vc03;
    float32x4_t vc10, vc11, vc12, vc13;
    float32x4_t vc20, vc21, vc22, vc23;
    float32x4_t vc30, vc31, vc32, vc33;

    float32x4_t u0, u1, u2, u3;
    float32x4_t v0, v1, v2, v3;
    for(int i = beginIdx; i < endIdx; i+=4)
    {
        vc00 = vdupq_n_f32(0.f);
        vc01 = vc00;
        vc02 = vc00;
        vc03 = vc00;
        vc10 = vc00;
        vc11 = vc00;
        vc12 = vc00;
        vc13 = vc00;
        vc20 = vc00;
        vc21 = vc00;
        vc22 = vc00;
        vc23 = vc00;
        vc30 = vc00;
        vc31 = vc00;
        vc32 = vc00;
        vc33 = vc00;
        float* up = UTp;
        for(int ic = 0; ic < inChannels; ++ic, vp += 16, up += 16)
        {
            v0 = vld1q_f32(vp);
            v1 = vld1q_f32(vp + 4);
            v2 = vld1q_f32(vp + 8);
            v3 = vld1q_f32(vp + 12);
            u0 = vld1q_f32(up);
            u1 = vld1q_f32(up + 4);
            u2 = vld1q_f32(up + 8);
            u3 = vld1q_f32(up + 12);
#ifdef __aarch64__
            vc00 = vfmaq_f32(vc00, u0, v0);
            vc01 = vfmaq_f32(vc01, u0, v1);
            vc02 = vfmaq_f32(vc02, u0, v2);
            vc03 = vfmaq_f32(vc03, u0, v3);

            vc10 = vfmaq_f32(vc10, u1, v0);
            vc11 = vfmaq_f32(vc11, u1, v1);
            vc12 = vfmaq_f32(vc12, u1, v2);
            vc13 = vfmaq_f32(vc13, u1, v3);

            vc20 = vfmaq_f32(vc20, u2, v0);
            vc21 = vfmaq_f32(vc21, u2, v1);
            vc22 = vfmaq_f32(vc22, u2, v2);
            vc23 = vfmaq_f32(vc23, u2, v3);

            vc30 = vfmaq_f32(vc30, u3, v0);
            vc31 = vfmaq_f32(vc31, u3, v1);
            vc32 = vfmaq_f32(vc32, u3, v2);
            vc33 = vfmaq_f32(vc33, u3, v3);
#else
            vc00 = vmlaq_f32(vc00, u0, v0);
            vc01 = vmlaq_f32(vc01, u0, v1);
            vc02 = vmlaq_f32(vc02, u0, v2);
            vc03 = vmlaq_f32(vc03, u0, v3);

            vc10 = vmlaq_f32(vc10, u1, v0);
            vc11 = vmlaq_f32(vc11, u1, v1);
            vc12 = vmlaq_f32(vc12, u1, v2);
            vc13 = vmlaq_f32(vc13, u1, v3);

            vc20 = vmlaq_f32(vc20, u2, v0);
            vc21 = vmlaq_f32(vc21, u2, v1);
            vc22 = vmlaq_f32(vc22, u2, v2);
            vc23 = vmlaq_f32(vc23, u2, v3);

            vc30 = vmlaq_f32(vc30, u3, v0);
            vc31 = vmlaq_f32(vc31, u3, v1);
            vc32 = vmlaq_f32(vc32, u3, v2);
            vc33 = vmlaq_f32(vc33, u3, v3);
#endif
        }
        float* wp = WTp + i * 4;
        vst1q_f32(wp,      vc00);
        vst1q_f32(wp +  4, vc01);
        vst1q_f32(wp +  8, vc02);
        vst1q_f32(wp + 12, vc03);
        wp += wstride;
        vst1q_f32(wp,      vc10);
        vst1q_f32(wp +  4, vc11);
        vst1q_f32(wp +  8, vc12);
        vst1q_f32(wp + 12, vc13);
        wp += wstride;
        vst1q_f32(wp,      vc20);
        vst1q_f32(wp +  4, vc21);
        vst1q_f32(wp +  8, vc22);
        vst1q_f32(wp + 12, vc23);
        wp += wstride;
        vst1q_f32(wp,      vc30);
        vst1q_f32(wp +  4, vc31);
        vst1q_f32(wp +  8, vc32);
        vst1q_f32(wp + 12, vc33);
        wp += wstride;
    }
}

static inline void GEBPInnerKernel4x3x4(float* &vp, float* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    float32x4_t vc00, vc01, vc02;
    float32x4_t vc10, vc11, vc12;
    float32x4_t vc20, vc21, vc22;
    float32x4_t vc30, vc31, vc32;

    float32x4_t u0, u1, u2, u3;
    float32x4_t v0, v1, v2;
    for(int i = beginIdx; i < endIdx; i+=3)
    {
        vc00 = vdupq_n_f32(0.f);
        vc01 = vc00;
        vc02 = vc00;
        vc10 = vc00;
        vc11 = vc00;
        vc12 = vc00;
        vc20 = vc00;
        vc21 = vc00;
        vc22 = vc00;
        vc30 = vc00;
        vc31 = vc00;
        vc32 = vc00;

        float *up = UTp;
        for(int ic = 0; ic < inChannels; ++ic, vp += 12, up += 16)
        {
            v0 = vld1q_f32(vp);
            v1 = vld1q_f32(vp + 4);
            v2 = vld1q_f32(vp + 8);
            u0 = vld1q_f32(up);
            u1 = vld1q_f32(up + 4);
            u2 = vld1q_f32(up + 8);
            u3 = vld1q_f32(up + 12);
#ifdef __aarch64__
            vc00 = vfmaq_f32(vc00, u0, v0);
            vc01 = vfmaq_f32(vc01, u0, v1);
            vc02 = vfmaq_f32(vc02, u0, v2);

            vc10 = vfmaq_f32(vc10, u1, v0);
            vc11 = vfmaq_f32(vc11, u1, v1);
            vc12 = vfmaq_f32(vc12, u1, v2);

            vc20 = vfmaq_f32(vc20, u2, v0);
            vc21 = vfmaq_f32(vc21, u2, v1);
            vc22 = vfmaq_f32(vc22, u2, v2);

            vc30 = vfmaq_f32(vc30, u3, v0);
            vc31 = vfmaq_f32(vc31, u3, v1);
            vc32 = vfmaq_f32(vc32, u3, v2);
#else
            vc00 = vmlaq_f32(vc00, u0, v0);
            vc01 = vmlaq_f32(vc01, u0, v1);
            vc02 = vmlaq_f32(vc02, u0, v2);

            vc10 = vmlaq_f32(vc10, u1, v0);
            vc11 = vmlaq_f32(vc11, u1, v1);
            vc12 = vmlaq_f32(vc12, u1, v2);

            vc20 = vmlaq_f32(vc20, u2, v0);
            vc21 = vmlaq_f32(vc21, u2, v1);
            vc22 = vmlaq_f32(vc22, u2, v2);

            vc30 = vmlaq_f32(vc30, u3, v0);
            vc31 = vmlaq_f32(vc31, u3, v1);
            vc32 = vmlaq_f32(vc32, u3, v2);
#endif
        }
        float* wp = WTp + i * 4;
        vst1q_f32(wp,      vc00);
        vst1q_f32(wp +  4, vc01);
        vst1q_f32(wp +  8, vc02);
        wp += wstride;
        vst1q_f32(wp,      vc10);
        vst1q_f32(wp +  4, vc11);
        vst1q_f32(wp +  8, vc12);
        wp += wstride;
        vst1q_f32(wp,      vc20);
        vst1q_f32(wp +  4, vc21);
        vst1q_f32(wp +  8, vc22);
        wp += wstride;
        vst1q_f32(wp,      vc30);
        vst1q_f32(wp +  4, vc31);
        vst1q_f32(wp +  8, vc32);
        wp += wstride;
    }
}

static inline void GEBPInnerKernel4x2x4(float* &vp, float* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    float32x4_t vc00, vc01;
    float32x4_t vc10, vc11;
    float32x4_t vc20, vc21;
    float32x4_t vc30, vc31;

    float32x4_t u0, u1, u2, u3;
    float32x4_t v0, v1;
    for(int i = beginIdx; i < endIdx; i+=2)
    {
        vc00 = vdupq_n_f32(0.f);
        vc01 = vc00;
        vc10 = vc00;
        vc11 = vc00;
        vc20 = vc00;
        vc21 = vc00;
        vc30 = vc00;
        vc31 = vc00;

        float *up = UTp;
        for(int ic = 0; ic < inChannels; ++ic, vp += 8, up += 16)
        {
            v0 = vld1q_f32(vp);
            v1 = vld1q_f32(vp + 4);
            u0 = vld1q_f32(up);
            u1 = vld1q_f32(up + 4);
            u2 = vld1q_f32(up + 8);
            u3 = vld1q_f32(up + 12);
#ifdef __aarch64__
            vc00 = vfmaq_f32(vc00, u0, v0);
            vc01 = vfmaq_f32(vc01, u0, v1);

            vc10 = vfmaq_f32(vc10, u1, v0);
            vc11 = vfmaq_f32(vc11, u1, v1);

            vc20 = vfmaq_f32(vc20, u2, v0);
            vc21 = vfmaq_f32(vc21, u2, v1);

            vc30 = vfmaq_f32(vc30, u3, v0);
            vc31 = vfmaq_f32(vc31, u3, v1);
#else
            vc00 = vmlaq_f32(vc00, u0, v0);
            vc01 = vmlaq_f32(vc01, u0, v1);

            vc10 = vmlaq_f32(vc10, u1, v0);
            vc11 = vmlaq_f32(vc11, u1, v1);

            vc20 = vmlaq_f32(vc20, u2, v0);
            vc21 = vmlaq_f32(vc21, u2, v1);

            vc30 = vmlaq_f32(vc30, u3, v0);
            vc31 = vmlaq_f32(vc31, u3, v1);
#endif
        }
        float* wp = WTp + i * 4;
        vst1q_f32(wp,      vc00);
        vst1q_f32(wp +  4, vc01);
        wp += wstride;
        vst1q_f32(wp,      vc10);
        vst1q_f32(wp +  4, vc11);
        wp += wstride;
        vst1q_f32(wp,      vc20);
        vst1q_f32(wp +  4, vc21);
        wp += wstride;
        vst1q_f32(wp,      vc30);
        vst1q_f32(wp +  4, vc31);
        wp += wstride;
    }
}

static inline void GEBPInnerKernel4x1x4(float* &vp, float* UTp, float* WTp, const int beginIdx, const int endIdx, int inChannels, const int wstride)
{
    float32x4_t vc00, vc10, vc20, vc30;
    float32x4_t u0, u1, u2, u3;
    float32x4_t v0;
    for(int i = beginIdx; i < endIdx; i++)
    {
        vc00 = vdupq_n_f32(0.f);
        vc10 = vc00;
        vc20 = vc00;
        vc30 = vc00;
        float *up = UTp;
        for(int ic = 0; ic < inChannels; ++ic, vp += 4, up += 16)
        {
            v0 = vld1q_f32(vp);
            u0 = vld1q_f32(up);
            u1 = vld1q_f32(up + 4);
            u2 = vld1q_f32(up + 8);
            u3 = vld1q_f32(up + 12);
#ifdef __aarch64__
            vc00 = vfmaq_f32(vc00, u0, v0);
            vc10 = vfmaq_f32(vc10, u1, v0);
            vc20 = vfmaq_f32(vc20, u2, v0);
            vc30 = vfmaq_f32(vc30, u3, v0);
#else
            vc00 = vmlaq_f32(vc00, u0, v0);
            vc10 = vmlaq_f32(vc10, u1, v0);
            vc20 = vmlaq_f32(vc20, u2, v0);
            vc30 = vmlaq_f32(vc30, u3, v0);
#endif
        }
        float *wp = WTp + i * 4;
        vst1q_f32(wp,      vc00);
        wp += wstride;
        vst1q_f32(wp,      vc10);
        wp += wstride;
        vst1q_f32(wp,      vc20);
        wp += wstride;
        vst1q_f32(wp,      vc30);
        wp += wstride;
    }
}

static void GEMMCubic(float* output, int ldout, float* WT, float* VT, const int ldvt, float* UT, const int ldut, const int inChannels, const int outChannels, const int nRowBlocks, const int nColBlocks)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int wstride = nBlocks * 4;

    for(int oc = 0; oc < outChannels; oc += 4)
    {
        float *vp = VT;
        float *UTp = UT + oc / 4 * inChannels * 16;
        float *WTp = WT + oc * nRowBlocks * nColBlocks * 4;
        GEBPInnerKernel4x4x4(vp, UTp, WTp, 0, nBlocksAligned, inChannels, wstride);
        switch(nBlocks % 4)
        {
        case 1:
            GEBPInnerKernel4x1x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        case 2:
            GEBPInnerKernel4x2x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        case 3:
            GEBPInnerKernel4x3x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        }
    }
}

static void GEMMCubic_fp16(float* output, int ldout, float* WT, fix16_t* VT, const int ldvt, fix16_t* UT, const int ldut, const int inChannels, const int outChannels, const int nRowBlocks, const int nColBlocks)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int wstride = nBlocks * 4;

    for(int oc = 0; oc < outChannels; oc += 4)
    {
        fix16_t *vp = VT;
        fix16_t *UTp = UT + oc / 4 * inChannels * 16;
        float *WTp = WT + oc * nRowBlocks * nColBlocks * 4;
        GEBPInnerKernel4x4x4_fp16(vp, UTp, WTp, 0, nBlocksAligned, inChannels, wstride);
        switch(nBlocks % 4)
        {
        case 1:
            GEBPInnerKernel4x1x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        case 2:
            GEBPInnerKernel4x2x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        case 3:
            GEBPInnerKernel4x3x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
            break;
        }
    }
}

static void GEMMCubicFourOutputChannels(float* output, int ldout, float* WT, float* VT, float* UT, const int inChannels, const int outChannels, const int nRowBlocks, const int nColBlocks, const int oc)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int wstride = nBlocks * 4;

    float *UTp = UT + oc / 4 * inChannels * 16;
    float *WTp = WT + oc * nRowBlocks * nColBlocks * 4;
    float *vp = VT;

    GEBPInnerKernel4x4x4(vp, UTp, WTp, 0, nBlocksAligned, inChannels, wstride);
    switch(nBlocks % 4)
    {
    case 1:
        GEBPInnerKernel4x1x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    case 2:
        GEBPInnerKernel4x2x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    case 3:
        GEBPInnerKernel4x3x4(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    }
}

static void GEMMCubicFourOutputChannels_fp16(float* output, int ldout, float* WT, fix16_t* VT, fix16_t* UT, const int inChannels, const int outChannels, const int nRowBlocks, const int nColBlocks, const int oc)
{
    const int nBlocks = nRowBlocks * nColBlocks;
    const int nBlocksAligned = nBlocks - nBlocks % 4;
    const int wstride = nBlocks * 4;

    fix16_t *vp = VT;
    fix16_t *UTp = UT + oc / 4 * inChannels * 16;
    float *WTp = WT + oc * nRowBlocks * nColBlocks * 4;

    GEBPInnerKernel4x4x4_fp16(vp, UTp, WTp, 0, nBlocksAligned, inChannels, wstride);
    switch(nBlocks % 4)
    {
    case 1:
        GEBPInnerKernel4x1x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    case 2:
        GEBPInnerKernel4x2x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    case 3:
        GEBPInnerKernel4x3x4_fp16(vp, UTp, WTp, nBlocksAligned, nBlocks, inChannels, wstride);
        break;
    }
}

static void winogradOutputTransform(float* output, int ldout, float* WT, int outChannels, int nRowBlocks, int nColBlocks, int num_threads)
{
    int nBlocks = nRowBlocks * nColBlocks;

    #pragma omp parallel for num_threads(num_threads) schedule(static)
    for(int oc = 0; oc < outChannels; ++oc)
    {
        const int offset = nRowBlocks * nColBlocks * 4 * oc;
        float *wp[4];
        wp[0] = WT + offset;
        wp[1] = wp[0] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[2] = wp[1] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[3] = wp[2] + outChannels * nRowBlocks * nColBlocks * 4;

        float32x4_t s0, s1, s2, s3;
        float32x2_t o0, o1;
        float32x2_t d0, d1, d2, d3;

        for(int j = 0; j < nColBlocks; ++j)
        {
            float* outRow0 = output  + oc * 4 * nBlocks + j * ldout * 2;
            float* outRow1 = outRow0 + ldout;
            for(int i = 0; i < nRowBlocks; ++i)
            {
                o0 = vld1_f32(outRow0);
                o1 = vld1_f32(outRow1);

                s0 = vld1q_f32(wp[0]);
                wp[0] += 4;
                s1 = vld1q_f32(wp[1]);
                wp[1] += 4;
                s2 = vld1q_f32(wp[2]);
                wp[2] += 4;
                s3 = vld1q_f32(wp[3]);
                wp[3] += 4;

                s0 = s0 + s1 + s2;
                s1 = s1 - s2 + s3;
                float32x4x2_t rows = vtrnq_f32(s0, s1);
                d0 = vget_low_f32(rows.val[0]);
                d1 = vget_low_f32(rows.val[1]);
                d2 = vget_high_f32(rows.val[0]);
                d3 = vget_high_f32(rows.val[1]);
                o0 = d0 + d1 + d2;
                o1 = d1 - d2 + d3;

                vst1_f32(outRow0, o0);
                vst1_f32(outRow1, o1);
                outRow0 += 2;
                outRow1 += 2;
            }
        }
    }
}

static void winogradOutputTransformBias(float* output, int ldout, float* WT, int outChannels, int nRowBlocks, int nColBlocks, float *biasArr, int num_threads)
{
    int nBlocks = nRowBlocks * nColBlocks;

    #pragma omp parallel for num_threads(num_threads) schedule(static)
    for(int oc = 0; oc < outChannels; ++oc)
    {
        float32x2_t vBias = vdup_n_f32(biasArr[oc]);
        const int offset = nRowBlocks * nColBlocks * 4 * oc;
        float *wp[4];
        wp[0] = WT + offset;
        wp[1] = wp[0] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[2] = wp[1] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[3] = wp[2] + outChannels * nRowBlocks * nColBlocks * 4;

        float32x4_t s0, s1, s2, s3;
        float32x2_t o0, o1;
        float32x2_t d0, d1, d2, d3;

        for(int j = 0; j < nColBlocks; ++j)
        {
            float* outRow0 = output  + oc * 4 * nBlocks + j * ldout * 2;
            float* outRow1 = outRow0 + ldout;
            for(int i = 0; i < nRowBlocks; ++i)
            {
                s0 = vld1q_f32(wp[0]);
                wp[0] += 4;
                s1 = vld1q_f32(wp[1]);
                wp[1] += 4;
                s2 = vld1q_f32(wp[2]);
                wp[2] += 4;
                s3 = vld1q_f32(wp[3]);
                wp[3] += 4;

                s0 = s0 + s1 + s2;
                s1 = s1 - s2 + s3;
                float32x4x2_t rows = vtrnq_f32(s0, s1);
                d0 = vget_low_f32(rows.val[0]);
                d1 = vget_low_f32(rows.val[1]);
                d2 = vget_high_f32(rows.val[0]);
                d3 = vget_high_f32(rows.val[1]);
                o0 = d0 + d1 + d2 + vBias;
                o1 = d1 - d2 + d3 + vBias;

                vst1_f32(outRow0, o0);
                vst1_f32(outRow1, o1);
                outRow0 += 2;
                outRow1 += 2;
            }
        }
    }
}

static void winogradOutputTransformBiasReLU(float* output, int ldout, float* WT, int outChannels, int nRowBlocks, int nColBlocks, float *biasArr, int num_threads)
{
    int nBlocks = nRowBlocks * nColBlocks;
    const float32x2_t vZero2 = vdup_n_f32(0.f);

    #pragma omp parallel for num_threads(num_threads) schedule(static)
    for(int oc = 0; oc < outChannels; ++oc)
    {
        float32x2_t vBias = vdup_n_f32(biasArr[oc]);
        const int offset = nRowBlocks * nColBlocks * 4 * oc;
        float *wp[4];
        wp[0] = WT + offset;
        wp[1] = wp[0] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[2] = wp[1] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[3] = wp[2] + outChannels * nRowBlocks * nColBlocks * 4;

        float32x4_t s0, s1, s2, s3;
        float32x2_t o0, o1;
        float32x2_t d0, d1, d2, d3;

        for(int j = 0; j < nColBlocks; ++j)
        {
            float* outRow0 = output  + oc * 4 * nBlocks + j * ldout * 2;
            float* outRow1 = outRow0 + ldout;
            for(int i = 0; i < nRowBlocks; ++i)
            {
                s0 = vld1q_f32(wp[0]);
                wp[0] += 4;
                s1 = vld1q_f32(wp[1]);
                wp[1] += 4;
                s2 = vld1q_f32(wp[2]);
                wp[2] += 4;
                s3 = vld1q_f32(wp[3]);
                wp[3] += 4;

                s0 = s0 + s1 + s2;
                s1 = s1 - s2 + s3;
                float32x4x2_t rows = vtrnq_f32(s0, s1);
                d0 = vget_low_f32(rows.val[0]);
                d1 = vget_low_f32(rows.val[1]);
                d2 = vget_high_f32(rows.val[0]);
                d3 = vget_high_f32(rows.val[1]);
                o0 = d0 + d1 + d2 + vBias;
                o1 = d1 - d2 + d3 + vBias;

                o0 = vmax_f32(o0, vZero2);
                o1 = vmax_f32(o1, vZero2);

                vst1_f32(outRow0, o0);
                vst1_f32(outRow1, o1);
                outRow0 += 2;
                outRow1 += 2;
            }
        }
    }
}

static void winogradOutputTransformReLU(float* output, int ldout, float* WT, int outChannels, int nRowBlocks, int nColBlocks, int num_threads)
{
    int nBlocks = nRowBlocks * nColBlocks;
    const float32x2_t vZero2 = vdup_n_f32(0.f);

    #pragma omp parallel for num_threads(num_threads) schedule(static)
    for(int oc = 0; oc < outChannels; ++oc)
    {
        const int offset = nRowBlocks * nColBlocks * 4 * oc;
        float *wp[4];
        wp[0] = WT + offset;
        wp[1] = wp[0] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[2] = wp[1] + outChannels * nRowBlocks * nColBlocks * 4;
        wp[3] = wp[2] + outChannels * nRowBlocks * nColBlocks * 4;

        float32x4_t s0, s1, s2, s3;
        float32x2_t o0, o1;
        float32x2_t d0, d1, d2, d3;

        for(int j = 0; j < nColBlocks; ++j)
        {
            float* outRow0 = output  + oc * 4 * nBlocks + j * ldout * 2;
            float* outRow1 = outRow0 + ldout;
            for(int i = 0; i < nRowBlocks; ++i)
            {
                s0 = vld1q_f32(wp[0]);
                wp[0] += 4;
                s1 = vld1q_f32(wp[1]);
                wp[1] += 4;
                s2 = vld1q_f32(wp[2]);
                wp[2] += 4;
                s3 = vld1q_f32(wp[3]);
                wp[3] += 4;

                s0 = s0 + s1 + s2;
                s1 = s1 - s2 + s3;
                float32x4x2_t rows = vtrnq_f32(s0, s1);
                d0 = vget_low_f32(rows.val[0]);
                d1 = vget_low_f32(rows.val[1]);
                d2 = vget_high_f32(rows.val[0]);
                d3 = vget_high_f32(rows.val[1]);
                o0 = d0 + d1 + d2;
                o1 = d1 - d2 + d3;

                o0 = vmax_f32(o0, vZero2);
                o1 = vmax_f32(o1, vZero2);

                vst1_f32(outRow0, o0);
                vst1_f32(outRow1, o1);
                outRow0 += 2;
                outRow1 += 2;
            }
        }
    }
}

template<typename T>
static void winogradNonFusedTransform_inner(float *output, int ldout, float* WT, T* VT, int ldvt, T* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr)
{
    if (4 == sizeof(VT[0]))
        winogradInputFrameTransformStride((float*)VT, ldvt, inChannels, input, frameStride, ldin, nRowBlocks, nColBlocks, 1);
    else if (2 == sizeof(VT[0]))
        winogradInputFrameTransformStride_fp16((fix16_t*)VT, ldvt, inChannels, input, frameStride, ldin, nRowBlocks, nColBlocks, 1);
    if (4 == sizeof(VT[0]))
    {
        for(int i = 0; i < 4; ++i)
            GEMMCubic(output, ldout,
                      WT + i * outChannels * nRowBlocks * nColBlocks * 4,
                      (float*)VT + i * nRowBlocks * nColBlocks * inChannels * 4, ldvt,
                      (float*)UT + i * inChannels * outChannels * 4, ldut,
                      inChannels, outChannels, nRowBlocks, nColBlocks);
    }
    else if (2 == sizeof(VT[0]))
    {
        for(int i = 0; i < 4; ++i)
            GEMMCubic_fp16(output, ldout,
                           WT + i * outChannels * nRowBlocks * nColBlocks * 4,
                           (fix16_t*)VT + i * nRowBlocks * nColBlocks * inChannels * 4, ldvt,
                           (fix16_t*)UT + i * inChannels * outChannels * 4, ldut,
                           inChannels, outChannels, nRowBlocks, nColBlocks);
    }
    switch (outType)
    {
    case None:
        winogradOutputTransform(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, 1);
        break;
    case ReLU:
        winogradOutputTransformReLU(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, 1);
        break;
    case Bias:
        winogradOutputTransformBias(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, biasArr, 1);
        break;
    case BiasReLU:
        winogradOutputTransformBiasReLU(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, biasArr, 1);
        break;
    }
}

template void winogradNonFusedTransform_inner<fix16_t>(float *output, int ldout, float* WT, fix16_t* VT, int ldvt, fix16_t* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr);
template void winogradNonFusedTransform_inner<float>(float *output, int ldout, float* WT, float* VT, int ldvt, float* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr);

template<typename T>
static void winogradNonFusedTransformMT_inner(float *output, int ldout, float* WT, T* VT, int ldvt, T* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr, int num_threads)
{
    if (4 == sizeof(VT[0]))
        winogradInputFrameTransformStride((float *)VT, ldvt, inChannels, input, frameStride, ldin, nRowBlocks, nColBlocks, num_threads);
    else if (2 == sizeof(VT[0]))
        winogradInputFrameTransformStride_fp16((fix16_t*)VT, ldvt, inChannels, input, frameStride, ldin, nRowBlocks, nColBlocks, num_threads);

    if (4 == sizeof(VT[0]))
    {
        #pragma omp parallel for num_threads(num_threads) schedule(static)
        for(int i = 0; i < outChannels; i++)
        {
            const int oc = (i * 4) % outChannels;
            const int mi = i / (outChannels / 4);
            GEMMCubicFourOutputChannels(output, ldout,
                                        WT + mi * outChannels * nRowBlocks * nColBlocks * 4,
                                        (float*)VT + mi * nRowBlocks * nColBlocks * inChannels * 4,
                                        (float*)UT + mi * inChannels * outChannels * 4,
                                        inChannels, outChannels, nRowBlocks, nColBlocks, oc);
        }
    }
    else if (2 == sizeof(VT[0]))
    {
        #pragma omp parallel for num_threads(num_threads) schedule(static)
        for(int i = 0; i < outChannels; i++)
        {
            const int oc = (i * 4) % outChannels;
            const int mi = i / (outChannels / 4);
            GEMMCubicFourOutputChannels_fp16(output, ldout,
                                             WT + mi * outChannels * nRowBlocks * nColBlocks * 4,
                                             (fix16_t*)VT + mi * nRowBlocks * nColBlocks * inChannels * 4,
                                             (fix16_t*)UT + mi * inChannels * outChannels * 4,
                                             inChannels, outChannels, nRowBlocks, nColBlocks, oc);
        }
    }

    switch (outType)
    {
    case None:
        winogradOutputTransform(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, num_threads);
        break;
    case ReLU:
        winogradOutputTransformReLU(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, num_threads);
        break;
    case Bias:
        winogradOutputTransformBias(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, biasArr, num_threads);
        break;
    case BiasReLU:
        winogradOutputTransformBiasReLU(output, ldout, WT, outChannels, nRowBlocks, nColBlocks, biasArr, num_threads);
        break;
    }
}

template void winogradNonFusedTransformMT_inner<fix16_t>(float *output, int ldout, float* WT, fix16_t* VT, int ldvt, fix16_t* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr, int num_threads);
template void winogradNonFusedTransformMT_inner<float>(float *output, int ldout, float* WT, float* VT, int ldvt, float* UT, int ldut, int inChannels, int outChannels, float* input, int frameStride, int ldin, int nRowBlocks, int nColBlocks, WinogradOutType outType, float* biasArr, int num_threads);

template<typename T>
void winogradNonFusedTransform(float *output, int outChannels, float* WT, T* VT, T* UT, float* input, int inChannels, int inputw, int inputh, WinogradOutType outType, float* biasArr, int num_threads)
{
    const int inputFrameStride = inputw * inputh;
    const int nRowBlocks = inputw / 2 - 1;
    const int nColBlocks = inputh / 2 - 1;
    const int ldout = nRowBlocks * 2;
    const int ldvt =  nRowBlocks * 4;
    const int ldut = 16 * inChannels;
    if(num_threads == 1)
        winogradNonFusedTransform_inner(output, ldout, WT, VT, ldvt, UT, ldut, inChannels, outChannels, input, inputFrameStride, inputw, nRowBlocks, nColBlocks, outType, biasArr);
    else
        winogradNonFusedTransformMT_inner(output, ldout, WT, VT, ldvt, UT, ldut, inChannels, outChannels, input, inputFrameStride, inputw, nRowBlocks, nColBlocks, outType, biasArr, num_threads);
}

template void winogradNonFusedTransform<fix16_t>(float *output, int outChannels, float* WT, fix16_t* VT, fix16_t* UT, float* input, int inChannels, int inputw, int inputh, WinogradOutType outType, float* biasArr, int num_threads);
template void winogradNonFusedTransform<float>(float *output, int outChannels, float* WT, float* VT, float* UT, float* input, int inChannels, int inputw, int inputh, WinogradOutType outType, float* biasArr, int num_threads);
