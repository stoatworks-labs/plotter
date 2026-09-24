#pragma once

#include "Machine.h"
#include "PassBuffer.h"
#include "Planner.h"
#include "StoatworksAboutParams.h"
#include "Tracer.h"
#include "render/Ink.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
    Plotter -- a pen plotter drawing the clip, as an FFGL effect for Resolume.

    **The one idea.** galvo is a laser: a fast mirror redrawing the whole path
    every frame. A pen plotter is the opposite machine. It is slow, it moves
    a pen under a velocity and acceleration limit, it has to lift and drop
    the pen between strokes, and the ink stays on the paper. Point it at the
    clip and it starts drawing the current frame's path; by the time it is a
    few strokes in, the clip has moved on. What falls out: fast content never
    gets finished; the sheet is a collage of the moments each stroke was
    drawn; the line is heavy where the pen is slow and thin on the straights;
    a blot at every pen-down; staircases at a coarse step; and pen changes
    that cost a trip to the carousel.

    **The pipeline**, and where each part lives:

      detect     GPU   Shaders.cpp   copy, edge at trace size, stabilise
      read back  ->    Plotter.cpp   one glReadPixels, only when work is asked for
      trace      CPU   Tracer.cpp    galvo's: thin, walk, simplify
      plan       CPU   Planner.cpp   strokes, pens, order, trapezoid blocks
      move       CPU   Machine.cpp   real elapsed time, in double; quantise
      ink        GPU   render/Ink    absorbance per unit time along the path
      composite  GPU   Shaders.cpp   paper, ink, travel, carriage, mix

    Everything between the readback and the renderer has no GL in it.

    See AGENTS.md for the traps.
*/
class Plotter : public CFFGLPlugin
{
public:
	Plotter();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. The harness uses these; the plugin never does. --------

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale );

	/// Perturb bits (Machine.h). Always zero in the shipped plugin; the
	/// negative controls set them to prove the checks can fail.
	void SetPerturbForTest( unsigned bits )
	{
		perturb = bits;
	}

	/// Hand the machine a job directly, bypassing the tracer. The strokes
	/// are in paper units. The plan is made from where the carriage is, as
	/// a traced job would be. With `once`, the plugin does not ask the
	/// tracer for more work when this job is done, so the harness measures
	/// exactly what it planted.
	void SetJobForTest( const std::vector< plotter::Stroke >& strokes, bool once );

	/// The paper, as RGBA floats (absorbance rgb, travel a), bottom row first.
	bool ReadPaperForTest( std::vector< float >& rgba ) const
	{
		return ink.Read( rgba );
	}

	const std::vector< plotter::Contour >& LastContours() const
	{
		return contours;
	}
	const std::vector< plotter::Stroke >& LastStrokes() const
	{
		return strokes;
	}
	plotter::Machine& GetMachine()
	{
		return machine;
	}
	/// Milliseconds the CPU half of the last frame took, and of that the
	/// readback + trace + plan, which only runs on frames that ask for work.
	double LastCpuMillis() const
	{
		return cpuMillis;
	}
	double LastTraceMillis() const
	{
		return traceMillis;
	}
	bool TracedLastFrame() const
	{
		return tracedLastFrame;
	}
	long TraceCount() const
	{
		return traceCount;
	}
	long SheetCount() const
	{
		return sheetCount;
	}
	std::size_t LastSampleCount() const
	{
		return lastSampleCount;
	}

	/// The order the host shows them in.
	enum ParamID : FFUInt32
	{
		//Machine
		PT_MAX_SPEED,
		PT_ACCELERATION,
		PT_PEN_SETTLE,
		PT_STEP_SIZE,
		PT_CORNER_ANGLE,

		//Pens
		PT_PENS,
		PT_PEN_WIDTH,
		PT_FLOW,
		PT_PALETTE,
		PT_OPTIMISE,

		//Path
		PT_THRESHOLD,
		PT_DETAIL,
		PT_CHASE,

		//Sheet
		PT_AUTO_SHEET,
		PT_NEW_SHEET,
		PT_PAPER,
		PT_SHOW_CARRIAGE,
		PT_SHOW_TRAVEL,
		PT_MIX,

		//About. Last in the enum so no saved composition's ids shift if it
		//grows. SetParamGroup collapses RUNS of consecutive same-group ids,
		//so the order above is load-bearing: append only.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// Read the mask back, trace it, plan a job from where the carriage is.
	void takeWork( int traceWidth, int traceHeight, float aspect, bool chase );
	/// The machine's parameters from the controls.
	plotter::MachineParams machineParams() const;

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader edgeShader;
	ffglex::FFGLShader stabiliseShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	plotter::PassBuffer copyBuffer;        ///< the picture, ours, mipmapped
	plotter::PassBuffer edgeBuffer;        ///< gradient + colour, trace size
	plotter::PassBuffer stableBuffer[ 2 ]; ///< ping-pong: stabilised gradient + colour
	int stableCurrent = 0;
	bool historyValid = false;

	plotter::InkRenderer ink;

	//--- the CPU half ------------------------------------------------------
	std::vector< unsigned char > readback;
	std::vector< unsigned char > mask;
	plotter::Tracer tracer;
	std::vector< plotter::Contour > contours;
	std::vector< plotter::Stroke > strokes;
	std::vector< plotter::Block > blocks;
	plotter::Machine machine;
	std::vector< plotter::Sample > samples;

	std::vector< plotter::Stroke > plantedStrokes;
	bool plantedJob      = false; ///< a test job is in the machine
	bool plantedOnce     = false; ///< ...and no tracing afterwards
	bool plantedPlanned  = false; ///< ...and it has been planned once
	bool newSheetPending = false;
	double sheetSeconds  = 0.0;   ///< time on the current sheet
	double idleSeconds   = 0.0;   ///< time since an idle machine last asked
	unsigned perturb     = 0;

	double cpuMillis        = 0.0;
	double traceMillis      = 0.0;
	bool tracedLastFrame    = false;
	long traceCount         = 0;
	long sheetCount         = 0;
	std::size_t lastSampleCount = 0;

	//---------------------------------------------------------------------
	// Host clock units. The FFGL header never says what unit SetTime is in,
	// and hosts disagree: Resolume hands over MILLISECONDS, the offline
	// harness sends seconds. Decided by comparing the host's deltas with the
	// wall clock over a few frames (galvo), and until decided the wall clock
	// is used -- wrong in origin, right in rate. Everything downstream sees a
	// frame delta in seconds, in double: the host's clock is ~500 million ms
	// into a session, where a float resolves 0.03 s.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
