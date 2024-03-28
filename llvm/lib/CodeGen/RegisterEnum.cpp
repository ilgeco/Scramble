

#include <algorithm>
namespace utils {

unsigned int MR[14] = {73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 13};

void scrambleRegister() { std::random_shuffle(&MR[0], &MR[14]); }

} // namespace utils