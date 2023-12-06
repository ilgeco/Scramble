#include "SecretMixer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>

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

namespace SecretMixer {

// Check if a register is defined and use
// Return the register id or -1
int checkIfDefAndUse(const llvm::MachineInstr &MI) {
  for (const auto &Def : MI.defs()) {

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
  return -1;
}

// If a single instruction use and define the same ragister use the mapping
// defined in SameLineRemapper to remap the register At the end of the
// MachineBasicBlock if some map is still hanging insert mov instruction to
// restore Correct Register
void SecretMixer::sameLineRegisterRemap(llvm::MachineBasicBlock &MBB) {
  llvm::DenseSet<int> AlreadyMapped;
  for (auto &MI : MBB) {

    // Check if use def pair appear remap def and add it to the set
    auto SameReg = checkIfDefAndUse(MI);
    auto IsSameRegAlredyMapped = AlreadyMapped.contains(SameReg);

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
    SameReg = checkIfDefAndUse(MI);

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
  while (InsPoint->getOpcode() >= llvm::ARM::t2B &&
         InsPoint->getOpcode() <= llvm::ARM::t2Bcc) {
    InsPoint++;
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

std::pair<LastElem, bool> SecretMixer::whichNext() {
  auto NextElem = RandomChooser.next();
  bool InsertXor =
      NextElem != LastChoosenElem && LastChoosenElem != LastElem::Uninitialized
          ? true
          : false;
  LastChoosenElem = NextElem;
  return {NextElem, InsertXor};
}

// Clone operand inside a new MachineInstruction
void SecretMixer::remapOperand(llvm::MachineInstrBuilder &MIB,
                               const llvm::MachineOperand &Op) {

  if (Op.isReg()) {
    if (CloneRemapper.contains(Op.getReg())) {
      llvm::Register Val(CloneRemapper[Op.getReg()]);

      MIB.addReg(Val, copyRegFlags(Op));
      return;
    }

    llvm::Register Val(Op.getReg());
    MIB.addReg(Val, copyRegFlags(Op));
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

void SecretMixer::defaultOperandAssign(const llvm::MachineInstr &MI,
                                       llvm::MachineInstrBuilder &Builder) {
  // Insert all the register operands
  for (auto Op : MI.operands()) {
    remapOperand(Builder, Op);
    MF.getRegInfo().verifyUseLists();
  }
}

void SecretMixer::moveccOperandAssign(const llvm::MachineInstr &MI,
                                      llvm::MachineInstrBuilder &Builder) {
  remapOperand(Builder, MI.getOperand(0));
  remapOperand(Builder, MI.getOperand(1));
  remapOperand(Builder, MI.getOperand(2));
  remapOperand(Builder, MI.getOperand(3));
  Builder.addReg(llvm::ARM::NoRegister);
}

void SecretMixer::cloneInstruction() {
  llvm::DebugLoc DL;
  auto *TII = MF.getSubtarget().getInstrInfo();
  // Add register livein to first MBB
  auto *FirstMBB = &*MF.begin();
  for (auto [Reg, NewReg] : CloneRemapper) {
    if (std::find_if(FirstMBB->livein_begin(), FirstMBB->livein_end(),
                     [Reg](const llvm::MachineBasicBlock::RegisterMaskPair &RMP)
                         -> bool { return RMP.PhysReg == Reg; }) !=
        FirstMBB->livein_end()) {
      FirstMBB->addLiveIn(NewReg);
    }
  }

  // Remap each register so they cannot be use and define by the same inst
  for (auto &MBB : MF) {
    sameLineRegisterRemap(MBB);
  }

  // Cycle trought all MBB and clone elements
  auto IteratorMbb = MF.begin();
  auto EndMbb = MF.end();
  IteratorMbb++;

  for (; IteratorMbb != EndMbb; IteratorMbb++) {

    auto &MBB = *IteratorMbb;
    auto IteratorMI = MBB.begin();
    auto EndMi = MBB.end();

    // Clone Instruction
    while (IteratorMI != EndMi) {
      auto OldIter = IteratorMI;
      auto &MI = *OldIter;
      IteratorMI++;
      auto Opcode = MI.getOpcode();
      if ((Opcode >= llvm::ARM::t2B && Opcode <= llvm::ARM::t2Bcc) ||
          (Opcode >= llvm::ARM::t2CMPri && Opcode <= llvm::ARM::t2CMPrs))
        continue;
      auto [NextElem, InsertXor] = whichNext();
      if (InsertXor) {
        llvm::BuildMI(MBB, OldIter, DL, TII->get(llvm::ARM::t2MOVr))
            .addDef(llvm::ARM::LR)
            .addReg(llvm::ARM::LR)
            .addImm(14)
            .addReg(llvm::ARM::NoRegister)
            .addReg(llvm::ARM::NoRegister);
      }

      // Get insert point
      auto InsertPoint = OldIter;
      if (NextElem == LastElem::Orig) {
        InsertPoint = IteratorMI;
      }

      auto Builder = llvm::BuildMI(MBB, InsertPoint, DL, MI.getDesc());
      if (CloneOperandsFunctions.contains(Opcode)) {
        CloneOperandsFunctions[Opcode](this, MI, Builder);
      } else {
        defaultOperandAssign(MI, Builder);
      }
    }
  }

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