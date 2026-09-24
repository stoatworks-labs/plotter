#include "Planner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace plotter
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

float distanceSquared( const Vec2& a, const Vec2& b )
{
	return ( a.x - b.x ) * ( a.x - b.x ) + ( a.y - b.y ) * ( a.y - b.y );
}

double distance( const Vec2& a, const Vec2& b )
{
	return std::sqrt( static_cast< double >( distanceSquared( a, b ) ) );
}

/// The trapezoid for one move: entry speed, exit speed, the cruise limit
/// and the acceleration, over a length. The peak is the cruise speed when
/// the move is long enough to get there and back, and otherwise the speed at
/// which accelerating from vIn meets decelerating to vOut.
void profile( Block& block, double vMax, double accel )
{
	const double L = block.length;
	accel          = std::max( accel, 1e-9 );
	double vPeak   = std::sqrt( std::max( 0.0, ( 2.0 * accel * L + block.vIn * block.vIn + block.vOut * block.vOut ) * 0.5 ) );
	vPeak          = std::min( vPeak, vMax );
	vPeak          = std::max( vPeak, std::max( block.vIn, block.vOut ) );

	const double d1 = ( vPeak * vPeak - block.vIn * block.vIn ) / ( 2.0 * accel );
	const double d3 = ( vPeak * vPeak - block.vOut * block.vOut ) / ( 2.0 * accel );
	const double d2 = std::max( 0.0, L - d1 - d3 );

	block.vPeak = vPeak;
	block.accel = accel;
	block.t1    = ( vPeak - block.vIn ) / accel;
	block.t3    = ( vPeak - block.vOut ) / accel;
	block.t2    = vPeak > 0.0 ? d2 / vPeak : 0.0;
}

Block dwell( Block::Kind kind, Vec2 at, double seconds, int pen, const float ink[ 3 ], int stroke )
{
	Block block;
	block.kind   = kind;
	block.a      = at;
	block.b      = at;
	block.length = 0.0;
	block.t2     = std::max( 0.0, seconds );
	block.pen    = pen;
	block.stroke = stroke;
	for( int c = 0; c < 3; ++c )
		block.ink[ c ] = ink[ c ];
	return block;
}

/// A rest-to-rest move with the pen up.
void travel( std::vector< Block >& out, Vec2 from, Vec2 to, const MachineParams& params, int pen, const float ink[ 3 ] )
{
	const double L = distance( from, to );
	if( L <= 1e-9 )
		return;
	Block block;
	block.kind   = Block::Kind::Travel;
	block.a      = from;
	block.b      = to;
	block.length = L;
	block.vIn    = 0.0;
	block.vOut   = 0.0;
	block.pen    = pen;
	for( int c = 0; c < 3; ++c )
		block.ink[ c ] = ink[ c ];
	profile( block, params.maxSpeed * params.travelFactor, params.acceleration );
	out.push_back( block );
}
} // namespace

//---------------------------------------------------------------------------
void BuildStrokes( const std::vector< Contour >& contours, int traceWidth, int traceHeight, float aspect,
                   const unsigned char* rgba, Palette palette, int pens, std::vector< Stroke >& out )
{
	out.clear();
	const float sx = aspect / static_cast< float >( std::max( traceWidth, 1 ) );
	const float sy = 1.0f / static_cast< float >( std::max( traceHeight, 1 ) );

	auto colourAt = [ & ]( float px, float py, float acc[ 3 ] ) {
		const int x = std::clamp( static_cast< int >( std::lround( px ) ), 0, traceWidth - 1 );
		const int y = std::clamp( static_cast< int >( std::lround( py ) ), 0, traceHeight - 1 );
		const unsigned char* t = rgba + ( static_cast< std::size_t >( y ) * traceWidth + x ) * 4;
		acc[ 0 ] += t[ 1 ] / 255.0f;
		acc[ 1 ] += t[ 2 ] / 255.0f;
		acc[ 2 ] += t[ 3 ] / 255.0f;
	};

	for( const Contour& contour : contours )
	{
		const std::size_t n = contour.points.size();
		if( n < 2 )
			continue;

		Stroke stroke;
		stroke.closed = contour.closed;
		stroke.points.reserve( n );
		for( const Vec2& p : contour.points )
			stroke.points.push_back( Vec2{ ( p.x + 0.5f ) * sx, ( p.y + 0.5f ) * sy } );

		//The clip's colour along the contour: a sample every couple of trace
		//pixels, averaged. The readback carries the colour read at the edge
		//pass's mip level, so it is already the colour of the region the edge
		//runs through rather than a coin flip between its two sides.
		float source[ 3 ] = { 0.5f, 0.5f, 0.5f };
		if( rgba != nullptr )
		{
			float acc[ 3 ] = { 0.0f, 0.0f, 0.0f };
			int count      = 0;
			const std::size_t segments = contour.closed ? n : n - 1;
			for( std::size_t s = 0; s < segments; ++s )
			{
				const Vec2& a = contour.points[ s ];
				const Vec2& b = contour.points[ ( s + 1 ) % n ];
				const int steps = std::max( 1, static_cast< int >( std::ceil( distance( a, b ) / kColourSampleSpacing ) ) );
				for( int i = 0; i < steps; ++i )
				{
					const float t = static_cast< float >( i ) / static_cast< float >( steps );
					colourAt( a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t, acc );
					++count;
				}
			}
			if( count > 0 )
				for( int c = 0; c < 3; ++c )
					source[ c ] = acc[ c ] / static_cast< float >( count );
		}

		stroke.pen = NearestPen( palette, pens, source );
		PenColour( palette, stroke.pen, source, stroke.ink );
		out.push_back( std::move( stroke ) );
	}
}

//---------------------------------------------------------------------------
void OrderStrokes( std::vector< Stroke >& strokes, Vec2 cursor, int currentPen, bool optimise, bool byPen )
{
	if( !optimise || strokes.size() < 2 )
		return;

	//The group a stroke sorts into: its pen, or -- the negative control --
	//one group for everything, so the tour interleaves pens and a job of four
	//pens changes pen nearly every stroke.
	auto group = [ byPen ]( const Stroke& s ) { return byPen ? s.pen : 0; };

	//The pens in use, the current one first, then ascending: the carriage
	//finishes what it is holding before it goes to the carousel.
	std::vector< int > pens;
	for( const Stroke& s : strokes )
		if( std::find( pens.begin(), pens.end(), group( s ) ) == pens.end() )
			pens.push_back( group( s ) );
	std::sort( pens.begin(), pens.end() );
	if( const auto it = std::find( pens.begin(), pens.end(), currentPen ); it != pens.end() )
		std::rotate( pens.begin(), it, it + 1 );

	std::vector< Stroke > ordered;
	ordered.reserve( strokes.size() );
	std::vector< bool > taken( strokes.size(), false );

	for( int pen : pens )
	{
		//A nearest-neighbour tour within the pen, galvo's OrderContours: an
		//open stroke may be drawn backwards and a closed one may start at
		//whichever vertex is nearest.
		while( true )
		{
			std::size_t best   = strokes.size();
			std::size_t bestAt = 0;
			float bestCost     = 1e30f;
			bool bestReverse   = false;

			for( std::size_t i = 0; i < strokes.size(); ++i )
			{
				if( taken[ i ] || group( strokes[ i ] ) != pen )
					continue;
				const Stroke& s = strokes[ i ];
				if( s.closed )
				{
					for( std::size_t k = 0; k < s.points.size(); ++k )
					{
						const float d = distanceSquared( s.points[ k ], cursor );
						if( d < bestCost )
						{
							bestCost    = d;
							best        = i;
							bestAt      = k;
							bestReverse = false;
						}
					}
				}
				else
				{
					const float df = distanceSquared( s.points.front(), cursor );
					const float db = distanceSquared( s.points.back(), cursor );
					if( df < bestCost )
					{
						bestCost    = df;
						best        = i;
						bestAt      = 0;
						bestReverse = false;
					}
					if( db < bestCost )
					{
						bestCost    = db;
						best        = i;
						bestAt      = 0;
						bestReverse = true;
					}
				}
			}
			if( best >= strokes.size() )
				break;

			taken[ best ] = true;
			Stroke s      = strokes[ best ];
			if( s.closed && bestAt != 0 )
				std::rotate( s.points.begin(), s.points.begin() + static_cast< std::ptrdiff_t >( bestAt ), s.points.end() );
			if( bestReverse )
				std::reverse( s.points.begin(), s.points.end() );
			cursor = s.closed ? s.points.front() : s.points.back();
			ordered.push_back( std::move( s ) );
		}
	}

	strokes.swap( ordered );
}

//---------------------------------------------------------------------------
double RestToRestSeconds( double length, double maxSpeed, double acceleration )
{
	if( length <= 0.0 )
		return 0.0;
	const double cruiseDistance = maxSpeed * maxSpeed / acceleration;//v^2/2a to get there and the same back
	if( length > cruiseDistance )
		return length / maxSpeed + maxSpeed / acceleration;
	return 2.0 * std::sqrt( length / acceleration );
}

int PlanBlocks( const std::vector< Stroke >& strokes, const MachineParams& params, Vec2 cursor, int currentPen,
                std::vector< Block >& out )
{
	out.clear();
	int changes = 0;
	Vec2 pos    = cursor;
	int pen     = currentPen;

	const double cornerRad = std::max( params.cornerAngleDeg, 0.1 ) * kPi / 180.0;

	for( std::size_t index = 0; index < strokes.size(); ++index )
	{
		const Stroke& stroke = strokes[ index ];
		const std::size_t n  = stroke.points.size();
		if( n < 2 )
			continue;

		//A pen change is a trip to the carousel: travel there, wait, travel
		//on. The carriage starts a session holding nothing, so the first
		//stroke of a fresh machine always pays one.
		if( stroke.pen != pen )
		{
			travel( out, pos, params.carousel, params, pen, stroke.ink );
			out.push_back( dwell( Block::Kind::PenChange, params.carousel, params.penChange, stroke.pen, stroke.ink, -1 ) );
			pos = params.carousel;
			pen = stroke.pen;
			++changes;
		}

		travel( out, pos, stroke.points[ 0 ], params, pen, stroke.ink );
		out.push_back( dwell( Block::Kind::Settle, stroke.points[ 0 ], params.settle, pen, stroke.ink, static_cast< int >( index ) ) );

		//The segments, with their junction speeds. A junction is the cruise
		//speed when the turn is gentle and zero when it is a corner; the two
		//ends are zero because the pen starts from its settle and stops for
		//the next one.
		const std::size_t segments = stroke.closed ? n : n - 1;
		std::vector< double > length( segments ), junction( segments + 1, 0.0 );
		for( std::size_t s = 0; s < segments; ++s )
			length[ s ] = distance( stroke.points[ s ], stroke.points[ ( s + 1 ) % n ] );

		for( std::size_t j = 1; j < segments; ++j )
		{
			const Vec2& prev = stroke.points[ j - 1 ];
			const Vec2& at   = stroke.points[ j ];
			const Vec2& next = stroke.points[ ( j + 1 ) % n ];
			const double ix = at.x - prev.x, iy = at.y - prev.y;
			const double ox = next.x - at.x, oy = next.y - at.y;
			const double il = std::sqrt( ix * ix + iy * iy ), ol = std::sqrt( ox * ox + oy * oy );
			if( il <= 1e-9 || ol <= 1e-9 )
				continue;
			//The junction speed falls with the turn: the cruise speed straight
			//on, nothing at Corner Angle or sharper. A motion controller
			//limits a junction by the sideways jerk it would take, which is
			//monotone in the turn; this is that curve's simplest shape, and
			//it is what puts more ink on a tighter bend.
			const double cosTurn = std::clamp( ( ix * ox + iy * oy ) / ( il * ol ), -1.0, 1.0 );
			const double turn    = std::acos( cosTurn );
			junction[ j ]        = params.maxSpeed * std::max( 0.0, 1.0 - turn / cornerRad );
		}

		//Reachability. Backward: no junction may be faster than the next one
		//can be reached from under the deceleration limit. Forward: none may be
		//faster than the previous one lets it accelerate to. After both, every
		//segment's entry and exit are consistent with its length.
		const double a2 = 2.0 * std::max( params.acceleration, 1e-9 );
		for( std::size_t s = segments; s-- > 0; )
			junction[ s ] = std::min( junction[ s ], std::sqrt( junction[ s + 1 ] * junction[ s + 1 ] + a2 * length[ s ] ) );
		for( std::size_t s = 0; s < segments; ++s )
			junction[ s + 1 ] = std::min( junction[ s + 1 ], std::sqrt( junction[ s ] * junction[ s ] + a2 * length[ s ] ) );

		for( std::size_t s = 0; s < segments; ++s )
		{
			if( length[ s ] <= 1e-9 )
				continue;
			Block block;
			block.kind   = Block::Kind::Draw;
			block.a      = stroke.points[ s ];
			block.b      = stroke.points[ ( s + 1 ) % n ];
			block.length = length[ s ];
			block.vIn    = junction[ s ];
			block.vOut   = junction[ s + 1 ];
			block.pen    = pen;
			block.stroke = static_cast< int >( index );
			for( int c = 0; c < 3; ++c )
				block.ink[ c ] = stroke.ink[ c ];
			profile( block, params.maxSpeed, params.acceleration );
			out.push_back( block );
		}

		const Vec2 end = stroke.closed ? stroke.points[ 0 ] : stroke.points[ n - 1 ];
		out.push_back( dwell( Block::Kind::Settle, end, params.settle, pen, stroke.ink, static_cast< int >( index ) ) );
		pos = end;
	}

	return changes;
}

//---------------------------------------------------------------------------
namespace
{
double distanceAlong( const Block& block, double t )
{
	t = std::clamp( t, 0.0, block.Duration() );
	double s;
	if( t < block.t1 )
		s = block.vIn * t + 0.5 * block.accel * t * t;
	else if( t < block.t1 + block.t2 )
		s = ( block.vPeak * block.vPeak - block.vIn * block.vIn ) / ( 2.0 * block.accel ) + block.vPeak * ( t - block.t1 );
	else
	{
		const double tau = t - block.t1 - block.t2;
		s = block.length - ( ( block.vPeak * block.vPeak - block.vOut * block.vOut ) / ( 2.0 * block.accel ) )
		    + block.vPeak * tau - 0.5 * block.accel * tau * tau;
	}
	return std::clamp( s, 0.0, block.length );
}
} // namespace

Vec2 PositionInBlock( const Block& block, double t )
{
	if( block.length <= 0.0 )
		return block.a;
	const double s = distanceAlong( block, t ) / block.length;
	return Vec2{ static_cast< float >( block.a.x + ( block.b.x - block.a.x ) * s ),
	             static_cast< float >( block.a.y + ( block.b.y - block.a.y ) * s ) };
}

double SpeedInBlock( const Block& block, double t )
{
	t = std::clamp( t, 0.0, block.Duration() );
	if( block.length <= 0.0 )
		return 0.0;
	if( t < block.t1 )
		return block.vIn + block.accel * t;
	if( t < block.t1 + block.t2 )
		return block.vPeak;
	return std::max( 0.0, block.vPeak - block.accel * ( t - block.t1 - block.t2 ) );
}

} // namespace plotter
