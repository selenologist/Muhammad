// Our vendoring glue for memononen/nanovg (pinned ce3bf745): one translation
// unit that instantiates the NanoVG core + the GL3 backend against Mesa's
// glext prototypes. Everything else includes <nanovg.h> + nanovg_gl.h with
// NANOVG_GL3 defined and links against this.
#define GL_GLEXT_PROTOTYPES 1
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <nanovg.h>

#define NANOVG_GL3_IMPLEMENTATION 1
#include <nanovg_gl.h>
