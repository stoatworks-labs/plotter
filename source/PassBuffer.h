#pragma once

#include <FFGLSDK.h>

namespace plotter
{
/**
    An off-screen buffer for one stage of the chain.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame and
    is a no-op in the overwhelming majority of them. The glow buffers change
    size whenever Glow Size moves, which an operator may well be dragging, so
    this path has to be cheap.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first.

    **It owns its filtering**, because this plugin's buffers want three
    different answers:

    - the edge history buffers are read texel-for-texel by the temporal pass
      and must not be filtered at all, or a stabilised edge would creep
      outwards a fraction of a texel per frame and dissolve into a smear over
      a few seconds;
    - the glow buffers are read *between* texels by the separable blur and
      need `GL_LINEAR`, which is what lets one tap average two texels and
      halves the sample count;
    - the stable-edge buffer needs a full mip chain, because the centroid the
      strip coordinate rotates about is read out of its top mip in one fetch.

    Note that the mip chain is what makes the centroid affordable: summing the
    edge mask over the frame is a reduction, and a hardware box filter is a
    reduction the driver has already written.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back -- and for the edge
	/// history buffers, which feed back into themselves, it is noise that never
	/// washes out.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. The centroid pass
	/// needs it as a uniform: `textureQueryLevels` is GLSL 4.30 and these
	/// shaders are 4.10.
	float MaxMipLevel() const;

	/// Clear to transparent black. The edge history needs this when the effect
	/// is re-enabled or the source changes size, so the first stabilised frame
	/// blends against nothing rather than against the last clip.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	/// Exchange the GL objects with another buffer. The paper is resampled on
	/// a resize by drawing the old sheet into a fresh buffer and then swapping
	/// the two, so the old one can be destroyed without a second copy.
	void Swap( PassBuffer& other );

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace plotter
