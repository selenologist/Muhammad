// Headless panel render for CI: draws the real p5ui::draw() into an EGL
// (software-GL) pbuffer and writes a PNG. Usage:
//   render_golden out.png [state] [width]   state: init|pad|arp|bass
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#ifndef P5_BUILD_INFO
#define P5_BUILD_INFO "dev-cmake-unstamped"
#endif
#include "p5_common.hpp"
#include "p5_engine.hpp"
#include "p5_gui.hpp"

static p5ui::State stateFor(const char *name, p5::Engine &e, int &programOut)
{
    p5::Patch p;
    int program = -1;                 // -1 == INIT
    const bool trimMode = !std::strcmp(name, "trim");
    const bool tuneMode = !std::strcmp(name, "tune");
    const bool debugMode = !std::strcmp(name, "debug");
    if (!std::strcmp(name, "init") || trimMode || tuneMode || debugMode) { p5::patchInit(p); }
    else if (!std::strcmp(name, "pad"))  program = 1;
    else if (!std::strcmp(name, "arp"))  program = 42;
    else if (!std::strcmp(name, "bass")) program = 91;
    else if (!std::strcmp(name, "pick")) { /* INIT content, picker overlay */ }
    else if (!std::strcmp(name, "bank")) program = 1;   // bp columns overlaid below
    else { std::fprintf(stderr, "unknown state '%s'\n", name); std::exit(2); }

    if (program >= 0) e.loadFactory((uint32_t)program);
    else { p5::patchInit(p); e.setPatch(p); }
    programOut = program;
    const p5::Patch &pp = e.patch();

    p5ui::State s;
    if (debugMode)
    {   s.debugOpen = true;
        s.debugText =
            "Muhammad build: 1.13.3-platformgui aabbccd-d 2026-10-17 12:00\n"
            "gui: api x11  host clap.gui svc present  timer registered  rr 2/3  scale 1.00\n"
            "window: win 1280x352  parent 1280x352  ancestor 1400x352\n"
            "audio: sr 44100  oversample 2  polyphony 8  live voices 3  simd avx2\n"
            "patch: src fac  name Peace Be Upon Him\n"
            "tuning: D Pent Test  tones 5  root 2  mask 15  walk 0\n"
            "state: modal 10  tuneFocus 0  wheelN 5  pickerTab 0 page 0\n"
            "store: /home/user/Documents/Muhammad Synth\n";
    }
    if (tuneMode)
    {   // polar editor: melodee's PYTH chromatic dial + matching dev table
        s.tuneOpen = true;
        std::snprintf(s.tuneName, sizeof s.tuneName, "Pythagorean");
        static const float demo[12] = { 113.69f, 203.91f, 294.13f, 407.82f, 498.04f, 611.73f,
                                        701.96f, 815.64f, 905.87f, 996.09f, 1109.78f, 1200.0f };
        static const short devd[12] = { 2, 1, -1, 1, 0, 2, 0, 3, 1, -1, 2, 0 };
        s.wheelC = demo; s.wheelN = 12; s.wheelHot = 5;
        for (int i = 0; i < 12; i++) s.tuneDev[48 + i] = devd[i];
        s.tuneUp = 5.f; s.tuneDn = 2.f; s.tuneRoot = 7;
    }
    if (trimMode)
    {   // voice trim page, partly deflected, for the golden
        s.trimOpen = true;
        for (int i = 0; i < 8; i++) for (int k = 0; k < 2; k++)
        {   s.trimV[4*i+2*k] = (short)(64 + ((i + k) % 3) * 6 - 6);
            s.trimV[4*i+2*k+1] = (short)(64 - i * 3 + k * 2); }
    }
    for (int i = 0; i < p5ui::kNumKnobs; i++)
    {
        const int pid = p5ui::kKnobs[i].pid;
        double v;
        if (pid == 80) v = 0.5;                 // TRIM default (plugin-side, not patch)
        else if (pid == 82) v = 0.0;            // MASTER TUNE default
        else if (pid == 85) v = 0.0;            // PAN WIDTH default (plugin-side)
        else if (pid == 86) v = 0.0;            // DRIFT default (plugin-side)
        else v = pp.raw[pid];
        s.knob[i] = p5::knobNorm(pid, v);
    }
    for (int i = 0; i < p5ui::kNumBtns; i++)
    {
        const auto &b = p5ui::kBtns[i];
        if (b.ledmode == 1 && b.pid >= 0)
            s.led[i] = b.pid == 81 ? e.oversample() > 1   // OVERSAMPLE button: engine state, not a patch byte
                                   : pp.raw[b.pid] != 0;
        else if (b.ledmode == 2) s.led[i] = program >= 0 && (program % 8) == b.value && (program / 8) % 8 == 0;
    }
    std::memcpy(s.name, pp.raw + p5::P5_NAME, 20);
    s.name[20] = 0;
    for (char *q = s.name + 19; q >= s.name && *q == ' '; *q-- = 0) {}
    s.bank = program >= 0 ? program / 8 + 1 : 0;
    s.program = program >= 0 ? program % 8 + 1 : 0;
    if (s.bank > 0) std::snprintf(s.bpSlot, sizeof s.bpSlot, "%d-%d", s.bank, s.program);
    if (!std::strcmp(name, "bank"))
    {   // pending bank moved: "(n)" in its fixed LEFT column, digits don't shift
        std::snprintf(s.bpSlot, sizeof s.bpSlot, "%d-%d", 1, 2);
        std::snprintf(s.bpPend, sizeof s.bpPend, "(%d)", 3);
    }
    if (!std::strcmp(name, "pick"))
    {
        static const char hint[] = "ASSIGN ARMED - PRESS PROGRAM 1..8";
        s.bindHint = hint;   // shown under the readout in the real flow
        static const char title[] = "row = load   ASSIGN then 1-8";
        s.pickerTitle = title;
        s.pickerTab = 0; s.pickerPage = 0; s.pickerPages = 29; s.pickerCount = p5ui::pick::rows;
        for (int i = 0; i < p5ui::pick::rows; i++)
        {
            if (i == 0) std::snprintf(s.pickerRows[i], 44, "  INIT - Peace Be Upon Him (default)");
            else if (i == 1) std::snprintf(s.pickerRows[i], 44, "  INIT (bare patch)");
            else { char nm[24]; p5::Engine::patchNameAt((uint32_t)(i - 1), nm);
                   std::snprintf(s.pickerRows[i], 44, "%3d %s", i - 2, nm); }
        }
    }

    return s;
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: render_golden out.png [init|pad|arp|bass] [width]\n"); return 1; }
    const char *out = argv[1];
    const char *state = argc > 2 ? argv[2] : "init";
    bool stamp = false; int H_arg = 0; (void)stamp;;
    int argi = 3;
    const auto take = [&](const char *fl) {
        while (argi < argc && !std::strcmp(argv[argi], fl)) { stamp = true; argi++; }
    };
    take("-stamp");
    const int W = argi < argc ? std::atoi(argv[argi++]) : (int)p5ui::kPanelW;
    if (argi < argc && argv[argi][0] != '-') { H_arg = std::atoi(argv[argi++]); }
    take("-stamp");
    const int H = H_arg ? H_arg : (int)std::lround(W * p5ui::kPanelH / p5ui::kPanelW);

    // Headless-capable EGL: surfaceless (Mesa) -> device platform -> legacy default.
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                           EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
    {
        dpy = EGL_NO_DISPLAY;
        EGLDeviceEXT devs[8]; EGLint ndev = 0;
        auto qdev = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        if (qdev && qdev(8, devs, &ndev) && ndev > 0)
            dpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[0], nullptr);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
        {
            dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
            { std::fprintf(stderr, "EGL init failed (no display/device?)\n"); return 3; }
        }
    }

    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint nCfg = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &nCfg) || nCfg < 1)
    { std::fprintf(stderr, "no EGL config\n"); return 4; }

    const EGLint surfAttr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surfAttr);  // may fail on surfaceless
    eglBindAPI(EGL_OPENGL_API);
    const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
                               EGL_CONTEXT_MINOR_VERSION, 3,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                               EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT)   // llvmpipe without 3.3 core: fall back to legacy
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, nullptr);
    if (ctx == EGL_NO_CONTEXT) { std::fprintf(stderr, "no GL context\n"); return 6; }

    // Render target: pbuffer if the platform gives one, else our own FBO.
    bool madeCurrent = (surf != EGL_NO_SURFACE && eglMakeCurrent(dpy, surf, surf, ctx));
    if (!madeCurrent)
    {
        surf = EGL_NO_SURFACE;
        if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        { std::fprintf(stderr, "eglMakeCurrent failed\n"); return 5; }
        extern void golden_bindFbo(int, int);   // defined at bottom (GL FBO setup)
        golden_bindFbo(W, H);
    }
    std::fprintf(stderr, "GL: %s | %s\n", (const char *)glGetString(GL_VERSION),
                 (const char *)glGetString(GL_RENDERER));

    p5::Engine engine(p5::KernelMode::Scalar);
    engine.activate(44100);
    int program = -1;
    p5ui::State st = stateFor(state, engine, program);
    if (stamp) st.build = P5_BUILD_INFO;

    NVGcontext *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    p5ui::Gui gui = p5ui::create(vg);
    if (gui.fontId < 0) { std::fprintf(stderr, "font load failed\n"); return 6; }

    glViewport(0, 0, W, H);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, (float)W, (float)H, 1.0f);
    p5ui::draw(gui, st, (float)W, (float)H);
    nvgEndFrame(vg);
    glFinish();

    std::vector<unsigned char> pixels((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (int y = 0; y < H / 2; y++)                       // GL is bottom-up
        for (int x = 0; x < W * 4; x++)
            std::swap(pixels[(size_t)y * W * 4 + x], pixels[(size_t)(H - 1 - y) * W * 4 + x]);

    if (!stbi_write_png(out, W, H, 4, pixels.data(), W * 4))
    { std::fprintf(stderr, "png write failed\n"); return 7; }
    std::printf("wrote %s %dx%d state=%s\n", out, W, H, state);

    nvgDeleteGL3(vg);
    eglDestroyContext(dpy, ctx); eglDestroySurface(dpy, surf); eglTerminate(dpy);
    return 0;
}

// Surfaceless render target: RGBA8 colour + stencil8 attachment.
void golden_bindFbo(int W, int H)
{
    GLuint fbo = 0, tex = 0, rb = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) std::fprintf(stderr, "FBO incomplete: 0x%X\n", status);
}
