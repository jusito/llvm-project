//=== AArch64PostLegalizerCombiner.cpp --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Post-legalization combines on generic MachineInstrs.
///
/// The combines here must preserve instruction legality.
///
/// Lowering combines (e.g. pseudo matching) should be handled by
/// AArch64PostLegalizerLowering.
///
/// Combines which don't rely on instruction legality should go in the
/// AArch64PreLegalizerCombiner.
///
//===----------------------------------------------------------------------===//

#include "AArch64TargetMachine.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/CombinerInfo.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutor.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/GISelKnownBits.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/MIPatternMatch.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/Support/Debug.h"

#define GET_GICOMBINER_DEPS
#include "AArch64GenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_DEPS

#define DEBUG_TYPE "aarch64-postlegalizer-combiner"

using namespace llvm;
using namespace MIPatternMatch;

namespace {

#define GET_GICOMBINER_TYPES
#include "AArch64GenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_TYPES

/// This combine tries do what performExtractVectorEltCombine does in SDAG.
/// Rewrite for pairwise fadd pattern
///   (s32 (g_extract_vector_elt
///           (g_fadd (vXs32 Other)
///                  (g_vector_shuffle (vXs32 Other) undef <1,X,...> )) 0))
/// ->
///   (s32 (g_fadd (g_extract_vector_elt (vXs32 Other) 0)
///              (g_extract_vector_elt (vXs32 Other) 1))
bool matchExtractVecEltPairwiseAdd(
    MachineInstr &MI, MachineRegisterInfo &MRI,
    std::tuple<unsigned, LLT, Register> &MatchInfo) {
  Register Src1 = MI.getOperand(1).getReg();
  Register Src2 = MI.getOperand(2).getReg();
  LLT DstTy = MRI.getType(MI.getOperand(0).getReg());

  auto Cst = getIConstantVRegValWithLookThrough(Src2, MRI);
  if (!Cst || Cst->Value != 0)
    return false;
  // SDAG also checks for FullFP16, but this looks to be beneficial anyway.

  // Now check for an fadd operation. TODO: expand this for integer add?
  auto *FAddMI = getOpcodeDef(TargetOpcode::G_FADD, Src1, MRI);
  if (!FAddMI)
    return false;

  // If we add support for integer add, must restrict these types to just s64.
  unsigned DstSize = DstTy.getSizeInBits();
  if (DstSize != 16 && DstSize != 32 && DstSize != 64)
    return false;

  Register Src1Op1 = FAddMI->getOperand(1).getReg();
  Register Src1Op2 = FAddMI->getOperand(2).getReg();
  MachineInstr *Shuffle =
      getOpcodeDef(TargetOpcode::G_SHUFFLE_VECTOR, Src1Op2, MRI);
  MachineInstr *Other = MRI.getVRegDef(Src1Op1);
  if (!Shuffle) {
    Shuffle = getOpcodeDef(TargetOpcode::G_SHUFFLE_VECTOR, Src1Op1, MRI);
    Other = MRI.getVRegDef(Src1Op2);
  }

  // We're looking for a shuffle that moves the second element to index 0.
  if (Shuffle && Shuffle->getOperand(3).getShuffleMask()[0] == 1 &&
      Other == MRI.getVRegDef(Shuffle->getOperand(1).getReg())) {
    std::get<0>(MatchInfo) = TargetOpcode::G_FADD;
    std::get<1>(MatchInfo) = DstTy;
    std::get<2>(MatchInfo) = Other->getOperand(0).getReg();
    return true;
  }
  return false;
}

void applyExtractVecEltPairwiseAdd(
    MachineInstr &MI, MachineRegisterInfo &MRI, MachineIRBuilder &B,
    std::tuple<unsigned, LLT, Register> &MatchInfo) {
  unsigned Opc = std::get<0>(MatchInfo);
  assert(Opc == TargetOpcode::G_FADD && "Unexpected opcode!");
  // We want to generate two extracts of elements 0 and 1, and add them.
  LLT Ty = std::get<1>(MatchInfo);
  Register Src = std::get<2>(MatchInfo);
  LLT s64 = LLT::scalar(64);
  B.setInstrAndDebugLoc(MI);
  auto Elt0 = B.buildExtractVectorElement(Ty, Src, B.buildConstant(s64, 0));
  auto Elt1 = B.buildExtractVectorElement(Ty, Src, B.buildConstant(s64, 1));
  B.buildInstr(Opc, {MI.getOperand(0).getReg()}, {Elt0, Elt1});
  MI.eraseFromParent();
}

bool isSignExtended(Register R, MachineRegisterInfo &MRI) {
  // TODO: check if extended build vector as well.
  unsigned Opc = MRI.getVRegDef(R)->getOpcode();
  return Opc == TargetOpcode::G_SEXT || Opc == TargetOpcode::G_SEXT_INREG;
}

bool isZeroExtended(Register R, MachineRegisterInfo &MRI) {
  // TODO: check if extended build vector as well.
  return MRI.getVRegDef(R)->getOpcode() == TargetOpcode::G_ZEXT;
}

bool matchAArch64MulConstCombine(
    MachineInstr &MI, MachineRegisterInfo &MRI,
    std::function<void(MachineIRBuilder &B, Register DstReg)> &ApplyFn) {
  assert(MI.getOpcode() == TargetOpcode::G_MUL);
  Register LHS = MI.getOperand(1).getReg();
  Register RHS = MI.getOperand(2).getReg();
  Register Dst = MI.getOperand(0).getReg();
  const LLT Ty = MRI.getType(LHS);

  // The below optimizations require a constant RHS.
  auto Const = getIConstantVRegValWithLookThrough(RHS, MRI);
  if (!Const)
    return false;

  APInt ConstValue = Const->Value.sext(Ty.getSizeInBits());
  // The following code is ported from AArch64ISelLowering.
  // Multiplication of a power of two plus/minus one can be done more
  // cheaply as shift+add/sub. For now, this is true unilaterally. If
  // future CPUs have a cheaper MADD instruction, this may need to be
  // gated on a subtarget feature. For Cyclone, 32-bit MADD is 4 cycles and
  // 64-bit is 5 cycles, so this is always a win.
  // More aggressively, some multiplications N0 * C can be lowered to
  // shift+add+shift if the constant C = A * B where A = 2^N + 1 and B = 2^M,
  // e.g. 6=3*2=(2+1)*2.
  // TODO: consider lowering more cases, e.g. C = 14, -6, -14 or even 45
  // which equals to (1+2)*16-(1+2).
  // TrailingZeroes is used to test if the mul can be lowered to
  // shift+add+shift.
  unsigned TrailingZeroes = ConstValue.countr_zero();
  if (TrailingZeroes) {
    // Conservatively do not lower to shift+add+shift if the mul might be
    // folded into smul or umul.
    if (MRI.hasOneNonDBGUse(LHS) &&
        (isSignExtended(LHS, MRI) || isZeroExtended(LHS, MRI)))
      return false;
    // Conservatively do not lower to shift+add+shift if the mul might be
    // folded into madd or msub.
    if (MRI.hasOneNonDBGUse(Dst)) {
      MachineInstr &UseMI = *MRI.use_instr_begin(Dst);
      unsigned UseOpc = UseMI.getOpcode();
      if (UseOpc == TargetOpcode::G_ADD || UseOpc == TargetOpcode::G_PTR_ADD ||
          UseOpc == TargetOpcode::G_SUB)
        return false;
    }
  }
  // Use ShiftedConstValue instead of ConstValue to support both shift+add/sub
  // and shift+add+shift.
  APInt ShiftedConstValue = ConstValue.ashr(TrailingZeroes);

  unsigned ShiftAmt, AddSubOpc;
  // Is the shifted value the LHS operand of the add/sub?
  bool ShiftValUseIsLHS = true;
  // Do we need to negate the result?
  bool NegateResult = false;

  if (ConstValue.isNonNegative()) {
    // (mul x, 2^N + 1) => (add (shl x, N), x)
    // (mul x, 2^N - 1) => (sub (shl x, N), x)
    // (mul x, (2^N + 1) * 2^M) => (shl (add (shl x, N), x), M)
    APInt SCVMinus1 = ShiftedConstValue - 1;
    APInt CVPlus1 = ConstValue + 1;
    if (SCVMinus1.isPowerOf2()) {
      ShiftAmt = SCVMinus1.logBase2();
      AddSubOpc = TargetOpcode::G_ADD;
    } else if (CVPlus1.isPowerOf2()) {
      ShiftAmt = CVPlus1.logBase2();
      AddSubOpc = TargetOpcode::G_SUB;
    } else
      return false;
  } else {
    // (mul x, -(2^N - 1)) => (sub x, (shl x, N))
    // (mul x, -(2^N + 1)) => - (add (shl x, N), x)
    APInt CVNegPlus1 = -ConstValue + 1;
    APInt CVNegMinus1 = -ConstValue - 1;
    if (CVNegPlus1.isPowerOf2()) {
      ShiftAmt = CVNegPlus1.logBase2();
      AddSubOpc = TargetOpcode::G_SUB;
      ShiftValUseIsLHS = false;
    } else if (CVNegMinus1.isPowerOf2()) {
      ShiftAmt = CVNegMinus1.logBase2();
      AddSubOpc = TargetOpcode::G_ADD;
      NegateResult = true;
    } else
      return false;
  }

  if (NegateResult && TrailingZeroes)
    return false;

  ApplyFn = [=](MachineIRBuilder &B, Register DstReg) {
    auto Shift = B.buildConstant(LLT::scalar(64), ShiftAmt);
    auto ShiftedVal = B.buildShl(Ty, LHS, Shift);

    Register AddSubLHS = ShiftValUseIsLHS ? ShiftedVal.getReg(0) : LHS;
    Register AddSubRHS = ShiftValUseIsLHS ? LHS : ShiftedVal.getReg(0);
    auto Res = B.buildInstr(AddSubOpc, {Ty}, {AddSubLHS, AddSubRHS});
    assert(!(NegateResult && TrailingZeroes) &&
           "NegateResult and TrailingZeroes cannot both be true for now.");
    // Negate the result.
    if (NegateResult) {
      B.buildSub(DstReg, B.buildConstant(Ty, 0), Res);
      return;
    }
    // Shift the result.
    if (TrailingZeroes) {
      B.buildShl(DstReg, Res, B.buildConstant(LLT::scalar(64), TrailingZeroes));
      return;
    }
    B.buildCopy(DstReg, Res.getReg(0));
  };
  return true;
}

void applyAArch64MulConstCombine(
    MachineInstr &MI, MachineRegisterInfo &MRI, MachineIRBuilder &B,
    std::function<void(MachineIRBuilder &B, Register DstReg)> &ApplyFn) {
  B.setInstrAndDebugLoc(MI);
  ApplyFn(B, MI.getOperand(0).getReg());
  MI.eraseFromParent();
}

/// Try to fold a G_MERGE_VALUES of 2 s32 sources, where the second source
/// is a zero, into a G_ZEXT of the first.
bool matchFoldMergeToZext(MachineInstr &MI, MachineRegisterInfo &MRI) {
  auto &Merge = cast<GMerge>(MI);
  LLT SrcTy = MRI.getType(Merge.getSourceReg(0));
  if (SrcTy != LLT::scalar(32) || Merge.getNumSources() != 2)
    return false;
  return mi_match(Merge.getSourceReg(1), MRI, m_SpecificICst(0));
}

void applyFoldMergeToZext(MachineInstr &MI, MachineRegisterInfo &MRI,
                          MachineIRBuilder &B, GISelChangeObserver &Observer) {
  // Mutate %d(s64) = G_MERGE_VALUES %a(s32), 0(s32)
  //  ->
  // %d(s64) = G_ZEXT %a(s32)
  Observer.changingInstr(MI);
  MI.setDesc(B.getTII().get(TargetOpcode::G_ZEXT));
  MI.removeOperand(2);
  Observer.changedInstr(MI);
}

/// \returns True if a G_ANYEXT instruction \p MI should be mutated to a G_ZEXT
/// instruction.
bool matchMutateAnyExtToZExt(MachineInstr &MI, MachineRegisterInfo &MRI) {
  // If this is coming from a scalar compare then we can use a G_ZEXT instead of
  // a G_ANYEXT:
  //
  // %cmp:_(s32) = G_[I|F]CMP ... <-- produces 0/1.
  // %ext:_(s64) = G_ANYEXT %cmp(s32)
  //
  // By doing this, we can leverage more KnownBits combines.
  assert(MI.getOpcode() == TargetOpcode::G_ANYEXT);
  Register Dst = MI.getOperand(0).getReg();
  Register Src = MI.getOperand(1).getReg();
  return MRI.getType(Dst).isScalar() &&
         mi_match(Src, MRI,
                  m_any_of(m_GICmp(m_Pred(), m_Reg(), m_Reg()),
                           m_GFCmp(m_Pred(), m_Reg(), m_Reg())));
}

void applyMutateAnyExtToZExt(MachineInstr &MI, MachineRegisterInfo &MRI,
                             MachineIRBuilder &B,
                             GISelChangeObserver &Observer) {
  Observer.changingInstr(MI);
  MI.setDesc(B.getTII().get(TargetOpcode::G_ZEXT));
  Observer.changedInstr(MI);
}

/// Match a 128b store of zero and split it into two 64 bit stores, for
/// size/performance reasons.
bool matchSplitStoreZero128(MachineInstr &MI, MachineRegisterInfo &MRI) {
  GStore &Store = cast<GStore>(MI);
  if (!Store.isSimple())
    return false;
  LLT ValTy = MRI.getType(Store.getValueReg());
  if (!ValTy.isVector() || ValTy.getSizeInBits() != 128)
    return false;
  if (ValTy.getSizeInBits() != Store.getMemSizeInBits())
    return false; // Don't split truncating stores.
  if (!MRI.hasOneNonDBGUse(Store.getValueReg()))
    return false;
  auto MaybeCst = isConstantOrConstantSplatVector(
      *MRI.getVRegDef(Store.getValueReg()), MRI);
  return MaybeCst && MaybeCst->isZero();
}

void applySplitStoreZero128(MachineInstr &MI, MachineRegisterInfo &MRI,
                            MachineIRBuilder &B,
                            GISelChangeObserver &Observer) {
  B.setInstrAndDebugLoc(MI);
  GStore &Store = cast<GStore>(MI);
  assert(MRI.getType(Store.getValueReg()).isVector() &&
         "Expected a vector store value");
  LLT NewTy = LLT::scalar(64);
  Register PtrReg = Store.getPointerReg();
  auto Zero = B.buildConstant(NewTy, 0);
  auto HighPtr = B.buildPtrAdd(MRI.getType(PtrReg), PtrReg,
                               B.buildConstant(LLT::scalar(64), 8));
  auto &MF = *MI.getMF();
  auto *LowMMO = MF.getMachineMemOperand(&Store.getMMO(), 0, NewTy);
  auto *HighMMO = MF.getMachineMemOperand(&Store.getMMO(), 8, NewTy);
  B.buildStore(Zero, PtrReg, *LowMMO);
  B.buildStore(Zero, HighPtr, *HighMMO);
  Store.eraseFromParent();
}

class AArch64PostLegalizerCombinerImpl : public GIMatchTableExecutor {
protected:
  CombinerHelper &Helper;
  const AArch64PostLegalizerCombinerImplRuleConfig &RuleConfig;

  const AArch64Subtarget &STI;
  MachineRegisterInfo &MRI;
  GISelChangeObserver &Observer;
  MachineIRBuilder &B;
  MachineFunction &MF;

public:
  AArch64PostLegalizerCombinerImpl(
      const AArch64PostLegalizerCombinerImplRuleConfig &RuleConfig,
      const AArch64Subtarget &STI, GISelChangeObserver &Observer,
      MachineIRBuilder &B, CombinerHelper &Helper);

  static const char *getName() { return "AArch64PostLegalizerCombiner"; }

  bool tryCombineAll(MachineInstr &I) const;

private:
#define GET_GICOMBINER_CLASS_MEMBERS
#include "AArch64GenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CLASS_MEMBERS
};

#define GET_GICOMBINER_IMPL
#include "AArch64GenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_IMPL

AArch64PostLegalizerCombinerImpl::AArch64PostLegalizerCombinerImpl(
    const AArch64PostLegalizerCombinerImplRuleConfig &RuleConfig,
    const AArch64Subtarget &STI, GISelChangeObserver &Observer,
    MachineIRBuilder &B, CombinerHelper &Helper)
    : Helper(Helper), RuleConfig(RuleConfig), STI(STI), MRI(*B.getMRI()),
      Observer(Observer), B(B), MF(B.getMF()),
#define GET_GICOMBINER_CONSTRUCTOR_INITS
#include "AArch64GenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CONSTRUCTOR_INITS
{
}

class AArch64PostLegalizerCombinerInfo : public CombinerInfo {
  GISelKnownBits *KB;
  MachineDominatorTree *MDT;

public:
  AArch64PostLegalizerCombinerImplRuleConfig RuleConfig;

  AArch64PostLegalizerCombinerInfo(bool EnableOpt, bool OptSize, bool MinSize,
                                   GISelKnownBits *KB,
                                   MachineDominatorTree *MDT)
      : CombinerInfo(/*AllowIllegalOps*/ true, /*ShouldLegalizeIllegal*/ false,
                     /*LegalizerInfo*/ nullptr, EnableOpt, OptSize, MinSize),
        KB(KB), MDT(MDT) {
    if (!RuleConfig.parseCommandLineOption())
      report_fatal_error("Invalid rule identifier");
  }

  bool combine(GISelChangeObserver &Observer, MachineInstr &MI,
               MachineIRBuilder &B) const override;
};

bool AArch64PostLegalizerCombinerInfo::combine(GISelChangeObserver &Observer,
                                               MachineInstr &MI,
                                               MachineIRBuilder &B) const {
  const auto &STI = MI.getMF()->getSubtarget<AArch64Subtarget>();
  const auto *LI = STI.getLegalizerInfo();
  CombinerHelper Helper(Observer, B, /*IsPreLegalize*/ false, KB, MDT, LI);
  AArch64PostLegalizerCombinerImpl Impl(RuleConfig, STI, Observer, B, Helper);
  Impl.setupMF(*MI.getMF(), KB);
  return Impl.tryCombineAll(MI);
}

class AArch64PostLegalizerCombiner : public MachineFunctionPass {
public:
  static char ID;

  AArch64PostLegalizerCombiner(bool IsOptNone = false);

  StringRef getPassName() const override {
    return "AArch64PostLegalizerCombiner";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  bool IsOptNone;
};
} // end anonymous namespace

void AArch64PostLegalizerCombiner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.setPreservesCFG();
  getSelectionDAGFallbackAnalysisUsage(AU);
  AU.addRequired<GISelKnownBitsAnalysis>();
  AU.addPreserved<GISelKnownBitsAnalysis>();
  if (!IsOptNone) {
    AU.addRequired<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.addRequired<GISelCSEAnalysisWrapperPass>();
    AU.addPreserved<GISelCSEAnalysisWrapperPass>();
  }
  MachineFunctionPass::getAnalysisUsage(AU);
}

AArch64PostLegalizerCombiner::AArch64PostLegalizerCombiner(bool IsOptNone)
    : MachineFunctionPass(ID), IsOptNone(IsOptNone) {
  initializeAArch64PostLegalizerCombinerPass(*PassRegistry::getPassRegistry());
}

bool AArch64PostLegalizerCombiner::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getProperties().hasProperty(
          MachineFunctionProperties::Property::FailedISel))
    return false;
  assert(MF.getProperties().hasProperty(
             MachineFunctionProperties::Property::Legalized) &&
         "Expected a legalized function?");
  auto *TPC = &getAnalysis<TargetPassConfig>();
  const Function &F = MF.getFunction();
  bool EnableOpt =
      MF.getTarget().getOptLevel() != CodeGenOpt::None && !skipFunction(F);
  GISelKnownBits *KB = &getAnalysis<GISelKnownBitsAnalysis>().get(MF);
  MachineDominatorTree *MDT =
      IsOptNone ? nullptr : &getAnalysis<MachineDominatorTree>();
  AArch64PostLegalizerCombinerInfo PCInfo(EnableOpt, F.hasOptSize(),
                                          F.hasMinSize(), KB, MDT);
  GISelCSEAnalysisWrapper &Wrapper =
      getAnalysis<GISelCSEAnalysisWrapperPass>().getCSEWrapper();
  auto *CSEInfo = &Wrapper.get(TPC->getCSEConfig());
  Combiner C(PCInfo, TPC);
  return C.combineMachineInstrs(MF, CSEInfo);
}

char AArch64PostLegalizerCombiner::ID = 0;
INITIALIZE_PASS_BEGIN(AArch64PostLegalizerCombiner, DEBUG_TYPE,
                      "Combine AArch64 MachineInstrs after legalization", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(GISelKnownBitsAnalysis)
INITIALIZE_PASS_END(AArch64PostLegalizerCombiner, DEBUG_TYPE,
                    "Combine AArch64 MachineInstrs after legalization", false,
                    false)

namespace llvm {
FunctionPass *createAArch64PostLegalizerCombiner(bool IsOptNone) {
  return new AArch64PostLegalizerCombiner(IsOptNone);
}
} // end namespace llvm
