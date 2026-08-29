/* The guest's end of the GPU bridge: the context Flycast's GL renderer thinks
 * it is drawing into, when the drawing is actually happening on a real GPU
 * outside the sandbox.
 *
 * There is no driver on this side. Flycast reaches OpenGL through glad, which
 * declares every entry point as a POINTER; the generated half fills those
 * pointers with wrappers that pack their arguments and hand them to the one
 * callback a guest may call out through (waterbox/gl-bridge.h says why that is
 * the only shape available). The renderer itself is untouched.
 *
 * What this file adds is the object Flycast asks for a context: a
 * GLGraphicsContext that made nothing, owns nothing, and answers questions
 * about a device it cannot see. The answers come from the driver, through the
 * bridge - postInit asks GL_VERSION and gets the truth.
 *
 * WHAT THIS COSTS: the GPU is outside the sandbox. It is outside the
 * savestate, outside this core's determinism, and different on every machine.
 * A run recorded this way is not guaranteed to replay anywhere, including
 * here. The softpipe in waterbox/gl-osmesa.cpp is the deterministic answer to
 * the same question, and it is what draws unless a project asks for this.
 */
#include "rend/gles/gles.h"
#include "wsi/gl_context.h"

#include "gl-bridge.h"   /* miniBox source/gl: the shared contract */

/* generated from miniBox's master list; install refuses a host whose list is
 * shorter than this core was built against */
bool chimera_gl_install(chimera_gl_bridge_fn bridge);

namespace
{

/* What the host offered, kept from the moment SetGpuBridge is called until the
 * renderer is chosen. Null on every ordinary run. */
chimera_gl_bridge_fn g_bridge;

class ChimeraBridgedGLContext final : public GLGraphicsContext
{
public:
	/* No window and no display: the context is the host's, and this object
	 * exists so the renderer has something to ask. The base registers itself
	 * as THE graphics context and, in postInit, asks the driver its version -
	 * through the bridge, which is the only honest way for a guest to learn
	 * anything about a GPU it cannot see. */
	ChimeraBridgedGLContext() : GLGraphicsContext(nullptr, nullptr) {}

	bool init()
	{
		postInit();
		return getMajorVersion() > 0;
	}

	/* not an override: the base has no term(), because on a desktop the window
	 * system owns the teardown. */
	void term() { preTerm(); }

	/* There is nothing to swap into: the frontend takes the finished frame
	 * through the ABI, as it does from every other renderer here. */
	void swap() override {}
};

ChimeraBridgedGLContext *g_context;

} // namespace

/* Kept by cinterface's SetGpuBridge, before Init, because Init is where the
 * renderer is chosen. Offering is not using: a project that did not ask for
 * hardware never reaches chimera_gl_start_bridged and the softpipe draws. */
void chimera_gl_bridge_offer(chimera_gl_bridge_fn bridge)
{
	g_bridge = bridge;
}

bool chimera_gl_bridge_offered()
{
	return g_bridge != nullptr;
}

/* Brings the OpenGL renderer up on the host's context. False means the caller
 * should fall back to something that draws inside the sandbox: an older host
 * whose entry-point list is shorter than this core was built against declines
 * here rather than calling into a hole. */
bool chimera_gl_start_bridged()
{
	if (g_bridge == nullptr)
		return false;

	if (!chimera_gl_install(g_bridge))
	{
		fprintf(stderr, "chimera: the host's GL entry points are older than this core\n");
		return false;
	}

	if (g_context == nullptr)
		g_context = new ChimeraBridgedGLContext();

	if (!g_context->init())
		return false;

	fprintf(stderr, "chimera: OpenGL is %s (%s), on a GPU outside the sandbox\n",
		glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "?",
		glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
	return true;
}
