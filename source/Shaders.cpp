#include "Shaders.h"

namespace plotter
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. MaxUV is folded in once, in the
	//copy pass; every pass after it works on a texture we allocated, where the
	//picture really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy. galvo's.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and on a logo that
	//shows up as a false edge down the side of the frame -- which this plugin
	//would then dutifully draw.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. The mip chain built on this texture
	//is a box filter, and averaging premultiplied samples is the correct
	//filter; averaging straight colour smears the colour of transparent pixels
	//into the picture.
	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: edge, at the trace resolution, plus the colour under it.
//---------------------------------------------------------------------------
const char* const kEdgeShader = R"(#version 410 core

uniform sampler2D CopyTexture;
uniform vec2 Step;        //one Sobel tap, in picture space: a trace texel times 2^Detail
uniform float Lod;        //mip level to read: log2( picture / trace ) + Detail

in vec2 uv;
out vec4 fragColor;

//Luma or alpha, tinsel's fourth mode and galvo's default. The alpha sets a
//floor so a dark logo on transparency still has a boundary; the luma adds the
//detail inside the shape. NOT max( luma * a, a ), which is identically 1.0
//for every opaque pixel and found no edges on any clip without alpha.
float channel( vec2 at )
{
	vec4 c = textureLod( CopyTexture, at, Lod );
	vec3 straight = c.a > 0.0031 ? c.rgb / c.a : c.rgb;
	float luma = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
	return c.a * ( 0.35 + 0.65 * luma );
}

void main()
{
	//Sobel. Two 3x3 convolutions; the magnitude of the pair is the gradient.
	float tl = channel( uv + vec2( -Step.x,  Step.y ) );
	float tc = channel( uv + vec2(     0.0,  Step.y ) );
	float tr = channel( uv + vec2(  Step.x,  Step.y ) );
	float ml = channel( uv + vec2( -Step.x,     0.0 ) );
	float mr = channel( uv + vec2(  Step.x,     0.0 ) );
	float bl = channel( uv + vec2( -Step.x, -Step.y ) );
	float bc = channel( uv + vec2(     0.0, -Step.y ) );
	float br = channel( uv + vec2(  Step.x, -Step.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );

	//Divide by four, the sum of one side of the kernel, so a clean black-to-
	//white step gives exactly 1.0 and Threshold means the same on any footage.
	float edge = length( vec2( gx, gy ) ) * 0.25;

	//The colour of the region this edge runs through: the clip read BLURRED,
	//at the level the Sobel reads at, un-premultiplied, with a transparent
	//pixel taken as white paper rather than black ink.
	vec4 c = textureLod( CopyTexture, uv, Lod );
	vec3 colour = c.a > 0.05 ? c.rgb / c.a : vec3( 1.0 );

	fragColor = vec4( edge, clamp( colour, 0.0, 1.0 ) );
}
)";

//---------------------------------------------------------------------------
// Pass 3: stabilise.
//---------------------------------------------------------------------------
const char* const kStabiliseShader = R"(#version 410 core

uniform sampler2D EdgeTexture;
uniform sampler2D HistoryTexture; //the previous frame's output of this pass
uniform float Attack;             //0..1 blend towards a *stronger* edge
uniform float Release;            //0..1 blend towards a *weaker* edge
uniform float Reset;              //1 to ignore history entirely

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 edge = texture( EdgeTexture, uv );
	float current = edge.r;
	float history = texture( HistoryTexture, uv ).r;

	//Asymmetric on purpose (tinsel, galvo). A symmetric IIR is a low-pass, and
	//a low-pass on an edge signal trades flicker for lag. What actually goes
	//wrong on footage is that edges *drop out* for a frame, so an edge that
	//appears is believed at once and one that vanishes is given a few frames
	//to return.
	float blend = current > history ? Attack : Release;
	float stable = mix( history, current, blend ) * ( 1.0 - Reset ) + current * Reset;

	//r: the stabilised gradient, thresholded on the CPU after the readback.
	//gba: the colour, passed through unfiltered.
	fragColor = vec4( stable, edge.gba );
}
)";

//---------------------------------------------------------------------------
// Pass 4: the ink. One instanced quad per machine interval.
//
// galvo's beam renderer, with the quantum renamed: each interval deposits
// InkRate * dt of absorbance (times TravelRate instead when the pen is up),
// spread over however far the carriage moved in it by the exact convolution
// of a uniform segment with a Gaussian nib. That is why the line is heavy
// where the pen is slow and why a pen that stops leaves a blot, with no 1/v
// computed anywhere and no special case for a carriage at rest.
//
// Shared constants first: `Extent` is the half-width of the quad around each
// segment in units of the nib's sigma. 4.5 sigma keeps 99.9993% of a
// Gaussian, and the fragment stage subtracts the profile's value at exactly
// that distance so what is thrown away is thrown away smoothly. Two stages
// have to agree about the number, which is why it is one string.
//---------------------------------------------------------------------------
const char* const kInkConstants = R"(
const float Extent     = 4.5;
const float InvSqrt2Pi = 0.39894228040143268;
)";

// The two attributes per sample are one buffer bound twice, the second offset
// by one Sample, so instance i sees sample i and sample i+1 with nothing
// duplicated. That is also why the draw asks for n-1 instances: n would read
// one Sample past the end of the buffer.
//
// `sampleA`/`sampleB`, not `sample`: `sample` is a GLSL reserved word.
const char* const kInkVertexBody = R"(
layout( location = 0 ) in vec4 sampleA;  //x, y paper units; dt seconds; pen down
layout( location = 1 ) in vec4 inkA;     //r, g, b of the ink
layout( location = 2 ) in vec4 sampleB;
layout( location = 3 ) in vec4 inkB;

uniform float InkRate;    //absorbance x paper-height^2 per second of pen-down time
uniform float TravelRate; //the same for pen-up travel, 0 when Show Travel is off
uniform float NibSigma;   //paper units: 1 = the paper height
uniform float Aspect;     //paper width / height; x runs 0..Aspect

flat out float segLength;
flat out float segSigma;
flat out float segEnergy;
flat out vec4 segWeights; //rgb: absorbance per channel per unit energy; a: travel
out vec2 segUV;           //x along the segment from its centre, y across

//Not isnan()/isinf(): those are the first thing a fast-math compiler folds to
//constant false. A comparison chain has no intrinsic to fold, and one NaN in
//the paper is there for the life of the sheet.
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

void main()
{
	vec2 a = sampleA.xy;
	vec2 b = sampleB.xy;

	//The whole ink model, in one line: a quantum per interval, not a density
	//per pixel. The fragment stage spreads it over however far the carriage
	//moved, so nothing divides by a speed, and a pen that lingers puts more
	//ink in one place because it spent more intervals there.
	float down   = sampleA.w;
	float energy = max( sampleA.z, 0.0 ) * mix( TravelRate, InkRate, down );
	float sigma  = NibSigma;

	//Ink is subtractive: a red pen absorbs green and blue and leaves red.
	//Travel goes into alpha, which the composite draws as a faint line.
	vec3 absorb = ( vec3( 1.0 ) - clamp( inkA.rgb, 0.0, 1.0 ) ) * down;
	vec4 weights = vec4( absorb, 1.0 - down );

	vec2 delta = b - a;
	float span = length( delta );
	vec2 dir   = span > 1e-9 ? delta / span : vec2( 1.0, 0.0 );

	//A floor of a twentieth of a nib, so the box is never zero-area. The
	//fragment stage does not divide by it below a quarter of a sigma anyway.
	float len = max( span, 0.05 * sigma );

	bool ok = usable( a.x ) && usable( a.y ) && usable( b.x ) && usable( b.y )
	       && usable( energy ) && energy > 0.0 && sigma > 0.0;

	if( !ok )
	{
		//A degenerate quad off the frustum: no area, rasterises nothing.
		//Returning without writing gl_Position would be undefined.
		segLength   = 0.0;
		segSigma    = 1.0;
		segEnergy   = 0.0;
		segWeights  = vec4( 0.0 );
		segUV       = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	//An oriented box around the capsule, padded by Extent sigma on all sides.
	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;

	//Corners from the vertex index, as a triangle strip.
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = a + dir * ( 0.5 * len );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength  = len;
	segSigma   = sigma;
	segEnergy  = energy;
	segWeights = weights;
	segUV      = vec2( sx * halfAlong, sy * halfAcross );

	//Paper units are isotropic -- one unit is the paper height on both axes --
	//so the nib is round. The divide is the only place the frame's shape
	//enters the ink pass at all.
	gl_Position = vec4( pos.x / Aspect * 2.0 - 1.0, pos.y * 2.0 - 1.0, 0.0, 1.0 );
}
)";

// The closed form, evaluated per fragment:
//
//     f(u,v) = (E/L) * G_sigma(v) * [ Phi((u + L/2)/sigma) - Phi((u - L/2)/sigma) ]
//
// with u measured from the segment's centre. It integrates to E over the
// plane for any L and any sigma, which is what makes the ink on the sheet
// equal to InkRate times the pen-down time (`--trapezoid` measures it), and
// it tends to a finite Gaussian dot as L goes to zero, which is the blot.
const char* const kInkFragmentBody = R"(
flat in float segLength;
flat in float segSigma;
flat in float segEnergy;
flat in vec4 segWeights;
in vec2 segUV;

out vec4 fragColor;

//The standard normal CDF. GLSL has no erf, so the usual tanh approximation,
//good to about 3e-4. The clamp is not tidiness: a driver computing tanh as
//(e^2x - 1)/(e^2x + 1) overflows around x = 44 and yields inf/inf = NaN.
float ncdf( float x )
{
	float t = clamp( 0.7978845608 * ( x + 0.044715 * x * x * x ), -8.0, 8.0 );
	return 0.5 * ( 1.0 + tanh( t ) );
}

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across: a normalised Gaussian, minus its value at the quad's own edge, so
	//the cut at Extent sigma is a smooth zero and not a step.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across = max( across - pedestal, 0.0 );

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, taken explicitly. A difference of two nearly equal
		//CDFs each carrying 3e-4 of error is several percent wrong by the
		//time L is a quarter of a sigma -- and a short segment is exactly the
		//blot this plugin is about. The limit costs one exp and is exact.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv );
	}
	else
	{
		float halfLen = 0.5 * segLength;//`half` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	float deposit = segEnergy * across * along;
	fragColor = segWeights * deposit;
}
)";

//---------------------------------------------------------------------------
// Pass 5: resample. The old paper into the new one on a resize -- the
// photofinish trap: a buffer that holds state across frames must survive a
// reallocation. Bilinear, from a texture that has been set to filter.
//---------------------------------------------------------------------------
const char* const kResampleShader = R"(#version 410 core

uniform sampler2D PaperTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( PaperTexture, uv );
}
)";

//---------------------------------------------------------------------------
// Pass 6: composite.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D CopyTexture;   //the clip, premultiplied
uniform sampler2D PaperTexture;  //rgb absorbance, a travel density

uniform float PaperMode;   //0 white, 1 cream, 2 ghost, 3 clip, 4 alpha
uniform vec3 PaperColour;
uniform float Ghost;       //how much of the clip shows through Ghost paper
uniform float MixAmount;
uniform float Aspect;

uniform float ShowCarriage;
uniform vec2 CarriagePos;  //paper units
uniform float CarriageRadius;
uniform float NibSigma;
uniform vec3 CarriageInk;
uniform float PenDown;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 source = texture( CopyTexture, uv );
	vec4 ink = texture( PaperTexture, uv );

	//Beer-Lambert: the paper's light through the ink. Every channel's
	//absorbance is what the pens laid down; a black pen absorbs all three.
	vec3 transmit = exp( -max( ink.rgb, vec3( 0.0 ) ) );
	//Travel is a faint grey pencil line.
	float travel = 0.35 * ( 1.0 - exp( -max( ink.a, 0.0 ) ) );

	int mode = int( PaperMode + 0.5 );
	vec3 straight = source.a > 0.0031 ? source.rgb / source.a : vec3( 0.0 );

	vec4 result;
	if( mode == 4 )
	{
		//Ink over nothing, premultiplied, for the layer below. The opacity is
		//what the darkest channel absorbed; the colour is what is left.
		float alpha = 1.0 - min( transmit.r, min( transmit.g, transmit.b ) );
		vec3 rgb = transmit - vec3( 1.0 - alpha );
		float tw = travel * ( 1.0 - alpha );
		result = vec4( rgb + vec3( 0.4 ) * tw, alpha + tw );
	}
	else
	{
		//The sheet is OPAQUE, whatever the clip's alpha: paper is not
		//transparent where the picture is. Resolume's own demo clips carry
		//alpha, and an effect that passes it through vanishes on them.
		vec3 paper = PaperColour;
		if( mode == 2 )
			paper = mix( PaperColour, straight, Ghost * source.a );
		else if( mode == 3 )
			paper = source.rgb + ( 1.0 - source.a ) * PaperColour;
		vec3 rgb = paper * transmit * ( 1.0 - travel );
		result = vec4( rgb, 1.0 );
	}

	if( ShowCarriage > 0.5 )
	{
		//The carriage: a ring, with the nib as a dot in its ink. Drawn in
		//paper units so it is round at any raster.
		vec2 p = vec2( uv.x * Aspect, uv.y );
		float d = length( p - CarriagePos );
		float px = 1.0 / float( textureSize( PaperTexture, 0 ).y );
		float ring = 1.0 - smoothstep( 0.0, 1.5 * px, abs( d - CarriageRadius ) - 0.75 * px );
		float nib = 1.0 - smoothstep( 2.0 * NibSigma, 2.0 * NibSigma + px, d );
		vec3 ringColour = mix( vec3( 0.55 ), CarriageInk, PenDown );
		float ringAlpha = ring * ( mode == 4 ? 1.0 : 0.85 );
		result.rgb = mix( result.rgb, ringColour * ( mode == 4 ? 1.0 : 1.0 ), ringAlpha );
		result.rgb = mix( result.rgb, CarriageInk, nib );
		if( mode == 4 )
			result.a = max( result.a, max( ringAlpha, nib ) );
	}

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly.
//---------------------------------------------------------------------------
std::string InkVertexSource()
{
	return std::string( "#version 410 core\n" ) + kInkConstants + kInkVertexBody;
}

std::string InkFragmentSource()
{
	return std::string( "#version 410 core\n" ) + kInkConstants + kInkFragmentBody;
}

} // namespace plotter
