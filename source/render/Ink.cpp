#include "render/Ink.h"

#include "Diag.h"
#include "Shaders.h"

// FFGLSDK.h includes every other scoped binding and omits this one (SDK
// b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

using namespace ffglex;

namespace plotter
{
namespace
{
/// The GL state this renderer changes, captured so it can be put back.
/// FFGL requires the context returned in a default state, and Resolume
/// renders the rest of the composition with whatever it finds -- a plugin
/// that leaves additive blending on makes the *next* effect look broken.
struct ScopedGLState
{
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	GLboolean blend      = GL_FALSE;
	GLint srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;

	ScopedGLState()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		blend = glIsEnabled( GL_BLEND );
		glGetIntegerv( GL_BLEND_SRC_RGB, &srcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &dstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &srcA );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &dstA );
	}
	~ScopedGLState()
	{
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glBlendFuncSeparate( static_cast< GLenum >( srcRGB ), static_cast< GLenum >( dstRGB ),
		                     static_cast< GLenum >( srcA ), static_cast< GLenum >( dstA ) );
		if( blend )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		glBindVertexArray( 0 );
	}
	ScopedGLState( const ScopedGLState& ) = delete;
	ScopedGLState& operator=( const ScopedGLState& ) = delete;
};
} // namespace

bool InkRenderer::InitGL()
{
	const std::string vertex   = InkVertexSource();
	const std::string fragment = InkFragmentSource();

	if( !inkShader.Compile( vertex.c_str(), fragment.c_str() ) )
	{
		diag::error( "the ink shader failed to compile - the effect will do nothing" );
		return false;
	}
	if( !resampleShader.Compile( kVertexShader, kResampleShader ) )
	{
		diag::error( "the resample shader failed to compile - the effect will do nothing" );
		return false;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		return false;
	}

	glGenVertexArrays( 1, &inkVAO );
	glGenBuffers( 1, &inkVBO );
	if( inkVAO == 0 || inkVBO == 0 )
	{
		diag::error( "failed to create the ink vertex array" );
		return false;
	}

	//One buffer of Samples, read twice. Attributes 0/1 start at the beginning
	//and 2/3 one Sample in, so instance i sees samples i and i+1. That is why
	//the draw must ask for n-1 instances: n would read one past the end.
	//
	//glVertexAttribDivisor is VAO state: set it with this VAO bound or it
	//lands somewhere worse than nowhere.
	glBindVertexArray( inkVAO );
	glBindBuffer( GL_ARRAY_BUFFER, inkVBO );

	const GLsizei stride = static_cast< GLsizei >( sizeof( Sample ) );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, stride, nullptr );
	glVertexAttribDivisor( 0, 1 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( float ) * 4 ) );
	glVertexAttribDivisor( 1, 1 );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( Sample ) ) );
	glVertexAttribDivisor( 2, 1 );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( Sample ) + sizeof( float ) * 4 ) );
	glVertexAttribDivisor( 3, 1 );

	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	return true;
}

void InkRenderer::DeInitGL()
{
	inkShader.FreeGLResources();
	resampleShader.FreeGLResources();
	quad.Release();

	if( inkVBO != 0 )
	{
		glDeleteBuffers( 1, &inkVBO );
		inkVBO = 0;
	}
	if( inkVAO != 0 )
	{
		glDeleteVertexArrays( 1, &inkVAO );
		inkVAO = 0;
	}
	paper.Destroy();
	spare.Destroy();
	width = height = 0;
}

bool InkRenderer::Ensure( int requestedWidth, int requestedHeight, float /*aspect*/, bool clearOnResize )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	const bool same = paper.IsValid() && width == requestedWidth && height == requestedHeight;
	if( same )
		return true;

	//Linear, because the composite reads it texel-for-texel (where linear is
	//nearest) and a resize reads it BETWEEN texels.
	if( !paper.IsValid() || clearOnResize )
	{
		paper.Destroy();
		if( !paper.Ensure( requestedWidth, requestedHeight, GL_RGBA32F, PassBuffer::Sampling::Linear ) )
			return false;
		width  = requestedWidth;
		height = requestedHeight;
		return true;
	}

	//The sheet survives: draw the old paper into a new one before letting
	//the old one go. `Ensure` on the spare clears it; the resample fills it.
	spare.Destroy();
	if( !spare.Ensure( requestedWidth, requestedHeight, GL_RGBA32F, PassBuffer::Sampling::Linear ) )
		return false;
	{
		ScopedGLState state;
		ScopedFBOBinding fbo( spare.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, requestedWidth, requestedHeight );
		glDisable( GL_BLEND );
		ScopedShaderBinding shader( resampleShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, paper.TextureID() );
		resampleShader.Set( "PaperTexture", 0 );
		quad.Draw();
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	paper.Swap( spare );
	spare.Destroy();

	width  = requestedWidth;
	height = requestedHeight;
	return true;
}

void InkRenderer::Clear()
{
	paper.Clear();
}

bool InkRenderer::Deposit( const Sample* samples, int n, const Params& params, float aspect )
{
	if( width <= 0 || height <= 0 || !inkShader.IsReady() )
		return false;
	if( samples == nullptr )
		n = 0;
	const int segments = std::max( 0, n - 1 );
	if( segments == 0 )
		return true;

	ScopedGLState state;

	//Upload. GL_STREAM_DRAW and a fresh glBufferData every frame, so the
	//driver orphans the old storage rather than waiting for last frame's draw
	//to finish reading it.
	glBindBuffer( GL_ARRAY_BUFFER, inkVBO );
	glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( static_cast< std::size_t >( n ) * sizeof( Sample ) ),
	              samples, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	ScopedFBOBinding fbo( paper.GetGLID(), ScopedFBOBinding::RB_REVERT );
	//ScopedFBOBinding restores the framebuffer and says nothing about the
	//viewport.
	glViewport( 0, 0, width, height );

	//Additive: ink on ink is more ink. The composite's exponential is what
	//saturates it.
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ONE, GL_ONE );

	ScopedShaderBinding shader( inkShader.GetGLID() );
	inkShader.Set( "InkRate", static_cast< float >( params.inkRate ) );
	inkShader.Set( "TravelRate", static_cast< float >( params.travelRate ) );
	inkShader.Set( "NibSigma", params.nibSigma );
	inkShader.Set( "Aspect", aspect );

	glBindVertexArray( inkVAO );
	glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, segments );
	glBindVertexArray( 0 );
	return true;
}

bool InkRenderer::Read( std::vector< float >& rgba ) const
{
	if( !paper.IsValid() )
		return false;
	rgba.resize( static_cast< std::size_t >( width ) * height * 4 );
	GLint previous = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	glBindFramebuffer( GL_FRAMEBUFFER, paper.GetGLID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, rgba.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previous ) );
	return true;
}

} // namespace plotter
