
#ifndef LLVM_SECRET_MIXER
#define LLVM_SECRET_MIXER

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen//MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/MC/MCRegister.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <utility>

#define GET_REGINFO_ENUM
#include "/home/ilgeco/opt/llvm-project/build/lib/Target/ARM/ARMGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#include "/home/ilgeco/opt/llvm-project/build/lib/Target/ARM/ARMGenInstrInfo.inc"

namespace SecretMixer {

enum class LastElem : unsigned long { Orig, Clone, Uninitialized };

class RandomElem {
public:
  RandomElem() : RanDev(), Rng(RanDev()), UniformDist(0, 1) {}

  LastElem next() { return LastElem(UniformDist(Rng)); }

private:
  std::random_device RanDev;
  std::mt19937 Rng;
  std::uniform_int_distribution<std::mt19937::result_type> UniformDist;
};

class SecretMixer {
public:
  void cloneInstruction();

  SecretMixer(llvm::MachineFunction &MF)
      : LastChoosenElem(LastElem::Uninitialized), MF(MF) {

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
    CloneOperandsFunctions[649] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[650] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[651] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[652] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[653] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[654] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[655] = &SecretMixer::moveccOperandAssign;
    CloneOperandsFunctions[656] = &SecretMixer::moveccOperandAssign;

    // Frame Index Remapper init with stack args
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
        std::string Name = Alloca->getName().str();
        size_t PositionI = 0;
        size_t PositionE = -1;
        PositionE = Name.find_first_of('.', PositionI + 1);
        if (PositionE == std::string::npos)
          continue;
        NameIndex.emplace_back(Name.substr(PositionI, PositionE - PositionI),
                               FrameIndex.getIndex());
      }
    }

    for (auto &[Name, Index] : NameIndex) {
      auto matchIter = std::find_if(
          NameIndex.begin(), NameIndex.end(),
          [&Name](const std::pair<std::string, int> &Pair) {
            return Pair.first.find(Name) == 0 && Pair.first != Name;
          });
      if (matchIter != NameIndex.end()) {
        FrameIndexRemapper.insert({Index, matchIter->second});
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
  }

private:
  llvm::DenseMap<int, int> SameLineRemapper;
  llvm::DenseMap<int, int> CloneRemapper;
  llvm::DenseMap<int, int> FrameIndexRemapper;
  LastElem LastChoosenElem;
  llvm::MachineFunction &MF;
  RandomElem RandomChooser;
  llvm::DenseMap<int,
                 std::function<void(SecretMixer *, const llvm::MachineInstr &,
                                    llvm::MachineInstrBuilder &)>>
      CloneOperandsFunctions;

  void remapOperand(llvm::MachineInstrBuilder &MIB,
                    const llvm::MachineOperand &Op);
  void defaultOperandAssign(const llvm::MachineInstr &MI,
                            llvm::MachineInstrBuilder &Builder);
  void moveccOperandAssign(const llvm::MachineInstr &MI,
                           llvm::MachineInstrBuilder &Builder);
  void sameLineRegisterRemap(llvm::MachineBasicBlock &MIB);
  std::pair<LastElem, bool> whichNext();
};

} // namespace SecretMixer
#endif