// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2022 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "gemm_arm.h"

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

#include "arm_usability.h"

#include "cpu.h"

namespace ncnn {

#include "gemm_bf16s_fp16s.h"
#include "gemm_fp16s.h"

static int gemm_arm_fp16s(const Mat& A, const Mat& B, const Mat& C, Mat& top_blob, int broadcast_type_C, int transA, int transB, float alpha, int nT, const Option& opt)
{
    const int M = transA ? A.w : (A.dims == 3 ? A.c : A.h) * A.elempack;
    const int K = transA ? (A.dims == 3 ? A.c : A.h) * A.elempack : A.w;
    const int N = transB ? (B.dims == 3 ? B.c : B.h) * B.elempack : B.w;

    // NCNN_LOGE("M/N/K = %d %d %d", M, N, K);

    int TILE_M, TILE_N, TILE_K;
    get_optimal_tile_mnk_bf16s_fp16s(M, N, K, TILE_M, TILE_N, TILE_K, nT);

    // NCNN_LOGE("TILE M/N/K = %d %d %d", TILE_M, TILE_N, TILE_K);

    int nn_M = (M + TILE_M - 1) / TILE_M;
    int nn_N = (N + TILE_N - 1) / TILE_N;

    Mat ATX(TILE_K * TILE_M, (K + TILE_K - 1) / TILE_K, nT, 2u, opt.blob_allocator);
    Mat BT(TILE_K * TILE_N, (K + TILE_K - 1) / TILE_K, (N + TILE_N - 1) / TILE_N, 2u, opt.blob_allocator);

    Mat tmpX;
    if (K > TILE_K)
        tmpX.create(TILE_N, TILE_M, nT, 4u, opt.blob_allocator);

    // pack B
    #pragma omp parallel for num_threads(nT)
    for (int ppj = 0; ppj < nn_N; ppj++)
    {
        const int j = ppj * TILE_N;

        for (int k = 0; k < K; k += TILE_K)
        {
            const int max_jj = std::min((N - j), TILE_N);
            const int max_kk = std::min((K - k), TILE_K);

            Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

            if (transB)
            {
                pack_B_tile_bf16_fp16(B, BT_tile, j, max_jj, k, max_kk);
            }
            else
            {
                transpose_pack_B_tile_bf16_fp16(B, BT_tile, j, max_jj, k, max_kk);
            }
        }
    }

    #pragma omp parallel for num_threads(nT)
    for (int ppi = 0; ppi < nn_M; ppi++)
    {
        const int i = ppi * TILE_M;

        Mat tmp;
        if (K > TILE_K)
            tmp = tmpX.channel(get_omp_thread_num());

        int j = 0;
        for (; j < N; j += TILE_N)
        {
            int k = 0;
            for (; k < K; k += TILE_K)
            {
                const int max_ii = std::min((M - i), TILE_M);
                const int max_jj = std::min((N - j), TILE_N);
                const int max_kk = std::min((K - k), TILE_K);

                // NCNN_LOGE("max_ii/jj/kk = %d %d %d", max_ii, max_jj, max_kk);

                Mat AT_tile = ATX.channel(get_omp_thread_num()).row_range(k / TILE_K, 1);

                Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

                if (j == 0)
                {
                    if (transA)
                    {
                        transpose_pack_A_tile_bf16_fp16(A, AT_tile, i, max_ii, k, max_kk);
                    }
                    else
                    {
                        pack_A_tile_bf16_fp16(A, AT_tile, i, max_ii, k, max_kk);
                    }
                }

                bool k_end = k + TILE_K >= K;

                gemm_transB_packed_tile_fp16s(AT_tile, BT_tile, C, top_blob, broadcast_type_C, tmp, alpha, i, max_ii, j, max_jj, k, max_kk, k_end);
            }
        }
    }

    return 0;
}

static int gemm_AT_arm_fp16s(const Mat& AT, const Mat& B, const Mat& C, Mat& top_blob, int broadcast_type_C, int M, int K, int transB, float alpha, int nT, const Option& opt)
{
    const int N = transB ? (B.dims == 3 ? B.c : B.h) * B.elempack : B.w;

    // NCNN_LOGE("M/N/K = %d %d %d", M, N, K);

    int TILE_M, TILE_N, TILE_K;
    get_optimal_tile_mnk_bf16s_fp16s(M, N, K, TILE_M, TILE_N, TILE_K, nT);

    // NCNN_LOGE("TILE M/N/K = %d %d %d", TILE_M, TILE_N, TILE_K);

    int nn_M = (M + TILE_M - 1) / TILE_M;
    int nn_N = (N + TILE_N - 1) / TILE_N;

    Mat BT(TILE_K * TILE_N, (K + TILE_K - 1) / TILE_K, (N + TILE_N - 1) / TILE_N, 2u, opt.blob_allocator);

    Mat tmpX;
    if (K > TILE_K)
        tmpX.create(TILE_N, TILE_M, nT, 4u, opt.blob_allocator);

    // pack B
    #pragma omp parallel for num_threads(nT)
    for (int ppj = 0; ppj < nn_N; ppj++)
    {
        const int j = ppj * TILE_N;

        for (int k = 0; k < K; k += TILE_K)
        {
            const int max_jj = std::min((N - j), TILE_N);
            const int max_kk = std::min((K - k), TILE_K);

            Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

            if (transB)
            {
                pack_B_tile_bf16_fp16(B, BT_tile, j, max_jj, k, max_kk);
            }
            else
            {
                transpose_pack_B_tile_bf16_fp16(B, BT_tile, j, max_jj, k, max_kk);
            }
        }
    }

    #pragma omp parallel for num_threads(nT)
    for (int ppi = 0; ppi < nn_M; ppi++)
    {
        const int i = ppi * TILE_M;

        Mat tmp;
        if (K > TILE_K)
            tmp = tmpX.channel(get_omp_thread_num());

        int j = 0;
        for (; j < N; j += TILE_N)
        {
            int k = 0;
            for (; k < K; k += TILE_K)
            {
                const int max_ii = std::min((M - i), TILE_M);
                const int max_jj = std::min((N - j), TILE_N);
                const int max_kk = std::min((K - k), TILE_K);

                // NCNN_LOGE("max_ii/jj/kk = %d %d %d", max_ii, max_jj, max_kk);

                Mat AT_tile = AT.channel(i / TILE_M).row_range(k / TILE_K, 1);

                Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

                bool k_end = k + TILE_K >= K;

                gemm_transB_packed_tile_fp16s(AT_tile, BT_tile, C, top_blob, broadcast_type_C, tmp, alpha, i, max_ii, j, max_jj, k, max_kk, k_end);
            }
        }
    }

    return 0;
}

static int gemm_BT_arm_fp16s(const Mat& A, const Mat& BT, const Mat& C, Mat& top_blob, int broadcast_type_C, int N, int K, int transA, float alpha, int nT, const Option& opt)
{
    const int M = transA ? A.w : (A.dims == 3 ? A.c : A.h) * A.elempack;

    // NCNN_LOGE("M/N/K = %d %d %d", M, N, K);

    int TILE_M, TILE_N, TILE_K;
    get_optimal_tile_mnk_bf16s_fp16s(M, N, K, TILE_M, TILE_N, TILE_K, nT);

    // NCNN_LOGE("TILE M/N/K = %d %d %d", TILE_M, TILE_N, TILE_K);

    int nn_M = (M + TILE_M - 1) / TILE_M;
    // int nn_N = (N + TILE_N - 1) / TILE_N;

    Mat ATX(TILE_K * TILE_M, (K + TILE_K - 1) / TILE_K, nT, 2u, opt.blob_allocator);

    Mat tmpX;
    if (K > TILE_K)
        tmpX.create(TILE_N, TILE_M, nT, 4u, opt.blob_allocator);

    #pragma omp parallel for num_threads(nT)
    for (int ppi = 0; ppi < nn_M; ppi++)
    {
        const int i = ppi * TILE_M;

        Mat tmp;
        if (K > TILE_K)
            tmp = tmpX.channel(get_omp_thread_num());

        int j = 0;
        for (; j < N; j += TILE_N)
        {
            int k = 0;
            for (; k < K; k += TILE_K)
            {
                const int max_ii = std::min((M - i), TILE_M);
                const int max_jj = std::min((N - j), TILE_N);
                const int max_kk = std::min((K - k), TILE_K);

                // NCNN_LOGE("max_ii/jj/kk = %d %d %d", max_ii, max_jj, max_kk);

                Mat AT_tile = ATX.channel(get_omp_thread_num()).row_range(k / TILE_K, 1);

                Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

                if (j == 0)
                {
                    if (transA)
                    {
                        transpose_pack_A_tile_bf16_fp16(A, AT_tile, i, max_ii, k, max_kk);
                    }
                    else
                    {
                        pack_A_tile_bf16_fp16(A, AT_tile, i, max_ii, k, max_kk);
                    }
                }

                bool k_end = k + TILE_K >= K;

                gemm_transB_packed_tile_fp16s(AT_tile, BT_tile, C, top_blob, broadcast_type_C, tmp, alpha, i, max_ii, j, max_jj, k, max_kk, k_end);
            }
        }
    }

    return 0;
}

static int gemm_AT_BT_arm_fp16s(const Mat& AT, const Mat& BT, const Mat& C, Mat& top_blob, int broadcast_type_C, int M, int N, int K, float alpha, int nT, const Option& opt)
{
    // NCNN_LOGE("M/N/K = %d %d %d", M, N, K);

    int TILE_M, TILE_N, TILE_K;
    get_optimal_tile_mnk_bf16s_fp16s(M, N, K, TILE_M, TILE_N, TILE_K, nT);

    // NCNN_LOGE("TILE M/N/K = %d %d %d", TILE_M, TILE_N, TILE_K);

    int nn_M = (M + TILE_M - 1) / TILE_M;
    // int nn_N = (N + TILE_N - 1) / TILE_N;

    Mat tmpX;
    if (K > TILE_K)
        tmpX.create(TILE_N, TILE_M, nT, 4u, opt.blob_allocator);

    #pragma omp parallel for num_threads(nT)
    for (int ppi = 0; ppi < nn_M; ppi++)
    {
        const int i = ppi * TILE_M;

        Mat tmp;
        if (K > TILE_K)
            tmp = tmpX.channel(get_omp_thread_num());

        int j = 0;
        for (; j < N; j += TILE_N)
        {
            int k = 0;
            for (; k < K; k += TILE_K)
            {
                const int max_ii = std::min((M - i), TILE_M);
                const int max_jj = std::min((N - j), TILE_N);
                const int max_kk = std::min((K - k), TILE_K);

                // NCNN_LOGE("max_ii/jj/kk = %d %d %d", max_ii, max_jj, max_kk);

                Mat AT_tile = AT.channel(i / TILE_M).row_range(k / TILE_K, 1);

                Mat BT_tile = BT.channel(j / TILE_N).row_range(k / TILE_K, 1);

                bool k_end = k + TILE_K >= K;

                gemm_transB_packed_tile_fp16s(AT_tile, BT_tile, C, top_blob, broadcast_type_C, tmp, alpha, i, max_ii, j, max_jj, k, max_kk, k_end);
            }
        }
    }

    return 0;
}

int Gemm_arm::create_pipeline_fp16s(const Option& opt)
{
    if (constantA)
    {
        const int M = constantM;
        const int K = constantK;

        int TILE_M, TILE_N, TILE_K;
        get_optimal_tile_mnk_bf16s_fp16s(M, 0, K, TILE_M, TILE_N, TILE_K, opt.num_threads);

        const int nn_M = (M + TILE_M - 1) / TILE_M;

        AT_data.create(TILE_K * TILE_M, (K + TILE_K - 1) / TILE_K, (M + TILE_M - 1) / TILE_M, 2u, opt.blob_allocator);
        if (AT_data.empty())
            return -100;

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int ppj = 0; ppj < nn_M; ppj++)
        {
            const int i = ppj * TILE_M;

            for (int k = 0; k < K; k += TILE_K)
            {
                const int max_ii = std::min((M - i), TILE_M);
                const int max_kk = std::min((K - k), TILE_K);

                Mat AT_tile = AT_data.channel(i / TILE_M).row_range(k / TILE_K, 1);

                if (transA)
                {
                    transpose_pack_A_tile_fp32_to_fp16(A_data, AT_tile, i, max_ii, k, max_kk);
                }
                else
                {
                    pack_A_tile_fp32_to_fp16(A_data, AT_tile, i, max_ii, k, max_kk);
                }
            }
        }

        if (opt.lightmode)
        {
            A_data.release();
        }
    }

    if (constantB)
    {
        const int N = constantN;
        const int K = constantK;

        int TILE_M, TILE_N, TILE_K;
        get_optimal_tile_mnk_bf16s_fp16s(0, N, K, TILE_M, TILE_N, TILE_K, opt.num_threads);

        const int nn_N = (N + TILE_N - 1) / TILE_N;

        BT_data.create(TILE_K * TILE_N, (K + TILE_K - 1) / TILE_K, (N + TILE_N - 1) / TILE_N, 2u, opt.blob_allocator);
        if (BT_data.empty())
            return -100;

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int ppj = 0; ppj < nn_N; ppj++)
        {
            const int j = ppj * TILE_N;

            for (int k = 0; k < K; k += TILE_K)
            {
                const int max_jj = std::min((N - j), TILE_N);
                const int max_kk = std::min((K - k), TILE_K);

                Mat BT_tile = BT_data.channel(j / TILE_N).row_range(k / TILE_K, 1);

                if (transB)
                {
                    pack_B_tile_fp32_to_fp16(B_data, BT_tile, j, max_jj, k, max_kk);
                }
                else
                {
                    transpose_pack_B_tile_fp32_to_fp16(B_data, BT_tile, j, max_jj, k, max_kk);
                }
            }
        }

        if (opt.lightmode)
        {
            B_data.release();
        }
    }

    if (constantC && constant_broadcast_type_C != -1)
    {
        int elemcount = 1;
        if (constant_broadcast_type_C == 1 || constant_broadcast_type_C == 2 || constant_broadcast_type_C == 3)
        {
            elemcount = constantM;
        }
        else if (constant_broadcast_type_C == 4)
        {
            elemcount = constantN;
        }

        int C_elempack = 1;
        if (opt.use_packing_layout)
        {
            C_elempack = elemcount % 4 == 0 ? 4 : 1;
        }

        convert_packing(C_data, CT_data, C_elempack, opt);

        // pre-multiply C with beta
        if (beta != 1.f)
        {
            const int size = CT_data.total() * C_elempack;
            for (int i = 0; i < size; i++)
            {
                CT_data[i] *= beta;
            }
        }

        if (opt.lightmode)
        {
            C_data.release();
        }
    }

    if (constantA || constantB || constantC)
    {
        nT = opt.num_threads;
    }

    return 0;
}

int Gemm_arm::forward_fp16s(const std::vector<Mat>& bottom_blobs, std::vector<Mat>& top_blobs, const Option& opt) const
{
    int M;
    int N;
    if (constantA && constantB)
    {
        M = constantM;
        N = constantN;
    }
    else if (constantA)
    {
        const Mat& B = bottom_blobs[0];
        M = constantM;
        N = transB ? (B.dims == 3 ? B.c : B.h) * B.elempack : B.w;
    }
    else if (constantB)
    {
        const Mat& A = bottom_blobs[0];
        M = transA ? A.w : (A.dims == 3 ? A.c : A.h) * A.elempack;
        N = constantN;
    }
    else
    {
        const Mat& A = bottom_blobs[0];
        const Mat& B = bottom_blobs[1];
        M = transA ? A.w : (A.dims == 3 ? A.c : A.h) * A.elempack;
        N = transB ? (B.dims == 3 ? B.c : B.h) * B.elempack : B.w;
    }

    Mat C;
    int broadcast_type_C = 0;
    if (constantC)
    {
        C = CT_data;
        broadcast_type_C = constant_broadcast_type_C;
    }
    else
    {
        if (constantA && constantB)
        {
            C = bottom_blobs.size() == 1 ? bottom_blobs[0] : Mat();
        }
        else if (constantA)
        {
            C = bottom_blobs.size() == 2 ? bottom_blobs[1] : Mat();
        }
        else if (constantB)
        {
            C = bottom_blobs.size() == 2 ? bottom_blobs[1] : Mat();
        }
        else
        {
            C = bottom_blobs.size() == 3 ? bottom_blobs[2] : Mat();
        }

        if (!C.empty())
        {
            if (C.dims == 1 && C.w == 1)
            {
                // scalar
                broadcast_type_C = 0;
            }
            if (C.dims == 1 && C.w * C.elempack == M)
            {
                // M
                // auto broadcast from h to w is the ncnn-style convention
                broadcast_type_C = 1;
            }
            if (C.dims == 1 && C.w * C.elempack == N)
            {
                // N
                broadcast_type_C = 4;
            }
            if (C.dims == 2 && C.w == 1 && C.h * C.elempack == M)
            {
                // Mx1
                broadcast_type_C = 2;
            }
            if (C.dims == 2 && C.w == N && C.h * C.elempack == M)
            {
                // MxN
                broadcast_type_C = 3;
            }
            if (C.dims == 2 && C.w == N && C.h * C.elempack == 1)
            {
                // 1xN
                broadcast_type_C = 4;
            }

            // cast to fp32
            {
                Mat CT_data;
                cast_float16_to_float32(C, CT_data);
                C = CT_data;
            }

            // pre-multiply C with beta
            if (beta != 1.f)
            {
                Mat CT_data;
                CT_data.create_like(C, opt.workspace_allocator);

                const int size = C.total() * C.elempack;
                for (int i = 0; i < size; i++)
                {
                    CT_data[i] = C[i] * beta;
                }

                C = CT_data;
            }
        }
    }

    int out_elempack = 1;
    if (opt.use_packing_layout)
    {
        out_elempack = M % 4 == 0 ? 4 : 1;
    }
    if (output_elempack)
        out_elempack = output_elempack;
    size_t out_elemsize = 2u * out_elempack;

    Mat& top_blob = top_blobs[0];
    if (output_N1M)
        top_blob.create(N, 1, M / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
    else
        top_blob.create(N, M / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
    if (top_blob.empty())
        return -100;

    int _nT = nT ? nT : opt.num_threads;
    if (nT != 0 && opt.num_threads != nT)
    {
        // force num_threads the same as in create_pipeline
        // so we could use pre-packed A/B from the same tile config
        NCNN_LOGE("opt.num_threads %d changed, gemm will use load-time value %d", opt.num_threads, nT);
    }

    int ret = 0;
    if (constantA && constantB)
    {
        ret = gemm_AT_BT_arm_fp16s(AT_data, BT_data, C, top_blob, broadcast_type_C, constantM, constantN, constantK, alpha, _nT, opt);
    }
    else if (constantA)
    {
        const Mat& B = bottom_blobs[0];
        ret = gemm_AT_arm_fp16s(AT_data, B, C, top_blob, broadcast_type_C, constantM, constantK, transB, alpha, _nT, opt);
    }
    else if (constantB)
    {
        const Mat& A = bottom_blobs[0];
        ret = gemm_BT_arm_fp16s(A, BT_data, C, top_blob, broadcast_type_C, constantN, constantK, transA, alpha, _nT, opt);
    }
    else
    {
        const Mat& A = bottom_blobs[0];
        const Mat& B = bottom_blobs[1];
        ret = gemm_arm_fp16s(A, B, C, top_blob, broadcast_type_C, transA, transB, alpha, _nT, opt);
    }

    return ret;
}

} // namespace ncnn
