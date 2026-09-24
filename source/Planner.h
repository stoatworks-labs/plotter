#pragma once

#include "Controls.h"
#include "Tracer.h"

#include <cstddef>
#include <vector>

/**
    From contours to a plotter job. No GL in here.

    A plotter driver takes a drawing and turns it into what the machine will
    actually do, in this order:

    1. **Strokes.** Each traced contour becomes one stroke in paper units,
       with the colour the clip had under it and the pen nearest that colour.
    2. **Order.** With Optimise on, strokes are grouped by pen -- a pen change
       is a trip to the carousel, so real drivers did every stroke of one pen
       before touching the next -- and within a pen taken by nearest start,
       reversing an open stroke or rotating a closed one to suit. Off, they
       are drawn in the order the tracer found them, which interleaves pens.
    3. **Blocks.** The motion plan: a list of straight moves, each under a
       trapezoidal velocity profile with an entry speed, a peak and an exit
       speed, plus the dwells (pen settle, pen change) between them. The
       junction speed at a vertex falls linearly with the turn, from the
       cruise speed straight on to zero at Corner Angle -- a corner is a full
       stop -- and a backward-then-forward pass makes
       every junction reachable under the acceleration limit, the way a
       motion controller plans a G-code file.

    Everything the picture then shows -- the heavy ends, the blot, the
    corner that is darker than the straight -- follows from the profile in
    the block and the deposit per unit time in the renderer. Nothing here
    decides how dark a line is.

    Coordinates are paper units: x in [0, aspect], y in [0, 1], y up.
*/
namespace plotter
{

struct Stroke
{
	std::vector< Vec2 > points;
	bool closed = false;
	int pen     = 0;
	float ink[ 3 ] = { 0.0f, 0.0f, 0.0f };
};

/// Convert the tracer's contours (trace pixels, row 0 at the bottom) into
/// strokes. `rgba` is the trace-size readback, four bytes a texel with the
/// clip's straight colour in g, b, a; it may be null, in which case every
/// stroke is grey. `pens` is how many of the palette are on the carousel.
void BuildStrokes( const std::vector< Contour >& contours, int traceWidth, int traceHeight, float aspect,
                   const unsigned char* rgba, Palette palette, int pens, std::vector< Stroke >& out );

/// Reorder in place. `cursor` is where the carriage is now; `currentPen` is
/// the pen it holds, or -1 for none. With `optimise`, strokes are grouped by
/// pen -- the current pen's group first -- and each group is a nearest-start
/// tour. Without it the order is left alone. `byPen` false is the negative
/// control: one tour over every stroke, pens interleaved.
void OrderStrokes( std::vector< Stroke >& strokes, Vec2 cursor, int currentPen, bool optimise, bool byPen = true );

/// One straight move or one dwell.
struct Block
{
	enum class Kind
	{
		Travel,   ///< pen up, between strokes or to and from the carousel
		Draw,     ///< pen down, one segment of a stroke
		Settle,   ///< pen down and still: the blot at each end of a stroke
		PenChange ///< pen up and still, at the carousel
	};

	Kind kind = Kind::Travel;
	Vec2 a, b;
	double length = 0.0;
	/// The trapezoid: entry speed, peak speed, exit speed, and the three
	/// durations. A dwell has all speeds zero and its time in t2.
	double vIn = 0.0, vPeak = 0.0, vOut = 0.0, accel = 1.0;
	double t1 = 0.0, t2 = 0.0, t3 = 0.0;
	int pen = 0;
	float ink[ 3 ] = { 0.0f, 0.0f, 0.0f };
	/// Which stroke this belongs to, or -1.
	int stroke = -1;

	double Duration() const
	{
		return t1 + t2 + t3;
	}
};

struct MachineParams
{
	double maxSpeed        = 0.35; ///< paper heights per second
	double acceleration    = 2.0;  ///< paper heights per second squared
	double settle          = 0.12; ///< seconds, at each end of a stroke
	double cornerAngleDeg  = 30.0; ///< a turn sharper than this stops the pen
	double travelFactor    = kTravelSpeedFactor;
	double penChange       = kPenChangeSeconds;
	Vec2 carousel{ kCarouselX, kCarouselY };
};

/// The time a move of length L takes from rest to rest: L/v + v/a when it
/// reaches cruise, 2 sqrt( L/a ) when it does not. The closed form the
/// planner's blocks reduce to for a single move, and what `--trapezoid`
/// checks the picture against.
double RestToRestSeconds( double length, double maxSpeed, double acceleration );

/// Plan every stroke into blocks, starting from `cursor` holding `currentPen`
/// (-1 for none: the first stroke then begins with a trip to the carousel).
/// Returns the number of pen changes -- trips to the carousel -- planned.
int PlanBlocks( const std::vector< Stroke >& strokes, const MachineParams& params, Vec2 cursor, int currentPen,
                std::vector< Block >& out );

/// Where the carriage is `t` seconds into a block, and how fast it is going.
Vec2 PositionInBlock( const Block& block, double t );
double SpeedInBlock( const Block& block, double t );

} // namespace plotter
