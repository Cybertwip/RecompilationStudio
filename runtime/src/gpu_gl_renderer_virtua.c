#include "gpu_gl_renderer.h"
#include "gpu_render.h"

#include <stddef.h>
#include <string.h>

/* Virtua presents the software renderer through /dev/fb0. OpenGL is not part
 * of the platform contract, so selecting it reports unavailable and the
 * renderer facade deterministically falls back to its complete software path. */
const GpuRenderBackend *gl_backend_get(void) { return NULL; }

/* ── native-wide 2D-backdrop x-stretch state ───────────────────────────────
 *
 * These are not GL state, they only happen to live in gpu_gl_renderer.c. Two
 * translation units that are in every build read them: gpu_sw_renderer.c's
 * wide_bd_get(), which is the software half of the same feature, and
 * debug_server.c's ws_backdrop_stretch / ws_dbg_stretch commands. Because this
 * file replaces gpu_gl_renderer.c rather than joining it (runtime.cmake's
 * PSX_VIRTUA branch does a REMOVE_ITEM), stubbing only its functions left ten
 * undefined symbols at link time -- a stand-in has to cover the data surface
 * too. Mutually exclusive TUs, so there is no duplicate definition.
 *
 * Values are the GL renderer's initialisers verbatim, so the software path
 * behaves identically: wide_bd_get() gates on native-wide being active
 * (g_wide_w > 0 && g_wide_cur), which is what actually decides whether any of
 * this applies, and _on = 1 means "not disabled by the debug command". */
int g_ws_bd_stretch_on   = 1;   /* feature on (gated by native-wide + per-prim gate) */
int g_ws_bd_stretch_pct  = 0;   /* 0 = auto (g_wide_w/native_w); else pct/100 */
int g_ws_bd_phase_thresh = 24;  /* "narrow" margin (px) */
int g_ws_bd_phase_mode   = 1;   /* retained for the debug command */

/* Per-frame diagnostics. The GL renderer refreshes these at the end of each
 * frame; nothing does here, because there is no GL frame -- so ws_backdrop_
 * stretch reports the initial values on Virtua. That is the honest answer for
 * a backend that never ran, and it is a report, not a control. */
int g_bdg_applied = 0, g_bdg_prims = 0, g_bdg_clearx = -999999;
int g_bdg_cur = 0, g_bdg_base = 0, g_bdg_w = 0, g_bdg_off = 0;
int gl_renderer_init_context(struct SDL_Window *win) { (void)win; return 0; }
void gl_renderer_set_swap_interval(int interval) { (void)interval; }
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz)
{ (void)enabled; (void)host_hz; (void)target_hz; }
void gl_renderer_set_interpolation_suspended(int suspended) { (void)suspended; }
void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps)
{
    if (enabled) *enabled = 0;
    if (suspended) *suspended = 0;
    if (history_frames) *history_frames = 0;
    if (host_hz) *host_hz = 0.0;
    if (target_hz) *target_hz = 0.0;
    if (swaps) *swaps = 0;
}
void gl_renderer_runtime_diag(uint64_t out[6]) { if (out) memset(out, 0, 6 * sizeof(uint64_t)); }
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3)
{ (void)pixels; (void)src_w; (void)src_h; (void)linear; (void)force_4_3; }
void gl_renderer_present_blank(void) {}
void gl_renderer_sync_cpu(void) {}
void gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3)
{ (void)disp_x; (void)disp_y; (void)w; (void)h; (void)linear; (void)force_4_3; }
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear)
{ (void)disp_x; (void)disp_y; (void)disp_h; (void)linear; return 0; }
void gl_renderer_set_display_aspect(int num, int den) { (void)num; (void)den; }
void gl_renderer_shutdown(void) {}
int gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out)
{ (void)x; (void)y; (void)w; (void)h; (void)out; return 0; }
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5])
{
    if (gpu_dirty) *gpu_dirty = 0;
    if (pending) memset(pending, 0, 5 * sizeof(int));
    if (pack) memset(pack, 0, 5 * sizeof(int));
}
uint64_t gl_renderer_coh_total(void) { return 0; }
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out) { (void)seq; (void)out; return 0; }
uint64_t gl_renderer_pres_total(void) { return 0; }
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out) { (void)seq; (void)out; return 0; }
int gl_renderer_perf_aggregate(int wide_filter, double out[18])
{ (void)wide_filter; if (out) memset(out, 0, 18 * sizeof(double)); return 0; }
void gl_renderer_set_ws_ablate(int mode) { (void)mode; }
int gl_renderer_get_ws_ablate(void) { return 0; }
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac)
{ if (out_tex_frac) *out_tex_frac = 0.0; return 0; }
void gl_renderer_batch_diag(uint64_t out[8]) { if (out) memset(out, 0, 8 * sizeof(uint64_t)); }
void gl_renderer_set_wide_fast(int on) { (void)on; }
int gl_renderer_get_wide_fast(void) { return 0; }
int gl_renderer_vram_diff(uint32_t *count, int bbox[4],
                          int samples[8][2], uint16_t samples_px[8][2])
{
    if (count) *count = 0;
    if (bbox) memset(bbox, 0, 4 * sizeof(int));
    if (samples) memset(samples, 0, 8 * 2 * sizeof(int));
    if (samples_px) memset(samples_px, 0, 8 * 2 * sizeof(uint16_t));
    return 0;
}
