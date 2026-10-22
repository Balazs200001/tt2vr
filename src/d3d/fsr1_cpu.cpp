#include "d3d/fsr1.hpp"

#include <cmath>
#include <cstring>

// AMD's headers as C++ (CPU constant setup only). Their macros stay inside this file.
#pragma warning(push, 0)
#define A_CPU 1
#include "fidelityfx-fsr1/ffx_a.h"
#include "fidelityfx-fsr1/ffx_fsr1.h"
#pragma warning(pop)

namespace d3d {

EasuConstants MakeEasuConstants(float inX, float inW, float inH, float texW, float texH, float outW, float outH)
{
    EasuConstants c{};
    FsrEasuCon(&c.con[0], &c.con[4], &c.con[8], &c.con[12], inW, inH, texW, texH, outW, outH);
    // con0.zw is the input position offset in input pixels (0.5 * in / out - 0.5): shift it to the rectangle's start.
    float offset = 0.0f;
    std::memcpy(&offset, &c.con[2], sizeof(offset));
    offset += inX;
    std::memcpy(&c.con[2], &offset, sizeof(offset));
    return c;
}

RcasConstants MakeRcasConstants(float sharpness)
{
    RcasConstants c{};
    FsrRcasCon(c.con, sharpness);
    return c;
}

} // namespace d3d
