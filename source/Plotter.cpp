#include "Plotter.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace plotter;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Plotter >,                                 // Create method
	"PL01",                                                   // Plugin unique ID of maximum length 4.
	"SW Plotter",                                             // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"A pen plotter drawing the clip. It traces the outlines in the current frame and draws them with a slow pen -- a speed limit, an acceleration limit, a lift and a drop between strokes -- onto paper the ink stays on. By the time it is a few strokes in the clip has moved on, so the sheet is a collage of the moments each stroke was drawn.\n\nThe line is heavy where the pen slows and thin on the straights, every pen-down leaves a blot, a coarse Step Size draws staircases, and a change of pen is a trip to the carousel. None of it is drawn; it falls out of the machine.\n\nNew Sheet tears the paper off; Auto Sheet does it on a timer.",// Plugin description
	"Plotter FFGL effect"                                     // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kPaletteNames[] = { "Technical", "Black", "Neon", "Source" };
const char* const kPaperNames[]   = { "White", "Cream", "Ghost", "Clip", "Alpha" };
constexpr int kPaletteCount       = static_cast< int >( Palette::Count );
constexpr int kPaperCount         = static_cast< int >( Paper::Count );

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// A host frame is between these, whatever the clock says. Shorter is a
/// scrub or a stall; longer is the machine having been asleep, and a plotter
/// asleep for a minute should not draw a minute of strokes in one frame.
constexpr double kMinFrame = 1.0 / 240.0;
constexpr double kMaxFrame = 0.25;

/// How often an idle machine (a blank clip) asks the tracer again.
constexpr double kIdleRetrySeconds = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

double millisSince( const std::chrono::steady_clock::time_point& since )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - since ).count();
}
} // namespace

Plotter::Plotter()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the machine's clock where it can, so that rendering the
	//same frame twice gives the same picture twice.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a four-pen technical plotter at 0.6 sheet-heights a
	// second drawing black, red, blue and green on white paper with the clip
	// ghosted under it, tearing the sheet off every thirty seconds -- so
	// there is ink on the sheet inside the first second and a half of a clip
	// and the frame is never empty while the pen thinks. Measured on
	// Resolume's own demo clips through `pltest --pipe`, 2026-09-24: at the
	// first cut (0.35 heights/s, 2 heights/s^2, rapids at 2x) six seconds
	// had drawn two short strokes, which is a dead video.
	//---------------------------------------------------------------------
	params[ PT_MAX_SPEED ]    = 0.6736f;//0.6 heights/s
	params[ PT_ACCELERATION ] = 0.6962f;//4.0 heights/s^2
	params[ PT_PEN_SETTLE ]   = 0.16f;  //0.08 s
	params[ PT_STEP_SIZE ]    = 0.177f; //0.001 heights
	params[ PT_CORNER_ANGLE ] = 0.4706f;//45 degrees

	params[ PT_PENS ]      = 4.0f;
	params[ PT_PEN_WIDTH ] = 0.3187f;//sigma 0.0025
	params[ PT_FLOW ]      = 0.6f;   //absorbance 2.0 at the reference speed
	params[ PT_PALETTE ]   = 0.0f;   //Technical
	params[ PT_OPTIMISE ]  = 1.0f;

	params[ PT_THRESHOLD ] = 0.50f;//0.14 after mapping: footage, not artwork
	params[ PT_DETAIL ]    = 0.20f;
	params[ PT_CHASE ]     = 0.0f;

	params[ PT_AUTO_SHEET ]    = 0.5963f;//30 s
	params[ PT_NEW_SHEET ]     = 0.0f;
	params[ PT_PAPER ]         = 2.0f;//Ghost
	params[ PT_SHOW_CARRIAGE ] = 1.0f;
	params[ PT_SHOW_TRAVEL ]   = 0.0f;
	params[ PT_MIX ]           = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every FF_TYPE_STANDARD parameter is a plain 0..1 float:
	// SetParamInfo clamps a STANDARD default into 0..1 *before* a range can
	// be attached (SDK b1afaf9), so the conversions live in Controls.cpp.
	// FF_TYPE_INTEGER is exempt, which is why Pens really is 1..8.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	SetParamInfof( PT_MAX_SPEED, "Max Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_ACCELERATION, "Acceleration", FF_TYPE_STANDARD );
	SetParamInfof( PT_PEN_SETTLE, "Pen Settle", FF_TYPE_STANDARD );
	SetParamInfof( PT_STEP_SIZE, "Step Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_CORNER_ANGLE, "Corner Angle", FF_TYPE_STANDARD );

	SetParamInfo( PT_PENS, "Pens", FF_TYPE_INTEGER, params[ PT_PENS ] );
	SetParamRange( PT_PENS, 1.0f, static_cast< float >( kPensPerPalette ) );
	SetParamInfof( PT_PEN_WIDTH, "Pen Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLOW, "Flow", FF_TYPE_STANDARD );
	declareOptions( PT_PALETTE, "Palette", kPaletteNames, kPaletteCount );
	SetParamInfo( PT_OPTIMISE, "Optimise", FF_TYPE_BOOLEAN, params[ PT_OPTIMISE ] >= 0.5f );

	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_DETAIL, "Detail", FF_TYPE_STANDARD );
	SetParamInfo( PT_CHASE, "Chase", FF_TYPE_BOOLEAN, params[ PT_CHASE ] >= 0.5f );

	SetParamInfof( PT_AUTO_SHEET, "Auto Sheet", FF_TYPE_STANDARD );
	SetParamInfo( PT_NEW_SHEET, "New Sheet", FF_TYPE_EVENT, false );
	declareOptions( PT_PAPER, "Paper", kPaperNames, kPaperCount );
	SetParamInfo( PT_SHOW_CARRIAGE, "Show Carriage", FF_TYPE_BOOLEAN, params[ PT_SHOW_CARRIAGE ] >= 0.5f );
	SetParamInfo( PT_SHOW_TRAVEL, "Show Travel", FF_TYPE_BOOLEAN, params[ PT_SHOW_TRAVEL ] >= 0.5f );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses RUNS of consecutive same-group ids, so the enum
	//order is load-bearing: append only.
	for( FFUInt32 i = PT_MAX_SPEED; i <= PT_CORNER_ANGLE; ++i )
		SetParamGroup( i, "Machine" );
	for( FFUInt32 i = PT_PENS; i <= PT_OPTIMISE; ++i )
		SetParamGroup( i, "Pens" );
	for( FFUInt32 i = PT_THRESHOLD; i <= PT_CHASE; ++i )
		SetParamGroup( i, "Path" );
	for( FFUInt32 i = PT_AUTO_SHEET; i <= PT_MIX; ++i )
		SetParamGroup( i, "Sheet" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Plotter effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Plotter::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &edgeShader, kEdgeShader, "edge" },
		{ &stabiliseShader, kStabiliseShader, "stabilise" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect simply
		//does nothing in Resolume, with no message anywhere. These two lines
		//are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Plotter: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !ink.InitGL() )
	{
		FFGLLog::LogToHost( "Plotter: ink renderer failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	historyValid = false;
	machine.Reset();
	sheetSeconds = 0.0;

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
MachineParams Plotter::machineParams() const
{
	MachineParams mp;
	mp.maxSpeed       = MaxSpeedFromParam( params[ PT_MAX_SPEED ] );
	mp.acceleration   = AccelerationFromParam( params[ PT_ACCELERATION ] );
	mp.settle         = PenSettleFromParam( params[ PT_PEN_SETTLE ] );
	mp.cornerAngleDeg = CornerAngleFromParam( params[ PT_CORNER_ANGLE ] );
	if( perturb & kPerturbInfiniteAccel )
		mp.acceleration = 1.0e6;//a million heights per second squared: cruise from the first instant
	if( perturb & kPerturbNoStopAtCorner )
		mp.cornerAngleDeg = 181.0;//no turn is that sharp
	return mp;
}

void Plotter::takeWork( int traceWidth, int traceHeight, float aspect, bool chase )
{
	const auto start = std::chrono::steady_clock::now();

	if( !plantedJob )
	{
		//-----------------------------------------------------------------
		// The one synchronous readback: four bytes per trace texel of the
		// buffer the stabilise pass just wrote -- the gradient and the colour
		// together, 230 KB at the default size -- and a pipeline stall. It
		// happens only on the frames that ask for work, which on a busy
		// picture is once a job and in Chase once a stroke.
		//-----------------------------------------------------------------
		readback.resize( static_cast< std::size_t >( traceWidth ) * traceHeight * 4 );
		{
			ScopedFBOBinding fbo( stableBuffer[ stableCurrent ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, traceWidth, traceHeight, GL_RGBA, GL_UNSIGNED_BYTE, readback.data() );
		}

		//Threshold on the CPU. The tracer wants a binary mask, and the byte it
		//gets is the stabilised gradient, so the threshold is one compare.
		const float threshold   = ThresholdFromParam( params[ PT_THRESHOLD ] );
		const unsigned char cut = static_cast< unsigned char >( std::clamp( std::lround( threshold * 255.0f ), 1L, 255L ) );
		mask.resize( static_cast< std::size_t >( traceWidth ) * traceHeight );
		for( std::size_t i = 0; i < mask.size(); ++i )
			mask[ i ] = readback[ i * 4 ] >= cut ? 255 : 0;

		TraceParams tp;
		tp.threshold = 128;
		tp.simplify  = kSimplify;
		tp.minLength = kMinLength;
		tracer.Trace( mask.data(), traceWidth, traceHeight, tp, contours );

		const Palette palette = static_cast< Palette >( std::clamp( static_cast< int >( std::lround( params[ PT_PALETTE ] ) ), 0, kPaletteCount - 1 ) );
		const int pens        = std::clamp( static_cast< int >( std::lround( params[ PT_PENS ] ) ), 1, kPensPerPalette );
		BuildStrokes( contours, traceWidth, traceHeight, aspect, readback.data(), palette, pens, strokes );
	}
	else
	{
		//A test job: the same strokes again, planned from where the carriage
		//is, exactly as a traced job would be.
		strokes        = plantedStrokes;
		plantedPlanned = true;
	}

	//The plan starts from where the carriage is and what it is holding. In
	//Chase the previous job is thrown away mid-way, which is the point: the
	//next stroke comes from now.
	(void)chase;
	const Vec2 cursor    = machine.Position();
	const int currentPen = machine.Pen();
	if( params[ PT_OPTIMISE ] >= 0.5f )
		OrderStrokes( strokes, cursor, currentPen, true, !( perturb & kPerturbNoPenSort ) );

	const int changes = PlanBlocks( strokes, machineParams(), cursor, currentPen, blocks );
	machine.SetJob( blocks, changes );

	++traceCount;
	tracedLastFrame = true;
	traceMillis     = millisSince( start );
}

//---------------------------------------------------------------------------
FFResult Plotter::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );
	const float aspect      = static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight );

	//The host's viewport, read before anything of ours changes it.
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding (SDK b1afaf9). Every pass's ResizeViewPort() leaks
	//into the pass after it, and the composite -- which draws to the host's own
	//framebuffer -- inherits whatever the last pass left.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. Normalise the host's clock to seconds and take the frame delta
	// from it, clamped. That delta is what the machine advances by.
	//---------------------------------------------------------------------
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
	double frameSeconds = 1.0 / 60.0;
	if( lastHostTime >= 0.0 )
		frameSeconds = std::clamp( now - lastHostTime, kMinFrame, kMaxFrame );
	lastHostTime = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// FFGLFBO::Initialise sizes its colour texture under a Scoped binding, and
	// every ffglex Scoped* binding *clears* to 0 on scope exit rather than
	// restoring what was there.
	//---------------------------------------------------------------------
	const int traceWidth  = std::min( kTraceWidth, pictureWidth );
	const int traceHeight = std::max( 8, static_cast< int >( std::lround( traceWidth / aspect ) ) );

	const bool traceSizeChanged = !stableBuffer[ 0 ].IsValid()
	                              || static_cast< int >( stableBuffer[ 0 ].GetWidth() ) != traceWidth
	                              || static_cast< int >( stableBuffer[ 0 ].GetHeight() ) != traceHeight;

	const bool allocated =
		copyBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& edgeBuffer.Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& stableBuffer[ 0 ].Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& stableBuffer[ 1 ].Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& ink.Ensure( pictureWidth, pictureHeight, aspect, ( perturb & kPerturbResizeClears ) != 0 );

	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}
	if( traceSizeChanged )
		historyValid = false;

	//---------------------------------------------------------------------
	// 1. The picture, into a texture of ours, with a mip chain on it.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel", 0.5f / static_cast< float >( pictureWidth ), 0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
	}
	copyBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. Edge and colour, at the trace resolution.
	//---------------------------------------------------------------------
	{
		const float baseLod = std::log2( std::max( 1.0f, static_cast< float >( pictureWidth ) / static_cast< float >( traceWidth ) ) );
		const float detail  = DetailFromParam( params[ PT_DETAIL ] );
		const float spread  = std::exp2( detail );

		ScopedFBOBinding fbo( edgeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		edgeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( edgeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		edgeShader.Set( "CopyTexture", 0 );
		edgeShader.Set( "Step", spread / static_cast< float >( traceWidth ), spread / static_cast< float >( traceHeight ) );
		edgeShader.Set( "Lod", baseLod + detail );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. Stabilise, ping-ponged against the previous frame's result.
	//---------------------------------------------------------------------
	{
		const int history = stableCurrent;
		const int target  = 1 - stableCurrent;
		ScopedFBOBinding fbo( stableBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		stableBuffer[ target ].ResizeViewPort();
		ScopedShaderBinding shader( stabiliseShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding edgeTexture( edgeBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding historyTexture( stableBuffer[ history ].TextureID() );

		stabiliseShader.Set( "EdgeTexture", 0 );
		stabiliseShader.Set( "HistoryTexture", 1 );
		stabiliseShader.Set( "Attack", kAttack );
		stabiliseShader.Set( "Release", kRelease );
		stabiliseShader.Set( "Reset", historyValid ? 0.0f : 1.0f );
		quad.Draw();
		stableCurrent = target;
	}
	historyValid = true;

	//---------------------------------------------------------------------
	// 4. The sheet. Tear it off on the event, or on the timer.
	//---------------------------------------------------------------------
	const auto cpuStart = std::chrono::steady_clock::now();
	tracedLastFrame     = false;

	sheetSeconds += frameSeconds;
	const float autoSheet = AutoSheetFromParam( params[ PT_AUTO_SHEET ] );
	if( autoSheet > 0.0f && sheetSeconds >= autoSheet )
		newSheetPending = true;
	if( newSheetPending )
	{
		ink.Clear();
		machine.Clear();
		sheetSeconds    = 0.0;
		idleSeconds     = 0.0;
		newSheetPending = false;
		++sheetCount;
	}

	//---------------------------------------------------------------------
	// 5. Work, and the machine. The tracer is asked for a job when the
	//    machine has none -- and, in Chase, whenever a stroke ends, so the
	//    next stroke is always from the frame on screen now.
	//---------------------------------------------------------------------
	const float stepSize = ( perturb & kPerturbNoQuantise ) ? 0.0f : StepSizeFromParam( params[ PT_STEP_SIZE ] );
	machine.SetStepSize( stepSize );

	const bool chase = params[ PT_CHASE ] >= 0.5f && !plantedJob;
	samples.clear();
	double remaining = frameSeconds;

	//The negative control for `--budget`: a job every frame, from its first
	//stroke, whatever the machine was in the middle of.
	if( perturb & kPerturbRetraceEachFrame )
		takeWork( traceWidth, traceHeight, aspect, false );

	for( int guard = 0; guard < 8 && remaining > 0.0; ++guard )
	{
		if( machine.JobDone() )
		{
			if( plantedJob && plantedOnce && plantedPlanned )
				break;
			//An idle machine on a blank clip would otherwise pay a readback
			//every frame for nothing.
			idleSeconds += guard == 0 ? frameSeconds : 0.0;
			if( machine.HasJob() || idleSeconds >= kIdleRetrySeconds || traceCount == 0 )
			{
				idleSeconds = 0.0;
				takeWork( traceWidth, traceHeight, aspect, false );
			}
			if( machine.JobDone() )
				break;
		}
		remaining = machine.Advance( remaining, chase, samples );
		if( remaining > 0.0 && chase && machine.StrokeEnded() && !machine.JobDone() )
			takeWork( traceWidth, traceHeight, aspect, true );
	}
	lastSampleCount = samples.size();
	cpuMillis       = millisSince( cpuStart );

	//---------------------------------------------------------------------
	// 6. Ink along the path.
	//---------------------------------------------------------------------
	{
		InkRenderer::Params ip;
		ip.nibSigma   = PenWidthFromParam( params[ PT_PEN_WIDTH ] );
		ip.inkRate    = InkRate( FlowFromParam( params[ PT_FLOW ] ), ip.nibSigma );
		ip.travelRate = params[ PT_SHOW_TRAVEL ] >= 0.5f ? kTravelInk * ip.inkRate : 0.0;
		if( !ink.Deposit( samples.data(), static_cast< int >( samples.size() ), ip, aspect ) )
			return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 7. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding paperTexture( ink.TextureID() );

		const Paper paper = static_cast< Paper >( std::clamp( static_cast< int >( std::lround( params[ PT_PAPER ] ) ), 0, kPaperCount - 1 ) );
		float paperColour[ 3 ];
		PaperColour( paper, paperColour );
		const Vec2 carriage = machine.Position();
		const float* penInk = machine.Ink();

		compositeShader.Set( "CopyTexture", 0 );
		compositeShader.Set( "PaperTexture", 1 );
		compositeShader.Set( "PaperMode", static_cast< float >( static_cast< int >( paper ) ) );
		compositeShader.Set( "PaperColour", paperColour[ 0 ], paperColour[ 1 ], paperColour[ 2 ] );
		compositeShader.Set( "Ghost", kGhostLevel );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		compositeShader.Set( "Aspect", aspect );
		compositeShader.Set( "ShowCarriage", params[ PT_SHOW_CARRIAGE ] >= 0.5f ? 1.0f : 0.0f );
		compositeShader.Set( "CarriagePos", carriage.x, carriage.y );
		compositeShader.Set( "CarriageRadius", kCarriageRadius );
		compositeShader.Set( "NibSigma", PenWidthFromParam( params[ PT_PEN_WIDTH ] ) );
		compositeShader.Set( "CarriageInk", penInk[ 0 ], penInk[ 1 ], penInk[ 2 ] );
		compositeShader.Set( "PenDown", machine.PenDown() ? 1.0f : 0.0f );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Plotter::DeInitGL()
{
	copyShader.FreeGLResources();
	edgeShader.FreeGLResources();
	stabiliseShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	edgeBuffer.Destroy();
	stableBuffer[ 0 ].Destroy();
	stableBuffer[ 1 ].Destroy();
	ink.DeInitGL();

	historyValid = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Plotter::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//An event arrives as 1.0 on press and 0.0 on release; the press is
	//remembered until the next frame.
	if( index == PT_NEW_SHEET )
	{
		if( value >= 0.5f )
			newSheetPending = true;
		return FF_SUCCESS;
	}

	params[ index ] = value;

	//Changing what an edge *is* invalidates the history, because the numbers
	//being blended are no longer measuring the same thing.
	if( index == PT_DETAIL )
		historyValid = false;

	return FF_SUCCESS;
}

float Plotter::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

//---------------------------------------------------------------------------
char* Plotter::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Plotter::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Plotter::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Plotter::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Plotter::SetJobForTest( const std::vector< Stroke >& job, bool once )
{
	plantedStrokes = job;
	plantedJob     = true;
	plantedOnce    = once;
	plantedPlanned = false;
	machine.Clear();
}
