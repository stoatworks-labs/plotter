/**
 * Plotter — browser demo.
 *
 * A pen plotter drawing the clip. The one idea, from `AGENTS.md`: **galvo is a
 * laser, a fast mirror redrawing the whole path every frame; a pen plotter is
 * the opposite machine. It is slow, and the ink stays.** The pen takes the
 * frame's path when it asks for work and draws it at its own pace under a speed
 * and an acceleration limit, so fast content never gets finished and the sheet
 * is a collage of the moments each stroke was drawn. Ink is deposited per unit
 * of TIME, spread over the distance covered, so the line is heavy where the pen
 * is slow — the ends of every stroke, every corner — and every pen-down is a
 * blot. Two stepper motors on a grid draw staircases. A change of pen is a trip
 * to the carousel. None of it is drawn; it falls out of the machine.
 *
 * Like galvo, this plugin is **not a shader**. It is a GPU edge detector, then
 * a synchronous readback, then a CPU tracer / planner / machine, then a GPU ink
 * renderer. The CPU middle has to exist here or the page has nothing to show at
 * all, so the whole of it is ported — and the two halves are not equally
 * faithful:
 *
 *   The shaders are the plugin's. `VERTEX`, `COPY`, `EDGE`, `STABILISE`,
 *   `INK_CONSTANTS`, `INK_VERTEX_BODY`, `INK_FRAGMENT_BODY`, `RESAMPLE` and
 *   `COMPOSITE` below are `kVertexShader` … `kCompositeShader` from
 *   `source/Shaders.cpp`, copied across unedited, and the ink pass is assembled
 *   from its pieces the way `InkVertexSource()`/`InkFragmentSource()` assemble
 *   it. `demo/tools/check_shaders.py` compares every piece character for
 *   character and `tools/verify.sh` runs it, because two copies of a shader is
 *   exactly the arrangement that drifts.
 *
 *   The CPU half is a PORT — of `Controls.cpp`, `Tracer.cpp` (galvo's, copied
 *   verbatim into the plugin), `Planner.cpp`, `Machine.cpp`, the CPU side of
 *   `render/Ink.cpp` and the frame sequence in `Plotter::ProcessOpenGL`, in
 *   that order, function for function. **Nothing checks a port but a reader.**
 *   `pltest --plan`, `--trapezoid`, `--ink`, `--budget`, `--steps`, `--pens`,
 *   `--persist` and `--trace` in the repository check the C++ originals and
 *   have no idea this page exists. Deliberately NOT ported: the `Perturb` test
 *   hooks (always zero in the plugin), `SetJobForTest` and `ReadPaperForTest`,
 *   because none of them is part of what the plugin does in a host.
 *
 * ------------------------------------------------------------ the clock
 *
 * The machine runs on the host clock. The plugin normalises Resolume's clock to
 * seconds, takes the frame delta clamped into 1/240 .. 1/4 s, and advances the
 * machine by exactly that — so the drawing accumulates by real elapsed time.
 * The page does the same with the kit's clock: `time` is seconds since the page
 * started, paused by Pause, stepped by Step, sent to zero by Restart; the delta
 * between two renders is what the machine advances by, through the same clamp.
 * The unit vote the plugin runs against Resolume's millisecond clock never runs
 * here, because the page declares seconds, as the repository's harness does.
 * Restart restarts the CLIP's clock, not the sheet: the sheet is torn off by
 * New Sheet (the plugin's event, a button here) or by Auto Sheet.
 *
 * ------------------------------------------------------------ the paper
 *
 * The plugin's paper is RGBA32F, and the reason is in AGENTS.md: a half-float
 * paper loses ink on every add, and on a buffer that only ever accumulates that
 * bias is systematic (0.8% low on a long stroke, 2.9% on a short one). WebGL2
 * renders to a float target only with EXT_color_buffer_float, and blends into a
 * 32-bit one only with EXT_float_blend (linear filtering of it, which the
 * resample on a resize needs, wants OES_texture_float_linear). So:
 *
 *   - with all three, the paper is RGBA32F, as the plugin's is;
 *   - with EXT_color_buffer_float but without the other two, the paper is
 *     RGBA16F, and the line under the canvas says so — that is the half-float
 *     bias the plugin measured and rejected, and a reader should know the page
 *     is carrying it;
 *   - without EXT_color_buffer_float the page refuses to start and says why,
 *     rather than accumulating ink into eight bits and drawing a plausible wrong
 *     picture.
 *
 * ------------------------------------------------------------ what is missing
 *
 * **Nothing audio.** Plotter has no audio path, so there is no caveat to make.
 *
 * **The stabilise buffers are RGBA8, not RGBA16F.** The plugin reads its
 * gradient-and-colour buffer back as RGBA bytes; WebGL2 will not read a float
 * framebuffer as bytes, so the two ping-ponged stabilise buffers are RGBA8 here.
 * The byte the tracer thresholds and the colour bytes the strokes take are the
 * same bytes either way; what is quantised is the stabilised gradient the
 * temporal filter feeds back to itself.
 *
 * **Pens is a dropdown.** It is FF_TYPE_INTEGER in the plugin, 1..8, and the kit
 * has no integer control, so it is a dropdown of its eight values.
 *
 * **New Sheet is a button.** It is FF_TYPE_EVENT in the plugin — a press is
 * remembered until the next frame — and the kit has no event control, so it is
 * a button in the inspector at the plugin's own position in the Sheet group.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * **JavaScript numbers are doubles.** The plugin's `Vec2` is float and its
 * profile arithmetic is double; here everything is double except the samples
 * uploaded to the GPU, which are Float32 as the plugin's `Sample` is.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core — so a pixel here
 * is not evidence about a pixel there.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The one backtick inside a comment is escaped, because a template literal has
// nowhere else to go; check_shaders.py decodes that one escape before comparing
// and rejects any other backslash, so the escape cannot hide a difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COPY = `#version 410 core

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
`;

const EDGE = `#version 410 core

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
`;

const STABILISE = `#version 410 core

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
`;

const INK_CONSTANTS = `
const float Extent     = 4.5;
const float InvSqrt2Pi = 0.39894228040143268;
`;

const INK_VERTEX_BODY = `
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
`;

const INK_FRAGMENT_BODY = `
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
		float halfLen = 0.5 * segLength;//\`half\` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	float deposit = segEnergy * across * along;
	fragColor = segWeights * deposit;
}
`;

const RESAMPLE = `#version 410 core

uniform sampler2D PaperTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( PaperTexture, uv );
}
`;

const COMPOSITE = `#version 410 core

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
`;

// The two ink stages are assembled around one shared constants string, because
// the vertex stage sizing the quad and the fragment stage subtracting the
// Gaussian's pedestal have to agree about `Extent`. Same assembly as
// InkVertexSource() / InkFragmentSource() in Shaders.cpp.
const INK_VERTEX = `#version 410 core\n${INK_CONSTANTS}${INK_VERTEX_BODY}`;
const INK_FRAGMENT = `#version 410 core\n${INK_CONSTANTS}${INK_FRAGMENT_BODY}`;

//===========================================================================
// A port of source/Controls.cpp — what a 0..1 slider means, in paper heights
// and seconds; the palettes; the nearest pen; the constants.
//
// Every FF_TYPE_STANDARD parameter is a plain 0..1 float even where it stands
// for a speed, a time or a length, because SetParamInfo clamps a STANDARD
// default into 0..1 before SetParamRange can widen it. These functions exist in
// the plugin so the harness and the effect cannot disagree about what a slider
// position means — this is a third copy, and only a reader is checking it.
//
// Units. Lengths are in paper HEIGHTS: x runs 0..aspect, y runs 0..1, y up.
// Speeds are heights per second, accelerations heights per second squared,
// times in seconds. The raster never enters.
//===========================================================================

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

/// C++ std::round / std::lround round half AWAY from zero; Math.round rounds
/// half toward +infinity. Nothing here is negative in practice, but a port is a
/// port.
const roundHalfAway = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

//--- Machine ----------------------------------------------------------------

const maxSpeedFromParam = (v) => geometric(0.05, 2.0, v);
const accelerationFromParam = (v) => geometric(0.1, 20.0, v);
const penSettleFromParam = (v) => lerp(0.0, 0.5, v);
const stepSizeFromParam = (v) => (v <= 0.0 ? 0.0 : geometric(0.0005, 0.025, v));
const cornerAngleFromParam = (v) => lerp(5.0, 90.0, v);

//--- Pens -------------------------------------------------------------------

const penWidthFromParam = (v) => geometric(0.0012, 0.012, v);
const flowFromParam = (v) => geometric(0.25, 8.0, v);

/// The speed at which Flow is the line's absorbance, in paper heights per
/// second. Not the current Max Speed, so that Max Speed changes the ink weight
/// the way it does on a real machine.
const kReferenceSpeed = 0.35;

/// The pen deposits ink per unit TIME: absorbance x paper-height^2 per second,
/// defined so a line drawn at kReferenceSpeed carries the Flow absorbance at
/// its centre whatever the pen width.
const inkRateFor = (flow, penSigma) => flow * kReferenceSpeed * penSigma * Math.sqrt(2.0 * Math.PI);

const PALETTE_TECHNICAL = 0;
const PALETTE_BLACK = 1;
const PALETTE_NEON = 2;
const PALETTE_SOURCE = 3;
const kPensPerPalette = 8;

// Eight pens each. Technical is the drawing-office set: black first, because a
// plotter with one pen has a black one. Neon is the marker set. Black is all
// black so that `Pens` still sorts the job without changing the colour.
const kTechnical = [
  [0.06, 0.06, 0.07], // black
  [0.75, 0.13, 0.10], // red
  [0.12, 0.31, 0.75], // blue
  [0.12, 0.55, 0.23], // green
  [0.88, 0.48, 0.06], // orange
  [0.48, 0.25, 0.63], // violet
  [0.42, 0.27, 0.14], // brown
  [0.48, 0.48, 0.48], // grey
];

const kNeon = [
  [0.95, 0.10, 0.60], // magenta
  [0.05, 0.80, 0.90], // cyan
  [0.98, 0.85, 0.05], // yellow
  [0.50, 0.95, 0.10], // lime
  [1.00, 0.45, 0.05], // orange
  [0.95, 0.40, 0.60], // pink
  [0.55, 0.20, 0.95], // violet
  [0.95, 0.15, 0.15], // red
];

const kBlack = [0.06, 0.06, 0.07];

function rgbToHsv(r, g, b) {
  const mx = Math.max(r, Math.max(g, b));
  const mn = Math.min(r, Math.min(g, b));
  const v = mx;
  const d = mx - mn;
  const s = mx > 1e-6 ? d / mx : 0.0;
  if (d < 1e-6) return [0.0, s, v];
  let h;
  if (mx === r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
  else if (mx === g) h = (b - r) / d + 2.0;
  else h = (r - g) / d + 4.0;
  return [h / 6.0, s, v];
}

function hsvToRgb(h, s, v) {
  h -= Math.floor(h);
  const i = Math.floor(h * 6.0);
  const f = h * 6.0 - i;
  const p = v * (1.0 - s);
  const q = v * (1.0 - s * f);
  const t = v * (1.0 - s * (1.0 - f));
  switch (i % 6) {
    case 0: return [v, t, p];
    case 1: return [q, v, p];
    case 2: return [p, v, t];
    case 3: return [p, q, v];
    case 4: return [t, p, v];
    default: return [v, p, q];
  }
}

/// A grey is drawn with the black pen: a source colour whose chroma is under
/// this fraction of its value.
const kGreyChroma = 0.3;
/// Source ink is capped at this value (HSV V) so a white edge still draws.
const kSourceInkCap = 0.7;

/// The ink colour of pen `index` in `palette`, 0..1 RGB. For Source, the
/// colour the stroke was traced with comes back capped, so a white edge on a
/// black clip is a grey line on white paper rather than nothing.
function penColour(palette, index, source) {
  index = clamp(index, 0, kPensPerPalette - 1);
  switch (palette) {
    case PALETTE_TECHNICAL:
      return kTechnical[index].slice();
    case PALETTE_NEON:
      return kNeon[index].slice();
    case PALETTE_SOURCE: {
      // The source colour with its value capped, so a bright edge on a dark
      // clip -- the commonest edge in footage -- is a grey line rather than
      // white ink on white paper.
      const [h, s, v] = rgbToHsv(clamp01(source[0]), clamp01(source[1]), clamp01(source[2]));
      return hsvToRgb(h, s, Math.min(v, kSourceInkCap));
    }
    case PALETTE_BLACK:
    default:
      return kBlack.slice();
  }
}

/// Which pen of the first `pens` in `palette` is nearest to a source colour.
/// For Source the Technical pens are the sort key.
function nearestPen(palette, pens, source) {
  pens = clamp(pens, 1, kPensPerPalette);
  if (palette === PALETTE_BLACK) return 0;

  // A grey is the black pen, whatever RGB distance says: white is far from
  // every pen and nearest, absurdly, the blue one. Most edges in footage are
  // low in saturation, and they belong to the pen a draughtsman would pick.
  if (palette !== PALETTE_NEON) {
    const mx = Math.max(source[0], Math.max(source[1], source[2]));
    const mn = Math.min(source[0], Math.min(source[1], source[2]));
    if (mx < 0.25 || mx - mn < kGreyChroma * mx) return 0;
  }

  const table = palette === PALETTE_NEON ? kNeon : kTechnical;
  let best = 0;
  let bestCost = 1e30;
  for (let i = 0; i < pens; i += 1) {
    let cost = 0.0;
    for (let c = 0; c < 3; c += 1) {
      const d = clamp01(source[c]) - table[i][c];
      cost += d * d;
    }
    if (cost < bestCost) {
      bestCost = cost;
      best = i;
    }
  }
  return best;
}

//--- Path -------------------------------------------------------------------

const thresholdFromParam = (v) => geometric(0.02, 1.0, v);
const detailFromParam = (v) => lerp(0.0, 3.0, v);

//--- Sheet ------------------------------------------------------------------

const autoSheetFromParam = (v) => (v <= 0.0 ? 0.0 : geometric(1.0, 300.0, v));

const PAPER_CREAM = 1;
const PAPER_ALPHA = 4;

/// The paper's colour for the two plain papers.
const paperColour = (paper) => (paper === PAPER_CREAM ? [0.98, 0.95, 0.88] : [1.0, 1.0, 1.0]);

/// How faintly the clip shows through Ghost paper.
const kGhostLevel = 0.22;

//--- Constants the controls do not reach -------------------------------------

/// The trace resolution, in pixels across. galvo makes this a control; here it
/// is fixed, because a plotter's resolution is its stepper pitch.
const kTraceWidth = 320;
/// Contours shorter than this, in trace pixels, are not worth a pen-down.
const kMinLength = 8.0;
/// Douglas-Peucker tolerance in trace pixels. galvo's default.
const kSimplify = 1.15;
/// The temporal filter's two halves. Asymmetric on purpose.
const kAttack = 0.91;
const kRelease = 0.25;
/// Where the pen carousel is, in paper units: just off the top-left corner.
const kCarouselX = 0.0;
const kCarouselY = 1.04;
/// How long the carriage rests at the carousel while the pen is swapped.
const kPenChangeSeconds = 0.35;
/// Travel (pen up) moves at this multiple of Max Speed.
const kTravelSpeedFactor = 3.0;
/// The faint line Show Travel draws, as a fraction of the pen's ink rate.
const kTravelInk = 0.08;
/// The pen carriage drawn by Show Carriage: its radius in paper heights.
const kCarriageRadius = 0.018;
/// The average colour along a stroke is taken every this many trace pixels.
const kColourSampleSpacing = 2.0;

//===========================================================================
// A port of source/Tracer.cpp — from a binary edge mask to ordered polylines.
// The plugin's copy is galvo's, verbatim (`far` renamed `farIndex` for MSVC),
// and this is galvo's demo port of it with the same rename.
//
// This is the half of the pipeline the GPU cannot do: contour following is
// pointer-chasing, which pixel comes *next*, and FFGL on macOS is GL 4.1 core
// with no compute shaders. So the plugin reads the mask back small and walks it
// on the CPU, and so does this.
//===========================================================================

/// The 8-neighbourhood. The four direct neighbours come first so that the walk
/// prefers a straight step over a diagonal one: on a thinned skeleton a
/// diagonal taken while a direct neighbour was available leaves that neighbour
/// orphaned as a one-pixel contour of its own.
const NEIGHBOUR_X = [1, 0, -1, 0, 1, -1, -1, 1];
const NEIGHBOUR_Y = [0, 1, 0, -1, 1, 1, -1, -1];

/// Zhang-Suen's neighbourhood, in its own order: P2 north, then clockwise.
const ZS_X = [0, 1, 1, 1, 0, -1, -1, -1];
const ZS_Y = [1, 1, 0, -1, -1, -1, 0, 1];

/// Scratch for the eight neighbours inside the thinning loop. See thin().
const neighbourhood = new Int32Array(8);

function distanceToSegment(p, a, b) {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len < 1e-6) return Math.sqrt((p.x - a.x) * (p.x - a.x) + (p.y - a.y) * (p.y - a.y));
  return Math.abs(dy * p.x - dx * p.y + b.x * a.y - b.y * a.x) / len;
}

/// Iterative Douglas-Peucker over an open run [first, last], marking survivors
/// in `keep`.
function simplifyRun(points, first, last, epsilon, keep) {
  keep[first] = 1;
  keep[last] = 1;
  const stack = [first, last];

  while (stack.length > 0) {
    const b = stack.pop();
    const a = stack.pop();
    if (b <= a + 1) continue;

    let worst = 0.0;
    let pick = a;
    for (let i = a + 1; i < b; i += 1) {
      const d = distanceToSegment(points[i], points[a], points[b]);
      if (d > worst) {
        worst = d;
        pick = i;
      }
    }

    if (worst > epsilon) {
      keep[pick] = 1;
      stack.push(a, pick);
      stack.push(pick, b);
    }
  }
}

function polylineLength(points, closed) {
  let length = 0.0;
  for (let i = 1; i < points.length; i += 1) {
    const dx = points[i].x - points[i - 1].x;
    const dy = points[i].y - points[i - 1].y;
    length += Math.sqrt(dx * dx + dy * dy);
  }
  if (closed && points.length > 2) {
    const dx = points[0].x - points[points.length - 1].x;
    const dy = points[0].y - points[points.length - 1].y;
    length += Math.sqrt(dx * dx + dy * dy);
  }
  return length;
}

function simplifyPolyline(points, epsilon, closed) {
  if (points.length < 3) return points.slice();

  const keep = new Uint8Array(points.length);

  if (!closed) {
    simplifyRun(points, 0, points.length - 1, epsilon, keep);
  } else {
    // A closed loop has no endpoints to anchor on, so anchor on the first point
    // and the one farthest from it. Each half is then an ordinary open run, and
    // the two anchors survive whatever the tolerance — which is what keeps a
    // circle from simplifying to nothing.
    let farIndex = 0;
    let farthest = -1.0;
    for (let i = 1; i < points.length; i += 1) {
      const dx = points[i].x - points[0].x;
      const dy = points[i].y - points[0].y;
      const d = dx * dx + dy * dy;
      if (d > farthest) {
        farthest = d;
        farIndex = i;
      }
    }

    simplifyRun(points, 0, farIndex, epsilon, keep);

    // The second half runs from `farIndex` round to the first point again,
    // which is not in the array twice — so it is simplified against a copy with
    // the start appended.
    const tail = points.slice(farIndex);
    tail.push(points[0]);
    const keepTail = new Uint8Array(tail.length);
    simplifyRun(tail, 0, tail.length - 1, epsilon, keepTail);
    for (let i = 0; i + 1 < tail.length; i += 1) if (keepTail[i]) keep[farIndex + i] = 1;
  }

  const out = [];
  for (let i = 0; i < points.length; i += 1) if (keep[i]) out.push(points[i]);
  return out;
}

class Tracer {
  constructor() {
    this.skeleton = new Uint8Array(0);
    this.visited = new Uint8Array(0);
  }

  /// `mask` is width x height bytes, row 0 first. The contours come back in the
  /// same coordinates, x and y being pixel indices.
  trace(mask, width, height, params) {
    const out = [];
    if (width < 3 || height < 3) return out;

    const count = width * height;
    if (this.skeleton.length !== count) {
      this.skeleton = new Uint8Array(count);
      this.visited = new Uint8Array(count);
    }
    const skeleton = this.skeleton;
    this.visited.fill(0);

    for (let i = 0; i < count; i += 1) skeleton[i] = mask[i] >= params.threshold ? 1 : 0;

    // The border is cleared so every neighbourhood lookup below can skip its
    // bounds check. A mask that reaches the edge of the frame loses one pixel
    // of it, which is the edge of the frame and not an edge in the picture.
    for (let x = 0; x < width; x += 1) {
      skeleton[x] = 0;
      skeleton[(height - 1) * width + x] = 0;
    }
    for (let y = 0; y < height; y += 1) {
      skeleton[y * width] = 0;
      skeleton[y * width + width - 1] = 0;
    }

    this.thin(width, height);
    this.walk(width, height, params, out);
    return out;
  }

  /// Zhang-Suen. Two sub-iterations per pass, deleting in batches so a pass
  /// sees the image as it was and not as it is becoming. Capped rather than run
  /// to convergence: a band three pixels wide is thin after two passes, and a
  /// pathological mask must not be allowed to hold the frame.
  thin(width, height) {
    const skeleton = this.skeleton;
    const doomed = [];
    // Hoisted out of the innermost loop on purpose (galvo's finding): Zhang-Suen
    // visits every lit texel up to thirty-two times, and allocating an
    // eight-element array inside that loop costs more than the rest of the CPU
    // chain. The C++ has this as a stack array and pays nothing for it.
    const p = neighbourhood;
    let changed = true;

    for (let pass = 0; pass < 16 && changed; pass += 1) {
      changed = false;
      for (let sub = 0; sub < 2; sub += 1) {
        doomed.length = 0;
        for (let y = 1; y < height - 1; y += 1) {
          for (let x = 1; x < width - 1; x += 1) {
            if (skeleton[y * width + x] === 0) continue;

            let neighbours = 0;
            let transitions = 0;
            let previous = skeleton[(y + ZS_Y[7]) * width + x + ZS_X[7]];
            for (let k = 0; k < 8; k += 1) {
              p[k] = skeleton[(y + ZS_Y[k]) * width + x + ZS_X[k]];
              neighbours += p[k];
              if (previous === 0 && p[k] === 1) transitions += 1;
              previous = p[k];
            }

            if (neighbours < 2 || neighbours > 6 || transitions !== 1) continue;

            // P2 north, P4 east, P6 south, P8 west in Zhang-Suen's naming.
            const n = p[0];
            const e = p[2];
            const s = p[4];
            const w = p[6];
            const first = sub === 0 ? n * e * s === 0 : n * e * w === 0;
            const second = sub === 0 ? e * s * w === 0 : n * s * w === 0;
            if (first && second) doomed.push(y * width + x);
          }
        }
        for (const index of doomed) skeleton[index] = 0;
        if (doomed.length > 0) changed = true;
      }
    }
  }

  walk(width, height, params, out) {
    const skeleton = this.skeleton;
    const visited = this.visited;

    const degree = (x, y) => {
      let count = 0;
      for (let k = 0; k < 8; k += 1) count += skeleton[(y + NEIGHBOUR_Y[k]) * width + x + NEIGHBOUR_X[k]];
      return count;
    };

    const walkFrom = (startX, startY) => {
      const raw = [];
      let x = startX;
      let y = startY;

      for (;;) {
        visited[y * width + x] = 1;
        raw.push({ x, y });

        let nextX = -1;
        let nextY = -1;
        for (let k = 0; k < 8; k += 1) {
          const nx = x + NEIGHBOUR_X[k];
          const ny = y + NEIGHBOUR_Y[k];
          if (nx < 1 || ny < 1 || nx >= width - 1 || ny >= height - 1) continue;
          if (skeleton[ny * width + nx] === 0 || visited[ny * width + nx]) continue;
          nextX = nx;
          nextY = ny;
          break;
        }
        if (nextX < 0) break;
        x = nextX;
        y = nextY;
      }

      // Two-pixel specks are noise, not drawing, and nothing under four pixels
      // can be told closed from open.
      if (raw.length < 4) return;

      // Closed if the walk came back next to where it began. Eight-adjacency,
      // because the skeleton is eight-connected.
      const last = raw[raw.length - 1];
      const closed =
        raw.length >= 8 && Math.abs(last.x - raw[0].x) <= 1.0 && Math.abs(last.y - raw[0].y) <= 1.0;

      const reduced = simplifyPolyline(raw, params.simplify, closed);
      if (reduced.length < 2) return;
      if (polylineLength(reduced, closed) < params.minLength) return;

      out.push({ points: reduced, closed });
    };

    // Endpoints first, so an open line is one stroke and not two halves. Then
    // junctions, so the arms of a T are walked from the crossing outward. Then
    // anything unvisited, which is the closed loops.
    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x] && degree(x, y) === 1) walkFrom(x, y);

    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x] && degree(x, y) >= 3) walkFrom(x, y);

    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x]) walkFrom(x, y);
  }
}

//===========================================================================
// A port of source/Planner.cpp — from contours to a plotter job. No GL.
//
// 1. Strokes: each contour becomes a stroke in paper units, with the colour the
//    clip had under it and the pen nearest that colour.
// 2. Order: with Optimise, grouped by pen (a pen change is a trip to the
//    carousel) and within a pen a nearest-start tour, reversing an open stroke
//    or rotating a closed one to suit.
// 3. Blocks: straight moves under a trapezoidal velocity profile with junction
//    speeds that fall linearly with the turn to zero at Corner Angle, plus the
//    dwells (pen settle, pen change), made reachable by a backward-then-forward
//    pass the way a motion controller plans G-code.
//
// Everything the picture then shows — the heavy ends, the blot, the corner
// darker than the straight — follows from the profile in the block and the
// deposit per unit time in the renderer. Nothing here decides how dark a line is.
//===========================================================================

const BLOCK_TRAVEL = 0;
const BLOCK_DRAW = 1;
const BLOCK_SETTLE = 2;
const BLOCK_PEN_CHANGE = 3;

const distanceSquared = (a, b) => (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
const distance = (a, b) => Math.sqrt(distanceSquared(a, b));

const blockDuration = (block) => block.t1 + block.t2 + block.t3;

function newBlock() {
  return {
    kind: BLOCK_TRAVEL,
    a: { x: 0, y: 0 },
    b: { x: 0, y: 0 },
    length: 0.0,
    // The trapezoid: entry speed, peak speed, exit speed, and the three
    // durations. A dwell has all speeds zero and its time in t2.
    vIn: 0.0,
    vPeak: 0.0,
    vOut: 0.0,
    accel: 1.0,
    t1: 0.0,
    t2: 0.0,
    t3: 0.0,
    pen: 0,
    ink: [0.0, 0.0, 0.0],
    /// Which stroke this belongs to, or -1.
    stroke: -1,
  };
}

/// The trapezoid for one move: entry speed, exit speed, the cruise limit and
/// the acceleration, over a length. The peak is the cruise speed when the move
/// is long enough to get there and back, and otherwise the speed at which
/// accelerating from vIn meets decelerating to vOut.
function profile(block, vMax, accel) {
  const L = block.length;
  accel = Math.max(accel, 1e-9);
  let vPeak = Math.sqrt(Math.max(0.0, (2.0 * accel * L + block.vIn * block.vIn + block.vOut * block.vOut) * 0.5));
  vPeak = Math.min(vPeak, vMax);
  vPeak = Math.max(vPeak, Math.max(block.vIn, block.vOut));

  const d1 = (vPeak * vPeak - block.vIn * block.vIn) / (2.0 * accel);
  const d3 = (vPeak * vPeak - block.vOut * block.vOut) / (2.0 * accel);
  const d2 = Math.max(0.0, L - d1 - d3);

  block.vPeak = vPeak;
  block.accel = accel;
  block.t1 = (vPeak - block.vIn) / accel;
  block.t3 = (vPeak - block.vOut) / accel;
  block.t2 = vPeak > 0.0 ? d2 / vPeak : 0.0;
}

function dwell(kind, at, seconds, pen, ink, stroke) {
  const block = newBlock();
  block.kind = kind;
  block.a = { x: at.x, y: at.y };
  block.b = { x: at.x, y: at.y };
  block.length = 0.0;
  block.t2 = Math.max(0.0, seconds);
  block.pen = pen;
  block.stroke = stroke;
  block.ink = [ink[0], ink[1], ink[2]];
  return block;
}

/// A rest-to-rest move with the pen up.
function travel(out, from, to, params, pen, ink) {
  const L = distance(from, to);
  if (L <= 1e-9) return;
  const block = newBlock();
  block.kind = BLOCK_TRAVEL;
  block.a = { x: from.x, y: from.y };
  block.b = { x: to.x, y: to.y };
  block.length = L;
  block.vIn = 0.0;
  block.vOut = 0.0;
  block.pen = pen;
  block.ink = [ink[0], ink[1], ink[2]];
  profile(block, params.maxSpeed * params.travelFactor, params.acceleration);
  out.push(block);
}

/// Convert the tracer's contours (trace pixels, row 0 at the bottom) into
/// strokes. `rgba` is the trace-size readback, four bytes a texel with the
/// clip's straight colour in g, b, a; it may be null, in which case every
/// stroke is grey. `pens` is how many of the palette are on the carousel.
function buildStrokes(contours, traceWidth, traceHeight, aspect, rgba, palette, pens) {
  const out = [];
  const sx = aspect / Math.max(traceWidth, 1);
  const sy = 1.0 / Math.max(traceHeight, 1);

  const colourAt = (px, py, acc) => {
    const x = clamp(roundHalfAway(px), 0, traceWidth - 1);
    const y = clamp(roundHalfAway(py), 0, traceHeight - 1);
    const t = (y * traceWidth + x) * 4;
    acc[0] += rgba[t + 1] / 255.0;
    acc[1] += rgba[t + 2] / 255.0;
    acc[2] += rgba[t + 3] / 255.0;
  };

  for (const contour of contours) {
    const n = contour.points.length;
    if (n < 2) continue;

    const stroke = { points: [], closed: contour.closed, pen: 0, ink: [0.0, 0.0, 0.0] };
    for (const p of contour.points) stroke.points.push({ x: (p.x + 0.5) * sx, y: (p.y + 0.5) * sy });

    // The clip's colour along the contour: a sample every couple of trace
    // pixels, averaged. The readback carries the colour read at the edge pass's
    // mip level, so it is already the colour of the region the edge runs
    // through rather than a coin flip between its two sides.
    let source = [0.5, 0.5, 0.5];
    if (rgba !== null) {
      const acc = [0.0, 0.0, 0.0];
      let count = 0;
      const segments = contour.closed ? n : n - 1;
      for (let s = 0; s < segments; s += 1) {
        const a = contour.points[s];
        const b = contour.points[(s + 1) % n];
        const steps = Math.max(1, Math.ceil(distance(a, b) / kColourSampleSpacing));
        for (let i = 0; i < steps; i += 1) {
          const t = i / steps;
          colourAt(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, acc);
          count += 1;
        }
      }
      if (count > 0) source = [acc[0] / count, acc[1] / count, acc[2] / count];
    }

    stroke.pen = nearestPen(palette, pens, source);
    stroke.ink = penColour(palette, stroke.pen, source);
    out.push(stroke);
  }
  return out;
}

/// Reorder. `cursor` is where the carriage is now; `currentPen` is the pen it
/// holds, or -1 for none. With `optimise`, strokes are grouped by pen — the
/// current pen's group first — and each group is a nearest-start tour. Without
/// it the order is left alone. `byPen` false is the plugin's negative control
/// and is never used here.
function orderStrokes(strokes, cursor, currentPen, optimise, byPen = true) {
  if (!optimise || strokes.length < 2) return strokes;

  const group = (s) => (byPen ? s.pen : 0);

  // The pens in use, the current one first, then ascending: the carriage
  // finishes what it is holding before it goes to the carousel.
  const pens = [];
  for (const s of strokes) if (!pens.includes(group(s))) pens.push(group(s));
  pens.sort((a, b) => a - b);
  const at = pens.indexOf(currentPen);
  if (at >= 0) {
    pens.splice(at, 1);
    pens.unshift(currentPen);
  }

  const ordered = [];
  const taken = new Array(strokes.length).fill(false);
  cursor = { x: cursor.x, y: cursor.y };

  for (const pen of pens) {
    // A nearest-neighbour tour within the pen, galvo's OrderContours: an open
    // stroke may be drawn backwards and a closed one may start at whichever
    // vertex is nearest.
    for (;;) {
      let best = strokes.length;
      let bestAt = 0;
      let bestCost = 1e30;
      let bestReverse = false;

      for (let i = 0; i < strokes.length; i += 1) {
        if (taken[i] || group(strokes[i]) !== pen) continue;
        const s = strokes[i];
        if (s.closed) {
          for (let k = 0; k < s.points.length; k += 1) {
            const d = distanceSquared(s.points[k], cursor);
            if (d < bestCost) {
              bestCost = d;
              best = i;
              bestAt = k;
              bestReverse = false;
            }
          }
        } else {
          const df = distanceSquared(s.points[0], cursor);
          const db = distanceSquared(s.points[s.points.length - 1], cursor);
          if (df < bestCost) {
            bestCost = df;
            best = i;
            bestAt = 0;
            bestReverse = false;
          }
          if (db < bestCost) {
            bestCost = db;
            best = i;
            bestAt = 0;
            bestReverse = true;
          }
        }
      }
      if (best >= strokes.length) break;

      taken[best] = true;
      const source = strokes[best];
      let points = source.points.slice();
      if (source.closed && bestAt !== 0) points = points.slice(bestAt).concat(points.slice(0, bestAt));
      if (bestReverse) points.reverse();
      const s = { points, closed: source.closed, pen: source.pen, ink: source.ink };
      cursor = s.closed ? s.points[0] : s.points[s.points.length - 1];
      ordered.push(s);
    }
  }

  return ordered;
}

/// The time a move of length L takes from rest to rest: L/v + v/a when it
/// reaches cruise, 2 sqrt( L/a ) when it does not. What `pltest --trapezoid`
/// checks the picture against; here only for the record.
function restToRestSeconds(length, maxSpeed, acceleration) {
  if (length <= 0.0) return 0.0;
  const cruiseDistance = (maxSpeed * maxSpeed) / acceleration;
  if (length > cruiseDistance) return length / maxSpeed + maxSpeed / acceleration;
  return 2.0 * Math.sqrt(length / acceleration);
}

/// Plan every stroke into blocks, starting from `cursor` holding `currentPen`
/// (-1 for none: the first stroke then begins with a trip to the carousel).
/// Returns the number of pen changes planned; the blocks go into `out`.
function planBlocks(strokes, params, cursor, currentPen, out) {
  out.length = 0;
  let changes = 0;
  let pos = { x: cursor.x, y: cursor.y };
  let pen = currentPen;

  const cornerRad = (Math.max(params.cornerAngleDeg, 0.1) * Math.PI) / 180.0;

  for (let index = 0; index < strokes.length; index += 1) {
    const stroke = strokes[index];
    const n = stroke.points.length;
    if (n < 2) continue;

    // A pen change is a trip to the carousel: travel there, wait, travel on.
    // The carriage starts a session holding nothing, so the first stroke of a
    // fresh machine always pays one.
    if (stroke.pen !== pen) {
      travel(out, pos, params.carousel, params, pen, stroke.ink);
      out.push(dwell(BLOCK_PEN_CHANGE, params.carousel, params.penChange, stroke.pen, stroke.ink, -1));
      pos = { x: params.carousel.x, y: params.carousel.y };
      pen = stroke.pen;
      changes += 1;
    }

    travel(out, pos, stroke.points[0], params, pen, stroke.ink);
    out.push(dwell(BLOCK_SETTLE, stroke.points[0], params.settle, pen, stroke.ink, index));

    // The segments, with their junction speeds. A junction is the cruise speed
    // when the turn is gentle and zero when it is a corner; the two ends are
    // zero because the pen starts from its settle and stops for the next one.
    const segments = stroke.closed ? n : n - 1;
    const length = new Array(segments).fill(0.0);
    const junction = new Array(segments + 1).fill(0.0);
    for (let s = 0; s < segments; s += 1) length[s] = distance(stroke.points[s], stroke.points[(s + 1) % n]);

    for (let j = 1; j < segments; j += 1) {
      const prev = stroke.points[j - 1];
      const at = stroke.points[j];
      const next = stroke.points[(j + 1) % n];
      const ix = at.x - prev.x;
      const iy = at.y - prev.y;
      const ox = next.x - at.x;
      const oy = next.y - at.y;
      const il = Math.sqrt(ix * ix + iy * iy);
      const ol = Math.sqrt(ox * ox + oy * oy);
      if (il <= 1e-9 || ol <= 1e-9) continue;
      // The junction speed falls with the turn: the cruise speed straight on,
      // nothing at Corner Angle or sharper. A motion controller limits a
      // junction by the sideways jerk it would take, which is monotone in the
      // turn; this is that curve's simplest shape, and it is what puts more ink
      // on a tighter bend.
      const cosTurn = clamp((ix * ox + iy * oy) / (il * ol), -1.0, 1.0);
      const turn = Math.acos(cosTurn);
      junction[j] = params.maxSpeed * Math.max(0.0, 1.0 - turn / cornerRad);
    }

    // Reachability. Backward: no junction may be faster than the next one can
    // be reached from under the deceleration limit. Forward: none may be faster
    // than the previous one lets it accelerate to. After both, every segment's
    // entry and exit are consistent with its length.
    const a2 = 2.0 * Math.max(params.acceleration, 1e-9);
    for (let s = segments; s-- > 0;)
      junction[s] = Math.min(junction[s], Math.sqrt(junction[s + 1] * junction[s + 1] + a2 * length[s]));
    for (let s = 0; s < segments; s += 1)
      junction[s + 1] = Math.min(junction[s + 1], Math.sqrt(junction[s] * junction[s] + a2 * length[s]));

    for (let s = 0; s < segments; s += 1) {
      if (length[s] <= 1e-9) continue;
      const block = newBlock();
      block.kind = BLOCK_DRAW;
      block.a = { x: stroke.points[s].x, y: stroke.points[s].y };
      block.b = { x: stroke.points[(s + 1) % n].x, y: stroke.points[(s + 1) % n].y };
      block.length = length[s];
      block.vIn = junction[s];
      block.vOut = junction[s + 1];
      block.pen = pen;
      block.stroke = index;
      block.ink = [stroke.ink[0], stroke.ink[1], stroke.ink[2]];
      profile(block, params.maxSpeed, params.acceleration);
      out.push(block);
    }

    const end = stroke.closed ? stroke.points[0] : stroke.points[n - 1];
    out.push(dwell(BLOCK_SETTLE, end, params.settle, pen, stroke.ink, index));
    pos = { x: end.x, y: end.y };
  }

  return changes;
}

function distanceAlong(block, t) {
  t = clamp(t, 0.0, blockDuration(block));
  let s;
  if (t < block.t1) s = block.vIn * t + 0.5 * block.accel * t * t;
  else if (t < block.t1 + block.t2)
    s = (block.vPeak * block.vPeak - block.vIn * block.vIn) / (2.0 * block.accel) + block.vPeak * (t - block.t1);
  else {
    const tau = t - block.t1 - block.t2;
    s = block.length - (block.vPeak * block.vPeak - block.vOut * block.vOut) / (2.0 * block.accel)
      + block.vPeak * tau - 0.5 * block.accel * tau * tau;
  }
  return clamp(s, 0.0, block.length);
}

/// Where the carriage is `t` seconds into a block.
function positionInBlock(block, t) {
  if (block.length <= 0.0) return { x: block.a.x, y: block.a.y };
  const s = distanceAlong(block, t) / block.length;
  return { x: block.a.x + (block.b.x - block.a.x) * s, y: block.a.y + (block.b.y - block.a.y) * s };
}

/// And how fast it is going. Not used by the page; ported for completeness.
function speedInBlock(block, t) {
  t = clamp(t, 0.0, blockDuration(block));
  if (block.length <= 0.0) return 0.0;
  if (t < block.t1) return block.vIn + block.accel * t;
  if (t < block.t1 + block.t2) return block.vPeak;
  return Math.max(0.0, block.vPeak - block.accel * (t - block.t1 - block.t2));
}

//===========================================================================
// A port of source/Machine.cpp — the plotter's moving parts, advanced by real
// elapsed time. No GL.
//
// A job is a list of blocks. The machine keeps its place in that list — which
// block, and how far into it — and each host frame it is told how much time
// passed and asked what the pen did. What the pen did comes back as samples:
// where the carriage was at the start of each interval, how long the interval
// lasted, and whether the pen was down. The renderer turns those into ink.
//
// Sub-sampling: an interval is short enough that the speed changes by under a
// percent across it, that the carriage moves under half a step, and that it is
// never longer than 1/240 s. Quantisation: each sample's position is rounded
// to the nearest step on each axis, from the closed-form position, never
// accumulated.
//===========================================================================

/// No interval is longer than this, whatever the block is doing.
const kMaxInterval = 1.0 / 240.0;
/// Across one interval the speed changes by at most this fraction of the
/// block's peak, in the acceleration and deceleration phases.
const kSpeedChangePerInterval = 0.01;
/// And the carriage moves by at most this fraction of a step.
const kStepsPerInterval = 0.5;
/// Floors and caps, so a pathological block cannot hold the frame.
const kMinInterval = 1.0e-5;
const kMaxIntervalsPerBlock = 4096;

/// Samples are written straight into the layout the ink pass reads: eight
/// floats, two attributes per instance, the same `struct Sample` the plugin
/// uploads — x, y, dt, on, r, g, b, pad. Growable so the buffer is allocated
/// once rather than per frame.
class SampleBuffer {
  constructor() {
    this.data = new Float32Array(8 * 8192);
    this.count = 0;
  }

  clear() {
    this.count = 0;
  }

  push(x, y) {
    if ((this.count + 1) * 8 > this.data.length) {
      const grown = new Float32Array(this.data.length * 2);
      grown.set(this.data);
      this.data = grown;
    }
    const at = this.count * 8;
    const d = this.data;
    d[at] = x;
    d[at + 1] = y;
    d[at + 2] = 0;
    d[at + 3] = 0;
    d[at + 4] = 0;
    d[at + 5] = 0;
    d[at + 6] = 0;
    d[at + 7] = 0;
    this.count += 1;
  }

  /// `Sample& head = out.back(); head.dt = …` — fill in the interval that
  /// starts at the last sample pushed.
  setHead(dt, on, r, g, b) {
    const at = (this.count - 1) * 8;
    const d = this.data;
    d[at + 2] = dt;
    d[at + 3] = on;
    d[at + 4] = r;
    d[at + 5] = g;
    d[at + 6] = b;
  }
}

class Machine {
  constructor() {
    this.blocks = [];
    this.index = 0;
    this.tInBlock = 0.0;
    this.step = 0.0;
    this.position = { x: 0.0, y: 0.0 };
    this.pen = -1;
    this.penDown = false;
    this.ink = [0.0, 0.0, 0.0];
    this.strokesCompleted = 0;
    this.penChangesMade = 0;
    this.penChangesPlanned = 0;
    this.inkSeconds = 0.0;
    this.jobSeconds = 0.0;
    this.jobElapsed = 0.0;
    this.strokeEnded = false;
  }

  /// Adopt a new job. The machine continues from where it is; the plan was
  /// made from there.
  setJob(blocks, penChanges) {
    this.blocks = blocks;
    this.index = 0;
    this.tInBlock = 0.0;
    this.penChangesPlanned = penChanges;
    this.jobElapsed = 0.0;
    this.jobSeconds = 0.0;
    for (const b of blocks) this.jobSeconds += blockDuration(b);
    this.strokeEnded = false;
    if (blocks.length === 0) this.penDown = false;
  }

  hasJob() {
    return this.blocks.length > 0;
  }

  jobDone() {
    return this.blocks.length === 0 || this.index >= this.blocks.length;
  }

  /// The stepper pitch in paper units, 0 for none.
  setStepSize(pitch) {
    this.step = Math.max(0.0, pitch);
  }

  /// Forget the job and the place in it; the carriage stays where it is.
  clear() {
    this.blocks = [];
    this.index = 0;
    this.tInBlock = 0.0;
    this.jobElapsed = 0.0;
    this.jobSeconds = 0.0;
    this.penDown = false;
    this.strokeEnded = false;
  }

  /// Everything back to the start of a session: carriage parked at the
  /// carousel, pen up, holding nothing.
  reset() {
    this.clear();
    // The carriage parks at the carousel with no pen, so a session's first
    // stroke costs one swap and one travel, not a trip across the sheet first.
    this.position = { x: kCarouselX, y: kCarouselY };
    this.pen = -1;
    this.strokesCompleted = 0;
    this.penChangesMade = 0;
    this.inkSeconds = 0.0;
    this.ink = [0.0, 0.0, 0.0];
  }

  quantise(p) {
    if (this.step <= 0.0) return p;
    // Rounded from the closed-form position each time, never accumulated, so
    // the grid cannot drift and the rounding is the same at any frame rate.
    return {
      x: roundHalfAway(p.x / this.step) * this.step,
      y: roundHalfAway(p.y / this.step) * this.step,
    };
  }

  /// Advance by `seconds` and append the samples produced. The first sample
  /// appended is where the carriage was at the start, so the intervals chain
  /// across frames without a gap. Returns the time NOT consumed: the machine
  /// stops early at the end of the job, or — with `stopAtStrokeEnd` — the end
  /// of a stroke, so the caller can re-plan and call again with the rest.
  advance(seconds, stopAtStrokeEnd, out) {
    this.strokeEnded = false;
    if (this.jobDone() || seconds <= 0.0) return seconds;

    let remaining = seconds;

    // The chain starts where the carriage is. Its dt and pen state are filled
    // in by the first interval below; if there is none, the renderer sees a
    // single sample and draws nothing.
    {
      const q = this.quantise(this.position);
      out.push(q.x, q.y);
    }

    while (remaining > 0.0 && this.index < this.blocks.length) {
      const block = this.blocks[this.index];
      const duration = blockDuration(block);
      const left = Math.max(0.0, duration - this.tInBlock);
      const take = Math.min(remaining, left);
      const down = block.kind === BLOCK_DRAW || block.kind === BLOCK_SETTLE;

      if (take > 0.0) {
        // How finely to sample this stretch. See the class comment.
        let h = kMaxInterval;
        if (block.t1 > 0.0 || block.t3 > 0.0) h = Math.min(h, (kSpeedChangePerInterval * block.vPeak) / block.accel);
        if (this.step > 0.0 && block.vPeak > 0.0) h = Math.min(h, (kStepsPerInterval * this.step) / block.vPeak);
        h = Math.max(h, kMinInterval);

        const n = clamp(Math.ceil(take / h), 1, kMaxIntervalsPerBlock);
        const dt = take / n;
        for (let i = 1; i <= n; i += 1) {
          const p = positionInBlock(block, this.tInBlock + (take * i) / n);
          out.setHead(dt, down ? 1.0 : 0.0, block.ink[0], block.ink[1], block.ink[2]);
          const q = this.quantise(p);
          out.push(q.x, q.y);
        }

        if (down) this.inkSeconds += take;
        this.tInBlock += take;
        remaining -= take;
        this.jobElapsed += take;
      }

      this.position = positionInBlock(block, this.tInBlock);
      this.penDown = down;
      this.pen = block.pen;
      this.ink = [block.ink[0], block.ink[1], block.ink[2]];

      // Finished this block?
      if (take >= left - 1e-12) {
        if (block.kind === BLOCK_PEN_CHANGE) this.penChangesMade += 1;

        // The settle at the END of a stroke is the last block with that
        // stroke's index; the one at the start is followed by its segments.
        const strokeEnd = block.kind === BLOCK_SETTLE
          && (this.index + 1 >= this.blocks.length || this.blocks[this.index + 1].stroke !== block.stroke);
        this.index += 1;
        this.tInBlock = 0.0;
        if (strokeEnd) {
          this.strokesCompleted += 1;
          this.strokeEnded = true;
          if (stopAtStrokeEnd) break;
        }
      }
    }

    if (this.jobDone()) this.penDown = false;
    return remaining;
  }
}

//===========================================================================
// A port of the CPU side of source/render/Ink.cpp — the paper, and the ink
// going onto it. galvo's beam renderer turned from light into absorbance: each
// machine interval deposits InkRate x dt while the pen is down, spread over the
// distance the carriage covered in it by the exact convolution of a uniform
// segment with a Gaussian nib. One float buffer at picture size, additive,
// never decayed: ink stays. rgb holds absorbance per channel, a the travel
// density for Show Travel.
//
// It survives a resize: the old sheet is drawn into the new one, bilinearly,
// before the old one is released.
//===========================================================================

class InkRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;
    this.inkShader = new Program(gl, INK_VERTEX, INK_FRAGMENT, 'ink', {
      attribs: { sampleA: 0, inkA: 1, sampleB: 2, inkB: 3 },
    });
    this.resampleShader = new Program(gl, VERTEX, RESAMPLE, 'resample');

    // The paper's format: see the note at the top of this file. The plugin's
    // paper is RGBA32F; WebGL2 blends into one only with EXT_float_blend and
    // filters one linearly only with OES_texture_float_linear.
    const floatBlend = gl.getExtension('EXT_float_blend') !== null;
    const floatLinear = gl.getExtension('OES_texture_float_linear') !== null;
    this.format = floatBlend && floatLinear ? gl.RGBA32F : gl.RGBA16F;
    this.formatName = this.format === gl.RGBA32F ? 'RGBA32F' : 'RGBA16F';

    // Linear, because the composite reads it texel-for-texel (where linear is
    // nearest) and a resize reads it BETWEEN texels.
    this.paper = new PassBuffer(gl, { filter: 'linear' });
    this.spare = new PassBuffer(gl, { filter: 'linear' });
    this.width = 0;
    this.height = 0;

    // One buffer of Samples, read twice. Attributes 0/1 start at the beginning
    // and 2/3 one Sample in, so instance i sees samples i and i+1. That is why
    // the draw must ask for n-1 instances: n would read one past the end.
    // vertexAttribDivisor is VAO state: set it with this VAO bound.
    const STRIDE = 32;
    this.vao = gl.createVertexArray();
    this.vbo = gl.createBuffer();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    for (let i = 0; i < 4; i += 1) {
      gl.enableVertexAttribArray(i);
      gl.vertexAttribPointer(i, 4, gl.FLOAT, false, STRIDE, i * 16);
      gl.vertexAttribDivisor(i, 1);
    }
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
  }

  /// Allocate the paper at this size, resampling the old sheet into it if the
  /// size changed.
  ensure(requestedWidth, requestedHeight) {
    const gl = this.gl;
    if (requestedWidth <= 0 || requestedHeight <= 0) return false;
    const same = this.paper.texture !== null && this.width === requestedWidth && this.height === requestedHeight;
    if (same) return true;

    if (this.paper.texture === null) {
      this.paper.ensure(requestedWidth, requestedHeight, this.format);
      this.paper.clearTo(0, 0, 0, 0);
      this.width = requestedWidth;
      this.height = requestedHeight;
      return true;
    }

    // The sheet survives: draw the old paper into a new one before letting the
    // old one go.
    this.spare.dispose();
    this.spare.ensure(requestedWidth, requestedHeight, this.format);
    this.spare.bind();
    gl.disable(gl.BLEND);
    this.resampleShader.use();
    bindTexture(gl, 0, this.paper.texture);
    this.resampleShader.setSampler('PaperTexture', 0);
    this.quad.draw();
    gl.bindTexture(gl.TEXTURE_2D, null);

    const old = this.paper;
    this.paper = this.spare;
    this.spare = old;
    this.spare.dispose();

    this.width = requestedWidth;
    this.height = requestedHeight;
    return true;
  }

  /// Tear the sheet off: clear to nothing.
  clear() {
    if (this.paper.texture !== null) this.paper.clearTo(0, 0, 0, 0);
  }

  get texture() {
    return this.paper.texture;
  }

  /// Deposit `n` samples' worth of intervals (n - 1 of them).
  deposit(samples, n, params, aspect) {
    const gl = this.gl;
    if (this.width <= 0 || this.height <= 0) return false;
    const segments = Math.max(0, n - 1);
    if (segments === 0) return true;

    // STREAM_DRAW and a fresh bufferData every frame, so the driver orphans
    // the old storage rather than waiting for last frame's draw to finish
    // reading it.
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, samples.data.subarray(0, n * 8), gl.STREAM_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);

    this.paper.bind();

    // Additive: ink on ink is more ink. The composite's exponential is what
    // saturates it.
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.ONE, gl.ONE, gl.ONE, gl.ONE);

    this.inkShader.use();
    this.inkShader.set('InkRate', params.inkRate);
    this.inkShader.set('TravelRate', params.travelRate);
    this.inkShader.set('NibSigma', params.nibSigma);
    this.inkShader.set('Aspect', aspect);

    gl.bindVertexArray(this.vao);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, segments);
    gl.bindVertexArray(null);
    gl.disable(gl.BLEND);
    return true;
  }
}

//===========================================================================
// The renderer: Plotter::ProcessOpenGL, pass for pass.
//
//   1. copy       picture size, mipmapped.
//   2. edge       TRACE size (320 wide). Sobel at the mip level matching it,
//                 plus Detail; the clip's colour in gba.
//   3. stabilise  trace size, ping-ponged. The asymmetric temporal filter.
//   4. sheet      tear it off on the event, or on the timer.
//   5. work       the tracer is asked for a job when the machine has none —
//                 one readPixels — and the machine advances by the frame's
//                 real elapsed time.
//   6. ink        one instanced quad per machine interval, additive, into the
//                 paper.
//   7. composite  paper, ink, travel, carriage, mix, to the canvas.
//===========================================================================

/// Plotter.cpp: a host frame is between these whatever the clock says. Shorter
/// is a scrub or a stall; longer is the machine having been asleep, and a
/// plotter asleep for a minute should not draw a minute of strokes in one
/// frame. A backgrounded tab is the browser's version of the same accident.
const kMinFrame = 1.0 / 240.0;
const kMaxFrame = 0.25;

/// How often an idle machine (a blank clip) asks the tracer again.
const kIdleRetrySeconds = 0.25;

const PALETTE_COUNT = 4;
const PAPER_COUNT = 5;

/// What the line under the canvas reports. Filled by the renderer, read by a
/// timer — see where it is mounted.
const telemetry = {
  paper: '',
  sheetSeconds: 0,
  autoSheet: 0,
  sheets: 0,
  strokesInJob: 0,
  strokesDone: 0,
  jobElapsed: 0,
  jobSeconds: 0,
  penChanges: 0,
  penDown: false,
  pen: -1,
  contours: 0,
  traces: 0,
  cpuMillis: 0,
  traceMillis: 0,
  idle: true,
};

/// The New Sheet button reaches the renderer through this.
let rendererInstance = null;

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const edgeShader = new Program(gl, VERTEX, EDGE, 'edge');
  const stabiliseShader = new Program(gl, VERTEX, STABILISE, 'stabilise');
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');

  const copyBuffer = new PassBuffer(gl, { filter: 'linear', mip: true });
  const edgeBuffer = new PassBuffer(gl, { filter: 'nearest' });
  const stableBuffer = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];

  const ink = new InkRenderer(gl, quad);
  telemetry.paper = ink.formatName;

  const tracer = new Tracer();
  const machine = new Machine();
  const samples = new SampleBuffer();

  let readback = new Uint8Array(0);
  let mask = new Uint8Array(0);
  let contours = [];
  let strokes = [];
  let blocks = [];

  let stableCurrent = 0;
  let historyValid = false;
  let newSheetPending = false;
  let sheetSeconds = 0.0;
  let idleSeconds = 0.0;
  let traceCount = 0;
  let sheetCount = 0;
  let lastTime = null;
  let lastDetail = null;

  // InitGL.
  machine.reset();

  /// The machine's parameters from the controls.
  const machineParams = (p) => ({
    maxSpeed: maxSpeedFromParam(p('maxSpeed')),
    acceleration: accelerationFromParam(p('acceleration')),
    settle: penSettleFromParam(p('penSettle')),
    cornerAngleDeg: cornerAngleFromParam(p('cornerAngle')),
    travelFactor: kTravelSpeedFactor,
    penChange: kPenChangeSeconds,
    carousel: { x: kCarouselX, y: kCarouselY },
  });

  /// Read the mask back, trace it, plan a job from where the carriage is.
  function takeWork(p, traceWidth, traceHeight, aspect) {
    const start = performance.now();

    // The one synchronous readback: four bytes per trace texel of the buffer
    // the stabilise pass just wrote — the gradient and the colour together —
    // and a pipeline stall, exactly as the plugin's glReadPixels is. It happens
    // only on the frames that ask for work, which on a busy picture is once a
    // job and in Chase once a stroke.
    const texels = traceWidth * traceHeight;
    if (readback.length !== texels * 4) {
      readback = new Uint8Array(texels * 4);
      mask = new Uint8Array(texels);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, stableBuffer[stableCurrent].fbo);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    gl.readPixels(0, 0, traceWidth, traceHeight, gl.RGBA, gl.UNSIGNED_BYTE, readback);

    // Threshold on the CPU. The tracer wants a binary mask, and the byte it
    // gets is the stabilised gradient, so the threshold is one compare.
    const threshold = thresholdFromParam(p('threshold'));
    const cut = clamp(roundHalfAway(threshold * 255.0), 1, 255);
    for (let i = 0; i < texels; i += 1) mask[i] = readback[i * 4] >= cut ? 255 : 0;

    contours = tracer.trace(mask, traceWidth, traceHeight, {
      threshold: 128,
      simplify: kSimplify,
      minLength: kMinLength,
    });

    const palette = clamp(Math.round(p('palette')), 0, PALETTE_COUNT - 1);
    const pens = clamp(pensValue(p('pens')), 1, kPensPerPalette);
    strokes = buildStrokes(contours, traceWidth, traceHeight, aspect, readback, palette, pens);

    // The plan starts from where the carriage is and what it is holding. In
    // Chase the previous job is thrown away mid-way, which is the point: the
    // next stroke comes from now.
    const cursor = machine.position;
    const currentPen = machine.pen;
    if (p('optimise') >= 0.5) strokes = orderStrokes(strokes, cursor, currentPen, true, true);

    blocks = [];
    const changes = planBlocks(strokes, machineParams(p), cursor, currentPen, blocks);
    machine.setJob(blocks, changes);

    traceCount += 1;
    telemetry.traces = traceCount;
    telemetry.contours = contours.length;
    telemetry.strokesInJob = strokes.length;
    telemetry.traceMillis = performance.now() - start;
  }

  rendererInstance = {
    /// The event: a press is remembered until the next frame.
    newSheet() {
      newSheetPending = true;
    },

    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // Time. The plugin normalises the host's clock to seconds and takes
      // the frame delta from it, clamped. That delta is what the machine
      // advances by.
      //------------------------------------------------------------------
      let frameSeconds = 1.0 / 60.0;
      if (lastTime !== null) frameSeconds = clamp(time - lastTime, kMinFrame, kMaxFrame);
      lastTime = time;

      const aspect = width / height;

      //------------------------------------------------------------------
      // Buffers.
      //------------------------------------------------------------------
      const traceWidth = Math.min(kTraceWidth, width);
      const traceHeight = Math.max(8, roundHalfAway(traceWidth / aspect));

      const traceSizeChanged = stableBuffer[0].texture === null
        || stableBuffer[0].width !== traceWidth || stableBuffer[0].height !== traceHeight;

      copyBuffer.ensure(width, height, gl.RGBA16F);
      edgeBuffer.ensure(traceWidth, traceHeight, gl.RGBA16F);
      // RGBA8 and not RGBA16F, for the one reason at the top of this file:
      // WebGL2 will not readPixels a float framebuffer as bytes, and the plugin
      // reads this buffer back as bytes too.
      stableBuffer[0].ensure(traceWidth, traceHeight, gl.RGBA8);
      stableBuffer[1].ensure(traceWidth, traceHeight, gl.RGBA8);
      ink.ensure(width, height);

      if (traceSizeChanged) historyValid = false;

      // Changing what an edge *is* invalidates the history, because the numbers
      // being blended are no longer measuring the same thing. SetFloatParameter
      // does this in the plugin; there is no parameter callback here, so it is
      // noticed instead.
      if (p('detail') !== lastDetail) historyValid = false;
      lastDetail = p('detail');

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The picture, into a texture of ours, with a mip chain on it.
      //------------------------------------------------------------------
      copyBuffer.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      // The host hands an FFGL plugin a texture the picture may not fill; here
      // it always does, so MaxUV is 1.
      copyShader.set('MaxUV', 1, 1);
      copyShader.set('HalfTexel', 0.5 / input.width, 0.5 / input.height);
      quad.draw();
      copyBuffer.generateMipmap();

      //------------------------------------------------------------------
      // 2. Edge and colour, at the trace resolution.
      //------------------------------------------------------------------
      {
        const baseLod = Math.log2(Math.max(1.0, width / traceWidth));
        const detail = detailFromParam(p('detail'));
        const spread = Math.pow(2, detail);

        edgeBuffer.bind();
        edgeShader.use();
        bindTexture(gl, 0, copyBuffer.texture);
        edgeShader.setSampler('CopyTexture', 0);
        edgeShader.set('Step', spread / traceWidth, spread / traceHeight);
        edgeShader.set('Lod', baseLod + detail);
        quad.draw();
      }

      //------------------------------------------------------------------
      // 3. Stabilise, ping-ponged against the previous frame's result.
      //------------------------------------------------------------------
      {
        const history = stableCurrent;
        const target = 1 - stableCurrent;
        stableBuffer[target].bind();
        stabiliseShader.use();
        bindTexture(gl, 0, edgeBuffer.texture);
        bindTexture(gl, 1, stableBuffer[history].texture);
        stabiliseShader.setSampler('EdgeTexture', 0);
        stabiliseShader.setSampler('HistoryTexture', 1);
        stabiliseShader.set('Attack', kAttack);
        stabiliseShader.set('Release', kRelease);
        stabiliseShader.set('Reset', historyValid ? 0.0 : 1.0);
        quad.draw();
        stableCurrent = target;
      }
      historyValid = true;

      //------------------------------------------------------------------
      // 4. The sheet. Tear it off on the event, or on the timer.
      //------------------------------------------------------------------
      const cpuStart = performance.now();

      sheetSeconds += frameSeconds;
      const autoSheet = autoSheetFromParam(p('autoSheet'));
      if (autoSheet > 0.0 && sheetSeconds >= autoSheet) newSheetPending = true;
      if (newSheetPending) {
        ink.clear();
        machine.clear();
        sheetSeconds = 0.0;
        idleSeconds = 0.0;
        newSheetPending = false;
        sheetCount += 1;
      }

      //------------------------------------------------------------------
      // 5. Work, and the machine. The tracer is asked for a job when the
      //    machine has none — and, in Chase, whenever a stroke ends, so the
      //    next stroke is always from the frame on screen now.
      //------------------------------------------------------------------
      machine.setStepSize(stepSizeFromParam(p('stepSize')));

      const chase = p('chase') >= 0.5;
      samples.clear();
      let remaining = frameSeconds;

      for (let guard = 0; guard < 8 && remaining > 0.0; guard += 1) {
        if (machine.jobDone()) {
          // An idle machine on a blank clip would otherwise pay a readback
          // every frame for nothing.
          idleSeconds += guard === 0 ? frameSeconds : 0.0;
          if (machine.hasJob() || idleSeconds >= kIdleRetrySeconds || traceCount === 0) {
            idleSeconds = 0.0;
            takeWork(p, traceWidth, traceHeight, aspect);
          }
          if (machine.jobDone()) break;
        }
        remaining = machine.advance(remaining, chase, samples);
        if (remaining > 0.0 && chase && machine.strokeEnded && !machine.jobDone()) {
          takeWork(p, traceWidth, traceHeight, aspect);
        }
      }
      telemetry.cpuMillis = performance.now() - cpuStart;

      //------------------------------------------------------------------
      // 6. Ink along the path.
      //------------------------------------------------------------------
      const nibSigma = penWidthFromParam(p('penWidth'));
      const inkRate = inkRateFor(flowFromParam(p('flow')), nibSigma);
      ink.deposit(samples, samples.count, {
        nibSigma,
        inkRate,
        travelRate: p('showTravel') >= 0.5 ? kTravelInk * inkRate : 0.0,
      }, aspect);

      //------------------------------------------------------------------
      // 7. Composite, straight to the canvas. The kit bound it and set the
      //    viewport before calling us, and several framebuffers of other
      //    sizes have been bound since.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      const paper = clamp(Math.round(p('paper')), 0, PAPER_COUNT - 1);
      const colour = paperColour(paper);
      const carriage = machine.position;
      const penInk = machine.ink;

      compositeShader.use();
      bindTexture(gl, 0, copyBuffer.texture);
      bindTexture(gl, 1, ink.texture);
      compositeShader.setSampler('CopyTexture', 0);
      compositeShader.setSampler('PaperTexture', 1);
      compositeShader.set('PaperMode', paper);
      compositeShader.set('PaperColour', colour[0], colour[1], colour[2]);
      compositeShader.set('Ghost', kGhostLevel);
      compositeShader.set('MixAmount', p('mix'));
      compositeShader.set('Aspect', aspect);
      compositeShader.set('ShowCarriage', p('showCarriage') >= 0.5 ? 1.0 : 0.0);
      compositeShader.set('CarriagePos', carriage.x, carriage.y);
      compositeShader.set('CarriageRadius', kCarriageRadius);
      compositeShader.set('NibSigma', nibSigma);
      compositeShader.set('CarriageInk', penInk[0], penInk[1], penInk[2]);
      compositeShader.set('PenDown', machine.penDown ? 1.0 : 0.0);
      quad.draw();

      // What the line under the canvas reports.
      telemetry.sheetSeconds = sheetSeconds;
      telemetry.autoSheet = autoSheet;
      telemetry.sheets = sheetCount;
      telemetry.strokesDone = machine.strokesCompleted;
      telemetry.jobElapsed = machine.jobElapsed;
      telemetry.jobSeconds = machine.jobSeconds;
      telemetry.penChanges = machine.penChangesMade;
      telemetry.penDown = machine.penDown;
      telemetry.pen = machine.pen;
      telemetry.idle = machine.jobDone();
    },
  };

  return rendererInstance;
}

//===========================================================================
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: the About block, which is a text line and four buttons that open a
// browser.
//===========================================================================

/// Pens is FF_TYPE_INTEGER, 1..8, exempt from the 0..1 clamp, so the plugin
/// stores the integer itself. The kit has no integer control, so it is a
/// dropdown of its eight values; the index into the dropdown is not the
/// plugin's value, `pensValue` converts.
const PENS_ELEMENTS = ['1', '2', '3', '4', '5', '6', '7', '8'];
const pensValue = (index) => 1 + clamp(Math.round(index), 0, kPensPerPalette - 1);
const pensIndex = (value) => value - 1;

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const seconds = (v) => `${v.toFixed(3)} s`;

const demo = mountDemo({
  name: 'Plotter',
  pluginId: 'PL01',
  kind: 'effect',
  tagline:
    'A pen plotter drawing the clip. It traces the outlines in the current frame and draws them with a slow pen — a speed limit, an acceleration limit, a lift and a drop between strokes, a trip to the carousel to change colour — onto paper the ink stays on. By the time it is a few strokes in the clip has moved on, so the sheet is a collage of the moments each stroke was drawn. The line is heavy where the pen slows and thin on the straights, every pen-down leaves a blot, a coarse Step Size draws staircases: none of it is drawn, it falls out of the machine. The shaders here are the plugin’s own; the tracer, the planner, the machine and the paper’s bookkeeping are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/plotter',
  page: 'https://stoatworks-labs.com/software/plotter/',

  // The stock sentence says "same maths", which is only half true here: the
  // shaders are the plugin's, the machine between them is a port.
  blurb:
    'It is Plotter’s own GLSL, ported from the repository to WebGL2, with the CPU half — galvo’s tracer, the planner, the machine that advances by real elapsed time and the paper’s bookkeeping — ported to JavaScript by hand; nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // Paper = Alpha puts the ink over nothing, premultiplied, for the layer
  // below — so what sits behind it is a real question.
  showBackdrop: true,

  // The paper is a float buffer the ink pass adds into by blending, and the
  // detect chain runs in RGBA16F. Eight bits would quantise the deposit, and
  // the deposit is the whole claim.
  needFloat: true,

  params: [
    std('maxSpeed', 'Max Speed', 0.6736, 'Machine', {
      display: (v) => `${maxSpeedFromParam(v).toFixed(3)} heights/s`,
      hint: 'The pen’s cruising speed, in paper heights per second, 0.05 to 2 geometrically. A real A3 plotter does about 1.3; the bottom is a pen you can watch think. Halve it and the whole drawing darkens, because ink goes down per unit of time.',
    }),
    std('acceleration', 'Acceleration', 0.6962, 'Machine', {
      display: (v) => `${accelerationFromParam(v).toFixed(2)} heights/s²`,
      hint: 'Paper heights per second squared, 0.1 to 20. At the default speed the pen takes v/a to reach cruise over v²/2a of paper, which is what makes the heavy ends of a stroke a few pixels long rather than invisible or the whole stroke.',
    }),
    std('penSettle', 'Pen Settle', 0.16, 'Machine', {
      display: (v) => seconds(penSettleFromParam(v)),
      hint: 'How long the pen rests on the paper at pen-down before it moves, and at the end of a stroke before it lifts, 0 to 0.5 s. Every one of those rests is a blot.',
    }),
    std('stepSize', 'Step Size', 0.177, 'Machine', {
      display: (v) => (stepSizeFromParam(v) === 0 ? 'off' : `${stepSizeFromParam(v).toFixed(4)} heights`),
      hint: 'The stepper pitch in paper heights: off at the bottom, then 0.0005 to 0.025. Half a pixel at 1080p is where quantisation stops showing; 0.025 is a staircase anyone can see, k steps by one for a slope of 1/k.',
    }),
    std('cornerAngle', 'Corner Angle', 0.4706, 'Machine', {
      display: (v) => `${cornerAngleFromParam(v).toFixed(1)}°`,
      hint: 'A vertex whose turn is this sharp or sharper is a corner: the pen stops there and leaves from rest. A gentler turn is taken at a speed that falls linearly from cruise at no turn to zero here — which is what puts more ink on a tighter bend.',
    }),

    opt('pens', 'Pens', PENS_ELEMENTS, pensIndex(4), 'Pens',
      'How many of the palette are on the carousel. FF_TYPE_INTEGER in the plugin, a dropdown here. With Optimise the driver does every stroke of one pen before touching the next; every change is a trip off the top-left corner of the sheet and a 0.35 s wait.'),
    std('penWidth', 'Pen Width', 0.3187, 'Pens', {
      display: (v) => `σ ${penWidthFromParam(v).toFixed(4)}`,
      hint: 'The nib’s Gaussian sigma as a fraction of the paper height, 0.0012 to 0.012. A wider nib carries proportionally more ink, as a real one does, so Flow means the same darkness at every width.',
    }),
    std('flow', 'Flow', 0.6, 'Pens', {
      display: (v) => `absorbance ${flowFromParam(v).toFixed(2)}`,
      hint: 'The ink deposited at the reference speed of 0.35 heights/s, as an absorbance, 0.25 to 8. An absorbance of 1 transmits 37% of the paper’s light; 3 is black. Ink is subtractive — a red pen absorbs green and blue.',
    }),
    opt('palette', 'Palette', ['Technical', 'Black', 'Neon', 'Source'], 0, 'Pens',
      'Technical is black and seven drawing-office colours; Black is every pen black, so Pens only sorts; Neon is the marker set; Source is the stroke’s own colour with its value capped at 0.7 so a white edge still draws, sorted onto the Technical pens. Greys go to the black pen before any distance is measured.'),
    bool('optimise', 'Optimise', 1, 'Pens',
      'Group the strokes by pen — the pen the carriage holds first — and take each group nearest-start-first, reversing an open stroke or rotating a closed one to suit. Off, the tracer’s order, pens interleaved, and a change of pen nearly every stroke.'),

    std('threshold', 'Threshold', 0.5, 'Path', {
      display: (v) => thresholdFromParam(v).toFixed(3),
      hint: 'The gradient magnitude at which a pixel is an edge, 0.02 to 1 geometrically. A clean black-to-white step measures 1.0; footage is low in luma contrast, so the default is 0.14 rather than galvo’s 0.25.',
    }),
    std('detail', 'Detail', 0.2, 'Path', {
      display: (v) => `${detailFromParam(v).toFixed(2)} mip levels`,
      hint: 'Mip levels above the trace resolution that the Sobel runs at, 0 to 3. 0 detects at the trace buffer’s own pixel; 3 finds only the shape of a logo and ignores everything inside it.',
    }),
    bool('chase', 'Chase', 0, 'Path',
      'Re-trace at the end of every stroke, so the next stroke always comes from the frame on screen now. Off, the pen finishes the job it took before it looks at the clip again.'),

    std('autoSheet', 'Auto Sheet', 0.5963, 'Sheet', {
      display: (v) => (autoSheetFromParam(v) === 0 ? 'never' : `${autoSheetFromParam(v).toFixed(1)} s`),
      hint: 'How long a sheet stays on the machine before it is torn off and the next job starts on a clean one: never at the bottom, then 1 to 300 s. A finished job on a still clip is re-traced onto the same sheet until then — the pen goes over its own lines, as a plotter does.',
    }),
    // New Sheet, FF_TYPE_EVENT, sits here in the plugin's order. The kit has no
    // event control; a button is inserted at this position after the panel is
    // built (see below).
    opt('paper', 'Paper', ['White', 'Cream', 'Ghost', 'Clip', 'Alpha'], 2, 'Sheet',
      'Ghost is white paper with the clip printed faintly on it, so the frame is never empty while the pen thinks. Clip is the clip as the paper. Alpha is ink over nothing, premultiplied, for the layer below. The sheet is opaque in every other mode whatever the clip’s alpha.'),
    bool('showCarriage', 'Show Carriage', 1, 'Sheet',
      'The pen carriage: a ring, with the nib as a dot in its ink, drawn in paper units so it is round at any raster. Grey with the pen up, the pen’s colour with it down.'),
    bool('showTravel', 'Show Travel', 0, 'Sheet',
      'A faint pencil line along the pen-up moves — between strokes, and to and from the carousel — at 8% of the pen’s ink rate.'),
    std('mix', 'Mix', 1.0, 'Sheet'),
  ],

  // Outlines that a pen can follow. A logo-shaped outline on transparency is
  // what the plugin is for; the moving scene shows the collage of moments; the
  // geometry card is fine lines at a 320-wide trace and finds little, which is
  // correct and looks like a fault.
  sources: ['alpha', 'scene', 'spot', 'bars', 'grid', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Watch it think': { maxSpeed: 0.25, acceleration: 0.35, penSettle: 0.45 },
    'Coarse steps': { stepSize: 0.92, penWidth: 0.12, maxSpeed: 0.45 },
    'Heavy corners': { cornerAngle: 0.05, acceleration: 0.4, penSettle: 0.3, flow: 0.7 },
    'Black pen, chasing': { palette: 1, pens: pensIndex(1), chase: 1 },
    'Eight neon markers on cream': { palette: 2, pens: pensIndex(8), paper: 1, penWidth: 0.6 },
    'Source colours': { palette: 3, pens: pensIndex(8) },
    'Show the travel': { showTravel: 1 },
    'Ink for the layer below': { paper: 4 },
    'Fast machine': { maxSpeed: 0.95, acceleration: 0.95, penSettle: 0.04 },
    'Never tear off': { autoSheet: 0 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Plotter is not a shader: it is a GPU edge detector, a synchronous readback, and then galvo’s contour tracer, a planner that turns strokes into trapezoidal motion blocks with junction speeds, a machine that advances through those blocks by real elapsed time and quantises to the step grid, and the paper’s bookkeeping — Tracer.cpp, Planner.cpp, Machine.cpp, Controls.cpp and the CPU side of render/Ink.cpp. All of it is ported here function for function, because without it the page would have nothing to draw. Nothing checks a port but a reader; the repository’s pltest --plan, --trapezoid, --ink, --budget, --steps, --pens, --persist and --trace check the C++ and have never heard of this page.',
    'The GPU half is not a port. The detect chain, the ink pass and the composite are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the nine pieces drifts — the ink pass is assembled from three of them at run time, here as in the plugin.',
    'The machine runs on this page’s clock, advanced by the real time between renders through the plugin’s own clamp of 1/240 to 1/4 s, so the drawing accumulates as it does in a host. Pause stops the machine; Step advances it one sixtieth of a second; Restart restarts the clip’s clock and not the sheet. The sheet is torn off by New Sheet — the plugin’s FF_TYPE_EVENT, a button here — or by Auto Sheet. The unit vote the plugin runs against Resolume’s millisecond clock never runs here, because the page declares seconds, as the repository’s harness does.',
    'The paper is a float buffer only where WebGL2 allows one. The plugin’s paper is RGBA32F, and it measured why: a half-float paper loses ink on every add, 0.8% low on a long stroke and 2.9% on a short one. Here the paper is RGBA32F when the browser offers EXT_float_blend and OES_texture_float_linear, and RGBA16F — carrying exactly that bias — when it does not; the line under the canvas says which. Without EXT_color_buffer_float at all the page refuses to start rather than accumulate ink into eight bits.',
    'The page traces at the plugin’s own fixed trace width of 320 and reads the gradient-and-colour buffer back with readPixels only when the machine asks for work, which stalls the pipeline exactly where the plugin’s glReadPixels does. The two stabilise buffers are RGBA8 here rather than RGBA16F, because WebGL2 will not read a float framebuffer as bytes; the bytes the tracer thresholds and the strokes are coloured from are the same bytes in both. What is coarser here is the stabilised gradient the temporal filter feeds back to itself.',
    'Pens is FF_TYPE_INTEGER in the plugin, 1 to 8. The kit has no integer control, so it is a dropdown of its eight values.',
    'JavaScript numbers are doubles. The plugin’s Vec2 is float and its profile arithmetic double; here everything between the readback and the GPU is double, except the samples uploaded to the ink pass, which are Float32 as the plugin’s Sample is.',
    'The plugin’s Perturb test hooks, SetJobForTest and ReadPaperForTest are not ported: none of them is part of what the plugin does in a host, and the negative controls they exist for run only in the repository’s harness.',
    'There is no audio caveat on this page: Plotter has no audio path.',
    'The plugin’s proof — a stroke’s timing against L/v + v/a, the ink along it against 1/v, the strokes on the sheet after t seconds against the schedule, a staircase against the step pitch, pen changes against the pen count, the sheet through a resize, and every one of those failing on a deliberately broken plugin — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported machine did.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// New Sheet. The plugin's one event, between Auto Sheet and Paper in the Sheet
// group; the kit builds no control for an event, so the button goes in by hand
// at the same position. Skipped in embed mode, where there is no inspector.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const after = document.querySelector('#p-autoSheet')?.closest('.prow');
  if (after && rendererInstance) {
    const row = document.createElement('div');
    row.className = 'prow prow--boolean';
    const label = document.createElement('label');
    label.className = 'prow__name';
    label.textContent = 'New Sheet';
    label.title = 'Tear the paper off: the sheet is cleared and the machine forgets its job. FF_TYPE_EVENT in the plugin; a press is remembered until the next frame.';
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'btn';
    button.id = 'p-newSheet';
    button.textContent = 'Tear off';
    button.addEventListener('click', () => {
      rendererInstance.newSheet();
      demo.redraw();
    });
    row.append(label, button);
    after.after(row);
  }
}

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported machine's own numbers: the
// paper's format, time on the sheet, where the machine is in its job, strokes
// finished, pen changes made, and the CPU cost. Not a measurement of anything.
// Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);

    setInterval(() => {
      const t = telemetry;
      const paper = t.paper === 'RGBA32F'
        ? 'Paper RGBA32F, as the plugin’s.'
        : 'Paper RGBA16F: this browser has no EXT_float_blend or OES_texture_float_linear, so the sheet carries the half-float bias the plugin rejected.';
      const sheet = t.autoSheet > 0
        ? `Sheet ${t.sheetSeconds.toFixed(1)} s of ${t.autoSheet.toFixed(0)} s`
        : `Sheet ${t.sheetSeconds.toFixed(1)} s, never torn off`;
      const job = t.idle
        ? (t.contours === 0 && t.traces > 0
          ? 'Nothing in the clip is over the threshold; the machine is idle and asks again every 0.25 s.'
          : 'The machine is between jobs.')
        : `Job: ${t.strokesInJob} stroke${t.strokesInJob === 1 ? '' : 's'}, ${t.jobElapsed.toFixed(1)} of ${t.jobSeconds.toFixed(1)} s, pen ${t.penDown ? 'down' : 'up'}${t.pen >= 0 ? ` holding pen ${t.pen + 1}` : ''}.`;
      line.textContent =
        `${sheet}, ${t.sheets} torn off. ${job} ${t.strokesDone} strokes drawn, ${t.penChanges} pen changes. `
        + `${t.cpuMillis.toFixed(2)} ms on the CPU half this frame; the last trace and plan took ${t.traceMillis.toFixed(1)} ms. ${paper}`;
    }, 250);
  }
}

// Keep the ported-but-unused functions reachable for a reader comparing against
// the C++: the plugin's harness uses these; the page does not.
export { restToRestSeconds, speedInBlock };
