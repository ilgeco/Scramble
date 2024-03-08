#include "SecretMixerImpl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

//  clang-format off
// ███████╗███╗   ██╗██╗   ██╗███╗   ███╗███████╗
// ██╔════╝████╗  ██║██║   ██║████╗ ████║██╔════╝
// █████╗  ██╔██╗ ██║██║   ██║██╔████╔██║███████╗
// ██╔══╝  ██║╚██╗██║██║   ██║██║╚██╔╝██║╚════██║
// ███████╗██║ ╚████║╚██████╔╝██║ ╚═╝ ██║███████║
// ╚══════╝╚═╝  ╚═══╝ ╚═════╝ ╚═╝     ╚═╝╚══════╝
//  clang-format on

enum CondCodes { // Meaning (integer)          Meaning (floating-point)
  EQ,            // Equal                      Equal
  NE,            // Not equal                  Not equal, or unordered
  HS,            // Carry set                  >, ==, or unordered
  LO,            // Carry clear                Less than
  MI,            // Minus, negative            Less than
  PL,            // Plus, positive or zero     >, ==, or unordered
  VS,            // Overflow                   Unordered
  VC,            // No overflow                Not unordered
  HI,            // Unsigned higher            Greater than, or unordered
  LS,            // Unsigned lower or same     Less than or equal
  GE,            // Greater than or equal      Greater than or equal
  LT,            // Less than                  Less than, or unordered
  GT,            // Greater than               Greater than
  LE,            // Less than or equal         <, ==, or unordered
  AL             // Always (unconditional)     Always (unconditional)
};

inline static CondCodes getOppositeCondition(CondCodes CC) {
  switch (CC) {
  default:
    llvm_unreachable("Unknown condition code");
  case EQ:
    return NE;
  case NE:
    return EQ;
  case HS:
    return LO;
  case LO:
    return HS;
  case MI:
    return PL;
  case PL:
    return MI;
  case VS:
    return VC;
  case VC:
    return VS;
  case HI:
    return LS;
  case LS:
    return HI;
  case GE:
    return LT;
  case LT:
    return GE;
  case GT:
    return LE;
  case LE:
    return GT;
  }
}

enum TOF {
  //===------------------------------------------------------------------===//
  // ARM Specific MachineOperand flags.

  MO_NO_FLAG = 0,

  /// MO_LO16 - On a symbol operand, this represents a relocation containing
  /// lower 16 bit of the address. Used only via movw instruction.
  MO_LO16 = 0x1,

  /// MO_HI16 - On a symbol operand, this represents a relocation containing
  /// higher 16 bit of the address. Used only via movt instruction.
  MO_HI16 = 0x2,

  /// MO_OPTION_MASK - Most flags are mutually exclusive; this mask selects
  /// just that part of the flag set.
  MO_OPTION_MASK = 0xf03,

  /// MO_COFFSTUB - On a symbol operand "FOO", this indicates that the
  /// reference is actually to the ".refptr.FOO" symbol.  This is used for
  /// stub symbols on windows.
  MO_COFFSTUB = 0x4,

  /// MO_GOT - On a symbol operand, this represents a GOT relative relocation.
  MO_GOT = 0x8,

  /// MO_SBREL - On a symbol operand, this represents a static base relative
  /// relocation. Used in movw and movt instructions.
  MO_SBREL = 0x10,

  /// MO_DLLIMPORT - On a symbol operand, this represents that the reference
  /// to the symbol is for an import stub.  This is used for DLL import
  /// storage class indication on Windows.
  MO_DLLIMPORT = 0x20,

  /// MO_SECREL - On a symbol operand this indicates that the immediate is
  /// the offset from beginning of section.
  ///
  /// This is the TLS offset for the COFF/Windows TLS mechanism.
  MO_SECREL = 0x40,

  /// MO_NONLAZY - This is an independent flag, on a symbol operand "FOO" it
  /// represents a symbol which, if indirect, will get special Darwin mangling
  /// as a non-lazy-ptr indirect symbol (i.e. "L_FOO$non_lazy_ptr"). Can be
  /// combined with MO_LO16, MO_HI16 or MO_NO_FLAG (in a constant-pool, for
  /// example).
  MO_NONLAZY = 0x80,

  /// MO_LO_0_7 - On a symbol operand, this represents a relocation containing
  /// bits 0 through 7 of the address. Used only with Thumb1 MOV and ADD
  // instructions.
  MO_LO_0_7 = 0x100,

  /// MO_LO_8_15 - On a symbol operand, this represents a relocation
  /// containing
  /// bits 8 through 15 of the address. Used only with Thumb1 MOV and ADD
  // instructions.
  MO_LO_8_15 = 0x200,

  /// MO_HI_0_7 - On a symbol operand, this represents a relocation containing
  /// bits 16 through 23 of the address. Used only with Thumb1 MOV and ADD
  // instructions.
  MO_HI_0_7 = 0x400,

  /// MO_HI_8_15 - On a symbol operand, this represents a relocation
  /// containing
  /// bits 24 through 31 of the address. Used only with Thumb1 MOV and ADD
  // instructions.
  MO_HI_8_15 = 0x800
};

namespace utils {
//  clang-format off
// ██╗   ██╗████████╗██╗██╗     ███████╗
// ██║   ██║╚══██╔══╝██║██║     ██╔════╝
// ██║   ██║   ██║   ██║██║     ███████╗
// ██║   ██║   ██║   ██║██║     ╚════██║
// ╚██████╔╝   ██║   ██║███████╗███████║
//  ╚═════╝    ╚═╝   ╚═╝╚══════╝╚══════╝
//  clang-format on

using namespace llvm;
/// getInstrPredicate - If instruction is predicated, returns its predicate
/// condition, otherwise returns AL. It also returns the condition code
/// register by reference.
CondCodes getInstrPredicate(const MachineInstr &MI, Register &PredReg) {
  int PIdx = MI.findFirstPredOperandIdx();
  if (PIdx == -1) {
    PredReg = 0;
    return AL;
  }

  PredReg = MI.getOperand(PIdx + 1).getReg();
  return (CondCodes)MI.getOperand(PIdx).getImm();
}

static MachineOperand makeImplicit(const MachineOperand &MO) {
  MachineOperand NewMO = MO;
  NewMO.setImplicit();
  return NewMO;
}

static MachineOperand getMovOperand(const MachineOperand &MO,
                                    unsigned TargetFlag) {
  unsigned TF = MO.getTargetFlags() | TargetFlag;
  switch (MO.getType()) {
  case MachineOperand::MO_Immediate: {
    unsigned Imm = MO.getImm();
    switch (TargetFlag) {
    case TOF::MO_HI_8_15:
      Imm = (Imm >> 24) & 0xff;
      break;
    case TOF::MO_HI_0_7:
      Imm = (Imm >> 16) & 0xff;
      break;
    case TOF::MO_LO_8_15:
      Imm = (Imm >> 8) & 0xff;
      break;
    case TOF::MO_LO_0_7:
      Imm = Imm & 0xff;
      break;
    case TOF::MO_HI16:
      Imm = (Imm >> 16) & 0xffff;
      break;
    case TOF::MO_LO16:
      Imm = Imm & 0xffff;
      break;
    default:
      llvm_unreachable("Only HI/LO target flags are expected");
    }
    return MachineOperand::CreateImm(Imm);
  }
  case MachineOperand::MO_ExternalSymbol:
    return MachineOperand::CreateES(MO.getSymbolName(), TF);
  case MachineOperand::MO_JumpTableIndex:
    return MachineOperand::CreateJTI(MO.getIndex(), TF);
  default:
    return MachineOperand::CreateGA(MO.getGlobal(), MO.getOffset(), TF);
  }
}

void ExpandMOV32BitImm(MachineBasicBlock &MBB,
                       MachineBasicBlock::iterator &MBBI,
                       const TargetInstrInfo *TII) {
  MachineInstr &MI = *MBBI;
  unsigned Opcode = MI.getOpcode();
  Register PredReg;
  CondCodes Pred = getInstrPredicate(MI, PredReg);
  Register DstReg = MI.getOperand(0).getReg();
  bool DstIsDead = MI.getOperand(0).isDead();
  bool isCC = Opcode == ARM::MOVCCi32imm || Opcode == ARM::t2MOVCCi32imm;
  const MachineOperand &MO = MI.getOperand(isCC ? 2 : 1);
  MachineInstrBuilder LO16, HI16;

  unsigned LO16Opc = 0;
  unsigned HI16Opc = 0;
  unsigned MIFlags = MI.getFlags();
  if (Opcode == ARM::t2MOVi32imm || Opcode == ARM::t2MOVCCi32imm) {
    LO16Opc = ARM::t2MOVi16;
    HI16Opc = ARM::t2MOVTi16;
  } else {
    llvm_unreachable("no tmove pls");
  }

  LO16 = BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(LO16Opc), DstReg);
  LO16.setMIFlags(MIFlags);
  LO16.add(getMovOperand(MO, TOF::MO_LO16));
  LO16.cloneMemRefs(MI);
  LO16.addImm(Pred).addReg(PredReg);
  if (isCC)
    LO16.add(makeImplicit(MI.getOperand(1)));
  LO16.copyImplicitOps(MI);

  MachineOperand HIOperand = getMovOperand(MO, TOF::MO_HI16);
  if (!(HIOperand.isImm() && HIOperand.getImm() == 0)) {
    HI16 = BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(HI16Opc))
               .addReg(DstReg, RegState::Define | getDeadRegState(DstIsDead))
               .addReg(DstReg);
    HI16.setMIFlags(MIFlags);
    HI16.add(HIOperand);
    HI16.cloneMemRefs(MI);
    HI16.addImm(Pred).addReg(PredReg);
    HI16.copyImplicitOps(MI);
  } else {
    LO16->getOperand(0).setIsDead(DstIsDead);
  }
  MI.eraseFromParent();
}

bool skip(int Opcode) {
  return (Opcode >= llvm::ARM::t2B && Opcode <= llvm::ARM::t2Bcc) ||
         (Opcode >= llvm::ARM::t2CMPri && Opcode <= llvm::ARM::t2CMPrs) ||
         (Opcode >= llvm::ARM::tB && Opcode <= llvm::ARM::tBcc) ||
         (Opcode >= llvm::ARM::tBXNS_RET && Opcode <= llvm::ARM::tBfar);
}

bool isMultiLinePseudoInst(int Opcode) {
  return Opcode == llvm::ARM::t2MOVi32imm;
}

// Query a MachineOperand and return a bitsetted integer with the correct flag
unsigned int copyRegFlags(const llvm::MachineOperand &MO) {
  unsigned int Res = 0;

  Res |= MO.isDef() ? llvm::RegState::Define : 0;
  Res |= MO.isImplicit() ? llvm::RegState::Implicit : 0;
  Res |= MO.isKill() ? llvm::RegState::Kill : 0;
  Res |= MO.isDead() ? llvm::RegState::Dead : 0;
  Res |= MO.isUndef() ? llvm::RegState::Undef : 0;
  Res |= MO.isEarlyClobber() ? llvm::RegState::EarlyClobber : 0;
  Res |= MO.isDebug() ? llvm::RegState::Debug : 0;
  Res |= MO.isInternalRead() ? llvm::RegState::InternalRead : 0;

  Res |= MO.getReg().isPhysical() && MO.isRenamable()
             ? llvm::RegState::Renamable
             : 0;
  Res |= MO.isDef() && MO.isUndef() ? llvm::RegState::DefineNoRead : 0;
  Res |= MO.isImplicit() && MO.isDef() ? llvm::RegState::ImplicitDefine : 0;
  Res |= MO.isImplicit() && MO.isKill() ? llvm::RegState::ImplicitKill : 0;

  return Res;
}

} // namespace utils

namespace SecretMixer {

// clang-format off
// ███████╗██████╗ ███████╗███████╗██████╗ ███████╗ ██████╗ ██╗███████╗████████╗███████╗██████╗ 
// ██╔════╝██╔══██╗██╔════╝██╔════╝██╔══██╗██╔════╝██╔════╝ ██║██╔════╝╚══██╔══╝██╔════╝██╔══██╗
// █████╗  ██████╔╝█████╗  █████╗  ██████╔╝█████╗  ██║  ███╗██║███████╗   ██║   █████╗  ██████╔╝
// ██╔══╝  ██╔══██╗██╔══╝  ██╔══╝  ██╔══██╗██╔══╝  ██║   ██║██║╚════██║   ██║   ██╔══╝  ██╔══██╗
// ██║     ██║  ██║███████╗███████╗██║  ██║███████╗╚██████╔╝██║███████║   ██║   ███████╗██║  ██║
// ╚═╝     ╚═╝  ╚═╝╚══════╝╚══════╝╚═╝  ╚═╝╚══════╝ ╚═════╝ ╚═╝╚══════╝   ╚═╝   ╚══════╝╚═╝  ╚═╝
// clang-format on                                                                                                                               

std::optional<int> FreeRegister::peekFirstFreeRegister() {
  if (!FreeRegisterSet.empty()) {
    int Ret = *FreeRegisterSet.begin();
    return Ret;
  }
  return {};
}

void FreeRegister::dump(){
 auto DumpRegs = [&] (auto String, auto& Set) {
  llvm::dbgs() << String;
  for (auto Reg : Set){
  llvm::dbgs() << Reg << " ";
  }
  llvm::dbgs() << "\n\n";
 };
 DumpRegs("InitSet:\n", InitSet);
 DumpRegs("FreeSet:\n", FreeRegisterSet);
 DumpRegs("OccSet:\n", OccRegisterSet);
}

int FreeRegister::getFreeRegister() {

  if (!FreeRegisterSet.empty()) {
    int Ret = *FreeRegisterSet.begin();
    setOccRegister(Ret);
    return Ret;
  }
  llvm_unreachable("Registers Ends Here\n");
}

void FreeRegister::setOccRegister(int Reg) {
  if (FreeRegisterSet.contains(Reg)) {
    FreeRegisterSet.erase(Reg);
    OccRegisterSet.insert(Reg);
  }
}

void FreeRegister::setNewOccRegister(int Reg) {
  if (FreeRegisterSet.contains(Reg)) {
    FreeRegisterSet.erase(Reg);
  }
  OccRegisterSet.insert(Reg);
}

void FreeRegister::setFreeRegister(int Reg) {
  if (OccRegisterSet.contains(Reg)) {
    OccRegisterSet.erase(Reg);
    FreeRegisterSet.insert(Reg);
  }
}

void FreeRegister::setFreeRegister(std::initializer_list<int> Regs) {
  for(auto reg : Regs){
    this->setNewFreeRegister(reg);
  }
}


void FreeRegister::setNewFreeRegister(int Reg) {
  if (OccRegisterSet.contains(Reg)) {
    OccRegisterSet.erase(Reg);
  }
  FreeRegisterSet.insert(Reg);
}

void FreeRegister::reset() {
  FreeRegisterSet.clear();
  OccRegisterSet.clear();
  for (auto [set, _] : RegisterMap) {
    if (InitSet.contains(set)) {
      OccRegisterSet.insert(set);
    } else {
      FreeRegisterSet.insert(set);
    }
  }
}


void FreeRegister::updateFreeRegisterPostIt(const llvm::MachineInstr &MI) {

  for (const auto &Op : MI.defs()) {
    if (Op.isReg() && RegisterMap.contains(Op.getReg())) {
      int Reg = Op.getReg();
      setOccRegister(Reg);
      setFreeRegister(RegisterMap[Reg]);
    }
  }
}

void FreeRegister::updateFreeRegisterPreIt(const llvm::MachineInstr &MI) {

  for (const auto &Op : MI.uses()) {
    if (Op.isReg() && RegisterMap.contains(Op.getReg())) {
      int Reg = Op.getReg();
      setOccRegister(Reg);
      setFreeRegister(RegisterMap[Reg]);
    }
  }
}

void FreeRegister::setRegisterMap(const llvm::DenseMap<int, int> &registerMap) {
  RegisterMap = registerMap;
}

bool FreeRegister::isFree(int Reg){
  return  FreeRegisterSet.contains(Reg);
}

void FreeRegister::setInitRegister(int Reg) { InitSet.insert(Reg); }


//! Check if a register is defined and use
//!  If true Return the register ID otherwise nullopt
std::optional<int> checkIfDefAndUse(const llvm::MachineInstr &MI) {
  for (const auto &Def :  MI.defs()) {

    if (!Def.isReg())
      continue;
    for (const auto &Op : MI.operands()) {
      if (!Op.isReg())
        continue;
      auto DefReg = Def.getReg();
      auto OpReg = Op.getReg();
      if (Op.isDef())
        continue;

      if (DefReg != OpReg)
        continue;
      return DefReg;
    }
  }
  return {};
}


// clang-format off
// ███████╗███████╗ ██████╗██████╗ ███████╗████████╗███╗   ███╗██╗██╗  ██╗███████╗██████╗
// ██╔════╝██╔════╝██╔════╝██╔══██╗██╔════╝╚══██╔══╝████╗ ████║██║╚██╗██╔╝██╔════╝██╔══██╗
// ███████╗█████╗  ██║     ██████╔╝█████╗     ██║   ██╔████╔██║██║ ╚███╔╝ █████╗  ██████╔╝
// ╚════██║██╔══╝  ██║     ██╔══██╗██╔══╝     ██║   ██║╚██╔╝██║██║ ██╔██╗ ██╔══╝  ██╔══██╗
// ███████║███████╗╚██████╗██║  ██║███████╗   ██║   ██║ ╚═╝ ██║██║██╔╝ ██╗███████╗██║  ██║
// ╚══════╝╚══════╝ ╚═════╝╚═╝  ╚═╝╚══════╝   ╚═╝   ╚═╝     ╚═╝╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝
// clang-format on

// Constructor of SecretMixer
SecretMixerImpl::SecretMixerImpl(llvm::MachineFunction &MF,
                                 llvm::MachineDominatorTree *MDT)
    : MF(MF), MDT(MDT) {

  // CloneRemapper

  // Set CPSR to noregister
  CloneRemapper[llvm::ARM::CPSR] = llvm::ARM::NoRegister;
  CloneRemapper[llvm::ARM::R0] = llvm::ARM::R3;
  CloneRemapper[llvm::ARM::R1] = llvm::ARM::R4;
  CloneRemapper[llvm::ARM::R2] = llvm::ARM::R5;
  CloneRemapper[llvm::ARM::R6] = llvm::ARM::R9;
  CloneRemapper[llvm::ARM::R7] = llvm::ARM::R10;
  CloneRemapper[llvm::ARM::R8] = llvm::ARM::R11;
  CloneRemapper[llvm::ARM::R12] = llvm::ARM::LR;

  // LineRemapper
  SameLineRemapper[llvm::ARM::R0] = llvm::ARM::R6;
  SameLineRemapper[llvm::ARM::R6] = llvm::ARM::R0;
  SameLineRemapper[llvm::ARM::R1] = llvm::ARM::R7;
  SameLineRemapper[llvm::ARM::R7] = llvm::ARM::R1;
  SameLineRemapper[llvm::ARM::R2] = llvm::ARM::R8;
  SameLineRemapper[llvm::ARM::R8] = llvm::ARM::R2;

  // CloneOperandsFunctions
  CloneOperandsFunctions[649] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[650] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[651] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[652] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[653] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[654] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[655] = &SecretMixerImpl::moveccOperandAssign;
  CloneOperandsFunctions[656] = &SecretMixerImpl::moveccOperandAssign;

  // FreeRegisterTracker

  freeRegisterTracker.setRegisterMap(SameLineRemapper);
  freeRegisterTracker.setInitRegister(
      {llvm::ARM::R0, llvm::ARM::R1, llvm::ARM::R2});
  freeRegisterTracker.reset();

  // Allocate more local variable in the stack
  //  Frame Index Remapper init with stack args
  auto FirstMBB = MF.begin();
  assert(FirstMBB != MF.end() && "Empty function\n");
  auto *MFI = &MF.getFrameInfo();
  std::vector<std::pair<std::string, int>> NameIndex;

  for (auto &MI : *FirstMBB) {
    if (MI.getOpcode() != llvm::ARM::t2STRi12)
      continue;
    auto FrameIndex = MI.getOperand(1);
    auto *Alloca = MFI->getObjectAllocation(FrameIndex.getIndex());
    if (Alloca != nullptr && Alloca->hasName()) {
      llvm::dbgs() << "Found " << Alloca->getName() << "\n";
      std::string Name = Alloca->getName().str();
      size_t PositionI = 0;
      size_t PositionE = -1;
      PositionE = Name.find_first_of('.', PositionI + 1);
      if (PositionE != std::string::npos) {
        NameIndex.emplace_back(Name.substr(PositionI, PositionE - PositionI),
                               FrameIndex.getIndex());
      } else {
        NameIndex.emplace_back(Name, FrameIndex.getIndex());
      }
    }
  }

  for (auto &[Name, Index] : NameIndex) {
    auto MatchIter =
        std::find_if(NameIndex.begin(), NameIndex.end(),
                     [&Name](const std::pair<std::string, int> &Pair) {
                       return Pair.first.find(Name) == 0 && Pair.first != Name;
                     });
    if (MatchIter != NameIndex.end()) {
      FrameIndexRemapper.insert({Index, MatchIter->second});
    }
  }
  // Map input to itself
  auto InputIter =
      std::find_if(NameIndex.begin(), NameIndex.end(),
                   [](const std::pair<std::string, int> &Pair) {
                     return Pair.first.find("input") != std::string::npos;
                   });
  if (InputIter != NameIndex.end()) {
    FrameIndexRemapper.insert({InputIter->second, InputIter->second});
  }

  for (auto frame : FrameIndexRemapper) {

    auto first_name = MFI->getObjectAllocation(frame.getFirst())->getName();
    auto sec_name = MFI->getObjectAllocation(frame.getSecond())->getName();

    llvm::dbgs() << "Primo mappa " << first_name << " in secondo " << sec_name
                 << "\n";
  }
}

/// If a single instruction use and define the same ragister use the mapping
/// defined in SameLineRemapper to remap the register At the end of the
/// MachineBasicBlock if some map is still hanging insert mov instruction to
/// restore Correct Register \p MBB
void SecretMixerImpl::sameLineRegisterRemap(llvm::MachineBasicBlock &MBB) {
  llvm::DenseSet<int> AlreadyMapped;
  for (auto &MI : MBB) {
    constexpr std::array<int, 10> P = {75, 74, 73, 85, 3};
    for (auto &OP : MI.operands()) {
      if (!OP.isReg())
        continue;
      if (std::find(P.begin(), P.end(), OP.getReg()) == P.end()) {
        llvm::dbgs() << OP.getReg() << "\n";
        llvm_unreachable("Non trovato");
      }
    }

    // Check if use def pair appear remap def and add it to the set
    auto SameReg = checkIfDefAndUse(MI).value_or(-1);
    auto IsSameRegAlredyMapped = AlreadyMapped.contains(SameReg);

    if (SameReg != -1 &&
        std::find_if(SameLineRemapper.begin(), SameLineRemapper.end(),
                     [SameReg](auto F) {
                       return SameReg == F.first || SameReg == F.second;
                     }) == SameLineRemapper.end()) {
      llvm::dbgs() << "Same reg not found " << SameReg << "\n";
      llvm_unreachable("Use Def Pair not handled");
    }

    // Remap use register alredyMapped
    for (auto &Op : MI.operands()) {
      if (!Op.isReg())
        continue;

      if (Op.isDef())
        continue;
      if (AlreadyMapped.contains(Op.getReg())) {
        Op.setReg(SameLineRemapper[Op.getReg()]);
      }
    }

    // If isSameRegAlredyMap is true remove the mapping
    if (IsSameRegAlredyMapped) {
      AlreadyMapped.erase(SameReg);
    }

    // Receck after remap AlredyMap
    SameReg = checkIfDefAndUse(MI).value_or(-1);

    if (SameReg == -1) {
      // Check if we are redefining a variable in the set
      for (auto &Def : MI.defs()) {
        auto Reg = Def.getReg();
        if (AlreadyMapped.contains(Reg)) {
          AlreadyMapped.erase(Reg);
        }
      }
      continue;
    }

    if (!SameLineRemapper.contains(SameReg))
      continue;
    // At this point we know that we have a use def pair and it's a known
    // register that we know how to remap
    auto NewReg = SameLineRemapper[SameReg];
    auto OldReg = SameReg;

    llvm::MachineOperand *MOP = MI.operands_end();
    do {
      MOP = std::find_if(MI.operands_begin(), MI.operands_end(),
                         [OldReg](llvm::MachineOperand &MO) {
                           return MO.isReg() && MO.getReg() == OldReg &&
                                  MO.isDef();
                         });
      if (MOP == MI.operands_end()) {
        break;
      }
      MOP->setReg(NewReg);
      AlreadyMapped.insert(OldReg);
    } while (MOP != MI.operands_end());
  }

  if (AlreadyMapped.empty())
    return;

  auto *TII = MBB.getParent()->getSubtarget().getInstrInfo();

  llvm::DebugLoc DL;
  auto InsPoint = MBB.rbegin();
  auto PeekInsPoint = std::next(InsPoint);
  while (utils::skip(PeekInsPoint->getOpcode())) {
    InsPoint = PeekInsPoint;
    PeekInsPoint = std::next(InsPoint);
  }

  for (auto RemainReg : AlreadyMapped) {

    auto Builder =
        llvm::BuildMI(MBB, &*InsPoint, DL, TII->get(llvm::ARM::t2MOVr));

    Builder.addDef(RemainReg)
        .addReg(SameLineRemapper[RemainReg])
        .addImm(14)
        .addReg(llvm::ARM::NoRegister)
        .addReg(llvm::ARM::NoRegister);
  }
}

// int SecretMixerImpl::getVoidFrame(int Size, llvm::Align Align) {
//   auto &FI = MF.getFrameInfo();
//   auto Pair = std::pair{Size, Align};
//   if (VoidFrames.find(Pair) != VoidFrames.end()) {
//     return VoidFrames[Pair];
//   }

//   auto Ret = FI.CreateStackObject(Size, Align, false);
//   VoidFrames.emplace(Pair, Ret);
//   return Ret;
// }

// Clone operand inside a new MachineInstruction
void SecretMixerImpl::remapOperand(llvm::MachineInstrBuilder &MIB,
                                   const llvm::MachineOperand &Op) {

  if (Op.isReg()) {
    if (CloneRemapper.contains(Op.getReg())) {
      llvm::Register Val(CloneRemapper[Op.getReg()]);

      MIB.addReg(Val, utils::copyRegFlags(Op));
      return;
    }

    llvm::Register Val(Op.getReg());
    MIB.addReg(Val, utils::copyRegFlags(Op));
    return;
  }

  if (Op.isImm()) {
    MIB.addImm(Op.getImm());
    return;
  }
  if (Op.isCImm()) {
    MIB.addCImm(Op.getCImm());
    return;
  }
  if (Op.isFPImm()) {
    MIB.addFPImm(Op.getFPImm());
    return;
  }
  if (Op.isMBB()) {
    MIB.addMBB(Op.getMBB(), Op.getTargetFlags());
    return;
  }
  if (Op.isFI()) {
    auto &FI = MF.getFrameInfo();

    auto Index = Op.getIndex();
    if (!FrameIndexRemapper.contains(Index)) {
      auto Size = FI.getObjectSize(Index);
      auto Align = FI.getObjectAlign(Index);
      auto SO = FI.CreateStackObject(Size, Align, false);
      FrameIndexRemapper[Index] = SO;
    }
    MIB.addFrameIndex(FrameIndexRemapper[Index]);
    return;
  }
  if (Op.isCPI()) {
    MIB.addConstantPoolIndex(Op.getIndex(), Op.getOffset(),
                             Op.getTargetFlags());
    return;
  }
  if (Op.isTargetIndex()) {
    MIB.addTargetIndex(Op.getIndex(), Op.getOffset(), Op.getTargetFlags());
    return;
  }
  if (Op.isJTI()) {
    MIB.addJumpTableIndex(Op.getIndex(), Op.getTargetFlags());
    return;
  }
  if (Op.isGlobal()) {
    MIB.addGlobalAddress(Op.getGlobal(), Op.getOffset(), Op.getTargetFlags());
    return;
  }
  if (Op.isSymbol()) {
    MIB.addExternalSymbol(Op.getSymbolName(), Op.getTargetFlags());
  }
  if (Op.isBlockAddress()) {
    MIB.addBlockAddress(Op.getBlockAddress(), Op.getOffset(),
                        Op.getTargetFlags());
    return;
  }
  if (Op.isRegMask()) {
    MIB.addRegMask(Op.getRegMask());
    return;
  }
  if (Op.isRegLiveOut()) {
    llvm_unreachable("No idea how to handle");
  }
  if (Op.isMetadata()) {
    MIB.addMetadata(Op.getMetadata());
  }
  if (Op.isMCSymbol()) {
    MIB.addSym(Op.getMCSymbol(), Op.getTargetFlags());
    return;
  }
  if (Op.isDbgInstrRef()) {

    llvm_unreachable("No idea how to handle");
  }
  if (Op.isCFIIndex()) {
    MIB.addCFIIndex(Op.getCFIIndex());
    return;
  }
  if (Op.isIntrinsicID()) {
    MIB.addIntrinsicID(Op.getIntrinsicID());
    return;
  }
  if (Op.isPredicate()) {
    MIB.addPredicate((llvm::CmpInst::Predicate)Op.getPredicate());
    return;
  }
  if (Op.isShuffleMask()) {
    MIB.addShuffleMask(Op.getShuffleMask());
    return;
  }

  llvm_unreachable("End of function without a proper handle?");
}

void SecretMixerImpl::defaultOperandAssign(const llvm::MachineInstr &MI,
                                           llvm::MachineInstrBuilder &Builder) {
  // Insert all the register operands
  for (auto Op : MI.operands()) {
    remapOperand(Builder, Op);
    MF.getRegInfo().verifyUseLists();
  }
}

void SecretMixerImpl::moveccOperandAssign(const llvm::MachineInstr &MI,
                                          llvm::MachineInstrBuilder &Builder) {
  remapOperand(Builder, MI.getOperand(0));
  remapOperand(Builder, MI.getOperand(1));
  remapOperand(Builder, MI.getOperand(2));
  remapOperand(Builder, MI.getOperand(3));
  Builder.addReg(llvm::ARM::NoRegister);
}

void SecretMixerImpl::loadRandom(llvm::MachineBasicBlock *MBB,
                                 llvm::MachineInstr *InsPoint,
                                 int RandomIndex) {

  auto RegCarry = freeRegisterTracker.getFreeRegister();
  auto RegRandom = freeRegisterTracker.getFreeRegister();
  llvm::DebugLoc DL;
  auto *TII = MF.getSubtarget().getInstrInfo();
  llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2MOVi32imm))
      .addDef(RegCarry)
      .addImm(0x50060808);
  llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2LDRi8))
      .addDef(RegRandom)
      .addReg(RegCarry)
      .addImm(0)
      .addImm(CondCodes::AL)
      .addReg(0);
  llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2STRi12))
      .addReg(RegRandom)
      .addFrameIndex(RandomIndex)
      .addImm(0)
      .addImm(CondCodes::AL)
      .addReg(0);
  freeRegisterTracker.setFreeRegister({RegRandom, RegCarry});
}

auto SecretMixerImpl::testAndShiftRegister(llvm::MachineBasicBlock *MBB,
                                           llvm::MachineInstr *InsPoint,
                                           int RandomIndex,
                                           int SuggestRegister = 0) {
  llvm::DebugLoc DL;
  auto *TII = MF.getSubtarget().getInstrInfo();
  int RegRandom = 0;
  if (SuggestRegister == 0) {
    RegRandom = freeRegisterTracker.getFreeRegister();
  } else {
    assert(freeRegisterTracker.isFree(SuggestRegister) == true);
    RegRandom = SuggestRegister;
  }
  if (SuggestRegister == 0) {
    llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2LDRi12))
        .addDef(RegRandom)
        .addFrameIndex(RandomIndex)
        .addImm(0)
        .addImm(CondCodes::AL)
        .addReg(0);
  }
  llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2TSTri))
      .addReg(RegRandom)
      .addImm(1)
      .addImm(CondCodes::AL)
      .addReg(0);
  llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2LSRri))
      .addDef(RegRandom)
      .addReg(RegRandom)
      .addImm(1)
      .addImm(CondCodes::AL)
      .addReg(0)
      .addReg(0);

  freeRegisterTracker.setFreeRegister({RegRandom});
  return std::make_pair(RegRandom, [=]() {
    llvm::BuildMI(*MBB, InsPoint, DL, TII->get(llvm::ARM::t2STRi12))
        .addReg(RegRandom)
        .addFrameIndex(RandomIndex)
        .addImm(0)
        .addImm(CondCodes::AL)
        .addReg(0);
  });
}

void SecretMixerImpl::recursiveConditionToIt(
    const llvm::DenseMap<llvm::MachineBasicBlock *,
                         std::vector<llvm::MachineInstr *>> &ITinsts,
    llvm::MachineBasicBlock *MBB, int Counter, int RandomIndex,
    llvm::DenseSet<llvm::MachineBasicBlock *> &BasicBlockHandled) {
  if (BasicBlockHandled.contains(MBB))
    return;
  BasicBlockHandled.insert(MBB);

  if (!ITinsts.contains(MBB)) {
    for (auto *Child : MBB->successors())
      recursiveConditionToIt(ITinsts, Child, 31, RandomIndex,
                             BasicBlockHandled);
    return;
  }

  freeRegisterTracker.reset();
  int RegOcc = 0;
  for (auto *ITinst : ITinsts.at(MBB)) {
    auto *NextInst = ITinst->getNextNode();
    constexpr auto Window = 2;
    assert(NextInst && "IT without inst inside??");
    for (auto I = 0; I < Window; ++I) {
      // Pre Update Register for uses
      assert(NextInst != nullptr && "IT Broken without two elements");
      freeRegisterTracker.updateFreeRegisterPreIt(*NextInst);
      NextInst = NextInst->getNextNode();
    }
    if (Counter == 31) {
      loadRandom(MBB, ITinst, RandomIndex);
      Counter = 0;
    } else {
      Counter++;
    }
    auto [tmp, functorStoreReg] =
        testAndShiftRegister(MBB, ITinst, RandomIndex, RegOcc);
    RegOcc = tmp;
    // Post Update Register for defs
    NextInst = ITinst->getNextNode();
    for (auto I = 0; I < Window; ++I) {
      assert(NextInst != nullptr && "IT Broken without two elements");
      freeRegisterTracker.updateFreeRegisterPostIt(*NextInst);
      NextInst = NextInst->getNextNode();
    }
    if (!freeRegisterTracker.isFree(RegOcc)) {
      functorStoreReg();
      RegOcc = 0;
    }
  }
  llvm::SmallVector<llvm::MachineBasicBlock *, 5> Result;
  MDT->getDescendants(MBB, Result);
  // for (auto *Elem : Result) {
  // llvm::dbgs() << "Finferli: " <<    "%bb." << MBB->getNumber() << " --
  // %bb." << Elem->getNumber() << "\n";
  // }
  auto *Tmp = std::remove_if(Result.begin(), Result.end(),
                             [MBB](llvm::MachineBasicBlock *LHS) {
                               size_t Size = LHS->pred_size();
                               if (MBB == LHS) {
                                 return true;
                               }
                               // llvm::dbgs() << "Discarded myself" <<   "%bb."
                               // << MBB->getNumber() << " -- %bb." <<
                               // LHS->getNumber() << "\n";
                               if (Size != 1) {
                                 // llvm::dbgs() << "Discarded too many papa: "
                                 // <<   "%bb." << MBB->getNumber() << " --
                                 // %bb." << LHS->getNumber() << "\n";
                                 return true;
                               }

                               if (*LHS->pred_begin() != MBB) {
                                 // llvm::dbgs() << "Discarded papa diversi: "
                                 // <<   "%bb." << MBB->getNumber() << " ~= " <<
                                 // "%bb." <<  (*LHS->pred_begin())->getNumber()
                                 // << " -- %bb." << LHS->getNumber() << "\n";
                                 return true;
                               }
                               return false;
                             });
  Result.erase(Tmp, Result.end());

  for (auto *DescMBB : MBB->successors()) {
    int ChildCounter = 31;
    if (std::find(Result.begin(), Result.end(), DescMBB) != Result.end()) {
      ChildCounter = Counter;
    }
    recursiveConditionToIt(ITinsts, DescMBB, ChildCounter, RandomIndex,
                           BasicBlockHandled);
  }
}

// Add a condition to IT based on the RandomGenerator
void SecretMixerImpl::addConditionToIT(
    llvm::DenseMap<llvm::MachineBasicBlock *, std::vector<llvm::MachineInstr *>>
        &ITinsts,
    int RandomIndex) {
  llvm::DenseSet<llvm::MachineBasicBlock *> BasicBlockHandled;
  // TODO MAKE IT RECURSIVE
  freeRegisterTracker.reset();
  auto &[randomMBB, _] = *ITinsts.begin();
  auto *MBB = &*randomMBB->getParent()->begin();
  recursiveConditionToIt(ITinsts, MBB, 31, RandomIndex, BasicBlockHandled);
}

std::pair<int, int> SecretMixerImpl::allocateStackSpaceForRandom() {
  auto &FI = MF.getFrameInfo();
  auto &DataL = MF.getDataLayout();
  auto Size = DataL.getPointerSize();
  auto Align = DataL.getStackAlignment();
  auto Random = FI.CreateStackObject(Size, Align, false);
  auto Carry = FI.CreateStackObject(Size, Align, false);
  return {Random, Carry};
}

// The clone instructions process is divided in 3 different phases
// First remap the register in each instruction if it is both used ad defined
// Second clone and remap the register of each instruction meanwhile generate an
// IT block scheduling both possible order Third for each IT instruction get a
// free register and generate the condition to control the IT block
void SecretMixerImpl::cloneInstructions() {
  llvm::DebugLoc DL;
  auto *TII = MF.getSubtarget().getInstrInfo();
  // Add register livein to first MBB
  // auto *FirstMBB = &*MF.begin();
  // for (auto [Reg, NewReg] : CloneRemapper) {
  //   if (std::find_if(FirstMBB->livein_begin(), FirstMBB->livein_end(),
  //                    [Reg](const llvm::MachineBasicBlock::RegisterMaskPair
  //                    &RMP)
  //                        -> bool { return RMP.PhysReg == Reg; }) !=
  //       FirstMBB->livein_end()) {
  //     FirstMBB->addLiveIn(NewReg);
  //   }
  // }

  // Add a Map between a MachineBasicBlock and all the NEW IT inserted
  llvm::DenseMap<llvm::MachineBasicBlock *, std::vector<llvm::MachineInstr *>>
      ITinsts;
  // Remap each register so they cannot be use and define by the same inst
  auto IteratorMbb = MF.begin();
  auto EndMbb = MF.end();
  IteratorMbb++;
  for (; IteratorMbb != EndMbb; IteratorMbb++) {
    sameLineRegisterRemap(*IteratorMbb);
  }

  IteratorMbb = MF.begin();
  EndMbb = MF.end();
  // Skip first BB it contains stack allocation already handled by the
  // constructor of secretMixer
  IteratorMbb++;
  for (; IteratorMbb != EndMbb; IteratorMbb++) {

    auto &MBB = *IteratorMbb;
    auto IteratorMI = MBB.begin();
    auto EndMi = MBB.end();
    // Clone Instruction
    while (IteratorMI != EndMi) {
      auto OldIter = IteratorMI;
      auto &MI = *OldIter;
      auto Opcode = MI.getOpcode();

      if (utils::isMultiLinePseudoInst(Opcode)) {
        auto PrevIter = OldIter;
        PrevIter--;
        utils::ExpandMOV32BitImm(MBB, OldIter, TII);
        IteratorMI = PrevIter;
        continue;
      }

      IteratorMI++;
      if (utils::skip(Opcode))
        continue;

      auto *ITMI = &*llvm::BuildMI(MBB, OldIter, DL, TII->get(llvm::ARM::t2IT))
                         .addImm(CondCodes::EQ)
                         .addImm(0b0111);
      ITinsts.getOrInsertDefault(&MBB).push_back(ITMI);
      // Get insert point
      auto InsertPoint = IteratorMI;
      for (int I = 0; I < 2; I++) {
        auto Builder = llvm::BuildMI(MBB, InsertPoint, DL, MI.getDesc());
        if (CloneOperandsFunctions.contains(Opcode)) {
          CloneOperandsFunctions[Opcode](this, MI, Builder);
        } else {
          defaultOperandAssign(MI, Builder);
        }
      }
      auto Builder = llvm::BuildMI(MBB, InsertPoint, DL, MI.getDesc());
      for (const auto &MOP : MI.operands()) {
        Builder->addOperand(MOP);
      }
    }
  }
  // auto CondInserter = [](llvm::MachineInstr *F, int Cond) {
  //   F->addOperand(llvm::MachineOperand::CreateImm(EQ));
  //   F->addOperand(llvm::MachineOperand::CreateReg(0, false));
  // };
  // for (auto &[MBB, Itinst] : ITinsts) {
  //   for (auto IT : Itinst) {
  //     IT = IT->getNextNode();
  //     CondInserter(IT, EQ);
  //     IT = IT->getNextNode();
  //     CondInserter(IT, EQ);
  //     IT = IT->getNextNode();
  //     CondInserter(IT, NE);
  //     IT = IT->getNextNode();
  //     CondInserter(IT, NE);
  //   }
  // }

  auto [Random, Carry] = allocateStackSpaceForRandom();
  addConditionToIT(ITinsts, Random);

  // Handle Random value and

  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      for (auto &OP : MI.operands()) {
        if (OP.isReg() && OP.getReg().isPhysical()) {
          OP.setIsRenamable(false);
        }
      }
    }
  }
}

} // namespace SecretMixer