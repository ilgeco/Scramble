namespace utils {

enum MixerRegisterIndex {
  R0,
  R1,
  R2,
  R3,
  R4,
  R5,
  R6,
  R7,
  R8,
  R9,
  R10,
  R11,
  R12,
  LR
};

extern unsigned int MR[14];

void scrambleRegister();
} // namespace utils