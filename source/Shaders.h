#pragma once

/**
    The passes, as GLSL source.

    The detect chain is galvo's, with one change to what the stabilise pass
    writes (see below):

    1. **copy**      picture size, mipmapped. Resolves MaxUV once; the mip
                     chain is what lets the edge pass detect at a scale.
    2. **edge**      TRACE size (320 wide). Sobel on luma-or-alpha, reading
                     the copy at the mip level that matches the trace
                     resolution plus Detail. Writes the gradient in r and the
                     clip's straight colour, read at that same level, in gba
                     -- so the colour a stroke gets is the colour of the
                     region its edge runs through, not a coin flip between
                     the two sides (galvo's Colour Mode = Clip trap).
    3. **stabilise** trace size, ping-ponged. The asymmetric temporal filter.
                     Writes the stabilised gradient in r -- NOT a threshold:
                     the CPU thresholds the byte it reads back, so the whole
                     readback is one RGBA byte read of the mask and the
                     colour together -- and passes the colour through.

    Then the plotter:

    4. **ink**       one instanced quad per machine interval, additive, into
                     the paper. galvo's energy-conserving segment renderer,
                     depositing absorbance per unit TIME.
    5. **resample**  the old paper into a new one, on a resize only.
    6. **composite** output size. Paper, ink, travel, carriage, mix.

    The ink shaders are assembled from a shared constants string plus a body,
    because the vertex stage sizing the quad and the fragment stage
    subtracting the Gaussian's pedestal have to agree about one number.
    `pltest --dump-shaders` writes exactly what the plugin compiles, and
    `tools/check-shaders.sh` runs glslc over that.

    Reserved words avoided as identifiers: patch sample input output filter
    common active half layout flat packed.
*/

#include <string>

namespace plotter
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kEdgeShader;
extern const char* const kStabiliseShader;
extern const char* const kResampleShader;
extern const char* const kCompositeShader;

/// The ink pass, assembled around kInkConstants.
std::string InkVertexSource();
std::string InkFragmentSource();

} // namespace plotter
