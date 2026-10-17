/* SPDX-License-Identifier: GPL-3.0-only
 * X11 reproduction probe: load muhammad.clap, embed its GUI in a fresh
 * top-level on the current $DISPLAY, pump frames through timer-support exactly
 * like the live GUI pump does, and keep painting until killed. Lets tests drive
 * synthetic clicks (XTEST) and screenshots to verify real button routing.
 * Usage: DISPLAY=:95 gui_probe ./muhammad.clap */
#include <clap/clap.h>
#include <clap/ext/timer-support.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("P5_STORE_DIR", "/tmp/muh-probe-store", 1);
    const char *path = argc > 1 ? argv[1] : "./muhammad.clap";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    auto *entry = (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    if (!entry || !entry->init(path)) { std::printf("entry fail\n"); return 2; }
    auto *getf = entry->get_factory;
    auto *factory = (const clap_plugin_factory_t *)getf(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    clap_host_t host{};
    host.clap_version = CLAP_VERSION_INIT;
    static const clap_host_gui_t probeHostGui = {
        [](const clap_host_t *) {},                                            // resize_hints_changed
        [](const clap_host_t *, uint32_t w, uint32_t h) -> bool
        {   const char *mode = getenv("P5_PROBE_RESIZE");   // refuse|accept(=Bitwig-class lie)
            const bool lie = mode && !strcmp(mode, "accept");
            std::fprintf(stderr, "[probe] host request_resize(%u,%u) -> %s\n", w, h,
                         lie ? "accept (will not honour)" : "refuse");
            return lie; },                                                   // request_resize
        [](const clap_host_t *) -> bool { return true; },                     // request_show
        [](const clap_host_t *) -> bool { return true; },                     // request_hide
        [](const clap_host_t *, bool) {}                                      // closed
    };
    host.get_extension = [](const clap_host_t *, const char *id) -> const void *
    {   return (!strcmp(id, CLAP_EXT_GUI)) ? (const void *)&probeHostGui : nullptr; };
    const clap_plugin_t *plugin = factory->create_plugin(factory, &host, desc->id);
    if (!plugin->init(plugin)) { std::printf("init fail\n"); return 2; }
    plugin->activate(plugin, 44100.0, 512.0, 1u /* steady */);

    Display *d = XOpenDisplay(nullptr);
    if (!d) { std::printf("no display\n"); return 3; }
    Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1280, 352, 1,
                                        BlackPixel(d, 0), WhitePixel(d, 0));
    XStoreName(d, parent, "MUHAMMAD-PROBE");
    XMapWindow(d, parent);
    XSync(d, False);

    auto *gui = (const clap_plugin_gui_t *)plugin->get_extension(plugin, CLAP_EXT_GUI);
    if (!gui) { std::printf("no gui ext\n"); return 4; }
    gui->create(plugin, CLAP_WINDOW_API_X11, false);
    clap_window_t w{};
    w.api = CLAP_WINDOW_API_X11;
    w.x11 = (decltype(w.x11))parent;
    if (!gui->set_parent(plugin, &w)) { std::printf("set_parent failed\n"); return 5; }
    gui->show(plugin);

    auto *timer = (const clap_plugin_timer_support_t *)plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT);
    std::printf("probe up; parent=%lu\n", (unsigned long)parent);
    FILE *ready = fopen("/tmp/probe_ready", "w");
    if (ready) { std::fprintf(ready, "%lu\n", (unsigned long)parent); fclose(ready); }

    const int frames = argc > 2 ? atoi(argv[2]) : 20000;   // default ~5 min
    for (int f = 0; f < frames; f++)
    {
        if (timer) timer->on_timer(plugin, 0);
        usleep(16000);
    }
    gui->hide(plugin);
    gui->destroy(plugin);
    plugin->deactivate(plugin);
    std::printf("probe done\n");
    return 0;
}
