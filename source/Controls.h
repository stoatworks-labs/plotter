#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every FF_TYPE_STANDARD parameter this plugin declares is a plain float in
    0..1, including the ones that stand for a speed, a time or a length.
    `SetParamInfo` clamps a standard default into 0..1 *before* returning, and
    `SetParamRange` can only be called afterwards -- so a parameter declared in
    seconds cannot declare a default in seconds. The conversions live here
    instead, in one file the plugin and the harness both use, so there is only
    ever one answer to what a slider position means.

    Units. Lengths are in **paper heights**: x runs 0..aspect, y runs 0..1 and
    y is up. Speeds are paper heights per second, accelerations paper heights
    per second squared, times in seconds. The picture's raster never enters:
    a plotter set up on a 720p composition draws the same picture, at the same
    pace, on a 4K one.

    Where a mapping is geometric rather than linear it is because the
    interesting range is at one end. A slider is a fixed number of pixels wide
    and spending half of them on a range nobody uses is the difference between
    a control that works and one with a sweet spot at 0.03.

    Option parameters are mapped by INDEX here (an option's range reads back
    0..1 from the host whatever its element count), and the integer parameter
    `Pens` passes through untouched, because FF_TYPE_INTEGER is exempt from the
    clamp.
*/
namespace plotter
{

//--- Machine ----------------------------------------------------------------

/// 0.05 to 2.0 paper heights per second, geometrically. The pen's cruising
/// speed. A real A3 plotter does about 0.4 m/s on 0.3 m of paper, which is
/// 1.3 heights a second and towards the top; the bottom is a pen you can
/// watch think. The default is 0.6.
float MaxSpeedFromParam( float value );

/// 0.1 to 20 paper heights per second squared, geometrically. At the default
/// speed the pen takes v / a to reach cruise: 0.6 / 4 = 0.15 s, over a
/// distance v^2 / 2a of 0.045 heights, which is what makes the heavy ends of
/// a stroke a few pixels long rather than invisible or the whole stroke.
float AccelerationFromParam( float value );

/// 0 to 0.5 s, linear. How long the pen rests on the paper at pen-down before
/// it moves, and at the end of a stroke before it lifts. Every one of those
/// rests is a blot.
float PenSettleFromParam( float value );

/// The stepper pitch in paper heights: 0 (no quantisation) at the bottom of
/// the slider, then 0.0005 to 0.025 geometrically. 0.0005 of 1080 lines is
/// half a pixel, which is where quantisation stops showing; 0.025 is a
/// staircase anyone can see.
float StepSizeFromParam( float value );

/// 5 to 90 degrees, linear. A vertex whose turn is this sharp or sharper is
/// a corner: the pen comes to a stop there and leaves again from rest. A
/// gentler turn is taken at a speed that falls linearly from the cruise
/// speed at no turn to zero here.
float CornerAngleFromParam( float value );

//--- Pens -------------------------------------------------------------------

/// The pen's Gaussian sigma as a fraction of the paper height, 0.0012 to
/// 0.012, geometrically. 0.0012 of 1080 lines is 1.3 pixels -- a 0.1 mm
/// technical pen on A3 is about 0.0003, but that is below a pixel at every
/// raster this runs at, so the range starts where a line can be seen.
float PenWidthFromParam( float value );

/// The ink deposited at the reference speed, as an absorbance: 0.25 to 8,
/// geometrically. An absorbance of 1 transmits 37% of the paper's light; 3
/// is black. See InkRate for how it becomes a deposit per second.
float FlowFromParam( float value );

/// The pen deposits ink per unit TIME, and this is the rate in absorbance x
/// paper-height^2 per second: the quantum the renderer spreads over however
/// far the pen moved in an interval.
///
/// Defined so that a line drawn at kReferenceSpeed with a pen of sigma
/// carries the Flow absorbance at its centre, whatever the pen width: a line
/// at speed v has Flow / (v sigma sqrt(2 pi)) per unit area at its centre,
/// so the rate is Flow x v_ref x sigma x sqrt(2 pi). A wider pen therefore
/// carries proportionally more ink, as a wider nib does, and the operator's
/// Flow means the same darkness at every width. Halve the speed and the line
/// is twice as dark; that is the plugin.
double InkRate( float flow, float penSigma );

/// The speed at which Flow is the line's absorbance, in paper heights per
/// second. Not the current Max Speed, so that Max Speed changes the ink weight
/// the way it does on a real machine.
constexpr float kReferenceSpeed = 0.35f;

/// The named palettes, each eight pens. `Pens` takes the first N.
enum class Palette
{
	Technical = 0, ///< black and seven drawing-office colours
	Black,         ///< every pen black: the pen count only changes the sorting
	Neon,          ///< saturated markers
	Source,        ///< the stroke's own colour, sorted onto the Technical pens
	Count
};

constexpr int kPensPerPalette = 8;

/// The ink colour of pen `index` in `palette`, 0..1 RGB. For Source, the
/// colour the stroke was traced with is passed in and comes back capped, so
/// a white edge on a black clip is a grey line on white paper rather than
/// nothing.
void PenColour( Palette palette, int index, const float source[ 3 ], float out[ 3 ] );

/// Which pen of the first `pens` in `palette` is nearest to a source colour.
/// For Source the Technical pens are the sort key.
int NearestPen( Palette palette, int pens, const float source[ 3 ] );

//--- Path -------------------------------------------------------------------

/// 0.02 to 1.0, geometrically. The gradient magnitude at which a pixel is an
/// edge; a clean black-to-white step measures 1.0, so the useful range for
/// footage is below 0.2 and for artwork around 0.3. galvo's mapping.
float ThresholdFromParam( float value );

/// 0 to 3, linear. Mip levels *above* the trace resolution that the Sobel
/// runs at: 0 detects at the trace buffer's own pixel, 3 finds only the shape
/// of a logo and ignores everything inside it. galvo's mapping.
float DetailFromParam( float value );

//--- Sheet ------------------------------------------------------------------

/// 0 (never) at the bottom of the slider, then 1 to 300 s geometrically: how
/// long a sheet stays on the machine before it is torn off and the next job
/// starts on a clean one.
float AutoSheetFromParam( float value );

enum class Paper
{
	White = 0,
	Cream,
	Ghost, ///< white paper with the clip printed faintly on it
	Clip,  ///< the clip is the paper
	Alpha, ///< ink over nothing, premultiplied, for the layer below
	Count
};

/// The paper's colour for the two plain papers.
void PaperColour( Paper paper, float out[ 3 ] );

/// How faintly the clip shows through Ghost paper.
constexpr float kGhostLevel = 0.22f;

//--- Constants the controls do not reach -------------------------------------

/// The trace resolution, in pixels across. galvo makes this a control; here
/// it is fixed, because a plotter's resolution is its stepper pitch and the
/// tracer's is only where the path comes from.
constexpr int kTraceWidth = 320;

/// Contours shorter than this, in trace pixels, are not worth a pen-down.
constexpr float kMinLength = 8.0f;

/// Douglas-Peucker tolerance in trace pixels. galvo's default.
constexpr float kSimplify = 1.15f;

/// The temporal filter's two halves (galvo's Stability at its default).
/// Asymmetric on purpose: rise nearly instantly, fall slowly.
constexpr float kAttack  = 0.91f;
constexpr float kRelease = 0.25f;

/// Where the pen carousel is, in paper units: just off the top-left corner
/// of the sheet. A pen change is a trip there and back.
constexpr float kCarouselX = 0.0f;
constexpr float kCarouselY = 1.04f;

/// How long the carriage rests at the carousel while the pen is swapped.
constexpr float kPenChangeSeconds = 0.35f;

/// Travel (pen up) moves at this multiple of Max Speed. Real machines rapid
/// faster than they draw; three is a plotter, not a router.
constexpr float kTravelSpeedFactor = 3.0f;

/// The faint line Show Travel draws, as a fraction of the pen's ink rate.
constexpr float kTravelInk = 0.08f;

/// The pen carriage drawn by Show Carriage: its radius in paper heights.
constexpr float kCarriageRadius = 0.018f;

/// A source colour whose chroma (max - min) is under this fraction of its
/// value is a grey, and a grey is drawn with the black pen.
constexpr float kGreyChroma = 0.3f;

/// Source ink is capped at this value (HSV V) so a white edge still draws.
constexpr float kSourceInkCap = 0.7f;

/// The average colour along a stroke is taken every this many trace pixels.
constexpr float kColourSampleSpacing = 2.0f;

} // namespace plotter
