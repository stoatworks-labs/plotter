#pragma once

#include "Machine.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <vector>

namespace plotter
{
/**
    The paper, and the ink going onto it.

    galvo's beam renderer (the vectrix lineage), turned from light into
    absorbance. Each machine interval deposits a fixed quantum, InkRate * dt
    while the pen is down, spread over the distance the carriage covered in
    it by the exact convolution of a uniform segment with a Gaussian nib.
    A line's darkness proportional to 1/speed falls out as the definition of
    equal ink per unit time; a pen that stops deposits a finite Gaussian
    blot with no special case; and the ink on the sheet is InkRate times the
    pen-down time whatever the machine did with the path, which is what
    `pltest --trapezoid` measures the motion by.

    One RGBA16F buffer at picture size, additive, never decayed: ink stays.
    rgb holds absorbance per channel (a red pen absorbs green and blue), a
    holds the travel density for Show Travel.

    **It survives a resize.** photofinish shipped a buffer that reallocated
    on resize and so cleared the one that held the previous frame; here the
    old sheet is drawn into the new one, bilinearly, before the old one is
    released. `pltest --persist` resizes mid-run and checks.

    Sixteen-bit float rather than thirty-two, as galvo chose it and measured
    it: a third of a percent of energy for half a millisecond a frame at 4K.
    A sheet that accumulates for minutes is the case that would change the
    answer, and a plotter's sheet does; a black line at absorbance 3 is still
    represented to 0.1% in half floats, and an absorbance past 12 is ink
    nobody can tell from ink at 8.
*/
class InkRenderer
{
public:
	struct Params
	{
		double inkRate    = 0.0;    ///< absorbance x height^2 per second, pen down
		double travelRate = 0.0;    ///< the same for pen-up moves; 0 draws no travel
		float nibSigma    = 0.0025f;///< paper units; 1 = the paper height
	};

	bool InitGL();
	void DeInitGL();

	/// Allocate the paper at this size, resampling the old sheet into it if
	/// the size changed. Call before anything binds a texture: allocating
	/// unbinds the active unit (SDK trap). `clearOnResize` is the negative
	/// control's hook and is never set by the plugin.
	bool Ensure( int width, int height, float aspect, bool clearOnResize = false );

	/// Tear the sheet off: clear to nothing.
	void Clear();

	/// Deposit `n` samples' worth of intervals (n - 1 of them).
	bool Deposit( const Sample* samples, int n, const Params& params, float aspect );

	GLuint TextureID() const
	{
		return paper.TextureID();
	}

	/// Read the paper back as RGBA floats, bottom row first. For the
	/// harness: 16-bit float texels come back through the driver's
	/// conversion, so what is measured is what was stored.
	bool Read( std::vector< float >& rgba ) const;

	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}

private:
	ffglex::FFGLShader inkShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer paper;
	PassBuffer spare;
	int width  = 0;
	int height = 0;

	GLuint inkVAO = 0;
	GLuint inkVBO = 0;
};

} // namespace plotter
