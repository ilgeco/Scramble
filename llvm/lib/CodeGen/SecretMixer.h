
#ifndef LLVM_SECRET_MIXER
#define LLVM_SECRET_MIXER

#include "SecretMixerImpl.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include <memory>

namespace SecretMixer {

class SecretMixer {
private:
  std::unique_ptr<SecretMixerImpl> Pimpl;

public:
  void cloneInstructions();

  SecretMixer(llvm::MachineFunction &MF, llvm::MachineDominatorTree *MDT) {
    Pimpl = std::make_unique<SecretMixerImpl>(MF, MDT);
  };
};

} // namespace SecretMixer
#endif