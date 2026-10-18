// power.cpp — XIAO-S3 stub: no battery divider is exposed (docs/mini-build.md §2.1), so STATUS bat=-1.
#include "../host_common/power.h"

namespace power {
void init() {}
void sample() {}
int  voltageMv() { return 0; }
int  percent()   { return -1; }
bool present()   { return false; }
}
