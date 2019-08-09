/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <iostream>
#include "GenerateKernel.h"

namespace fbgemm {

template <typename TA, typename TB, typename TC, typename accT>
thread_local asmjit::JitRuntime CodeGenBase<TA, TB, TC, accT>::rt_;

template <typename TA, typename TB, typename TC, typename accT>
thread_local asmjit::CodeHolder CodeGenBase<TA, TB, TC, accT>::code_;

template <typename TA, typename TB, typename TC, typename accT>
thread_local std::map<
    std::tuple<bool, int, int, int, int, int, int, int>,
    typename CodeGenBase<TA, TB, TC, accT>::jit_micro_kernel_fp>
    CodeGenBase<TA, TB, TC, accT>::codeCache_;

namespace x86 = asmjit::x86;

/**
 * Generate AVX2 instructions for initializing the C registers to 0 in 16-bit
 * Accumulation kernel.
 */
template <>
template <>
void CodeGenBase<uint8_t, int8_t, int32_t, int16_t>::initCRegs<
    inst_set_t::avx2>(
    x86::Emitter* a,
    int rowRegs,
    int colRegs,
    int leadingDimCReg) {
  for (int i = 0; i < rowRegs; ++i) {
    for (int j = 0; j < colRegs; ++j) {
      a->vxorps(
          CRegs_avx2_[i * leadingDimCReg + j],
          CRegs_avx2_[i * leadingDimCReg + j],
          CRegs_avx2_[i * leadingDimCReg + j]);
    }
  }
}

/**
 * Generate AVX2 instructions for computing block in the rank-k update of 16-bit
 * Accmulation kernel.
 */
template <>
template <>
void CodeGenBase<uint8_t, int8_t, int32_t, int16_t>::genComputeBlock<
    inst_set_t::avx2>(
    x86::Emitter* a,
    x86::Gp buffer_A,
    x86::Gp buffer_B,
    x86::Gp /* unused (reserved for prefetching)*/,
    int rowRegs,
    int colRegs,
    int lda,
    int leadingDimCReg) {
  // used for matrix A
  x86::Ymm AReg = x86::ymm12;

  x86::Ymm tmpReg = x86::ymm14;

  for (int i = 0; i < rowRegs; ++i) {
    // broadcast A
    a->vpbroadcastw(
        AReg, x86::dword_ptr(buffer_A, (i * lda) * sizeof(uint8_t)));
    for (int j = 0; j < colRegs; ++j) {
      a->vpmaddubsw(
          tmpReg, AReg, x86::dword_ptr(buffer_B, j * VLEN_ * sizeof(int8_t)));
      a->vpaddsw(
          CRegs_avx2_[i * leadingDimCReg + j],
          tmpReg,
          CRegs_avx2_[i * leadingDimCReg + j]);
      // Prefetching is hurting performance in some cases
      // because prefetch instructions itself consumes a slot
      // in pipeline issue thus slowing down the kernel.
      // if((i == rowRegs - 1) && j % 2 == 0){
      // a->prefetcht0(x86::dword_ptr(B_pf, j*VLEN_*sizeof(int8_t)));
      //}
    }
  }
}

/**
 * Generate AVX2 instructions for storing the C registers back to the memory in
 * 16-bit Accumulation kernel.
 */
template <>
template <>
void CodeGenBase<uint8_t, int8_t, int32_t, int16_t>::storeCRegs<
    inst_set_t::avx2>(
    x86::Emitter* a,
    int rowRegs,
    int colRegs,
    x86::Gp C_Offset,
    x86::Gp ldcReg,
    bool accum,
    int leadingDimCReg) {
  x86::Xmm extractDest128 = x86::xmm15;
  x86::Ymm extractDest256 = x86::ymm15;

  for (int i = 0; i < rowRegs; ++i) {
    a->imul(C_Offset, ldcReg, static_cast<asmjit::Imm>(i * sizeof(int32_t)));
    for (int j = 0; j < colRegs; ++j) {
      for (int idx = 0; idx < 2; ++idx) {
        a->vextracti128(
            extractDest128, CRegs_avx2_[i * leadingDimCReg + j], idx);
        a->vpmovsxwd(extractDest256, extractDest128);
        x86::Mem destAddr = x86::dword_ptr(
            a->zcx(), C_Offset, 0, (j * 2 + idx) * 8 * sizeof(int32_t));
        if (accum) {
          a->vpaddd(extractDest256, extractDest256, destAddr);
        }
        a->vmovups(destAddr, extractDest256);
      }
    }
  }
}

/**
 * Get or Create the AVX2 instructions for 16-bit Accumulation macro-kernel.
 *
 */
template <>
template <>
CodeGenBase<uint8_t, int8_t, int32_t, int16_t>::jit_micro_kernel_fp
CodeGenBase<uint8_t, int8_t, int32_t, int16_t>::getOrCreate<inst_set_t::avx2>(
    bool accum,
    int32_t mc,
    int32_t nc,
    int32_t kc,
    int32_t /* unused */) {
  std::tuple<bool, int, int, int, int, int, int, int> kernelSig;
  int kBlock;
  int nBlock;
  int mRegBlockSize;
  int nRegBlockSize;
  int nRegBlockSizeMin;
  int row_interleave;

  if (blocking_params) {
    kBlock = blocking_params->KCB;
    nBlock = blocking_params->NCB;
    mRegBlockSize = blocking_params->MR;
    nRegBlockSize = blocking_params->NR;
    nRegBlockSizeMin = blocking_params->NR_MIN;
    row_interleave = blocking_params->ROW_INTERLEAVE;
  } else {
    kBlock = PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::KCB;
    nBlock = PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::NCB;
    mRegBlockSize = PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::MR;
    nRegBlockSize = PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::NR;
    nRegBlockSizeMin =
        PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::NR_MIN;
    row_interleave =
        PackingTraits<uint8_t, int16_t, inst_set_t::avx2>::ROW_INTERLEAVE;
  }

  kernelSig = std::make_tuple(
      accum,
      mc,
      nc,
      nBlock,
      kBlock,
      mRegBlockSize,
      nRegBlockSize,
      nRegBlockSizeMin);

  if (codeCache_.find(kernelSig) != codeCache_.end()) {
    return codeCache_[kernelSig];
  }
  code_.reset(false);
  code_.init(rt_.codeInfo());
  x86::Assembler assembler(&code_);
  x86::Emitter* a = assembler.as<x86::Emitter>();

#if defined(FBGEMM_LOG_CODE)
  // generated code logging
  FILE* codeLogfile = fopen(
      getCodeLoggingFile<inst_set_t::avx2>(
          accum,
          mc,
          nc,
          nBlock,
          kBlock,
          mRegBlockSize,
          nRegBlockSize,
          nRegBlockSizeMin)
          .c_str(),
      "w");
  asmjit::FileLogger* codeLogger = new asmjit::FileLogger(codeLogfile);
  if (codeLogger) {
    code_.setLogger(codeLogger);
  }
#endif

  int mRegBlocks = mc / mRegBlockSize;
  int mRegBlocksRem = mc % mRegBlockSize;
  assert(kc % row_interleave == 0 && "kc must be a multiple of row_interleave");
  // assert((nc == nRegBlockSize) &&
  //"nc must be equal to the number of register blocks");

  // arguments to the function created
  x86::Gp buffer_A = a->zdi();
  x86::Gp buffer_B = a->zsi();
  x86::Gp B_pf = a->zdx();
  x86::Gp CBase = a->zcx();
  x86::Gp kSize = a->gpz(8);
  x86::Gp ldcReg = a->gpz(9);

  asmjit::FuncDetail func;
  func.init(
      asmjit::
          FuncSignatureT<void, uint8_t*, int8_t*, int8_t*, int32_t*, int, int>(
              asmjit::CallConv::kIdHost));

  asmjit::FuncFrame frame;
  frame.init(func);
  frame.setDirtyRegs(
      x86::Reg::kGroupVec,
      asmjit::Support::bitMask(0, 1, 2, 3, 4, 5, 6, 7) |
          asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15));
  frame.setDirtyRegs(
      x86::Reg::kGroupGp, asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14));

  asmjit::FuncArgsAssignment args(&func);
  args.assignAll(buffer_A, buffer_B, B_pf, CBase, kSize, ldcReg);

  args.updateFuncFrame(frame);
  frame.finalize();

  a->emitProlog(frame);
  a->emitArgsAssignment(frame, args);

  asmjit::Label Loopk = a->newLabel();
  asmjit::Label LoopMBlocks = a->newLabel();

  x86::Gp buffer_B_saved = a->gpz(10);
  x86::Gp C_Offset = a->gpz(11);
  // x86::Gp B_pf_saved = a->gpz(12);
  x86::Gp iIdx = a->gpz(13);
  x86::Gp kIdx = a->gpz(14);

  int colRegs = nc * row_interleave * sizeof(int8_t) / VLEN_;
  if (mRegBlocks > 0) {
    // move 0 to iteration variables
    a->mov(iIdx, 0);

    // save B_buffer address
    a->mov(buffer_B_saved, buffer_B);
    // a->mov(B_pf_saved, B_pf);

    a->bind(LoopMBlocks);
    a->inc(iIdx);

    int rowRegs = mRegBlockSize;

    // init C registers
    initCRegs<inst_set_t::avx2>(a, rowRegs, colRegs);

    // init k loop index
    a->mov(kIdx, 0);
    a->bind(Loopk);
    // k is incremented by row_interleave
    a->add(kIdx, static_cast<asmjit::Imm>(row_interleave));

    genComputeBlock<inst_set_t::avx2>(
        a, buffer_A, buffer_B, B_pf, rowRegs, colRegs, kBlock);

    // update buffer_A address for next k iteration
    a->add(
        buffer_A, static_cast<asmjit::Imm>(row_interleave * sizeof(uint8_t)));

    // update buffer_B address for next k iteration
    a->add(
        buffer_B,
        static_cast<asmjit::Imm>(nBlock * row_interleave * sizeof(int8_t)));
    // a->add(B_pf, static_cast<asmjit::Imm>(nBlock * row_interleave *
    // sizeof(int8_t)));

    a->cmp(kIdx, kSize);
    a->jl(Loopk);

    // store C matrix
    storeCRegs<inst_set_t::avx2>(a, rowRegs, colRegs, C_Offset, ldcReg, accum);

    // increment A for next block
    a->sub(buffer_A, kSize);
    a->add(
        buffer_A, static_cast<asmjit::Imm>((rowRegs)*kBlock * sizeof(uint8_t)));
    // increment C for next block
    a->imul(
        C_Offset, ldcReg, static_cast<asmjit::Imm>(rowRegs * sizeof(int32_t)));
    a->add(CBase, C_Offset);
    // reset B
    a->mov(buffer_B, buffer_B_saved);
    // a->mov(B_pf, B_pf_saved);

    a->cmp(iIdx, mRegBlocks);
    a->jl(LoopMBlocks);
  }
  // generate code for remainder
  if (mRegBlocksRem > 0) {
    asmjit::Label LoopkRem = a->newLabel();
    int rowRegs = mRegBlocksRem;

    // init C registers
    initCRegs<inst_set_t::avx2>(a, rowRegs, colRegs);

    // init k loop index
    a->mov(kIdx, 0);
    a->bind(LoopkRem);

    // k is incremented by row_interleave
    a->add(kIdx, static_cast<asmjit::Imm>(row_interleave));

    genComputeBlock<inst_set_t::avx2>(
        a, buffer_A, buffer_B, B_pf, rowRegs, colRegs, kBlock);

    // update buffer_A address for next k iteration
    a->add(
        buffer_A, static_cast<asmjit::Imm>(row_interleave * sizeof(uint8_t)));

    // update buffer_B address for next k iteration
    a->add(
        buffer_B,
        static_cast<asmjit::Imm>(nBlock * row_interleave * sizeof(int8_t)));
    // a->add(B_pf, static_cast<asmjit::Imm>(nBlock * row_interleave *
    // sizeof(int8_t)));

    a->cmp(kIdx, kSize);
    a->jl(LoopkRem);

    // store C matrix
    storeCRegs<inst_set_t::avx2>(a, rowRegs, colRegs, C_Offset, ldcReg, accum);
  }

  a->emitEpilog(frame);

  jit_micro_kernel_fp fn;
  asmjit::Error err = rt_.add(&fn, &code_);
  if (err) {
    std::cout << "Error: in fn add" << std::endl;
    return nullptr;
  }
  codeCache_[kernelSig] = fn;

#if defined(FBGEMM_LOG_CODE)
  fclose(codeLogfile);
  delete codeLogger;
#endif

  return fn;
}

} // namespace fbgemm
