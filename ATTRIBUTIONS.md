# Attributions

<!-- PROVISIONAL HAND COPY, 2026-09-24, in the shape stoatworks-backend's
     sync-attributions.py generates. Register this repo in its master lists
     and re-run the sync before the first release. -->

This project is built on other people's work. Thank you.

## Bundled or vendored

| Project | Licence | Where |
| --- | --- | --- |
| [FFGL SDK](https://github.com/resolume/ffgl) (Resolume) | MIT | `external/ffgl`, git submodule pinned to `b1afaf9` |
| [galvo](https://github.com/stoatworks-labs/galvo) (Stoatworks Labs) | MIT | `source/Tracer.{h,cpp}` copied verbatim (namespace and one local renamed); `source/PassBuffer.*`, `source/Diag.*`, the detect shaders in `source/Shaders.cpp`, the segment renderer in `source/render/Ink.*`, the nearest-neighbour ordering in `source/Planner.cpp`, and the harness shape in `tools/pltest/main.cpp` adapted from it |
| [vectrix](https://github.com/stoatworks-labs/vectrix) (Stoatworks Labs) | MIT | the energy-conserving segment renderer's lineage, by way of galvo |
| [tinsel](https://github.com/stoatworks-labs/tinsel) (Stoatworks Labs) | MIT | the luma-or-alpha detect mode, `PassBuffer`, the asymmetric temporal filter, `tools/sweep.py` and `tools/verify.sh` shapes |
| [toolpath](https://github.com/stoatworks-labs/toolpath) (Stoatworks Labs) | MIT | the `--pipe` contract, `--dump-shaders` + `check-shaders.sh`, the negative-control pattern |
| Stoatworks About block | MIT | `source/StoatworksAbout*.h`, vendored from stoatworks-backend |
| `scripts/release-lib.sh` | MIT | vendored from stoatworks-backend |

## System libraries

| Library | Licence | Used for |
| --- | --- | --- |
| zlib (macOS system) | zlib | the harness's PNG writer |
| OpenGL (macOS framework) | Apple | the plugin |
| GLEW (Windows, via vcpkg) | Modified BSD / MIT | the Windows build's GL loader |

## Ideas, not code

The trapezoidal velocity planner with backward-then-forward junction passes
is the shape every open motion controller uses (GRBL, Marlin, LinuxCNC's
trajectory planner); it is implemented here from the description, not
copied. Beer-Lambert absorbance for the ink is physics.
