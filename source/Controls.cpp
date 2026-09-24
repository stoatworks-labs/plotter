#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace plotter
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

//Eight pens each. Technical is the drawing-office set: black first, because a
//plotter with one pen has a black one. Neon is the marker set. Black is all
//black so that `Pens` still sorts the job without changing the colour.
const float kTechnical[ kPensPerPalette ][ 3 ] = {
	{ 0.06f, 0.06f, 0.07f }, //black
	{ 0.75f, 0.13f, 0.10f }, //red
	{ 0.12f, 0.31f, 0.75f }, //blue
	{ 0.12f, 0.55f, 0.23f }, //green
	{ 0.88f, 0.48f, 0.06f }, //orange
	{ 0.48f, 0.25f, 0.63f }, //violet
	{ 0.42f, 0.27f, 0.14f }, //brown
	{ 0.48f, 0.48f, 0.48f }, //grey
};

const float kNeon[ kPensPerPalette ][ 3 ] = {
	{ 0.95f, 0.10f, 0.60f }, //magenta
	{ 0.05f, 0.80f, 0.90f }, //cyan
	{ 0.98f, 0.85f, 0.05f }, //yellow
	{ 0.50f, 0.95f, 0.10f }, //lime
	{ 1.00f, 0.45f, 0.05f }, //orange
	{ 0.95f, 0.40f, 0.60f }, //pink
	{ 0.55f, 0.20f, 0.95f }, //violet
	{ 0.95f, 0.15f, 0.15f }, //red
};

const float kBlack[ 3 ] = { 0.06f, 0.06f, 0.07f };

void rgbToHsv( float r, float g, float b, float& h, float& s, float& v )
{
	const float mx = std::max( r, std::max( g, b ) );
	const float mn = std::min( r, std::min( g, b ) );
	v              = mx;
	const float d  = mx - mn;
	s              = mx > 1e-6f ? d / mx : 0.0f;
	if( d < 1e-6f )
	{
		h = 0.0f;
		return;
	}
	if( mx == r )
		h = ( g - b ) / d + ( g < b ? 6.0f : 0.0f );
	else if( mx == g )
		h = ( b - r ) / d + 2.0f;
	else
		h = ( r - g ) / d + 4.0f;
	h /= 6.0f;
}

void hsvToRgb( float h, float s, float v, float& r, float& g, float& b )
{
	h             = h - std::floor( h );
	const float i = std::floor( h * 6.0f );
	const float f = h * 6.0f - i;
	const float p = v * ( 1.0f - s );
	const float q = v * ( 1.0f - s * f );
	const float t = v * ( 1.0f - s * ( 1.0f - f ) );
	switch( static_cast< int >( i ) % 6 )
	{
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
}
} // namespace

//--- Machine ----------------------------------------------------------------

float MaxSpeedFromParam( float value )
{
	return geometric( 0.05f, 2.0f, value );
}

float AccelerationFromParam( float value )
{
	return geometric( 0.1f, 20.0f, value );
}

float PenSettleFromParam( float value )
{
	return lerp( 0.0f, 0.5f, value );
}

float StepSizeFromParam( float value )
{
	if( value <= 0.0f )
		return 0.0f;
	return geometric( 0.0005f, 0.025f, value );
}

float CornerAngleFromParam( float value )
{
	return lerp( 5.0f, 90.0f, value );
}

//--- Pens -------------------------------------------------------------------

float PenWidthFromParam( float value )
{
	return geometric( 0.0012f, 0.012f, value );
}

float FlowFromParam( float value )
{
	return geometric( 0.25f, 8.0f, value );
}

double InkRate( float flow, float penSigma )
{
	return static_cast< double >( flow ) * kReferenceSpeed * penSigma * std::sqrt( 2.0 * kPi );
}

void PenColour( Palette palette, int index, const float source[ 3 ], float out[ 3 ] )
{
	index = std::clamp( index, 0, kPensPerPalette - 1 );
	switch( palette )
	{
	case Palette::Technical:
		for( int c = 0; c < 3; ++c )
			out[ c ] = kTechnical[ index ][ c ];
		return;
	case Palette::Neon:
		for( int c = 0; c < 3; ++c )
			out[ c ] = kNeon[ index ][ c ];
		return;
	case Palette::Source:
	{
		//The source colour with its value capped, so a bright edge on a dark
		//clip -- the commonest edge in footage -- is a grey line rather than
		//white ink on white paper.
		float h = 0.0f, s = 0.0f, v = 0.0f;
		rgbToHsv( clamp01( source[ 0 ] ), clamp01( source[ 1 ] ), clamp01( source[ 2 ] ), h, s, v );
		hsvToRgb( h, s, std::min( v, kSourceInkCap ), out[ 0 ], out[ 1 ], out[ 2 ] );
		return;
	}
	case Palette::Black:
	default:
		for( int c = 0; c < 3; ++c )
			out[ c ] = kBlack[ c ];
		return;
	}
}

int NearestPen( Palette palette, int pens, const float source[ 3 ] )
{
	pens = std::clamp( pens, 1, kPensPerPalette );
	if( palette == Palette::Black )
		return 0;

	//A grey is the black pen, whatever RGB distance says: white is far from
	//every pen and nearest, absurdly, the blue one. Most edges in footage are
	//low in saturation, and they belong to the pen a draughtsman would pick.
	if( palette != Palette::Neon )
	{
		const float mx = std::max( source[ 0 ], std::max( source[ 1 ], source[ 2 ] ) );
		const float mn = std::min( source[ 0 ], std::min( source[ 1 ], source[ 2 ] ) );
		if( mx < 0.25f || ( mx - mn ) < kGreyChroma * mx )
			return 0;
	}

	const float( *table )[ 3 ] = palette == Palette::Neon ? kNeon : kTechnical;
	int best       = 0;
	float bestCost = 1e30f;
	for( int i = 0; i < pens; ++i )
	{
		float cost = 0.0f;
		for( int c = 0; c < 3; ++c )
		{
			const float d = clamp01( source[ c ] ) - table[ i ][ c ];
			cost += d * d;
		}
		if( cost < bestCost )
		{
			bestCost = cost;
			best     = i;
		}
	}
	return best;
}

//--- Path -------------------------------------------------------------------

float ThresholdFromParam( float value )
{
	return geometric( 0.02f, 1.0f, value );
}

float DetailFromParam( float value )
{
	return lerp( 0.0f, 3.0f, value );
}

//--- Sheet ------------------------------------------------------------------

float AutoSheetFromParam( float value )
{
	if( value <= 0.0f )
		return 0.0f;
	return geometric( 1.0f, 300.0f, value );
}

void PaperColour( Paper paper, float out[ 3 ] )
{
	if( paper == Paper::Cream )
	{
		out[ 0 ] = 0.98f;
		out[ 1 ] = 0.95f;
		out[ 2 ] = 0.88f;
		return;
	}
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 1.0f;
}

} // namespace plotter
