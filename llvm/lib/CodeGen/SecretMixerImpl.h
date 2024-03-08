
#ifndef LLVM_SECRET_MIXER_IMPL
#define LLVM_SECRET_MIXER_IMPL

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/ErrorHandling.h"
#include <functional>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include <utility>

#define GET_REGINFO_ENUM
#include "/home/ilgeco/opt/llvm-project/dist/lib/Target/ARM/ARMGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#include "/home/ilgeco/opt/llvm-project/dist/lib/Target/ARM/ARMGenInstrInfo.inc"

namespace SecretMixer {

class FreeRegister {
public:
  FreeRegister() = default;

  FreeRegister(const llvm::DenseMap<int, int> &RegisterMap,
               llvm::DenseSet<int> InitSet)
      : RegisterMap(RegisterMap), FreeRegisterSet(), OccRegisterSet(),
        InitSet(InitSet) {

    for (auto [set, free] : RegisterMap) {
      if (!InitSet.contains(set)) {
        OccRegisterSet.insert(set);
      } else {
        FreeRegisterSet.insert(free);
      }
    }
  }

  void setInitRegister(int Reg);
  template <class E>
  constexpr void setInitRegister(std::initializer_list<E> InitList) {
    if constexpr (std::is_convertible_v<E, int>) {
      InitSet.insert(InitList.begin(), InitList.end());
    } else {
      llvm_unreachable("Init Set initialized with no integer");
    }
  }

  void setRegisterMap(const llvm::DenseMap<int, int> &registerMap);
  void setNewOccRegister(int Reg);
  void setFreeRegister(int Reg);
  void setFreeRegister(std::initializer_list<int> Regs);
  void setNewFreeRegister(int Reg);
  std::optional<int> peekFirstFreeRegister();
  int getFreeRegister();
  void setOccRegister(int Reg);
  void reset();
  void updateFreeRegisterPreIt(const llvm::MachineInstr &MI);
  void updateFreeRegisterPostIt(const llvm::MachineInstr &MI);
  void dump();
  // Check if reister is Free
  bool isFree(int Reg);

private:
  llvm::DenseMap<int, int> RegisterMap;
  llvm::DenseSet<int> FreeRegisterSet;
  llvm::DenseSet<int> OccRegisterSet;
  llvm::DenseSet<int> InitSet;
};

class SecretMixerImpl {
public:
  void extracted(llvm::DenseMap<llvm::MachineBasicBlock *,
                                std::vector<llvm::MachineInstr *>> &ITinsts);
  void extracted();
  void cloneInstructions();
  SecretMixerImpl(llvm::MachineFunction &MF, llvm::MachineDominatorTree *MDT);

private:
  llvm::DenseMap<int, int> SameLineRemapper;
  llvm::DenseMap<int, int> CloneRemapper;
  llvm::DenseMap<int, int> FrameIndexRemapper;
  llvm::MachineFunction &MF;
  llvm::MachineDominatorTree *MDT;
  std::map<std::pair<int, llvm::Align>, int> VoidFrames;
  llvm::DenseMap<
      int, std::function<void(SecretMixerImpl *, const llvm::MachineInstr &,
                              llvm::MachineInstrBuilder &)>>
      CloneOperandsFunctions;
  FreeRegister freeRegisterTracker;

  int getVoidFrame(int Size, llvm::Align Align);
  // Clone the MachineOperand Op appending it to the MI contructed by MIB,
  // if Op is a register it'attemps a remap following the CloneRemapper entries
  void remapOperand(llvm::MachineInstrBuilder &MIB,
                    const llvm::MachineOperand &Op);
  // Default assign for all the MI that doesn't need extra code
  void defaultOperandAssign(const llvm::MachineInstr &MI,
                            llvm::MachineInstrBuilder &Builder);

  void moveccOperandAssign(const llvm::MachineInstr &MI,
                           llvm::MachineInstrBuilder &Builder);
  // Remap the same line if use and def the same register
  void sameLineRegisterRemap(llvm::MachineBasicBlock &MIB);

  // Add a condition to IT based on the RandomGenerator
  void
  addConditionToIT(llvm::DenseMap<llvm::MachineBasicBlock *,
                                  std::vector<llvm::MachineInstr *>> &ITinsts,
                   int RandomIndex);

  void loadRandom(llvm::MachineBasicBlock *MBB, llvm::MachineInstr *InsPoint,
                  int RandomIndex);

  auto testAndShiftRegister(llvm::MachineBasicBlock *MBB,
                            llvm::MachineInstr *InsPoint, int RandomIndex,
                            int suggestRegister);
  std::pair<int, int> allocateStackSpaceForRandom();
  void recursiveConditionToIt(
      const llvm::DenseMap<llvm::MachineBasicBlock *,
                           std::vector<llvm::MachineInstr *>> &ITinsts,
      llvm::MachineBasicBlock *MBB, int Counter, int RandomIndex,
      llvm::DenseSet<llvm::MachineBasicBlock *> &BasicBlockHandled);
};

} // namespace SecretMixer
#endif