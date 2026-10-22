# AMD FidelityFX Super Resolution 1.0 (FSR1) headers

Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR, tag `v1.0.2`, files `ffx-fsr/ffx_a.h`, `ffx-fsr/ffx_fsr1.h`, `license.txt` (MIT). Unmodified.

| File | SHA-256 |
|---|---|
| `ffx_a.h` | `f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb` |
| `ffx_fsr1.h` | `93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5` |
| `license.txt` | `db089274ce766da70f5b7d791029c3486f9f9e27c8c79c652689603d3192e802` |

Used by `src/d3d/fsr1.*` (PLAN Phase 9e): CMake embeds both headers into a generated C++ header at configure time (`cmake/embed_fsr1.cmake`), and the EASU / RCAS pixel shaders are compiled at runtime with an include handler that serves them. The CPU-side constant setup (`FsrEasuCon`, `FsrRcasCon`) compiles the same headers as C++ with `A_CPU`.
