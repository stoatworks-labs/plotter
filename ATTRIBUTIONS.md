# Attributions

Plotter is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Tracer, detect chain, segment renderer, harness shape — Stoatworks galvo

<https://github.com/stoatworks-labs/galvo>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Tracer.{h,cpp} is galvo's tracer copied verbatim (namespace renamed, and the local `far` renamed for MSVC, where it is a macro); the mip copy, Sobel-on-luma-or-alpha and asymmetric temporal filter passes, the energy-conserving segment renderer turned from light into absorbance in source/render/Ink.*, PassBuffer with the FFGLFBO leak fixed, Diag, the nearest-neighbour stroke ordering, the clock-unit voting and the harness shape in tools/pltest are galvo's, adapted.

### Energy-conserving segment renderer — Stoatworks vectrix

<https://github.com/stoatworks-labs/vectrix>  
Licence: MIT  
Copyright: Stoatworks Labs

The lineage of galvo's beam renderer, which deposits a Gaussian across each segment with the integral held constant along it, by way of galvo.

### PassBuffer, luma-or-alpha detect, the sweep and verify shapes — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper, the `a (0.35 + 0.65 luma)` detect mode, the asymmetric temporal filter, tools/sweep.py and the shape of tools/verify.sh, and the lesson that every ffglex::Scoped* binding clears rather than restores.

### --pipe contract, --dump-shaders, the negative-control pattern — Stoatworks toolpath

<https://github.com/stoatworks-labs/toolpath>  
Licence: MIT  
Copyright: Stoatworks Labs

The fleet's raw-RGBA --pipe mode with cue sheets and SIGPIPE ignored, --dump-shaders with check-shaders.sh over the exact strings the plugin compiles, the Perturb-bit negative controls and the "would this hold on another rasteriser" table.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Pen plotters and their drivers

The look of a plotter drawing (heavy ends, blots at every pen-down, dark corners, staircases at a coarse step, a trip to the carousel for every pen change) is the machine's, and the sort-by-pen-then-nearest-start ordering is what real plotter drivers did. No code, assets or binaries from any plotter, driver or firmware were used or examined.

## Standards and published specifications

What the implementation is measured against.

- **Trapezoidal velocity planning with backward-then-forward junction passes** — The shape every open motion controller uses (GRBL, Marlin, LinuxCNC's trajectory planner), implemented from the description: a block takes L/v + v/a when it reaches cruise and 2√(L/a) when it does not; the junction speed falls linearly with the turn to zero at Corner Angle.
- **The Beer-Lambert law** — Ink as absorbance: transmission is exp(-absorbance) per channel, so a red pen absorbs green and blue and two strokes crossing are darker than either.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
