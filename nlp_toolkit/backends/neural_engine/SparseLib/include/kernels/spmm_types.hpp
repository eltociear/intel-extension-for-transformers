//  Copyright (c) 2021 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#ifndef ENGINE_SPARSELIB_INCLUDE_KERNELS_SPMM_TYPES_HPP_
#define ENGINE_SPARSELIB_INCLUDE_KERNELS_SPMM_TYPES_HPP_
#include <cstdint>
#include <vector>

#include "param_types.hpp"
#include "utils.hpp"

namespace jd {
template <typename T>
class csrp_data_t;
template <typename T>
class bsc_data_t;
template <typename T>
class bsr_data_t;
namespace ssd {
/**
 * @brief tensors index configuration of this kernel.
 */
static constexpr int WEI = 0;
static constexpr int SRC = 1;
static constexpr int BIAS = 2;
static constexpr int DST = 3;
static constexpr int SCALES = 4;

/**
 * @brief Scenarios supported by spmm_vnni kernel/algorithm.
 */
enum class sparse_scheme : uint8_t {
  undef,
  sparse_x_dense,
  dense_x_sparse,
  sparse_x_sparse,
};

/**
 * @brief kernel parameters passed between kernel/primitive and jit_domain.
 */
struct flat_param_t {
  int64_t M;
  int64_t K;
  int64_t N;
  bool has_bias;
  bool append_sum;
  data_type output_type;
  sparse_scheme scheme;
  std::vector<int64_t> mkn_blocks;
  std::vector<int64_t> tile_shape;  // 2d vector for microkernel shape in terms of zmm registers
  bool sub_func;
  int64_t im_start;  // start m-idx of dest to be calculated
  int64_t im_end;    // end m-idx of dest to be calculated
  int64_t in_start;  // start n-idx of dest to be calculated
  int64_t in_end;    // end n-idx of dest to be calculated
  // sparse weight related
  bsr_data_t<int8_t>* sparse_ptr;
};

/**
 * @brief kernel data at runtime.
 */
struct flat_data_t {
  const void* ptr_seq_vals;  // sequence nonzeros of sparse weight.
  const void* ptr_dense;     // activation(K, N).
  const void* ptr_bias;      // bias(M, 1).
  void* ptr_dst;             // dst(M, N).
  const void* ptr_scales;
};

/**
 * @brief kernel parameters for kernel initialization
 */
template <typename T>
struct amx_params_t {
  dim_t num_tileM;
  dim_t tileM;
  dim_t tileN;
  dim_t shape[2];
  dim_t blocksize[2] = {16, 1};
  dim_t blocks_per_group = 64 / sizeof(T);
  dim_t nnz_group;
  dim_t nrowptr;
  dim_t* colidxs;
  dim_t* group_rowptr;
  T* weight;
  bool has_bias;
  bool bf16_out;
};

typedef amx_params_t<bfloat16_t> amx_bf16_params_t;

/**
 * @brief kernel inputs for kernel runtime
 */
template <typename src_t, typename wgt_t, typename dst_t>
struct amx_inputs_t {
  src_t* weight;
  wgt_t* src;
  float* bias;  // bias always be float for both bf16 and int8 kernels
  dst_t* dst;
};

typedef amx_inputs_t<bfloat16_t, bfloat16_t, float> amx_bf16f32_inputs_t;
typedef amx_inputs_t<bfloat16_t, bfloat16_t, bfloat16_t> amx_bf16bf16_inputs_t;

struct avx512_fp32_params_t {
  int64_t M;
  int64_t K;
  int64_t N;
  bool has_bias;
  bsc_data_t<float>* sparse_ptr;
  int64_t im_start;  // start m-idx of dest to be calculated
  int64_t im_end;    // end m-idx of dest to be calculated
  int64_t in_start;  // start n-idx of dest to be calculated
  int64_t in_end;    // end n-idx of dest to be calculated
  std::vector<postop_attr> postop_attrs;
};

/**
 * @brief kernel data at runtime.
 */
struct avx512_data_t {
  const float* dense;
  const float* sparse;
  const float* bias;
  float* dst;
};

}  // namespace ssd
}  // namespace jd
#endif  // ENGINE_SPARSELIB_INCLUDE_KERNELS_SPMM_TYPES_HPP_
