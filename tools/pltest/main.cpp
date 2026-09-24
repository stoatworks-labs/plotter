/**
    pltest -- render Plotter offline, and check what its pen is doing.

    Where the pen went and how long it took are facts, not matters of taste.
    Everything between the readback and the renderer is plain C++ with no GL
    in it, so some of these checks need no context at all; the rest drive the
    real plugin class in a headless CGL context on a synthetic clock -- a
    clock the harness drives through SetTime, never the wall clock, so every
    number here is reproducible.

        pltest --out /tmp/frame.png     a picture, on the test card
        pltest --list                   every parameter, its type and range
        pltest --names                  no name over 16 characters, none twice
        pltest --plan                   the planner and the machine, no GL
        pltest --trapezoid              a straight stroke takes L/v + v/a, or
                                        2 sqrt(L/a) when it never reaches v
        pltest --ink                    ink density along a stroke follows 1/v
        pltest --budget                 after t seconds, exactly the strokes
                                        the profile predicts are drawn
        pltest --steps                  a coarse Step Size draws a staircase
        pltest --pens                   pen changes = distinct pens (Optimise)
        pltest --persist                ink stays, survives a resize, and a
                                        New Sheet clears it
        pltest --trace                  a filled square, through the plugin's
                                        own detect passes, is one closed stroke
        pltest --negative               every check above FAILS on a perturbed
                                        plugin
        pltest --bench                  ms/frame at 720p through 4K, the
                                        tracer's share separately
        pltest --pipe                   raw frames in, raw frames out
        pltest --dump-shaders DIR       the exact GLSL the plugin compiles

    Every GL check takes `--size WxH`; verify.sh runs them at 320x180 and
    1280x720.

    `--pipe` takes the fleet's frame format, so one filming script can drive
    any of the FFGL plugins:

        ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
          | pltest --pipe --size 1920x1080 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov

    `--script` is a plain text file of `frame  Parameter Name  value` lines.
    A standard parameter is held before the first key and after the last, and
    linearly interpolated between. An OPTION, BOOLEAN or INTEGER parameter
    STEPS: it holds each key's value until the frame of the next key, so a
    move from White to Alpha paper never passes through Cream, Ghost and
    Clip on the way. An EVENT (New Sheet) is pressed on the frame of any key
    whose value is 0.5 or more. A cue naming no parameter is refused with
    exit 2. A partial frame at the end of stdin ends the stream cleanly; a
    failed render, or a reader that goes away, exits 1 (SIGPIPE is ignored so
    that a closed stdout is a failed write and not a silent 141).

    This plugin has MEMORY -- the sheet, the machine's place in its job, the
    stabilise history -- so a frame out of this pipe depends on every frame
    before it. A reel has to be filmed from its first frame.
*/

#include "Controls.h"
#include "Machine.h"
#include "Planner.h"
#include "Plotter.h"
#include "Shaders.h"
#include "Tracer.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace plotter;

namespace
{
constexpr double kPi = 3.14159265358979323846;

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

//---------------------------------------------------------------------------
// Slider positions for physical values: the inverses of Controls.cpp's
// mappings, so a check can ask for 0.3 heights a second by name and the
// plugin's own conversion takes it back to 0.3 (to a float ULP).
//---------------------------------------------------------------------------
float paramGeometric( float from, float to, float value )
{
	return std::log( value / from ) / std::log( to / from );
}
float paramLinear( float from, float to, float value )
{
	return ( value - from ) / ( to - from );
}
float speedParam( float v )
{
	return paramGeometric( 0.05f, 2.0f, v );
}
float accelParam( float a )
{
	return paramGeometric( 0.1f, 20.0f, a );
}
float settleParam( float s )
{
	return paramLinear( 0.0f, 0.5f, s );
}
float stepParam( float s )
{
	return s <= 0.0f ? 0.0f : paramGeometric( 0.0005f, 0.025f, s );
}
float widthParam( float sigma )
{
	return paramGeometric( 0.0012f, 0.012f, sigma );
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card. Not meant to look nice: each shape exercises one thing.
//
//   - a red filled square:   four straight edges and four real corners, and
//                            a colour the Technical palette has a pen for
//   - a blue hollow ring:    two concentric closed contours, another pen
//   - a green bar:           a long thin closed contour, a third pen
//   - three white dashes:    short open strokes, the black pen
//   - a transparent hole:    an edge only alpha can see
//
// `shift` moves the square to the right, in fractions of the width: what
// `--motion` uses to give Chase something to chase.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, float shift = 0.0f )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	const float w      = static_cast< float >( width );
	const float h      = static_cast< float >( height );
	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;
			float r = 0.10f + 0.08f * u, g = r, b = r, a = 1.0f;

			//Square, left.
			const float sx = 0.08f + shift;
			if( u > sx && u < sx + 0.36f / aspect && v > 0.30f && v < 0.66f )
			{
				r = 0.98f; g = 0.22f; b = 0.15f;
			}

			//Ring, middle.
			const float dx = ( u - 0.50f ) * aspect, dy = v - 0.50f;
			const float d  = std::sqrt( dx * dx + dy * dy );
			if( d < 0.17f && d > 0.11f )
			{
				r = 0.30f; g = 0.50f; b = 1.00f;
			}

			//Bar, right.
			if( u > 0.74f && u < 0.86f && v > 0.14f && v < 0.86f )
			{
				r = 0.25f; g = 0.85f; b = 0.35f;
			}

			//Transparent hole, top left.
			const float hx = ( u - 0.14f ) * aspect, hy = v - 0.14f;
			if( std::sqrt( hx * hx + hy * hy ) < 0.07f )
				a = 0.0f;

			//Short dashes, top right, white: open strokes for the black pen.
			{
				const float lengths[ 3 ] = { 0.05f, 0.08f, 0.11f };
				for( int k = 0; k < 3; ++k )
				{
					const float top = 0.94f - 0.05f * static_cast< float >( k );
					if( u > 0.60f && u < 0.60f + lengths[ k ] && v < top && v > top - 0.012f )
						r = g = b = 1.0f;
				}
			}

			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = static_cast< unsigned char >( std::min( 255.0f, r * a * 255.0f ) );
			p[ 1 ] = static_cast< unsigned char >( std::min( 255.0f, g * a * 255.0f ) );
			p[ 2 ] = static_cast< unsigned char >( std::min( 255.0f, b * a * 255.0f ) );
			p[ 3 ] = static_cast< unsigned char >( a * 255.0f );
		}
	}
	return image;
}

/// A card with nothing on it but one filled square of `side` pixels,
/// centred. For the end-to-end trace check.
std::vector< unsigned char > buildSquareCard( int width, int height, int side )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	const int x0 = ( width - side ) / 2, y0 = ( height - side ) / 2;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const bool in    = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side;
			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = in ? 240 : 20;
			p[ 3 ] = 255;
		}
	return image;
}

/// The same PCG hash the fleet uses, for reproducible per-frame noise.
uint32_t hashInt( uint32_t x )
{
	x          = x * 747796405u + 2891336453u;
	uint32_t w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

std::vector< unsigned char > addNoise( const std::vector< unsigned char >& card, int frame, float amount )
{
	std::vector< unsigned char > noisy = card;
	if( amount <= 0.0f )
		return noisy;
	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		const uint32_t seed = hashInt( static_cast< uint32_t >( i / 4 ) * 2654435761u ^ static_cast< uint32_t >( frame ) );
		const float jitter  = ( static_cast< float >( seed ) / 4294967296.0f - 0.5f ) * scale;
		for( int c = 0; c < 3; ++c )
		{
			const float value = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]    = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	//PLTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, so a
	//check that fails only in CI can be reproduced here (wipe's recipe, by way
	//of repousse). That renderer is not repeatable at the last bit.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "PLTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "pltest: PLTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

/// The plugin's input and output, in one place.
struct Rig
{
	int width = 0, height = 0;
	GLuint source = 0, output = 0, fbo = 0;
	FFGLTextureStruct inputStruct   = {};
	FFGLTextureStruct* inputs[ 1 ]  = { nullptr };
	ProcessOpenGLStruct process     = {};

	Rig( int w, int h, const std::vector< unsigned char >& card ) : width( w ), height( h )
	{
		source = makeTexture( w, h, card.data() );
		output = makeTexture( w, h, nullptr );
		fbo    = makeFramebuffer( output );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle = source;
		inputs[ 0 ]        = &inputStruct;
		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = fbo;
	}
	~Rig()
	{
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &output );
		glDeleteTextures( 1, &source );
	}
	Rig( const Rig& ) = delete;
	Rig& operator=( const Rig& ) = delete;

	void upload( const std::vector< unsigned char >& pixels )
	{
		glBindTexture( GL_TEXTURE_2D, source );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	FFResult render( Plotter& plugin )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process );
	}
};

/// The synthetic clock. It has to be synthetic: left to the wall clock the
/// harness renders a hundred frames in a few milliseconds, the machine
/// advances by the minimum frame each time, and nothing is reproducible --
/// resodoom's harness had to pace itself in real time because its engine
/// read steady_clock, and this one must not.
void driveClock( Plotter& plugin, int frame, double fps )
{
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( static_cast< double >( frame ) / fps );
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
const char* typeName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BOOLEAN: return "boolean";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_RED:
	case FF_TYPE_GREEN:
	case FF_TYPE_BLUE: return "colour";
	default: return "other";
	}
}

unsigned int findParameter( Plotter& plugin, const std::string& name )
{
	for( unsigned int i = 0; i < Plotter::PT_COUNT; ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
			return i;
	}
	return Plotter::PT_COUNT;
}

bool applySetting( Plotter& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name   = assignment.substr( 0, equals );
	const std::string value  = assignment.substr( equals + 1 );
	const unsigned int index = findParameter( plugin, name );
	if( index < Plotter::PT_COUNT )
	{
		plugin.SetFloatParameter( index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}
	error = "no parameter called '" + name + "'";
	return false;
}

/// Press an event parameter: 1 then 0, as a host does.
void press( Plotter& plugin, unsigned int index )
{
	plugin.SetFloatParameter( index, 1.0f );
	plugin.SetFloatParameter( index, 0.0f );
}

int listParameters( Plotter& plugin )
{
	std::printf( "%-3s %-20s %-9s %10s %9s %9s\n", "id", "name", "type", "default", "min", "max" );
	for( unsigned int i = 0; i < Plotter::PT_COUNT; ++i )
	{
		const char* name        = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );
		RangeStruct range       = plugin.GetParamRange( i );
		if( type == FF_TYPE_OPTION )
			range = { 0.0f, static_cast< float >( plugin.GetNumParamElements( i ) ) - 1.0f };
		if( type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT )
			range = { 0.0f, 1.0f };
		std::printf( "%-3u %-20s %-9s %10.4f %9.4f %9.4f\n", i, name ? name : "?", typeName( type ),
		             plugin.GetFloatParameter( i ), range.min, range.max );
	}
	return 0;
}

//---------------------------------------------------------------------------
// A plugin with a rig, driven on the synthetic clock. Every GL check is
// built on this so they all render the way the host would.
//---------------------------------------------------------------------------
struct Session
{
	Plotter plugin;
	std::unique_ptr< Rig > rig;
	int width = 0, height = 0, frame = 0;
	double fps = 60.0;

	bool begin( int w, int h, const std::vector< unsigned char >& card )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width  = static_cast< FFUInt32 >( w );
		viewport.height = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		rig.reset( new Rig( w, h, card ) );
		return true;
	}
	/// Change the raster mid-run: a new rig at the new size, the plugin
	/// kept. What a host does when the composition size changes.
	void resize( int w, int h, const std::vector< unsigned char >& card )
	{
		width  = w;
		height = h;
		rig.reset( new Rig( w, h, card ) );
	}
	bool set( const std::string& assignment )
	{
		std::string error;
		if( applySetting( plugin, assignment, error ) )
			return true;
		std::fprintf( stderr, "--set %s: %s\n", assignment.c_str(), error.c_str() );
		return false;
	}
	bool render()
	{
		driveClock( plugin, frame++, fps );
		return rig->render( plugin ) == FF_SUCCESS;
	}
	bool renderFrames( int n )
	{
		for( int i = 0; i < n; ++i )
			if( !render() )
				return false;
		return true;
	}
	bool paper( std::vector< float >& rgba )
	{
		return plugin.ReadPaperForTest( rgba );
	}
	std::vector< unsigned char > output()
	{
		return flipRows( readBackRaw( rig->fbo, width, height ), width, height );
	}
	void end()
	{
		rig.reset();
		plugin.DeInitGL();
	}
};

/// The paper's absorbance summed over every pixel, one channel.
double paperSum( const std::vector< float >& rgba, int channel = 0 )
{
	double sum = 0.0;
	for( size_t i = channel; i < rgba.size(); i += 4 )
		sum += rgba[ i ];
	return sum;
}

double paperMax( const std::vector< float >& rgba, int channel = 0 )
{
	double best = 0.0;
	for( size_t i = channel; i < rgba.size(); i += 4 )
		best = std::max( best, static_cast< double >( rgba[ i ] ) );
	return best;
}

/// One straight open stroke in paper units, black ink.
Stroke straightStroke( float x0, float y0, float x1, float y1, int pen = 0 )
{
	Stroke s;
	s.points = { Vec2{ x0, y0 }, Vec2{ x1, y1 } };
	s.closed = false;
	s.pen    = pen;
	const float none[ 3 ] = { 0, 0, 0 };
	PenColour( Palette::Technical, pen, none, s.ink );
	return s;
}

/// The absorbance weight of a Technical pen's channel: what one unit of
/// deposit adds to that channel.
double inkWeight( int pen, int channel )
{
	const float none[ 3 ] = { 0, 0, 0 };
	float ink[ 3 ];
	PenColour( Palette::Technical, pen, none, ink );
	return 1.0 - ink[ channel ];
}

/// The row whose centre is at paper y, and the paper y of a row's centre.
int rowOf( double y, int height )
{
	return std::clamp( static_cast< int >( std::floor( y * height ) ), 0, height - 1 );
}
double rowCentre( int row, int height )
{
	return ( row + 0.5 ) / height;
}
int colOf( double x, int width, float aspect )
{
	return std::clamp( static_cast< int >( std::floor( x / aspect * width ) ), 0, width - 1 );
}
double colCentre( int col, int width, float aspect )
{
	return ( col + 0.5 ) / width * aspect;
}

//---------------------------------------------------------------------------
// --names: every name fits the host's 16 characters and none is repeated.
//---------------------------------------------------------------------------
int runNames()
{
	Plotter plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( unsigned int i = 0; i < Plotter::PT_COUNT; ++i )
	{
		const char* raw = plugin.GetParamName( i );
		const std::string name = raw ? raw : "";
		if( name.size() > 16 )
		{
			std::printf( "  '%s' is %zu characters, over 16  FAILED\n", name.c_str(), name.size() );
			++failures;
		}
		if( !seen.insert( name ).second )
		{
			std::printf( "  '%s' is declared twice  FAILED\n", name.c_str() );
			++failures;
		}
	}
	const char* display = "SW Plotter";
	if( std::strlen( display ) > 16 )
	{
		std::printf( "  display name '%s' is over 16  FAILED\n", display );
		++failures;
	}
	std::printf( "  %u parameters%s; display name '%s' (%zu)\n", Plotter::PT_COUNT,
	             failures == 0 ? ", all within 16 characters and unique" : "", display, std::strlen( display ) );
	std::printf( failures == 0 ? "names: ok\n" : "names: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --plan: the planner and the machine, no GL. What CI can run.
//---------------------------------------------------------------------------
int runPlanCheck()
{
	int failures = 0;
	MachineParams mp;
	mp.maxSpeed     = 0.4;
	mp.acceleration = 1.5;
	mp.settle       = 0.1;

	//1. One straight stroke: the Draw block's duration is the closed form,
	//   in both regimes. Exact: the planner reduces to the formula.
	for( double L : { 0.9, 0.05 } )
	{
		std::vector< Block > blocks;
		PlanBlocks( { straightStroke( 0.2f, 0.5f, 0.2f + static_cast< float >( L ), 0.5f ) }, mp, Vec2{ 0.2f, 0.5f }, 0, blocks );
		double drawSeconds = 0.0;
		for( const Block& b : blocks )
			if( b.kind == Block::Kind::Draw )
				drawSeconds += b.Duration();
		const double closed = L > mp.maxSpeed * mp.maxSpeed / mp.acceleration
		                          ? L / mp.maxSpeed + mp.maxSpeed / mp.acceleration
		                          : 2.0 * std::sqrt( L / mp.acceleration );
		//The stroke's endpoints are floats, so its length carries a float
		//ULP (6e-8 relative) into the time.
		const bool ok = std::fabs( drawSeconds - closed ) <= 1e-6 * closed;
		std::printf( "  stroke of %.2f at v %.2f a %.2f: %.6f s planned, %.6f closed form (%s regime)  %s\n", L,
		             mp.maxSpeed, mp.acceleration, drawSeconds, closed, L > mp.maxSpeed * mp.maxSpeed / mp.acceleration ? "cruise" : "triangle",
		             verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	//2. A corner stops the pen; a straight-on junction does not. Two
	//   segments of 0.5: with a 90-degree turn each is rest-to-rest; with no
	//   turn the junction is the cruise speed and the total is shorter by
	//   exactly v/a; a 15-degree bend at Corner Angle 30 is half way.
	{
		auto polyline = [ & ]( double turnDeg ) {
			Stroke s;
			const float ang = static_cast< float >( turnDeg * kPi / 180.0 );
			s.points = { Vec2{ 0.1f, 0.3f }, Vec2{ 0.6f, 0.3f }, Vec2{ 0.6f + 0.5f * std::cos( ang ), 0.3f + 0.5f * std::sin( ang ) } };
			return s;
		};
		auto drawSeconds = [ & ]( const Stroke& s ) {
			std::vector< Block > blocks;
			PlanBlocks( { s }, mp, s.points[ 0 ], 0, blocks );
			double t = 0.0;
			for( const Block& b : blocks )
				if( b.kind == Block::Kind::Draw )
					t += b.Duration();
			return t;
		};
		const double sharp    = drawSeconds( polyline( 90.0 ) );
		const double straight = drawSeconds( polyline( 0.0 ) );
		const double bend     = drawSeconds( polyline( 15.0 ) );
		const double saved    = mp.maxSpeed / mp.acceleration;
		//At half the cruise speed through the junction, the two half-ramps
		//each cost (v - v/2)/a in time over the straight-on case, and the
		//distance they cover at the lower mean speed costs the rest: the
		//closed form is v/(4a) extra over straight on.
		const double bendExtra = mp.maxSpeed / ( 4.0 * mp.acceleration );
		const bool ok = std::fabs( ( sharp - straight ) - saved ) <= 1e-6 * sharp && std::fabs( ( bend - straight ) - bendExtra ) <= 1e-6 * sharp;
		std::printf( "  90-degree corner %.6f s, straight on %.6f s: %.6f s saved, v/a = %.6f; 15-degree bend costs %.6f over straight, v/4a = %.6f  %s\n",
		             sharp, straight, sharp - straight, saved, bend - straight, bendExtra, verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	//3. Reachability: on a zig-zag of short segments with gentle bends every
	//   junction is reachable from both sides under the acceleration limit.
	{
		Stroke s;
		for( int i = 0; i < 12; ++i )
			s.points.push_back( Vec2{ 0.1f + 0.03f * i, 0.5f + ( i % 2 ? 0.004f : 0.0f ) } );
		std::vector< Block > blocks;
		PlanBlocks( { s }, mp, s.points[ 0 ], 0, blocks );
		bool ok    = true;
		double worst = 0.0;
		for( const Block& b : blocks )
		{
			if( b.kind != Block::Kind::Draw )
				continue;
			const double a2L = 2.0 * mp.acceleration * b.length;
			worst = std::max( worst, std::max( b.vOut * b.vOut - ( b.vIn * b.vIn + a2L ), b.vIn * b.vIn - ( b.vOut * b.vOut + a2L ) ) );
			if( b.vPeak < std::max( b.vIn, b.vOut ) - 1e-9 || b.t1 < -1e-12 || b.t2 < -1e-12 || b.t3 < -1e-12 )
				ok = false;
		}
		ok = ok && worst <= 1e-9;
		std::printf( "  zig-zag of 11 short segments: every junction reachable (worst excess %.2e)  %s\n", worst, verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	//4. Pen changes: grouped by pen, a job of eight strokes over four pens
	//   changes pen four times from an empty carriage; interleaved, seven.
	{
		std::vector< Stroke > job;
		for( int i = 0; i < 8; ++i )
			job.push_back( straightStroke( 0.2f, 0.1f + 0.1f * i, 0.8f, 0.1f + 0.1f * i, i % 4 ) );
		std::vector< Stroke > sorted = job;
		OrderStrokes( sorted, Vec2{ 0, 0 }, -1, true, true );
		std::vector< Block > blocks;
		const int grouped     = PlanBlocks( sorted, mp, Vec2{ 0, 0 }, -1, blocks );
		std::vector< Stroke > mixed = job;
		OrderStrokes( mixed, Vec2{ 0, 0 }, -1, true, false );
		const int interleaved = PlanBlocks( mixed, mp, Vec2{ 0, 0 }, -1, blocks );
		const int unsorted    = PlanBlocks( job, mp, Vec2{ 0, 0 }, -1, blocks );
		const bool ok = grouped == 4 && interleaved > 4 && unsorted == 8;
		std::printf( "  8 strokes over 4 pens: %d changes grouped (want 4), %d on one tour, %d unsorted (want 8)  %s\n", grouped,
		             interleaved, unsorted, verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	//5. Nearest pen: white and grey go to black, a red to red, a blue to
	//   blue, and with one pen everything is that pen.
	{
		const float white[ 3 ] = { 1, 1, 1 }, grey[ 3 ] = { 0.5f, 0.5f, 0.5f }, red[ 3 ] = { 0.9f, 0.2f, 0.1f }, blue[ 3 ] = { 0.2f, 0.4f, 0.9f };
		const bool ok = NearestPen( Palette::Technical, 4, white ) == 0 && NearestPen( Palette::Technical, 4, grey ) == 0
		                && NearestPen( Palette::Technical, 4, red ) == 1 && NearestPen( Palette::Technical, 4, blue ) == 2
		                && NearestPen( Palette::Technical, 1, red ) == 0 && NearestPen( Palette::Black, 8, red ) == 0;
		std::printf( "  nearest pen: white %d grey %d red %d blue %d; one pen %d; Black palette %d  %s\n",
		             NearestPen( Palette::Technical, 4, white ), NearestPen( Palette::Technical, 4, grey ),
		             NearestPen( Palette::Technical, 4, red ), NearestPen( Palette::Technical, 4, blue ),
		             NearestPen( Palette::Technical, 1, red ), NearestPen( Palette::Black, 8, red ), verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	//6. The machine keeps time: advanced by frames of three different
	//   lengths, the pen-down time it reports is the plan's to 1e-9, the
	//   sample intervals sum to the time given, and it ends where the job
	//   ends. Then again with a step, and every sample is on the grid.
	for( double step : { 0.0, 0.01 } )
	{
		std::vector< Stroke > job = { straightStroke( 0.2f, 0.3f, 0.9f, 0.7f ), straightStroke( 0.9f, 0.2f, 0.3f, 0.2f, 1 ) };
		std::vector< Block > blocks;
		PlanBlocks( job, mp, Vec2{ 0, 0 }, -1, blocks );
		double planned = 0.0, total = 0.0;
		for( const Block& b : blocks )
		{
			total += b.Duration();
			if( b.kind == Block::Kind::Draw || b.kind == Block::Kind::Settle )
				planned += b.Duration();
		}
		Machine machine;
		machine.Reset();
		machine.SetStepSize( step );
		machine.SetJob( blocks, 2 );
		std::vector< Sample > samples;
		const double frames[ 3 ] = { 1.0 / 60.0, 1.0 / 30.0, 1.0 / 144.0 };
		double given = 0.0, sampled = 0.0;
		bool onGrid  = true;
		int i        = 0;
		while( !machine.JobDone() && i < 100000 )
		{
			const double dt = frames[ i++ % 3 ];
			samples.clear();
			const double unused = machine.Advance( dt, false, samples );
			given += dt - unused;
			for( size_t k = 0; k + 1 < samples.size(); ++k )
			{
				sampled += samples[ k ].dt;
				if( step > 0.0 )
				{
					const double rx = samples[ k ].x / step, ry = samples[ k ].y / step;
					if( std::fabs( rx - std::round( rx ) ) > 1e-4 || std::fabs( ry - std::round( ry ) ) > 1e-4 )
						onGrid = false;
				}
			}
		}
		const Vec2 end = machine.Position();
		const bool ok  = std::fabs( machine.InkSeconds() - planned ) <= 1e-9 && std::fabs( sampled - given ) <= 1e-6
		                && std::fabs( given - total ) <= 1e-9 && std::fabs( end.x - 0.3f ) < 1e-5 && std::fabs( end.y - 0.2f ) < 1e-5
		                && machine.StrokesCompleted() == 2 && machine.PenChangesMade() == 2 && onGrid;
		std::printf( "  machine, step %.3f: pen down %.6f s (plan %.6f), samples %.6f s of %.6f given, job %.6f; ends at (%.3f, %.3f); %ld strokes, %ld changes%s  %s\n",
		             step, machine.InkSeconds(), planned, sampled, given, total, end.x, end.y, machine.StrokesCompleted(),
		             machine.PenChangesMade(), step > 0.0 ? ( onGrid ? ", every sample on the grid" : ", OFF the grid" ) : "", verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	std::printf( failures == 0 ? "plan: ok\n" : "plan: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// The GL checks. Each takes the raster and the perturb bits; `--negative`
// runs each with the perturbation that must make it fail.
//---------------------------------------------------------------------------
struct CheckArgs
{
	int width  = 1280;
	int height = 720;
	unsigned perturb = 0;
	bool quiet = false;
};

/// A session on a blank card at the check's raster, with the plugin set up
/// the way every check wants it: white paper, no carriage, no travel, no
/// quantisation unless asked, and the perturbation.
bool beginCheck( Session& s, const CheckArgs& args, const std::vector< std::string >& settings )
{
	s.plugin.SetPerturbForTest( args.perturb );
	const std::vector< unsigned char > blank( static_cast< size_t >( args.width ) * args.height * 4, 0 );
	if( !s.begin( args.width, args.height, blank ) )
		return false;
	for( const char* base : { "Paper=0", "Show Carriage=0", "Show Travel=0", "Step Size=0", "Auto Sheet=0", "Palette=0", "Optimise=1", "Chase=0" } )
		if( !s.set( base ) )
			return false;
	for( const std::string& setting : settings )
		if( !s.set( setting ) )
			return false;
	return true;
}

//---------------------------------------------------------------------------
// --trapezoid
//---------------------------------------------------------------------------
// One straight stroke of length L, planted in the machine, at a speed v and
// acceleration a set through the plugin's own controls. Two things are
// measured OUT OF THE PICTURE and compared with the closed form
//
//     T = L/v + v/a   when L > v^2/a,   T = 2 sqrt( L/a )   otherwise,
//
// plus the two settles, which are pen-down time too:
//
//   * the pen-down time from the ink: every interval deposits InkRate x dt
//     spread over the plane (the renderer's conservation), so the absorbance
//     summed over the sheet is InkRate x weight x T x H^2, and T falls out
//     with no frame quantisation at all;
//   * the pen-down time from the frames: the first frame with ink to the
//     last frame the ink grew, at the harness's frame rate.
//
// Tolerances. The energy route: half-float accumulation (galvo measured 0.33%
// low at 16F), the tanh CDF (3e-4), the 4.5 sigma cut (7e-6), and the
// Gaussian's pixel-sum aliasing 2 exp( -2 pi^2 sigma_px^2 ), which at the
// check's sigma of 0.004 (0.72 px at 180 lines) is 7e-5: 1% in all. The frame
// route: one frame at each end, 2/fps.
//---------------------------------------------------------------------------
int runTrapezoidCheck( const CheckArgs& args )
{
	int failures = 0;
	constexpr float v = 0.3f, a = 0.5f, settle = 0.1f, sigma = 0.004f;
	const float aspect = static_cast< float >( args.width ) / args.height;

	for( double L : { 0.6, 0.02 } )
	{
		Session s;
		s.fps = 60.0;
		if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( v ) ), "Acceleration=" + std::to_string( accelParam( a ) ),
		                             "Pen Settle=" + std::to_string( settleParam( settle ) ), "Pen Width=" + std::to_string( widthParam( sigma ) ) } ) )
			return 1;

		const float x0 = 0.3f * aspect;
		s.plugin.SetJobForTest( { straightStroke( x0, 0.5f, x0 + static_cast< float >( L ), 0.5f ) }, true );

		std::vector< float > rgba;
		double last = 0.0;
		int firstInk = -1, lastGrowth = -1;
		for( int frame = 0; frame < 900; ++frame )
		{
			if( !s.render() )
				return 1;
			s.paper( rgba );
			const double sum = paperSum( rgba );
			if( sum > 0.0 && firstInk < 0 )
				firstInk = frame;
			if( sum > last * ( 1.0 + 1e-6 ) )
				lastGrowth = frame;
			last = sum;
			if( s.plugin.GetMachine().JobDone() && frame > lastGrowth + 5 )
				break;
		}
		s.end();

		const double inkRate  = InkRate( FlowFromParam( 0.6f ), sigma );
		const double weight   = inkWeight( 0, 0 );
		const double measured = last / ( inkRate * weight * static_cast< double >( args.height ) * args.height );
		const double closed   = L > v * v / a ? L / v + v / a : 2.0 * std::sqrt( L / a );
		const double expected = closed + 2.0 * settle;
		const double byFrames = firstInk >= 0 && lastGrowth >= 0 ? ( lastGrowth - firstInk + 1 ) / s.fps : -1.0;

		const bool okEnergy = std::fabs( measured - expected ) <= 0.01 * expected;
		const bool okFrames = std::fabs( byFrames - expected ) <= 2.0 / s.fps;
		std::printf( "  L %.2f at v %.2f a %.2f (%s): pen down %.4f s by ink, %.4f s by frames, %.4f s expected (%.4f + 2 x %.2f settle)  %s\n",
		             L, v, a, L > v * v / a ? "cruise" : "never reaches v", measured, byFrames, expected, closed, settle,
		             verdict( okEnergy && okFrames ) );
		if( !args.quiet )
			std::printf( "    ink route %+.3f%% (tolerance 1%%), frame route %+.4f s (tolerance %.4f s)\n",
			             ( measured - expected ) / expected * 100.0, byFrames - expected, 2.0 / s.fps );
		failures += ( okEnergy && okFrames ) ? 0 : 1;
	}

	std::printf( failures == 0 ? "trapezoid: ok\n" : "trapezoid: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --ink
//---------------------------------------------------------------------------
// One long horizontal stroke. The absorbance summed down a pixel column is the
// line's density per unit length at that x, and the pen deposits per unit
// TIME, so density is proportional to 1/v. In the acceleration zone the speed
// at distance s from the start is sqrt( 2 a s ), so a column at s carries
// v / sqrt( 2 a s ) times what a column at cruise carries; the deceleration
// zone mirrors it. Measured at the column nearest s = v^2 / 8a (where the
// speed is v/2 and the predicted ratio 2), at the middle, and at L - s.
//
// Tolerance: the Gaussian nib smooths the 1/sqrt(s) profile along the stroke
// by (sigma^2 / 2) f''/f = (sigma^2 / 2)(3 / 4 s^2), which at sigma 0.004 and
// s 0.045 is 0.3%; the interval's uniform spread is second order in a dt / v
// (under 1%); half-float 0.33%; the tanh CDF 3e-4. 2% in all. The across
// profile is the same at every column and cancels in the ratio, so the
// line's sub-pixel row does not enter -- it is put on a row centre anyway.
//---------------------------------------------------------------------------
int runInkCheck( const CheckArgs& args )
{
	constexpr float v = 0.3f, a = 0.25f, sigma = 0.004f;
	const float aspect = static_cast< float >( args.width ) / args.height;

	Session s;
	if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( v ) ), "Acceleration=" + std::to_string( accelParam( a ) ),
	                             "Pen Settle=0", "Pen Width=" + std::to_string( widthParam( sigma ) ) } ) )
		return 1;

	const int row   = args.height / 2;
	const float y0  = static_cast< float >( rowCentre( row, args.height ) );
	const int col0  = args.width / 8;
	const float x0  = static_cast< float >( colCentre( col0, args.width, aspect ) );
	const float L   = 0.75f * aspect;
	s.plugin.SetJobForTest( { straightStroke( x0, y0, x0 + L, y0 ) }, true );

	for( int frame = 0; frame < 2000 && !( s.plugin.GetMachine().JobDone() && frame > 2 ); ++frame )
		if( !s.render() )
			return 1;
	std::vector< float > rgba;
	s.paper( rgba );
	s.end();

	auto column = [ & ]( int col ) {
		double sum = 0.0;
		for( int y = 0; y < args.height; ++y )
			sum += rgba[ ( static_cast< size_t >( y ) * args.width + col ) * 4 ];
		return sum;
	};

	const double sHalf   = v * v / ( 8.0 * a );//where the speed is v/2
	const double cruise  = v * v / ( 2.0 * a );
	const int colAccel   = colOf( x0 + sHalf, args.width, aspect );
	const int colMid     = colOf( x0 + L * 0.5f, args.width, aspect );
	const int colDecel   = colOf( x0 + L - sHalf, args.width, aspect );
	const double sAccel  = colCentre( colAccel, args.width, aspect ) - x0;
	const double sDecel  = x0 + L - colCentre( colDecel, args.width, aspect );

	const double mid   = column( colMid );
	const double rAcc  = column( colAccel ) / mid;
	const double rDec  = column( colDecel ) / mid;
	const double pAcc  = v / std::sqrt( 2.0 * a * sAccel );
	const double pDec  = v / std::sqrt( 2.0 * a * sDecel );
	const double smoothing = ( sigma * sigma / 2.0 ) * ( 3.0 / ( 4.0 * sAccel * sAccel ) ) * 100.0;

	const bool ok = std::fabs( rAcc - pAcc ) <= 0.02 * pAcc && std::fabs( rDec - pDec ) <= 0.02 * pDec && sAccel < cruise && sDecel < cruise;
	std::printf( "  v %.2f a %.2f, cruise from %.3f: column density over the middle's is %.4f at s = %.4f (predicted %.4f) and %.4f at L - %.4f (predicted %.4f)  %s\n",
	             v, a, cruise, rAcc, sAccel, pAcc, rDec, sDecel, pDec, verdict( ok ) );
	if( !args.quiet )
		std::printf( "    tolerance 2%%: nib smoothing %.2f%% at this s and sigma, half-float 0.33%%, interval spread second order\n", smoothing );

	std::printf( ok ? "ink: ok\n" : "ink: 1 FAILED\n" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --budget
//---------------------------------------------------------------------------
// Five horizontal bars, planted as one job. The harness works out the
// schedule from the closed form -- the pen change, each travel, each settle,
// each draw, in the order the driver takes them (nearest end first, so the
// bars alternate direction) -- and at checkpoints through the job compares
// it with the sheet: which bars are complete, how far the current one has
// got, and which are untouched. The inked length is read as the 50% crossing
// of the cruise density along the bar's row; the tip of a moving line is a
// half CDF centred on the pen, so that crossing is the pen to within a pixel;
// a settled end carries a blot of inkRate x settle whose radius at half the
// cruise peak is sigma sqrt( 2 ln( blot / half cruise ) ), and is added to the
// prediction. Tolerance sigma + 1 px: the crossing is read off a profile no
// steeper than the nib, between two pixel columns.
//---------------------------------------------------------------------------
int runBudgetCheck( const CheckArgs& args )
{
	constexpr float v = 0.4f, a = 2.0f, settle = 0.1f, sigma = 0.004f;
	constexpr int bars = 5;
	const float aspect = static_cast< float >( args.width ) / args.height;

	Session s;
	if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( v ) ), "Acceleration=" + std::to_string( accelParam( a ) ),
	                             "Pen Settle=" + std::to_string( settleParam( settle ) ), "Pen Width=" + std::to_string( widthParam( sigma ) ) } ) )
		return 1;

	const int col0  = args.width / 6;
	const float x0  = static_cast< float >( colCentre( col0, args.width, aspect ) );
	const float L   = 0.55f * aspect;
	std::vector< int > rows;
	std::vector< Stroke > job;
	for( int i = 0; i < bars; ++i )
	{
		const int row = args.height / 5 + ( i * args.height ) / 8;
		rows.push_back( row );
		const float y = static_cast< float >( rowCentre( row, args.height ) );
		job.push_back( straightStroke( x0, y, x0 + L, y ) );
	}
	s.plugin.SetJobForTest( job, true );

	//The schedule, from the closed form. The machine starts parked at the
	//carousel with no pen: a change, then the bars nearest-end-first, which
	//alternates their direction.
	auto restToRest = [ & ]( double length, double speed ) {
		return length > speed * speed / a ? length / speed + speed / a : 2.0 * std::sqrt( length / a );
	};
	auto dist = [ & ]( float ax, float ay, float bx, float by ) {
		return std::sqrt( static_cast< double >( ( ax - bx ) * ( ax - bx ) + ( ay - by ) * ( ay - by ) ) );
	};
	struct BarPlan
	{
		double start, end;
		bool reversed;
	};
	std::vector< BarPlan > plan( bars );
	std::vector< bool > taken( bars, false );
	double t = kPenChangeSeconds;
	float px = kCarouselX, py = kCarouselY;
	for( int placed = 0; placed < bars; ++placed )
	{
		//Nearest end first, as the driver orders them.
		int best = -1;
		bool rev = false;
		double bestD = 1e30;
		for( int i = 0; i < bars; ++i )
		{
			if( taken[ i ] )
				continue;
			const float y   = job[ i ].points[ 0 ].y;
			const double d0 = dist( px, py, x0, y ), d1 = dist( px, py, x0 + L, y );
			if( d0 < bestD )
			{
				bestD = d0;
				best  = i;
				rev   = false;
			}
			if( d1 < bestD )
			{
				bestD = d1;
				best  = i;
				rev   = true;
			}
		}
		taken[ best ] = true;
		const float y = job[ best ].points[ 0 ].y;
		const float sx = rev ? x0 + L : x0;
		t += restToRest( dist( px, py, sx, y ), v * kTravelSpeedFactor ) + settle;
		const double start = t;
		t += restToRest( L, v );
		plan[ best ] = { start, t, rev };
		t += settle;
		px = rev ? x0 : x0 + L;
		py = y;
	}
	const double jobSeconds = t;

	//Distance along a rest-to-rest move of length L at time tau.
	auto along = [ & ]( double length, double tau ) {
		const double vp = std::min( static_cast< double >( v ), std::sqrt( a * length ) );
		const double t1 = vp / a, d1 = vp * vp / ( 2.0 * a );
		const double t2 = ( length - 2.0 * d1 ) / vp;
		if( tau <= 0.0 )
			return 0.0;
		if( tau < t1 )
			return 0.5 * a * tau * tau;
		if( tau < t1 + t2 )
			return d1 + vp * ( tau - t1 );
		const double r = tau - t1 - t2;
		return std::min( length, d1 + vp * t2 + vp * r - 0.5 * a * r * r );
	};

	const double inkRate    = InkRate( FlowFromParam( 0.6f ), sigma ) * inkWeight( 0, 0 );
	const double cruisePeak = inkRate / ( v * sigma * std::sqrt( 2.0 * kPi ) );
	//A settle is a dot of inkRate x settle, and the 50% crossing runs past
	//the end of the bar to where that dot has fallen to half the cruise peak.
	const double blotPeak   = inkRate * settle / ( 2.0 * kPi * sigma * sigma );
	const double blotRadius = sigma * std::sqrt( 2.0 * std::log( blotPeak / ( 0.5 * cruisePeak ) ) );
	const double tolerance  = sigma + 1.0 / args.height;//paper units

	int failures = 0;
	std::vector< float > rgba;
	const int checkEvery = 45;
	for( int frame = 0;; ++frame )
	{
		if( !s.render() )
			return 1;
		const double now = ( frame + 1 ) / s.fps;
		const bool last  = now > jobSeconds + 0.5;
		if( frame % checkEvery != checkEvery - 1 && !last )
			continue;

		s.paper( rgba );
		bool ok = true;
		std::string detail;
		for( int i = 0; i < bars; ++i )
		{
			//The inked extent along the bar, from its start end: the 50%
			//crossing of the cruise density, scanning from the start.
			double predicted;
			if( now >= plan[ i ].end )
				predicted = L + 2.0 * blotRadius;
			else if( now <= plan[ i ].start )
				predicted = 0.0;
			else
				predicted = along( L, now - plan[ i ].start ) + blotRadius;

			double peak = 0.0;
			int lastInked = -1;
			for( int x = 0; x < args.width; ++x )
			{
				const double d = rgba[ ( static_cast< size_t >( rows[ i ] ) * args.width + x ) * 4 ];
				peak = std::max( peak, d );
			}
			double extent = 0.0;
			if( peak > 0.5 * cruisePeak )
			{
				const int from = plan[ i ].reversed ? args.width - 1 : 0;
				const int step = plan[ i ].reversed ? -1 : 1;
				int first = -1;
				for( int x = from; x >= 0 && x < args.width; x += step )
				{
					const double d = rgba[ ( static_cast< size_t >( rows[ i ] ) * args.width + x ) * 4 ];
					if( d > 0.5 * cruisePeak )
					{
						if( first < 0 )
							first = x;
						lastInked = x;
					}
				}
				if( first >= 0 )
					extent = std::fabs( colCentre( lastInked, args.width, aspect ) - colCentre( first, args.width, aspect ) );
			}
			const bool untouched = predicted == 0.0 && now < plan[ i ].start - settle;
			const bool settling  = predicted == 0.0 && !untouched;
			const bool barOk = untouched ? peak < 0.05 * cruisePeak : settling ? true : std::fabs( extent - predicted ) <= tolerance;
			ok = ok && barOk;
			char buffer[ 96 ];
			std::snprintf( buffer, sizeof buffer, " bar%d %.3f/%.3f%s", i, extent, predicted, barOk ? "" : "!" );
			detail += buffer;
		}
		std::printf( "  t %.2f s:%s  %s\n", now, detail.c_str(), verdict( ok ) );
		failures += ok ? 0 : 1;
		if( last )
			break;
	}
	s.end();
	std::printf( "  job %.2f s planned by the harness, %.2f s by the machine; tolerance %.4f paper units (sigma + 1 px); a finished end reads %.4f longer, the settle's blot\n",
	             jobSeconds, s.plugin.GetMachine().JobSeconds(), tolerance, blotRadius );
	std::printf( failures == 0 ? "budget: ok\n" : "budget: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --steps
//---------------------------------------------------------------------------
// A stroke of slope 1/k at a coarse Step Size. Two stepper motors on a grid
// of pitch p draw it as runs of k p along x with a riser of p between. For
// each pixel column the ink-weighted centroid row is read; half way between
// grid points, one p either side of each run's centre, it must be the run's
// level -- where the unquantised line is p/k away -- and the x at which the
// centroid crosses half way between consecutive levels must advance by k p
// each time, to within one p (the spec's tolerance; the measured figure is
// under a pixel).
//
// Tolerance on the level: the centroid of a Gaussian of sigma >= 0.7 px
// sampled at pixel rows is exact to 1e-3 px (the aliasing term is
// 2 exp( -2 pi^2 sigma^2 )), and the nearest riser is half a p away, 3 sigma,
// so its tail moves the centroid by under 0.03 p: 0.1 px + 0.03 p.
//---------------------------------------------------------------------------
int runStepsCheck( const CheckArgs& args )
{
	constexpr float pitch = 0.025f, k = 6.0f, v = 0.4f;
	const float sigma  = pitch / 6.0f;
	const float aspect = static_cast< float >( args.width ) / args.height;

	Session s;
	if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( v ) ), "Pen Settle=0",
	                             "Pen Width=" + std::to_string( widthParam( sigma ) ), "Step Size=" + std::to_string( stepParam( pitch ) ) } ) )
		return 1;

	const float dy = 0.25f;
	const float x0 = 0.1f * aspect, y0 = 0.3f;
	s.plugin.SetJobForTest( { straightStroke( x0, y0, x0 + k * dy, y0 + dy ) }, true );
	for( int frame = 0; frame < 2000 && !( s.plugin.GetMachine().JobDone() && frame > 2 ); ++frame )
		if( !s.render() )
			return 1;
	std::vector< float > rgba;
	s.paper( rgba );
	s.end();

	//Centroid row per column, in paper units.
	std::vector< double > centroid( args.width, -1.0 );
	for( int x = 0; x < args.width; ++x )
	{
		double sum = 0.0, moment = 0.0;
		for( int y = 0; y < args.height; ++y )
		{
			const double d = rgba[ ( static_cast< size_t >( y ) * args.width + x ) * 4 ];
			sum += d;
			moment += d * rowCentre( y, args.height );
		}
		if( sum > 0.0 )
			centroid[ x ] = moment / sum;
	}

	const double pxUnit    = 1.0 / args.height;
	const double tolerance = 0.1 * pxUnit + 0.03 * pitch;
	int failures = 0;

	//The levels. Level n is at n p; the run at level n spans the x where the
	//line's y rounds to it, k p long. The pen sits on grid points, so its ink
	//is dots at multiples of p joined by the jumps between them, and the
	//risers stand on the grid point nearest each end of a run. Half way
	//between two grid points is the furthest a column can be from any of
	//that -- 3 sigma at sigma = p/6 -- so the two such columns one p either
	//side of the run's centre are read: on a staircase their centroid is the
	//level; on the unquantised line it is p/k = p/6 away.
	const int nFirst = static_cast< int >( std::round( y0 / pitch ) ), nLast = static_cast< int >( std::round( ( y0 + dy ) / pitch ) );
	int checked = 0, wrong = 0;
	double worstLevel = 0.0, leastLine = 1e9;
	for( int n = nFirst + 1; n < nLast; ++n )
	{
		const double level = n * pitch;
		const double xc    = x0 + ( n * pitch - y0 ) * k;//the run's centre, where the line crosses the level
		for( double side : { -1.0, 1.0 } )
		{
			const double want = xc + side * pitch;
			const double mid  = ( std::floor( want / pitch ) + 0.5 ) * pitch;//half way between grid points
			const int col     = colOf( mid, args.width, aspect );
			const double line = y0 + ( colCentre( col, args.width, aspect ) - x0 ) / k;
			leastLine         = std::min( leastLine, std::fabs( line - level ) );
			if( centroid[ col ] < 0.0 )
			{
				++wrong;
				continue;
			}
			const double off = std::fabs( centroid[ col ] - level );
			worstLevel       = std::max( worstLevel, off );
			++checked;
			if( off > tolerance )
				++wrong;
		}
	}
	const bool okLevels = checked > 0 && wrong == 0 && leastLine > tolerance;
	std::printf( "  slope 1/%.0f at pitch %.3f (%.1f px): %d columns between grid points on their level, worst %.3f px (tolerance %.3f px), %d off; the straight line is at least %.3f px away there  %s\n", k,
	             pitch, pitch * args.height, checked, worstLevel / pxUnit, tolerance / pxUnit, wrong, leastLine / pxUnit, verdict( okLevels ) );
	failures += okLevels ? 0 : 1;

	//The risers: where the centroid crosses half way between levels.
	std::vector< double > crossings;
	for( int n = nFirst + 1; n <= nLast; ++n )
	{
		const double half = ( n - 0.5 ) * pitch;
		for( int x = 1; x < args.width; ++x )
			if( centroid[ x - 1 ] >= 0.0 && centroid[ x ] >= 0.0 && centroid[ x - 1 ] < half && centroid[ x ] >= half )
			{
				const double t = ( half - centroid[ x - 1 ] ) / ( centroid[ x ] - centroid[ x - 1 ] );
				crossings.push_back( colCentre( x - 1, args.width, aspect ) + t * ( colCentre( x, args.width, aspect ) - colCentre( x - 1, args.width, aspect ) ) );
				break;
			}
	}
	double worstRun = 0.0;
	int runs = 0, badRuns = 0;
	for( size_t i = 1; i < crossings.size(); ++i )
	{
		const double run = crossings[ i ] - crossings[ i - 1 ];
		worstRun         = std::max( worstRun, std::fabs( run - k * pitch ) );
		++runs;
		if( std::fabs( run - k * pitch ) > pitch )
			++badRuns;
	}
	const int expectedRuns = nLast - nFirst - 1;
	const bool okRuns      = runs >= expectedRuns - 1 && badRuns == 0;
	std::printf( "  %d risers, %d runs of %.3f expected (k p = %.3f), worst %.4f (%.2f px) off, tolerance one p; %d bad  %s\n",
	             static_cast< int >( crossings.size() ), runs, k * pitch, k * pitch, worstRun, worstRun / pxUnit, badRuns, verdict( okRuns ) );
	failures += okRuns ? 0 : 1;

	std::printf( failures == 0 ? "steps: ok\n" : "steps: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --pens
//---------------------------------------------------------------------------
// Eight strokes over four pens, interleaved. With Optimise on the driver
// sorts by pen and the machine makes exactly four trips to the carousel --
// one per distinct pen, the first to pick up a pen at all -- counted by the
// machine as they happen. Off, it changes pen between every pair, eight
// trips with the pickup. And the
// sheet shows each stroke in its pen's colour: the ratio of a stroke's green
// absorbance to its red is (1 - ink_g) / (1 - ink_r), read at the stroke's
// middle. Tolerance 2%: half-float on two channels.
//---------------------------------------------------------------------------
int runPensCheck( const CheckArgs& args )
{
	constexpr float v = 1.0f, a = 8.0f, sigma = 0.004f;
	const float aspect = static_cast< float >( args.width ) / args.height;
	int failures = 0;

	for( int optimise : { 1, 0 } )
	{
		Session s;
		if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( v ) ), "Acceleration=" + std::to_string( accelParam( a ) ),
		                             "Pen Settle=0.05", "Pen Width=" + std::to_string( widthParam( sigma ) ), "Pens=4",
		                             "Optimise=" + std::to_string( optimise ) } ) )
			return 1;

		std::vector< Stroke > job;
		std::vector< int > rows;
		for( int i = 0; i < 8; ++i )
		{
			const int row = args.height / 8 + ( i * args.height * 3 ) / 32;
			rows.push_back( row );
			const float y = static_cast< float >( rowCentre( row, args.height ) );
			job.push_back( straightStroke( 0.2f * aspect, y, 0.8f * aspect, y, i % 4 ) );
		}
		s.plugin.SetJobForTest( job, true );
		for( int frame = 0; frame < 6000 && !( s.plugin.GetMachine().JobDone() && frame > 2 ); ++frame )
			if( !s.render() )
				return 1;
		std::vector< float > rgba;
		s.paper( rgba );
		const long made      = s.plugin.GetMachine().PenChangesMade();
		const int planned    = s.plugin.GetMachine().PenChangesPlanned();
		const long completed = s.plugin.GetMachine().StrokesCompleted();
		s.end();

		bool coloursOk = true;
		double worst   = 0.0;
		for( int i = 0; i < 8; ++i )
		{
			const int col   = colOf( 0.5f * aspect, args.width, aspect );
			const size_t at = ( static_cast< size_t >( rows[ i ] ) * args.width + col ) * 4;
			const double r = rgba[ at ], g = rgba[ at + 1 ];
			const double predicted = inkWeight( i % 4, 1 ) / inkWeight( i % 4, 0 );
			const double measured  = r > 0.0 ? g / r : -1.0;
			worst                  = std::max( worst, std::fabs( measured - predicted ) / predicted );
			if( r <= 0.0 || std::fabs( measured - predicted ) > 0.02 * predicted )
				coloursOk = false;
		}

		//Off, every stroke is a different pen from the one before it, and the
		//first is a pickup from an empty carriage: eight trips.
		const bool ok = completed == 8 && coloursOk && ( optimise ? ( made == 4 && planned == 4 ) : ( made == 8 && planned == 8 ) );
		std::printf( "  Optimise %s: 8 strokes over 4 pens, %d pen changes planned, %ld made (want %d), %ld strokes; every stroke in its pen's colour to %.2f%% (tolerance 2%%)  %s\n",
		             optimise ? "on" : "off", planned, made, optimise ? 4 : 8, completed, worst * 100.0, verdict( ok ) );
		failures += ok ? 0 : 1;
	}

	std::printf( failures == 0 ? "pens: ok\n" : "pens: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --persist
//---------------------------------------------------------------------------
// Ink stays: the sheet after thirty idle frames is byte for byte the sheet
// before them. A resize keeps it: the sheet at twice the raster holds the old
// sheet resampled -- each new texel is the bilinear blend of its four old
// neighbours at quarter offsets (9/16, 3/16, 3/16, 1/16), checked texel by
// texel to half-float plus 8-bit filter weights (0.6% of the old maximum),
// and the total absorbance is four times the old to 0.5%. New Sheet clears
// it to exactly nothing.
//---------------------------------------------------------------------------
int runPersistCheck( const CheckArgs& args )
{
	constexpr float sigma = 0.006f;
	const float aspect = static_cast< float >( args.width ) / args.height;
	int failures = 0;

	Session s;
	if( !beginCheck( s, args, { "Max Speed=" + std::to_string( speedParam( 1.0f ) ), "Acceleration=" + std::to_string( accelParam( 8.0f ) ),
	                             "Pen Settle=0.05", "Pen Width=" + std::to_string( widthParam( sigma ) ) } ) )
		return 1;
	s.plugin.SetJobForTest( { straightStroke( 0.25f * aspect, 0.3f, 0.75f * aspect, 0.7f ), straightStroke( 0.25f * aspect, 0.7f, 0.75f * aspect, 0.3f, 1 ) }, true );
	for( int frame = 0; frame < 2000 && !( s.plugin.GetMachine().JobDone() && frame > 2 ); ++frame )
		if( !s.render() )
			return 1;

	std::vector< float > before, after, big;
	s.paper( before );
	s.renderFrames( 30 );
	s.paper( after );
	const bool stays = before == after && paperSum( before ) > 0.0;
	std::printf( "  30 idle frames: the sheet is %s (sum %.4g)  %s\n", stays ? "unchanged, byte for byte" : "DIFFERENT", paperSum( before ), verdict( stays ) );
	failures += stays ? 0 : 1;

	//Resize to twice the raster.
	const int W = args.width, H = args.height;
	const std::vector< unsigned char > blank( static_cast< size_t >( 2 * W ) * 2 * H * 4, 0 );
	s.resize( 2 * W, 2 * H, blank );
	if( !s.render() )
		return 1;
	s.paper( big );

	const double oldMax = std::max( paperMax( before, 0 ), paperMax( before, 1 ) );
	const double tol    = 0.006 * oldMax + 1e-5;
	auto old = [ & ]( int x, int y, int c ) {
		x = std::clamp( x, 0, W - 1 );
		y = std::clamp( y, 0, H - 1 );
		return static_cast< double >( before[ ( static_cast< size_t >( y ) * W + x ) * 4 + c ] );
	};
	long bad = 0, tested = 0;
	double worst = 0.0;
	for( int y = 0; y < 2 * H; ++y )
		for( int x = 0; x < 2 * W; ++x )
			for( int c = 0; c < 2; ++c )
			{
				//New texel centre in old texel coordinates: (x + 0.5)/2 - 0.5.
				const double ox = ( x + 0.5 ) / 2.0 - 0.5, oy = ( y + 0.5 ) / 2.0 - 0.5;
				const int ix = static_cast< int >( std::floor( ox ) ), iy = static_cast< int >( std::floor( oy ) );
				const double fx = ox - ix, fy = oy - iy;
				const double expected = ( 1 - fx ) * ( 1 - fy ) * old( ix, iy, c ) + fx * ( 1 - fy ) * old( ix + 1, iy, c )
				                        + ( 1 - fx ) * fy * old( ix, iy + 1, c ) + fx * fy * old( ix + 1, iy + 1, c );
				const double got = big[ ( static_cast< size_t >( y ) * 2 * W + x ) * 4 + c ];
				if( expected <= 0.0 && got <= 0.0 )
					continue;
				++tested;
				worst = std::max( worst, std::fabs( got - expected ) );
				if( std::fabs( got - expected ) > tol )
					++bad;
			}
	const double ratio = paperSum( big ) / paperSum( before );
	const bool resized = bad == 0 && tested > 0 && std::fabs( ratio - 4.0 ) <= 0.02;
	std::printf( "  resized %dx%d -> %dx%d: %ld inked texels against the bilinear blend of their old neighbours, worst %.3g (tolerance %.3g), %ld bad; total x%.4f (want 4)  %s\n",
	             W, H, 2 * W, 2 * H, tested, worst, tol, bad, ratio, verdict( resized ) );
	failures += resized ? 0 : 1;

	//New Sheet.
	press( s.plugin, findParameter( s.plugin, "New Sheet" ) );
	if( !s.render() )
		return 1;
	std::vector< float > cleared;
	s.paper( cleared );
	const bool clean = paperSum( cleared ) == 0.0 && paperSum( cleared, 3 ) == 0.0 && s.plugin.SheetCount() == 1;
	std::printf( "  New Sheet: sum %.4g after, %ld sheets  %s\n", paperSum( cleared ), s.plugin.SheetCount(), verdict( clean ) );
	failures += clean ? 0 : 1;
	s.end();

	std::printf( failures == 0 ? "persist: ok\n" : "persist: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --trace
//---------------------------------------------------------------------------
// Through the plugin's own detect passes: a filled square of side H/3 in
// the picture is, after one frame, one closed stroke of the black pen whose
// perimeter in paper units is 4/3 to 8% (the Sobel band and the thinning
// put the skeleton within a trace texel of the edge on each side: at 320
// texels for 1280 that is a texel of 4 px on each of four sides, plus the
// simplification's 1.15 texels -- galvo's tolerance). Then, after ten
// seconds, the sheet has ink on the square's outline and nowhere else.
//---------------------------------------------------------------------------
int runTraceCheck( const CheckArgs& args )
{
	int failures = 0;
	const int side     = args.height / 3;
	const float aspect = static_cast< float >( args.width ) / args.height;

	Session s;
	s.plugin.SetPerturbForTest( args.perturb );
	if( !s.begin( args.width, args.height, buildSquareCard( args.width, args.height, side ) ) )
		return 1;
	for( const char* base : { "Paper=0", "Show Carriage=0", "Show Travel=0", "Auto Sheet=0", "Pens=4" } )
		s.set( base );
	if( !s.render() )
		return 1;

	const std::vector< Stroke >& strokes = s.plugin.LastStrokes();
	const float perimeter = strokes.size() == 1 ? PolylineLength( strokes[ 0 ].points, strokes[ 0 ].closed ) : 0.0f;
	const float expected  = 4.0f * side / static_cast< float >( args.height );
	const bool okTrace    = strokes.size() == 1 && strokes[ 0 ].closed && std::fabs( perimeter - expected ) <= 0.08f * expected
	                        && strokes[ 0 ].pen == 0;
	std::printf( "  filled %d px square at %dx%d: %zu stroke(s), %s, perimeter %.4f (expected %.4f +-8%%), pen %d (want 0, black)  %s\n",
	             side, args.width, args.height, strokes.size(), strokes.empty() ? "-" : ( strokes[ 0 ].closed ? "closed" : "open" ), perimeter,
	             expected, strokes.empty() ? -1 : strokes[ 0 ].pen, verdict( okTrace ) );
	failures += okTrace ? 0 : 1;

	//Ten seconds on: ink on the outline, none well inside or well outside.
	s.renderFrames( 600 );
	std::vector< float > rgba;
	s.paper( rgba );
	const double cx = 0.5 * aspect, cy = 0.5;
	const double halfSide = 0.5 * side / args.height;
	double onEdge = 0.0, inside = 0.0, outside = 0.0;
	int nEdge = 0, nIn = 0, nOut = 0;
	for( int y = 0; y < args.height; ++y )
		for( int x = 0; x < args.width; ++x )
		{
			const double px = colCentre( x, args.width, aspect ), py = rowCentre( y, args.height );
			const double d  = std::max( std::fabs( px - cx ), std::fabs( py - cy ) ) - halfSide;//Chebyshev distance to the square's edge
			const double v  = rgba[ ( static_cast< size_t >( y ) * args.width + x ) * 4 ];
			if( std::fabs( d ) < 0.02 )
			{
				onEdge += v;
				++nEdge;
			}
			else if( d < -0.06 )
			{
				inside += v;
				++nIn;
			}
			else if( d > 0.06 )
			{
				outside += v;
				++nOut;
			}
		}
	const bool okInk = onEdge > 0.0 && inside / std::max( nIn, 1 ) < 0.001 * onEdge / std::max( nEdge, 1 ) && outside / std::max( nOut, 1 ) < 0.001 * onEdge / std::max( nEdge, 1 );
	std::printf( "  after 10 s: ink within 0.02 of the outline %.4g, inside %.3g, outside %.3g (per pixel), %ld strokes done, job %.2f s, %.2f s in, block %zu of %zu, at (%.3f, %.3f), %ld traces  %s\n",
	             onEdge / std::max( nEdge, 1 ), inside / std::max( nIn, 1 ), outside / std::max( nOut, 1 ), s.plugin.GetMachine().StrokesCompleted(),
	             s.plugin.GetMachine().JobSeconds(), s.plugin.GetMachine().JobElapsed(), s.plugin.GetMachine().BlockIndex(),
	             s.plugin.GetMachine().Blocks().size(), s.plugin.GetMachine().Position().x, s.plugin.GetMachine().Position().y, s.plugin.TraceCount(), verdict( okInk ) );
	failures += okInk ? 0 : 1;
	s.end();

	std::printf( failures == 0 ? "trace: ok\n" : "trace: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --negative: every check FAILS on the perturbation built to break it.
//---------------------------------------------------------------------------
int runNegative( CheckArgs args )
{
	struct Case
	{
		const char* name;
		unsigned bits;
		const char* what;
		std::function< int( const CheckArgs& ) > check;
	};
	const Case cases[] = {
		{ "trapezoid", kPerturbInfiniteAccel, "infinite acceleration: a stroke takes L/v, not L/v + v/a", runTrapezoidCheck },
		{ "ink", kPerturbInfiniteAccel, "infinite acceleration: no heavier ends", runInkCheck },
		{ "budget", kPerturbRetraceEachFrame, "a new job every frame instead of on request", runBudgetCheck },
		{ "steps", kPerturbNoQuantise, "Step Size ignored: a slope, not a staircase", runStepsCheck },
		{ "pens", kPerturbNoPenSort, "Optimise ignores the pen: the carousel is visited nearly every stroke", runPensCheck },
		{ "persist", kPerturbResizeClears, "a resize clears the sheet", runPersistCheck },
	};
	int failures = 0;
	args.quiet = true;
	for( const Case& c : cases )
	{
		std::printf( "-- %s with %s --\n", c.name, c.what );
		CheckArgs perturbed = args;
		perturbed.perturb   = c.bits;
		const int result    = c.check( perturbed );
		const bool ok       = result != 0;
		std::printf( "  negative %s: the check %s on the perturbed plugin  %s\n", c.name, ok ? "FAILED, as it must" : "PASSED, which it must not", verdict( ok ) );
		failures += ok ? 0 : 1;
	}
	if( failures == 0 )
		std::printf( "negative: ok (%zu perturbations, every check failed on its own)\n", sizeof( cases ) / sizeof( cases[ 0 ] ) );
	else
		std::printf( "negative: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBench( int frames, double fps, bool with4k )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	std::vector< Size > sizes = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 } };
	if( with4k )
		sizes.push_back( { "3840x2160", 3840, 2160 } );

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides, default controls, the test card.\n", frames );
	std::printf( "Two passes: as shipped (the tracer runs once a job, so almost never), and with a trace forced\n"
	             "every frame, which is the tracer's share -- readback, trace, plan -- in the worst case.\n\n" );
	std::printf( "resolution   ms/frame   of which CPU   | traced every frame: ms/frame   tracer ms   samples/frame\n" );

	for( const Size& size : sizes )
	{
		double ms[ 2 ] = { 0.0, 0.0 }, cpu[ 2 ] = { 0.0, 0.0 }, trace = 0.0;
		size_t samples = 0;
		for( int pass = 0; pass < 2; ++pass )
		{
			Session s;
			s.fps = fps;
			s.plugin.SetPerturbForTest( pass == 1 ? kPerturbRetraceEachFrame : 0 );
			if( !s.begin( size.width, size.height, buildCard( size.width, size.height ) ) )
				return 1;
			s.renderFrames( 20 );
			glFinish();

			long traces = 0;
			const auto start = std::chrono::steady_clock::now();
			for( int frame = 0; frame < frames; ++frame )
			{
				s.render();
				cpu[ pass ] += s.plugin.LastCpuMillis();
				if( pass == 0 )
					samples += s.plugin.LastSampleCount();
				if( s.plugin.TracedLastFrame() )
				{
					trace += pass == 1 ? s.plugin.LastTraceMillis() : 0.0;
					++traces;
				}
			}
			glFinish();
			ms[ pass ] = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
			if( pass == 1 )
				trace /= std::max( traces, 1L );
			s.end();
		}
		std::printf( "%s   %7.3f    %7.3f        |                     %7.3f     %7.3f     %6zu\n", size.name, ms[ 0 ], cpu[ 0 ] / frames,
		             ms[ 1 ], trace, samples / static_cast< size_t >( frames ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders: the exact GLSL the plugin compiles, one file per stage.
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	struct Entry
	{
		const char* file;
		std::string text;
	};
	const Entry entries[] = {
		{ "vertex.vert", kVertexShader },
		{ "copy.frag", kCopyShader },
		{ "edge.frag", kEdgeShader },
		{ "stabilise.frag", kStabiliseShader },
		{ "resample.frag", kResampleShader },
		{ "composite.frag", kCompositeShader },
		{ "ink.vert", InkVertexSource() },
		{ "ink.frag", InkFragmentSource() },
	};
	for( const Entry& e : entries )
	{
		std::ofstream out( dir + "/" + e.file );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), e.file );
			return 1;
		}
		out << e.text;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( entries ) / sizeof( entries[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: raw RGBA frames in, raw RGBA frames out, through the real plugin.
//---------------------------------------------------------------------------
struct Track
{
	std::vector< std::pair< int, float > > keys;
	unsigned int type = FF_TYPE_STANDARD;
};

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].keys.emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.keys.begin(), entry.second.keys.end() );
	return tracks;
}

/// The value at a frame. A standard parameter ramps between keys; an option,
/// boolean or integer STEPS -- it is what the last key said until the next
/// key's frame -- because the values between two options are other options.
float valueAt( const Track& track, int frame )
{
	const auto& keys = track.keys;
	if( keys.empty() )
		return 0.0f;
	if( frame <= keys.front().first )
		return keys.front().second;
	if( frame >= keys.back().first )
		return keys.back().second;

	const bool stepped = track.type == FF_TYPE_OPTION || track.type == FF_TYPE_BOOLEAN || track.type == FF_TYPE_INTEGER;
	for( size_t i = 1; i < keys.size(); ++i )
	{
		if( frame < keys[ i ].first )
		{
			const auto& a = keys[ i - 1 ];
			const auto& b = keys[ i ];
			if( stepped )
				return a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
		if( frame == keys[ i ].first )
			return keys[ i ].second;
	}
	return keys.back().second;
}

/// An event is pressed on the frame of a key at 0.5 or more.
bool pressedAt( const Track& track, int frame )
{
	for( const auto& key : track.keys )
		if( key.first == frame && key.second >= 0.5f )
			return true;
	return false;
}

int runPipe( int width, int height, double fps, const std::string& scriptPath, const std::vector< std::string >& settings,
             int failRenderAt )
{
	//A closed stdout must be a failed write we can see, not a SIGPIPE that
	//kills the process with 141 before it can say so.
	std::signal( SIGPIPE, SIG_IGN );

	Session s;
	s.fps = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( s.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}

	//Resolve the script's parameter names to indices once, up front, and
	//refuse to run on a name that is not a parameter. A misspelled name that
	//silently did nothing would produce a take that looks deliberate and is
	//wrong.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( auto& entry : tracks )
		{
			const unsigned int index = findParameter( s.plugin, entry.first );
			if( index >= Plotter::PT_COUNT )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
			entry.second.type   = s.plugin.GetParamType( index );
			automation[ index ] = entry.second;
		}
	}

	const std::vector< unsigned char > blank( static_cast< size_t >( width ) * height * 4, 0 );
	if( !s.begin( width, height, blank ) )
		return 1;

	std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
	int status = 0;
	for( int index = 0;; ++index )
	{
		//A short read is normal on a pipe, so fill the frame before doing
		//anything with it. A partial frame at the end of the stream is the
		//end of the stream, not a frame.
		size_t filled = 0;
		while( filled < frame.size() )
		{
			const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
			if( got <= 0 )
				break;
			filled += static_cast< size_t >( got );
		}
		if( filled < frame.size() )
		{
			if( filled > 0 )
				std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", filled, frame.size(), width, height );
			break;
		}

		for( const auto& track : automation )
		{
			if( track.second.type == FF_TYPE_EVENT )
			{
				if( pressedAt( track.second, index ) )
					press( s.plugin, track.first );
			}
			else
				s.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );
		}

		//A raw frame arrives top row first and GL wants bottom row first.
		s.rig->upload( flipRows( frame, width, height ) );
		const bool rendered = index != failRenderAt && s.render();
		if( !rendered )
		{
			std::fprintf( stderr, "render failed at frame %d\n", index );
			status = 1;
			break;
		}

		const std::vector< unsigned char > out = s.output();
		size_t written = 0;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
				break;
			written += static_cast< size_t >( put );
		}
		if( written < out.size() )
		{
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			std::fprintf( stderr, "stdout closed at frame %d\n", index );
			status = 1;
			break;
		}
	}

	s.end();
	return status;
}

void usage()
{
	std::printf(
		"pltest -- render and check the Plotter pen-plotter effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/plotter.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --size WxH        picture size (default 1280x720); every check takes it\n"
		"  --frames N        frames to render before reading back (default 90; the sweep uses 150)\n"
		"  --fps N           synthetic frame rate (default 60)\n"
		"  --noise F         per-frame noise on the source, 0..1\n"
		"  --motion          the card's square drifts right, for Chase\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --press \"Name@F\"  press an event parameter before frame F. Repeatable.\n"
		"  --list            print every parameter, its type, default and range, then exit\n"
		"  --names           no parameter name over 16 characters, none twice (no GL)\n"
		"  --plan            the planner and the machine against their closed forms (no GL)\n"
		"  --trapezoid       a straight stroke takes L/v + v/a, or 2 sqrt(L/a)\n"
		"  --ink             ink density along a stroke follows 1/v\n"
		"  --budget          after t seconds, the strokes the profile predicts are drawn\n"
		"  --steps           a coarse Step Size draws a staircase of k steps by one\n"
		"  --pens            pen changes = distinct pens with Optimise, more without\n"
		"  --persist         ink stays, survives a resize, and New Sheet clears it\n"
		"  --trace           a filled square through the detect passes is one closed stroke\n"
		"  --negative        every check above FAILS on a perturbed plugin\n"
		"  --perturb BITS    run any check on a perturbed plugin (bits in Machine.h)\n"
		"  --bench           time ProcessOpenGL at 720p and 1080p; --bench-4k adds 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Parameter Name value'\n"
		"  --dump-shaders D  write the exact GLSL the plugin compiles into directory D\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/plotter.png";
	std::string cardPath, scriptPath, dumpDir;
	int width = 1280, height = 720, frames = 90;
	double fps  = 60.0;
	float noise = 0.0f;
	bool motion = false, verbose = false;
	bool wantList = false, wantNames = false, wantPlan = false, wantTrapezoid = false, wantInk = false, wantBudget = false;
	bool wantSteps = false, wantPens = false, wantPersist = false, wantTrace = false, wantNegative = false;
	bool wantBench = false, with4k = false, wantPipe = false;
	unsigned perturb = 0;
	int failRenderAt = -1;
	std::vector< std::string > settings;
	std::vector< std::pair< std::string, int > > presses;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			if( std::sscanf( argv[ ++i ], "%dx%d", &width, &height ) != 2 )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--motion" )
			motion = true;
		else if( argument == "--verbose" )
			verbose = true;
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--press" && hasNext )
		{
			const std::string spec = argv[ ++i ];
			const size_t at        = spec.find( '@' );
			if( at == std::string::npos )
			{
				std::fprintf( stderr, "--press wants Name@Frame, got '%s'\n", spec.c_str() );
				return 2;
			}
			presses.emplace_back( spec.substr( 0, at ), std::atoi( spec.substr( at + 1 ).c_str() ) );
		}
		else if( argument == "--perturb" && hasNext )
			perturb = static_cast< unsigned >( std::atoi( argv[ ++i ] ) );
		else if( argument == "--fail-render-at" && hasNext )
			failRenderAt = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--names" )
			wantNames = true;
		else if( argument == "--plan" )
			wantPlan = true;
		else if( argument == "--trapezoid" )
			wantTrapezoid = true;
		else if( argument == "--ink" )
			wantInk = true;
		else if( argument == "--budget" )
			wantBudget = true;
		else if( argument == "--steps" )
			wantSteps = true;
		else if( argument == "--pens" )
			wantPens = true;
		else if( argument == "--persist" )
			wantPersist = true;
		else if( argument == "--trace" )
			wantTrace = true;
		else if( argument == "--negative" )
			wantNegative = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--bench-4k" )
			wantBench = with4k = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "size, frames and fps must all be positive\n" );
		return 2;
	}

	//The checks that need no GPU, answered before a context is made -- which
	//also means they run on a machine where making one fails.
	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );
	if( wantList )
	{
		Plotter plugin;
		return listParameters( plugin );
	}
	if( wantNames || wantPlan )
	{
		int failures = 0;
		if( wantNames )
			failures += runNames();
		if( wantPlan )
			failures += runPlanCheck();
		return failures == 0 ? 0 : 1;
	}
	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	CheckArgs args;
	args.width   = width;
	args.height  = height;
	args.perturb = perturb;

	int result = 0;
	if( wantPipe )
		result = runPipe( width, height, fps, scriptPath, settings, failRenderAt );
	else if( wantNegative )
		result = runNegative( args );
	else if( wantTrapezoid || wantInk || wantBudget || wantSteps || wantPens || wantPersist || wantTrace )
	{
		int failures = 0;
		if( wantTrapezoid )
			failures += runTrapezoidCheck( args );
		if( wantInk )
			failures += runInkCheck( args );
		if( wantBudget )
			failures += runBudgetCheck( args );
		if( wantSteps )
			failures += runStepsCheck( args );
		if( wantPens )
			failures += runPensCheck( args );
		if( wantPersist )
			failures += runPersistCheck( args );
		if( wantTrace )
			failures += runTraceCheck( args );
		result = failures == 0 ? 0 : 1;
	}
	else if( wantBench )
		result = runBench( std::max( frames, 30 ), fps, with4k );
	else
	{
		Session s;
		s.fps = fps;
		s.plugin.SetPerturbForTest( perturb );
		for( const std::string& setting : settings )
			if( !s.set( setting ) )
				return 2;
		std::vector< unsigned int > pressIndex;
		for( const auto& p : presses )
		{
			const unsigned int index = findParameter( s.plugin, p.first );
			if( index >= Plotter::PT_COUNT )
			{
				std::fprintf( stderr, "--press %s: no parameter by that name\n", p.first.c_str() );
				return 2;
			}
			pressIndex.push_back( index );
		}

		const std::vector< unsigned char > card = buildCard( width, height );
		if( !s.begin( width, height, card ) )
			return 1;

		for( int frame = 0; frame < frames; ++frame )
		{
			for( size_t i = 0; i < presses.size(); ++i )
				if( presses[ i ].second == frame )
					press( s.plugin, pressIndex[ i ] );
			if( noise > 0.0f || motion )
			{
				const float shift = motion ? 0.1f * static_cast< float >( frame ) / static_cast< float >( fps ) : 0.0f;
				s.rig->upload( addNoise( motion ? buildCard( width, height, shift ) : card, frame, noise ) );
			}
			if( !s.render() )
			{
				std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
				return 1;
			}
		}

		const std::vector< unsigned char > image = s.output();
		if( !writePng( outPath, width, height, image ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s (%dx%d, %d frames, %zu strokes in the job, %ld drawn, %ld traces, %ld pen changes, job %.2f s)\n", outPath.c_str(), width,
		             height, frames, s.plugin.LastStrokes().size(), s.plugin.GetMachine().StrokesCompleted(), s.plugin.TraceCount(),
		             s.plugin.GetMachine().PenChangesMade(), s.plugin.GetMachine().JobSeconds() );
		if( verbose )
			for( const Stroke& st : s.plugin.LastStrokes() )
				std::printf( "  stroke: pen %d ink (%.2f %.2f %.2f) %zu points, %s, length %.3f, starts (%.3f, %.3f)\n", st.pen, st.ink[ 0 ],
				             st.ink[ 1 ], st.ink[ 2 ], st.points.size(), st.closed ? "closed" : "open", PolylineLength( st.points, st.closed ),
				             st.points[ 0 ].x, st.points[ 0 ].y );
		s.end();
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
