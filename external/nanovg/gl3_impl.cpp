// Our vendoring glue for memononen/nanovg (pinned ce3bf745): one translation
// unit that instantiates the NanoVG core + the GL3 backend against Mesa's
// glext prototypes. Everything else includes <nanovg.h> + nanovg_gl.h with
// NANOVG_GL3 defined and links against this.
#define GL_GLEXT_PROTOTYPES 1
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#ifdef _WIN32
// Windows Kits' GL/gl.h assumes windows.h came first (it defines neither
// WINGDIAPI nor APIENTRY itself); mingw-w64's copy includes it for us, so
// this only matters for MSVC but is harmless everywhere.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#ifdef _MSC_VER
// Windows Kits ships no GL/glext.h at all; the CMake MSVC branch puts the
// vendored Khronos pair (external/mingw-gl) on the include path BEFORE the
// system. Their prototypes are plain extern declarations (no dllimport), so
// GL 1.2+ calls bind straight to the gui_win32.cpp wglGetProcAddress thunks.
#endif
#endif

#include <nanovg.h>

#define NANOVG_GL3_IMPLEMENTATION 1
#include <nanovg_gl.h>
