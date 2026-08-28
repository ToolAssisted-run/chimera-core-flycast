/* An OpenGL that lives inside the sandbox.
 *
 * Flycast's own renderer is its OpenGL one: it is what its authors test, what
 * its users run, and what gets a game's picture right. It was unreachable in a
 * core because a core has no GPU and no driver - so this build gives it one
 * that is neither. Mesa's softpipe, compiled into the guest, IS the OpenGL
 * implementation: plain C, no JIT, no runtime CPU dispatch, so the picture is
 * decided entirely by code we compiled and is the same on every machine. See
 * ~/minihawk-tools/mesa-guest for how Mesa is built for the guest and the five
 * small patches it needs.
 *
 * OSMesa is the front end that renders into a memory buffer instead of a
 * window, which is exactly the shape a core wants: there is nothing to swap
 * into, and the frontend takes the finished frame through the ABI as it does
 * from the software rasteriser.
 *
 * The overlays a desktop draws over the picture - a light gun's crosshair, the
 * little VMU screens - belong to a user interface, and a core has none. They
 * are answered as absent.
 */
#include "rend/gles/gles.h"
#include "rend/osd.h"
#include "wsi/gl_context.h"

#include <cstdio>
#include <vector>

/* OSMesa's entry points, declared rather than included.
 *
 * <GL/osmesa.h> pulls in Mesa's own <GL/gl.h>, and this file already has
 * glad's, which declares the same types and the same several thousand enums.
 * The OSMesa API is four functions wide, so it is cheaper to say what they are
 * than to referee two headers. */
extern "C"
{
	typedef struct osmesa_context *OSMesaContext;
	typedef void (*OSMESAproc)();

	OSMesaContext OSMesaCreateContextExt(GLenum format, GLint depthBits,
		GLint stencilBits, GLint accumBits, OSMesaContext sharelist);
	void OSMesaDestroyContext(OSMesaContext ctx);
	GLboolean OSMesaMakeCurrent(OSMesaContext ctx, void *buffer, GLenum type,
		GLsizei width, GLsizei height);
	OSMESAproc OSMesaGetProcAddress(const char *funcName);
}

#define OSMESA_RGBA 0x1908

/* what the renderer draws over the picture on a desktop, and does not here */
u32 vmu_lcd_data[8][48 * 32];
bool vmu_lcd_status[8];
u64 vmuLastChanged[8];

const u32 *getCrosshairTextureData()
{
	static u32 blank[16 * 16];
	return blank;
}

std::pair<float, float> getCrosshairPosition(int playerNum)
{
	return std::make_pair(0.0f, 0.0f);
}

namespace
{

/* The default framebuffer OSMesa hands out. The renderer draws the machine's
 * picture into a framebuffer of its own and we read it back from there, so
 * this one only has to exist and be big enough that nothing is clipped
 * against it. */
const int kSurfaceWidth = 640;
const int kSurfaceHeight = 480;

class ChimeraGLContext : public GLGraphicsContext
{
public:
	ChimeraGLContext() : GLGraphicsContext(nullptr, nullptr) {}

	bool init()
	{
		/* 24 bit depth and 8 bit stencil: the renderer uses both, and asking
		 * for less means it silently loses modifier volumes. */
		context = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, nullptr);
		if (context == nullptr)
		{
			fprintf(stderr, "chimera: OSMesaCreateContext failed\n");
			return false;
		}

		surface.resize((size_t)kSurfaceWidth * kSurfaceHeight * 4);
		if (!OSMesaMakeCurrent(context, surface.data(), GL_UNSIGNED_BYTE,
				kSurfaceWidth, kSurfaceHeight))
		{
			fprintf(stderr, "chimera: OSMesaMakeCurrent failed\n");
			return false;
		}

		/* Flycast reaches GL through glad, which declares every entry point as
		 * a pointer. They are filled from the Mesa linked in beside us. */
		if (gladLoadGL((GLADloadfunc)OSMesaGetProcAddress) == 0)
		{
			fprintf(stderr, "chimera: gladLoadGL failed\n");
			return false;
		}

		postInit();   // asks the driver its version, which is now Mesa's
		return getMajorVersion() > 0;
	}

	/* not an override: the base has no term(), because on a desktop the window
	 * system owns the teardown. */
	void term()
	{
		preTerm();
		if (context != nullptr)
		{
			OSMesaDestroyContext(context);
			context = nullptr;
		}
	}

	/* There is nothing to swap into: the frontend takes the finished frame
	 * through the ABI, as it does from the software rasteriser. */
	void swap() override {}

private:
	OSMesaContext context = nullptr;
	std::vector<u8> surface;
};

ChimeraGLContext *g_context;

} // namespace

/* Whether the OpenGL renderer is up, asked by Renderer_if.cpp when it picks a
 * renderer and by the ABI when it goes looking for the picture. */
bool chimera_gl_available()
{
	return g_context != nullptr;
}

/* Brings OpenGL up. Returns false if Mesa will not start, and then the
 * software rasteriser draws instead - the picture is worse but there is one. */
bool chimera_gl_start()
{
	if (g_context != nullptr)
		return true;

	ChimeraGLContext *ctx = new ChimeraGLContext();
	if (!ctx->init())
	{
		ctx->term();
		delete ctx;
		return false;
	}
	g_context = ctx;
	fprintf(stderr, "chimera: OpenGL is %s (%s)\n",
		glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "?",
		glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
	return true;
}
