Khronos GL extension headers (GL/glext.h, GL/wglext.h), copied verbatim from
mingw-w64's sysroot (x86_64-w64-mingw32/include/GL, mingw-w64 runtime with
the standard Khronos permissive notice preserved in each file header).

WHY: the Windows Kits ships GL/gl.h but NO glext.h/wglext.h — so MSVC
builds of the panel had no GL 1.2+ declarations at all (CI: C1083). These
headers are used ONLY when compiling with MSVC (CMake adds the directory
under if(MSVC)); MinGW keeps its own sysroot copies, Linux/macOS never see
them. Unlike Microsoft's old glext, they declare prototypes WITHOUT any
dllimport decoration, so the GL 1.2+ calls bind directly to the extern "C"
wglGetProcAddress thunks in src/gui_win32.cpp — on every Windows toolchain.

KHR/khrplatform.h (same mingw-w64 sysroot source, verbatim) rides along: the
Khronos glext.h includes <KHR/khrplatform.h> and neither mingw's sysroot nor
Windows Kits exposes it to MSVC through the GL include path.
