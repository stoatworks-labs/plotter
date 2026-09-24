//Copied from galvo (github.com/stoatworks-labs/galvo, MIT): see Tracer.h.
#include "Tracer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace plotter
{
namespace
{
/// The 8-neighbourhood. The four direct neighbours come first so that the walk
/// prefers a straight step over a diagonal one: on a thinned skeleton a
/// diagonal taken while a direct neighbour was available leaves that neighbour
/// orphaned as a one-pixel contour of its own.
constexpr int kNeighbourX[ 8 ] = { 1, 0, -1, 0, 1, -1, -1, 1 };
constexpr int kNeighbourY[ 8 ] = { 0, 1, 0, -1, 1, 1, -1, -1 };

/// Zhang-Suen's neighbourhood, in its own order: P2 north, then clockwise.
constexpr int kZsX[ 8 ] = { 0, 1, 1, 1, 0, -1, -1, -1 };
constexpr int kZsY[ 8 ] = { 1, 1, 0, -1, -1, -1, 0, 1 };

float distanceToSegment( const Vec2& p, const Vec2& a, const Vec2& b )
{
	const float dx  = b.x - a.x;
	const float dy  = b.y - a.y;
	const float len = std::sqrt( dx * dx + dy * dy );
	if( len < 1e-6f )
		return std::sqrt( ( p.x - a.x ) * ( p.x - a.x ) + ( p.y - a.y ) * ( p.y - a.y ) );
	return std::fabs( dy * p.x - dx * p.y + b.x * a.y - b.y * a.x ) / len;
}

/// Iterative Douglas-Peucker over an open run [first, last] of `in`, marking
/// the survivors in `keep`.
void simplifyRun( const std::vector< Vec2 >& in, std::size_t first, std::size_t last, float epsilon,
                  std::vector< unsigned char >& keep )
{
	keep[ first ] = keep[ last ] = 1;
	std::vector< std::pair< std::size_t, std::size_t > > stack{ { first, last } };

	while( !stack.empty() )
	{
		const auto [ a, b ] = stack.back();
		stack.pop_back();
		if( b <= a + 1 )
			continue;

		float worst      = 0.0f;
		std::size_t pick = a;
		for( std::size_t i = a + 1; i < b; ++i )
		{
			const float d = distanceToSegment( in[ i ], in[ a ], in[ b ] );
			if( d > worst )
			{
				worst = d;
				pick  = i;
			}
		}

		if( worst > epsilon )
		{
			keep[ pick ] = 1;
			stack.push_back( { a, pick } );
			stack.push_back( { pick, b } );
		}
	}
}
} // namespace

float PolylineLength( const std::vector< Vec2 >& points, bool closed )
{
	float length = 0.0f;
	for( std::size_t i = 1; i < points.size(); ++i )
	{
		const float dx = points[ i ].x - points[ i - 1 ].x;
		const float dy = points[ i ].y - points[ i - 1 ].y;
		length += std::sqrt( dx * dx + dy * dy );
	}
	if( closed && points.size() > 2 )
	{
		const float dx = points.front().x - points.back().x;
		const float dy = points.front().y - points.back().y;
		length += std::sqrt( dx * dx + dy * dy );
	}
	return length;
}

void SimplifyPolyline( const std::vector< Vec2 >& in, float epsilon, bool closed, std::vector< Vec2 >& out )
{
	out.clear();
	if( in.size() < 3 )
	{
		out = in;
		return;
	}

	std::vector< unsigned char > keep( in.size(), 0 );

	if( !closed )
	{
		simplifyRun( in, 0, in.size() - 1, epsilon, keep );
	}
	else
	{
		//A closed loop has no endpoints to anchor on, so anchor on the two
		//points that are farthest apart along the loop's own geometry: the
		//first, and the one farthest from it. Each half is then an ordinary
		//open run. The two anchors survive whatever the tolerance, which is what
		//keeps a circle from simplifying to nothing.
		std::size_t farIndex = 0;
		float farthest  = -1.0f;
		for( std::size_t i = 1; i < in.size(); ++i )
		{
			const float dx = in[ i ].x - in[ 0 ].x;
			const float dy = in[ i ].y - in[ 0 ].y;
			const float d  = dx * dx + dy * dy;
			if( d > farthest )
			{
				farthest = d;
				farIndex = i;
			}
		}

		simplifyRun( in, 0, farIndex, epsilon, keep );

		//The second half runs from `farIndex` round to the first point again, which
		//is not in the vector twice -- so it is simplified against a copy with
		//the start appended.
		std::vector< Vec2 > tail( in.begin() + static_cast< std::ptrdiff_t >( farIndex ), in.end() );
		tail.push_back( in.front() );
		std::vector< unsigned char > keepTail( tail.size(), 0 );
		simplifyRun( tail, 0, tail.size() - 1, epsilon, keepTail );
		for( std::size_t i = 0; i + 1 < tail.size(); ++i )
			if( keepTail[ i ] )
				keep[ farIndex + i ] = 1;
	}

	for( std::size_t i = 0; i < in.size(); ++i )
		if( keep[ i ] )
			out.push_back( in[ i ] );
}

//---------------------------------------------------------------------------
void Tracer::Trace( const unsigned char* mask, int width, int height, const TraceParams& params,
                    std::vector< Contour >& out )
{
	out.clear();
	if( mask == nullptr || width < 3 || height < 3 )
		return;

	const std::size_t count = static_cast< std::size_t >( width ) * static_cast< std::size_t >( height );
	skeleton.assign( count, 0 );
	visited.assign( count, 0 );

	for( std::size_t i = 0; i < count; ++i )
		skeleton[ i ] = mask[ i ] >= params.threshold ? 1 : 0;

	//The border is cleared so every neighbourhood lookup below can skip its
	//bounds check. A mask that reaches the edge of the frame loses one pixel
	//of it, which is the edge of the frame and not an edge in the picture.
	for( int x = 0; x < width; ++x )
	{
		skeleton[ static_cast< std::size_t >( x ) ]                                            = 0;
		skeleton[ static_cast< std::size_t >( height - 1 ) * static_cast< std::size_t >( width ) + x ] = 0;
	}
	for( int y = 0; y < height; ++y )
	{
		skeleton[ static_cast< std::size_t >( y ) * static_cast< std::size_t >( width ) ]             = 0;
		skeleton[ static_cast< std::size_t >( y ) * static_cast< std::size_t >( width ) + width - 1 ] = 0;
	}

	thin( width, height );
	walk( width, height, params, out );
}

//---------------------------------------------------------------------------
void Tracer::thin( int width, int height )
{
	auto at = [ & ]( int x, int y ) -> unsigned char& {
		return skeleton[ static_cast< std::size_t >( y ) * static_cast< std::size_t >( width ) + x ];
	};

	//Zhang-Suen. Two sub-iterations per pass, deleting in batches so a pass
	//sees the image as it was and not as it is becoming. Capped rather than
	//run to convergence: a band three pixels wide is thin after two passes,
	//and a pathological mask must not be allowed to hold the frame.
	std::vector< std::size_t > doomed;
	bool changed = true;
	for( int pass = 0; pass < 16 && changed; ++pass )
	{
		changed = false;
		for( int sub = 0; sub < 2; ++sub )
		{
			doomed.clear();
			for( int y = 1; y < height - 1; ++y )
			{
				for( int x = 1; x < width - 1; ++x )
				{
					if( at( x, y ) == 0 )
						continue;

					int neighbours          = 0;
					int transitions         = 0;
					unsigned char previous  = at( x + kZsX[ 7 ], y + kZsY[ 7 ] );
					unsigned char p[ 8 ];
					for( int k = 0; k < 8; ++k )
					{
						p[ k ] = at( x + kZsX[ k ], y + kZsY[ k ] );
						neighbours += p[ k ];
						if( previous == 0 && p[ k ] == 1 )
							++transitions;
						previous = p[ k ];
					}

					if( neighbours < 2 || neighbours > 6 || transitions != 1 )
						continue;

					//P2 north, P4 east, P6 south, P8 west in Zhang-Suen's naming.
					const unsigned char n = p[ 0 ], e = p[ 2 ], s = p[ 4 ], w = p[ 6 ];
					const bool first  = sub == 0 ? ( n * e * s ) == 0 : ( n * e * w ) == 0;
					const bool second = sub == 0 ? ( e * s * w ) == 0 : ( n * s * w ) == 0;
					if( first && second )
						doomed.push_back( static_cast< std::size_t >( y ) * static_cast< std::size_t >( width ) + x );
				}
			}
			for( std::size_t index : doomed )
				skeleton[ index ] = 0;
			if( !doomed.empty() )
				changed = true;
		}
	}
}

//---------------------------------------------------------------------------
void Tracer::walk( int width, int height, const TraceParams& params, std::vector< Contour >& out )
{
	auto index = [ & ]( int x, int y ) {
		return static_cast< std::size_t >( y ) * static_cast< std::size_t >( width ) + x;
	};
	auto degree = [ & ]( int x, int y ) {
		int count = 0;
		for( int k = 0; k < 8; ++k )
			count += skeleton[ index( x + kNeighbourX[ k ], y + kNeighbourY[ k ] ) ];
		return count;
	};

	auto walkFrom = [ & ]( int startX, int startY ) {
		raw.clear();
		int x = startX;
		int y = startY;

		while( true )
		{
			visited[ index( x, y ) ] = 1;
			raw.push_back( Vec2{ static_cast< float >( x ), static_cast< float >( y ) } );

			int nextX = -1;
			int nextY = -1;
			for( int k = 0; k < 8; ++k )
			{
				const int nx = x + kNeighbourX[ k ];
				const int ny = y + kNeighbourY[ k ];
				if( nx < 1 || ny < 1 || nx >= width - 1 || ny >= height - 1 )
					continue;
				if( skeleton[ index( nx, ny ) ] == 0 || visited[ index( nx, ny ) ] )
					continue;
				nextX = nx;
				nextY = ny;
				break;
			}
			if( nextX < 0 )
				break;
			x = nextX;
			y = nextY;
		}

		//Two-pixel specks are noise, not drawing, and nothing under four pixels
		//can be told closed from open.
		if( raw.size() < 4 )
			return;

		//Closed if the walk came back next to where it began. Eight-adjacency,
		//because the skeleton is eight-connected.
		const bool closed = raw.size() >= 8
		                    && std::fabs( raw.back().x - raw.front().x ) <= 1.0f
		                    && std::fabs( raw.back().y - raw.front().y ) <= 1.0f;

		SimplifyPolyline( raw, params.simplify, closed, reduced );
		if( reduced.size() < 2 )
			return;
		if( PolylineLength( reduced, closed ) < params.minLength )
			return;

		Contour contour;
		contour.points = reduced;
		contour.closed = closed;
		out.push_back( std::move( contour ) );
	};

	//Endpoints first, so an open line is one stroke and not two halves. Then
	//junctions, so the arms of a T are walked from the crossing outward. Then
	//anything unvisited, which is the closed loops.
	for( int y = 1; y < height - 1; ++y )
		for( int x = 1; x < width - 1; ++x )
			if( skeleton[ index( x, y ) ] && !visited[ index( x, y ) ] && degree( x, y ) == 1 )
				walkFrom( x, y );

	for( int y = 1; y < height - 1; ++y )
		for( int x = 1; x < width - 1; ++x )
			if( skeleton[ index( x, y ) ] && !visited[ index( x, y ) ] && degree( x, y ) >= 3 )
				walkFrom( x, y );

	for( int y = 1; y < height - 1; ++y )
		for( int x = 1; x < width - 1; ++x )
			if( skeleton[ index( x, y ) ] && !visited[ index( x, y ) ] )
				walkFrom( x, y );
}

} // namespace plotter
