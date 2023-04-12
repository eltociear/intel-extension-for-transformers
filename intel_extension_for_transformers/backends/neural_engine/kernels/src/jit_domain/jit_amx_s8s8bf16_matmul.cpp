//  Copyright (c) 2022 Intel Corporation
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

#include "jit_domain/jit_amx_s8s8bf16_matmul.hpp"

#include "regs_pool.hpp"

namespace jd {

#define GET_OFF(field) offsetof(ssd::dynamic_quant_matmul_data_t, field)

void jit_amx_s8s8bf16_matmul_t::generate() {
  Xbyak::Label cfg_label;
  auto trans_block_col = param_.k / param_.tile_k;
  inLocalLabel();
  {
    const int cfg_size = sizeof(tileconfig_t);
    regs_pool rp(this, 1, {13, 4, 0});

    const auto reg_m_loop = rp.reg<Reg64>();
    const auto reg_n_loop = rp.reg<Reg64>();
    const auto reg_strideA = rp.reg<Reg64>();
    const auto reg_strideB = rp.reg<Reg64>();
    const auto reg_strideTmpbuf = rp.reg<Reg64>();
    const auto reg_tmpbuf = rp.reg<Reg64>();
    const auto reg_scale_w = rp.reg<Reg64>();
    const auto reg_scale_a = rp.reg<Reg64>();
    const auto reg_bias = rp.reg<Reg64>();
    const auto reg_dst = rp.reg<Reg64>();

    auto prepare_mask = [&]() {
      const auto reg_tmp = rp.reg<Xbyak::Reg32>();
      mov(reg_tmp, 0xffff >> param_.write_mask);
      kmovd(matC_n_mask_, reg_tmp);
    };

    auto ip_Mx16 = [&](int M, int block_num, bool need_mask = false, int no_mask_tail_n = 0) {
      const auto reg_tmp = rp.reg<Reg64>();
      // build block
      {
        const auto reg_matA_addr = rp.reg<Reg64>();
        const auto reg_matB_addr = rp.reg<Reg64>();
        for (int i = 0; i < block_num; i++) tilezero(Tmm(i));
        // prepare addr & stride;
        mov(reg_matA_addr, ptr[rp.p[0] + GET_OFF(activation)]);
        mov(reg_matB_addr, ptr[rp.p[0] + GET_OFF(reordered_weight)]);
        imul(reg_tmp, reg_m_loop, 16 * param_.k);
        add(reg_matA_addr, reg_tmp);
        imul(reg_tmp, reg_n_loop, param_.align_build_block_num * trans_block_col * 64 * (param_.tile_k / 4));
        add(reg_matB_addr, reg_tmp);
        for (int k_loop = 0; k_loop < param_.k / param_.tile_k; k_loop++) {
          tileloadd(Tmm(3), ptr[reg_matA_addr + reg_strideA + k_loop * param_.tile_k]);
          for (int idx = 0; idx < block_num; idx++) {
            int offset = (idx + no_mask_tail_n) * trans_block_col * 64 * (param_.tile_k / 4) + k_loop * 64;
            tileloadd(Tmm(4 + idx), ptr[reg_matB_addr + reg_strideB + offset]);
            tdpbssd(Tmm(idx), Tmm(3), Tmm(4 + idx));
          }
        }
      }
      // store block
      {
        for (int idx = 0; idx < block_num; idx++)
          tilestored(ptr[reg_tmpbuf + reg_strideTmpbuf + idx * 16 * sizeof(int)], Tmm(idx));
        // dequant + add_bias
        const auto zmms = rp.regs<Zmm, 4>();
        const auto reg_tmp2 = rp.reg<Reg64>();
        const auto reg_dst_offset = rp.reg<Reg64>();

        imul(reg_dst_offset, reg_m_loop, 16 * dst_n_dim_);
        imul(reg_tmp, reg_n_loop, param_.align_build_block_num * 16);
        add(reg_dst_offset, reg_tmp);
        imul(reg_tmp, reg_n_loop, 16 * param_.align_build_block_num * sizeof(int));  // offset of scale_w & bias
        imul(reg_tmp2, reg_m_loop, 16 * sizeof(float));                              // offset of scale_a
        for (int idx = 0; idx < block_num; idx++) {
          vmovups(zmms[0], ptr[reg_scale_w + reg_tmp + (idx + no_mask_tail_n) * 16 * sizeof(float)]);
          if (param_.add_bias) vmovups(zmms[1], ptr[reg_bias + reg_tmp + (idx + no_mask_tail_n) * 16 * sizeof(float)]);
          for (int row_loop = 0; row_loop < M; row_loop++) {
            vcvtdq2ps(zmms[2], ptr[reg_tmpbuf + (idx + row_loop * param_.align_build_block_num) * 16 * sizeof(float)]);
            vbroadcastss(zmms[3], dword[reg_scale_a + reg_tmp2 + row_loop * sizeof(float)]);
            vmulps(zmms[2], zmms[2], zmms[3]);
            if (param_.add_bias)
              vfmadd213ps(zmms[2], zmms[0], zmms[1]);
            else
              vmulps(zmms[2], zmms[2], zmms[0]);

            fp32_cvt_bf16(zmms[2]);
            RegExp write_back_addr = reg_dst + reg_dst_offset * sizeof(bfloat16_t) +
                                     ((no_mask_tail_n + idx) * 16 + row_loop * dst_n_dim_) * sizeof(bfloat16_t);
            vmovdqu16(need_mask ? ptr[write_back_addr] | matC_n_mask_ : ptr[write_back_addr], Ymm(zmms[2].getIdx()));
          }
        }
      }
    };

    auto align_loop_ip_Mx16 = [&](int M, int loop_num, std::string label_prefix) {
      L(label_prefix + "align_n_loop");
      ip_Mx16(M, param_.align_build_block_num);
      inc(reg_n_loop);
      cmp(reg_n_loop, loop_num);
      jl(label_prefix + "align_n_loop");
    };

    auto build_MxN_tile = [&](int M, std::string label_prefix = ".") {
      xor_(reg_n_loop, reg_n_loop);
      if (param_.write_mask == 0) {
        if (param_.align_n_loop > 0) align_loop_ip_Mx16(M, param_.align_n_loop, label_prefix);
        if (param_.tail_n_loop != 0) ip_Mx16(M, param_.tail_n_loop);
      } else {
        int no_mask_align_n_loop = (param_.n - 16) / (16 * param_.align_build_block_num);
        int no_mask_tail_n = ((param_.n - 16) % (16 * param_.align_build_block_num)) / 16;
        if (no_mask_align_n_loop > 0) align_loop_ip_Mx16(M, no_mask_align_n_loop, label_prefix);
        if (no_mask_tail_n != 0) ip_Mx16(M, no_mask_tail_n);
        ip_Mx16(M, 1, true, no_mask_tail_n);
      }
    };

    prepare_mask();
    xor_(reg_m_loop, reg_m_loop);
    mov(reg_strideA, param_.k);
    mov(reg_strideB, trans_block_col * 64);
    mov(reg_strideTmpbuf, 16 * param_.align_build_block_num * sizeof(int));
    mov(reg_tmpbuf, ptr[rp.p[0] + GET_OFF(tmp_buf)]);
    mov(reg_scale_a, ptr[rp.p[0] + GET_OFF(scale_a)]);
    mov(reg_scale_w, ptr[rp.p[0] + GET_OFF(scale_w)]);
    mov(reg_bias, ptr[rp.p[0] + GET_OFF(bias)]);
    mov(reg_dst, ptr[rp.p[0] + GET_OFF(dst)]);

    if (param_.align_m_loop > 0) {
      ldtilecfg(ptr[rip + cfg_label]);
      L("align_m_loop");
      build_MxN_tile(16);
      inc(reg_m_loop);
      cmp(reg_m_loop, param_.align_m_loop);
      jl("align_m_loop");
    }
    if (param_.tail_m != 0) {
      ldtilecfg(ptr[rip + cfg_label + cfg_size]);
      build_MxN_tile(param_.tail_m, ".tail_");
    }
  }
  outLocalLabel();
  L(cfg_label);
  db(reinterpret_cast<uint8_t*>(&param_.m_align_cfg), sizeof(tileconfig_t));
  db(reinterpret_cast<uint8_t*>(&param_.m_tail_cfg), sizeof(tileconfig_t));
}

}  // namespace jd
