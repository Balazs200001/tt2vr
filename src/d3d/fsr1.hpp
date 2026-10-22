// d3d::Fsr1 constants (PLAN 9e): the CPU half of AMD FidelityFX FSR 1.0 (third_party/fidelityfx-fsr1, MIT), compiled
// from the same headers as the shaders (A_CPU). The shaders themselves are in d3d::Blitter::Fsr.
#pragma once

#include <cstdint>

namespace d3d {

// EASU (upscale): input rectangle inX..inX+inW x inH of a texW x texH texture -> outW x outH output.
struct EasuConstants {
    std::uint32_t con[16];
};
EasuConstants MakeEasuConstants(float inX, float inW, float inH, float texW, float texH, float outW, float outH);

// RCAS (sharpen): sharpness in stops, 0 = strongest, 2 = mild (FSR's scale).
struct RcasConstants {
    std::uint32_t con[4];
};
RcasConstants MakeRcasConstants(float sharpness);

} // namespace d3d
