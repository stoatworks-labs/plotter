#include "PassBuffer.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace ffglex;

namespace plotter
{
PassBuffer::~PassBuffer()
{
	//Nothing GL can be done here -- a destructor may well run without a current
	//context. DeInitGL is where the release happens; this is only a reminder
	//that it has to.
}

bool PassBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling requestedSampling )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( fboID != 0 && width == requestedWidth && height == requestedHeight
	    && internalColorFormat == format && sampling == requestedSampling )
		return true;

	Destroy();

	if( !Initialise( requestedWidth, requestedHeight, format ) )
		return false;

	sampling = requestedSampling;

	{
		Scoped2DTextureBinding textureBinding( colorTextureID );

		//Clamp to edge on every buffer. The default is GL_REPEAT, and a blur
		//tap that wraps puts the right-hand edge of the picture into the
		//left-hand one -- which on a logo reads as a stray bulb floating off
		//the side of the frame with nothing to explain it.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

		switch( sampling )
		{
		case Sampling::Mipmapped:
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			//Allocate the chain now rather than on first use. glGenerateMipmap
			//on a texture with no levels beyond zero is legal but the driver
			//has to allocate them mid-frame, and doing it here keeps the
			//per-frame path free of allocation.
			glGenerateMipmap( GL_TEXTURE_2D );
			break;

		case Sampling::Linear:
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			break;

		case Sampling::Nearest:
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			break;
		}
	}

	Clear();
	return true;
}

void PassBuffer::Clear()
{
	if( fboID == 0 )
		return;

	GLint previousFBO           = 0;
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	glBindFramebuffer( GL_FRAMEBUFFER, previousFBO );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );
}

void PassBuffer::GenerateMipmaps()
{
	if( colorTextureID == 0 || sampling != Sampling::Mipmapped )
		return;

	Scoped2DTextureBinding textureBinding( colorTextureID );
	glGenerateMipmap( GL_TEXTURE_2D );
}

float PassBuffer::MaxMipLevel() const
{
	if( sampling != Sampling::Mipmapped || width <= 0 || height <= 0 )
		return 0.0f;

	return std::floor( std::log2( static_cast< float >( std::max( width, height ) ) ) );
}

void PassBuffer::Swap( PassBuffer& other )
{
	std::swap( width, other.width );
	std::swap( height, other.height );
	std::swap( internalColorFormat, other.internalColorFormat );
	std::swap( fboID, other.fboID );
	std::swap( depthBufferID, other.depthBufferID );
	std::swap( colorTextureID, other.colorTextureID );
	std::swap( sampling, other.sampling );
}

void PassBuffer::Destroy()
{
	//The SDK's Release() leaves this behind. Delete it first, then let the base
	//class deal with the framebuffer and the depth buffer.
	if( colorTextureID != 0 )
	{
		glDeleteTextures( 1, &colorTextureID );
		colorTextureID = 0;
	}

	Release();
}

} // namespace plotter
