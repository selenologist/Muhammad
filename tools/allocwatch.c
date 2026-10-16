/* SPDX-License-Identifier: GPL-3.0-only
 * LD_PRELOAD shim: counts heap allocations between allocwatch_set(1)/(0).
 * Usage: LD_PRELOAD=build/allocwatch.so ./clap_smoke ./muhammad.clap
 * The smoke test brackets plugin->process() calls so the count covers only
 * audio-thread work. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

static long g_count;
static int g_on;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void *(*real_aligned)(size_t, size_t);

void allocwatch_set(int on) { g_on = on; }
long allocwatch_count(void) { return g_count; }

static void tally(void) { if (g_on) g_count++; }

void *malloc(size_t s)
{
    if (!real_malloc) real_malloc = dlsym(RTLD_NEXT, "malloc");
    tally();
    return real_malloc(s);
}
void *calloc(size_t a, size_t b)
{
    if (!real_calloc) real_calloc = dlsym(RTLD_NEXT, "calloc");
    tally();
    return real_calloc(a, b);
}
void *realloc(void *p, size_t s)
{
    if (!real_realloc) real_realloc = dlsym(RTLD_NEXT, "realloc");
    tally();
    return real_realloc(p, s);
}
void *aligned_alloc(size_t al, size_t s)
{
    if (!real_aligned) real_aligned = dlsym(RTLD_NEXT, "aligned_alloc");
    tally();
    return real_aligned(al, s);
}
