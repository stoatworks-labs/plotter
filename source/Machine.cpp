#include "Machine.h"

#include <algorithm>
#include <cmath>

namespace plotter
{
namespace
{
/// No interval is longer than this, whatever the block is doing.
constexpr double kMaxInterval = 1.0 / 240.0;
/// Across one interval the speed changes by at most this fraction of the
/// block's peak, in the acceleration and deceleration phases.
constexpr double kSpeedChangePerInterval = 0.01;
/// And the carriage moves by at most this fraction of a step.
constexpr double kStepsPerInterval = 0.5;
/// Floors and caps, so a pathological block cannot hold the frame.
constexpr double kMinInterval = 1.0e-5;
constexpr int kMaxIntervalsPerBlock = 4096;
} // namespace

void Machine::SetJob( std::vector< Block > newBlocks, int penChanges )
{
	blocks            = std::move( newBlocks );
	index             = 0;
	tInBlock          = 0.0;
	penChangesPlanned = penChanges;
	jobElapsed        = 0.0;
	jobSeconds        = 0.0;
	for( const Block& b : blocks )
		jobSeconds += b.Duration();
	strokeEnded = false;
	if( blocks.empty() )
		penDown = false;
}

void Machine::SetStepSize( double pitch )
{
	step = std::max( 0.0, pitch );
}

void Machine::Clear()
{
	blocks.clear();
	index       = 0;
	tInBlock    = 0.0;
	jobElapsed  = 0.0;
	jobSeconds  = 0.0;
	penDown     = false;
	strokeEnded = false;
}

void Machine::Reset()
{
	Clear();
	//The carriage parks at the carousel with no pen, so a session's first
	//stroke costs one swap and one travel, not a trip across the sheet first.
	position         = Vec2{ kCarouselX, kCarouselY };
	pen              = -1;
	strokesCompleted = 0;
	penChangesMade   = 0;
	inkSeconds       = 0.0;
	ink[ 0 ] = ink[ 1 ] = ink[ 2 ] = 0.0f;
}

Vec2 Machine::quantise( Vec2 p ) const
{
	if( step <= 0.0 )
		return p;
	//Rounded from the closed-form position each time, never accumulated, so
	//the grid cannot drift and the rounding is the same at any frame rate.
	const double x = std::round( static_cast< double >( p.x ) / step ) * step;
	const double y = std::round( static_cast< double >( p.y ) / step ) * step;
	return Vec2{ static_cast< float >( x ), static_cast< float >( y ) };
}

double Machine::Advance( double seconds, bool stopAtStrokeEnd, std::vector< Sample >& out )
{
	strokeEnded = false;
	if( JobDone() || seconds <= 0.0 )
		return seconds;

	double remaining = seconds;

	//The chain starts where the carriage is. Its dt and pen state are filled
	//in by the first interval below; if there is none, the renderer sees a
	//single sample and draws nothing.
	{
		Sample start;
		const Vec2 q = quantise( position );
		start.x      = q.x;
		start.y      = q.y;
		out.push_back( start );
	}

	while( remaining > 0.0 && index < blocks.size() )
	{
		const Block& block    = blocks[ index ];
		const double duration = block.Duration();
		const double left     = std::max( 0.0, duration - tInBlock );
		const double take     = std::min( remaining, left );
		const bool down       = block.kind == Block::Kind::Draw || block.kind == Block::Kind::Settle;

		if( take > 0.0 )
		{
			//How finely to sample this stretch. See the class comment.
			double h = kMaxInterval;
			if( block.t1 > 0.0 || block.t3 > 0.0 )
				h = std::min( h, kSpeedChangePerInterval * block.vPeak / block.accel );
			if( step > 0.0 && block.vPeak > 0.0 )
				h = std::min( h, kStepsPerInterval * step / block.vPeak );
			h = std::max( h, kMinInterval );

			const int n = std::clamp( static_cast< int >( std::ceil( take / h ) ), 1, kMaxIntervalsPerBlock );
			const double dt = take / n;
			for( int i = 1; i <= n; ++i )
			{
				const Vec2 p = PositionInBlock( block, tInBlock + take * i / n );
				Sample& head = out.back();
				head.dt      = static_cast< float >( dt );
				head.on      = down ? 1.0f : 0.0f;
				head.r       = block.ink[ 0 ];
				head.g       = block.ink[ 1 ];
				head.b       = block.ink[ 2 ];

				Sample next;
				const Vec2 q = quantise( p );
				next.x       = q.x;
				next.y       = q.y;
				out.push_back( next );
			}

			if( down )
				inkSeconds += take;
			tInBlock += take;
			remaining -= take;
			jobElapsed += take;
		}

		position = PositionInBlock( block, tInBlock );
		penDown  = down;
		pen      = block.pen;
		for( int c = 0; c < 3; ++c )
			ink[ c ] = block.ink[ c ];

		//Finished this block?
		if( take >= left - 1e-12 )
		{
			if( block.kind == Block::Kind::PenChange )
				++penChangesMade;

			//The settle at the END of a stroke is the last block with that
			//stroke's index; the one at the start is followed by its segments.
			const bool strokeEnd = block.kind == Block::Kind::Settle
			                       && ( index + 1 >= blocks.size() || blocks[ index + 1 ].stroke != block.stroke );
			++index;
			tInBlock = 0.0;
			if( strokeEnd )
			{
				++strokesCompleted;
				strokeEnded = true;
				if( stopAtStrokeEnd )
					break;
			}
		}
	}

	if( JobDone() )
		penDown = false;
	return remaining;
}

} // namespace plotter
