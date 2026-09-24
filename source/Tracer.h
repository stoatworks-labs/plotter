#pragma once

/*
    Copied from galvo (github.com/stoatworks-labs/galvo, MIT, Stoatworks Labs),
    source/Tracer.h at its v0.1.0, namespace renamed and one local (`far`, an
    MSVC windef.h macro) renamed. Nothing else changed: the tracer is the
    part of a plotter that is not a plotter. See ATTRIBUTIONS.md.
*/

#include <vector>

/**
    From a binary edge mask to ordered polylines. No GL in here.

    This is the half of the pipeline the GPU cannot do. Contour following is
    pointer-chasing -- which pixel comes *next* -- and FFGL on macOS is GL 4.1
    core, with no compute shaders, no SSBOs and no image load/store. So the mask
    is read back small and the walking happens here, on the CPU, on a buffer of
    a few tens of kilobytes.

    Three steps:

    1. **Thin.** A Sobel edge at the trace resolution is two or three pixels
       wide. Walked as it is, the contour wanders back and forth across the
       band and draws a scribble on top of every line. Zhang-Suen reduces it to
       a one-pixel skeleton first.
    2. **Walk.** Endpoints first, then junctions, then whatever is left -- which
       is the closed loops. Starting an open line from its endpoint matters:
       starting in the middle produces two half-strokes instead of one whole
       one. A walk that arrives back next to where it started is a closed
       contour.
    3. **Simplify.** Douglas-Peucker with a tolerance in pixels, so a straight
       edge is two points and not two hundred. Closed contours are split at
       the vertex farthest from the first and simplified as two open halves.
*/
namespace plotter
{

struct Vec2
{
	float x = 0.0f;
	float y = 0.0f;
};

struct Contour
{
	std::vector< Vec2 > points;
	bool closed = false;
};

struct TraceParams
{
	/// Mask value at and above which a pixel is an edge.
	unsigned char threshold = 128;
	/// Douglas-Peucker tolerance, in mask pixels.
	float simplify = 1.0f;
	/// Contours shorter than this, in mask pixels after simplification, are
	/// dropped. Specks on footage are short; letters are not.
	float minLength = 8.0f;
};

/// Length of a polyline, including the closing segment for a closed one.
float PolylineLength( const std::vector< Vec2 >& points, bool closed );

/// Douglas-Peucker. `out` never aliases `in`.
void SimplifyPolyline( const std::vector< Vec2 >& in, float epsilon, bool closed, std::vector< Vec2 >& out );

class Tracer
{
public:
	/// `mask` is width x height bytes, row 0 first -- whichever way up the
	/// caller stores it; the contours come back in the same coordinates, with
	/// x and y being pixel indices. Scratch buffers are kept between calls.
	void Trace( const unsigned char* mask, int width, int height, const TraceParams& params,
	            std::vector< Contour >& out );

	/// The thinned skeleton from the last Trace, for the harness to look at.
	const std::vector< unsigned char >& Skeleton() const
	{
		return skeleton;
	}

private:
	void thin( int width, int height );
	void walk( int width, int height, const TraceParams& params, std::vector< Contour >& out );

	std::vector< unsigned char > skeleton;
	std::vector< unsigned char > visited;
	std::vector< Vec2 > raw;
	std::vector< Vec2 > reduced;
};

} // namespace plotter
