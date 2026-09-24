#pragma once

#include "Planner.h"

#include <cstddef>
#include <vector>

/**
    The plotter's moving parts, advanced by real elapsed time. No GL in here.

    A job is a list of blocks (Planner.h). The machine keeps its place in
    that list -- which block, and how far into it, in double -- and each host
    frame it is told how much time passed and asked what the pen did. What
    the pen did comes back as samples: where the carriage was at the start of
    each interval, how long the interval lasted, and whether the pen was
    down. The renderer turns those into ink.

    Two things happen between the block's closed-form position and the
    sample:

    **Sub-sampling.** An interval is short enough that the speed changes by
    under a percent across it, that the carriage moves under half a step,
    and that it is never longer than 1/240 s. The renderer spreads each
    interval's ink uniformly along it, so this is what makes the deposit
    follow 1/v within the acceleration zones to the tolerance `--ink` states,
    and what lets a coarse step size come out as a staircase rather than a
    slope.

    **Quantisation.** Two stepper motors can only stop on a grid, so each
    sample's position is rounded to the nearest step on each axis. The
    carriage is not; the grid is where the pen is *seen*, and the rounding
    is applied to the closed-form position, never accumulated.

    Time is double because the host's clock is ~500 million milliseconds
    into a session (fleet trap): the frame delta arrives here in seconds,
    relative, and a float never sees an absolute time.
*/
namespace plotter
{

/// One sample of where the carriage is. Also the vertex layout the renderer
/// reads, two per instance, so the fields are laid out for GL.
struct Sample
{
	float x = 0.0f;  ///< paper units, 0..aspect
	float y = 0.0f;  ///< paper units, 0..1, up
	float dt = 0.0f; ///< seconds this sample is held before the next
	float on = 0.0f; ///< 1 when the pen is down for the interval starting here
	float r = 0.0f;  ///< the ink
	float g = 0.0f;
	float b = 0.0f;
	float pad = 0.0f;
};

/// Test hooks the plugin carries at zero. Each one breaks the model in one
/// stated way so `pltest --negative` can prove a check fails.
enum PerturbBits : unsigned
{
	kPerturbNone           = 0,
	kPerturbInfiniteAccel  = 1u << 0, ///< the planner ignores Acceleration: every move is at cruise from its first instant
	kPerturbRetraceEachFrame = 1u << 1, ///< a new job every frame, from its first stroke, instead of on request
	kPerturbNoQuantise     = 1u << 2, ///< Step Size ignored
	kPerturbResizeClears   = 1u << 3, ///< a resize clears the paper instead of resampling it
	kPerturbNoPenSort      = 1u << 4, ///< Optimise orders by distance only, pens interleaved
	kPerturbNoStopAtCorner = 1u << 5, ///< every junction is the cruise speed
};

class Machine
{
public:
	/// Adopt a new job. The machine continues from where it is; the plan was
	/// made from there.
	void SetJob( std::vector< Block > blocks, int penChanges );
	bool HasJob() const
	{
		return !blocks.empty();
	}
	bool JobDone() const
	{
		return blocks.empty() || index >= blocks.size();
	}

	/// The stepper pitch in paper units, 0 for none.
	void SetStepSize( double step );

	/// Advance by `seconds` and append the samples produced. The first
	/// sample appended is where the carriage was at the start, so the
	/// intervals chain across frames without a gap.
	///
	/// Returns the time NOT consumed: the machine stops early when it
	/// reaches the end of the job, or -- when `stopAtStrokeEnd` -- the end
	/// of a stroke, so the caller can re-plan and call again with the rest.
	double Advance( double seconds, bool stopAtStrokeEnd, std::vector< Sample >& out );

	/// Forget the job and the place in it; the carriage stays where it is.
	void Clear();
	/// Everything back to the start of a session: carriage parked at the
	/// carousel, pen up, holding nothing.
	void Reset();

	Vec2 Position() const
	{
		return position;
	}
	int Pen() const
	{
		return pen;
	}
	bool PenDown() const
	{
		return penDown;
	}
	/// The ink of the block the carriage is in, or the last one.
	const float* Ink() const
	{
		return ink;
	}

	/// Strokes finished since Reset, and pen changes made.
	long StrokesCompleted() const
	{
		return strokesCompleted;
	}
	long PenChangesMade() const
	{
		return penChangesMade;
	}
	/// Pen changes the current job planned.
	int PenChangesPlanned() const
	{
		return penChangesPlanned;
	}
	/// Seconds of pen-down time since Reset.
	double InkSeconds() const
	{
		return inkSeconds;
	}
	/// Seconds the current job will take in all, and how far into it.
	double JobSeconds() const
	{
		return jobSeconds;
	}
	double JobElapsed() const
	{
		return jobElapsed;
	}
	std::size_t BlockIndex() const
	{
		return index;
	}
	const std::vector< Block >& Blocks() const
	{
		return blocks;
	}

	/// True on a frame in which a stroke ended. Cleared by the next Advance.
	bool StrokeEnded() const
	{
		return strokeEnded;
	}

private:
	Vec2 quantise( Vec2 p ) const;

	std::vector< Block > blocks;
	std::size_t index = 0;
	double tInBlock   = 0.0;
	double step       = 0.0;

	Vec2 position{ 0.0f, 0.0f };
	int pen       = -1;
	bool penDown  = false;
	float ink[ 3 ] = { 0.0f, 0.0f, 0.0f };

	long strokesCompleted = 0;
	long penChangesMade   = 0;
	int penChangesPlanned = 0;
	double inkSeconds     = 0.0;
	double jobSeconds     = 0.0;
	double jobElapsed     = 0.0;
	bool strokeEnded      = false;
};

} // namespace plotter
