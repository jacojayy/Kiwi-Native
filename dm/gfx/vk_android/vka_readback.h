// PEANUTS: texture readback for the companion runtime (the whole file; vka_readback.inc in vka_tex.c is the replay's side, dm/peanuts/rt/shim/shim_readback.cpp the runtime's).
// A companion's module may ask for a CPU copy of one texture the game keeps only on the GPU (a fog-of-war mask painted into a render target, a minimap rendered to a texture).
// The contract is the companion runtime's own (its "texture" module extension): plain 8-bit textures only, R8 or four bytes a pixel, one whole level 0 of at most 2048 a side
// and 16 MiB, found by its storage address or as the newest of a size and format, narrowed by selector flags; nothing ever waits for the GPU.
//
// How it runs here: vka_rb_submit only queues the request (any thread). The present thread picks the image under the texture cache's lock, holds it (vka_tex_acquire's
// reference and hold: not destroyed meanwhile), records one image-to-buffer copy into the present's own command buffer and hands the bytes over when the present
// has next waited for that submission's fence. Pure C, no Vulkan types: the runtime's shim and the tests on a PC include it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The limits of one request (the runtime's EDEN_DSMOD_TEXTURE_MAX_*), checked again here
enum { VKA_RB_MAX_SIDE = 2048, VKA_RB_MAX_BYTES = 16 << 20, VKA_RB_MAX_IN_FLIGHT = 2 };
// The runtime's timeout of a request, after which a request no present took is answered FAILED; the most images one request's picker is shown
enum { VKA_RB_TIMEOUT_MS = 2000, VKA_RB_MAX_CAND = 512 };
// The layout asked for
enum { VKA_RB_R8 = 1, VKA_RB_RGBA8 = 2 };
// A result's status (the runtime's numbers, passed through)
enum { VKA_RB_READY = 1, VKA_RB_NOT_FOUND = -4, VKA_RB_MISMATCH = -5, VKA_RB_UNSUPPORTED = -6, VKA_RB_FAILED = -7 };
// What a live image holds, as far as a readback tells formats apart; every other format (block-compressed, depth, float, packed, 16-bit) is VKA_RB_F_OTHER and is never converted
enum { VKA_RB_F_OTHER = 0, VKA_RB_F_R8, VKA_RB_F_RGBA8, VKA_RB_F_RGBA8_SRGB, VKA_RB_F_BGRA8, VKA_RB_F_BGRA8_SRGB };

// One live image as the picker sees it
typedef struct {
    uint64_t addr;          // its storage's address in the game's memory (what the game bound the texture's storage at); the name a request asks by
    uint32_t width, height; // the texture's level 0
    uint8_t format;         // VKA_RB_F_*: of the view the game draws it through when it is a colour target, else of the image
    uint8_t plain_2d;       // one 2D single-sample colour image with its level 0, one layer
    uint8_t current;        // the image holds its content (made, and no CPU write to its storage waits to be uploaded)
    uint8_t render_target;  // drawn into or cleared as a colour target since readbacks were first asked for
    uint8_t redrawn;        // ... in at least two different frames
    uint64_t generation;    // raised whenever the GPU writes the image (0: not since readbacks were first asked for); the newest whose write is on the GPU's queue
} vka_rb_cand;

typedef struct { int32_t status; uint32_t index, candidates; } vka_rb_pick;   // status VKA_RB_READY: candidate `index` is read

typedef struct {
    int32_t status;         // VKA_RB_*
    uint64_t addr;          // the image read
    uint32_t width, height;
    uint64_t generation;
    uint32_t candidates;
    const uint8_t *rows;    // VKA_RB_READY: height rows of row_bytes each, the image's first row first, valid during the call only
    size_t row_bytes;
    int bgra;               // the rows hold B, G, R, A
} vka_rb_result;

typedef struct {
    uint64_t addr;          // 0: the newest image of this size and format
    uint32_t width, height;
    uint32_t format;        // VKA_RB_R8 or VKA_RB_RGBA8
    // Chooses among the images at that address (addr != 0) or of that size (addr 0, at most VKA_RB_MAX_CAND), on the present thread with the texture cache locked:
    // it must only compute (no allocation, no lock). Called again at the next present when the image chosen holds nothing a copy may read yet
    vka_rb_pick (*pick)(void *user, const vka_rb_cand *c, uint32_t n);
    // The answer, exactly once for a request vka_rb_submit took. Pixels come on the replay's hand-over thread (a copy's conversion never runs on the present
    // thread); an answer without pixels on the present thread, or inside a later vka_rb_submit when no present took the request within VKA_RB_TIMEOUT_MS
    void (*done)(void *user, const vka_rb_result *r);
    void *user;
} vka_rb_request;

// 1 when a request can be taken now (the replay runs and the GPU has not hung)
int vka_rb_available(void);
// Queues one request; never blocks, never touches the GPU. 1 = taken (done is called once, later), 0 = not (out of the limits, both slots busy, or not available)
int vka_rb_submit(const vka_rb_request *rq);

// ---- pure rules (tested on a PC: dm/peanuts/rt/test/svc_test.cpp) ----
// VkFormat numbers (vulkan_core.h; vka_readback.inc asserts them)
enum { VKA_RB_VK_R8_UNORM = 9, VKA_RB_VK_R8G8B8A8_UNORM = 37, VKA_RB_VK_R8G8B8A8_SRGB = 43, VKA_RB_VK_B8G8R8A8_UNORM = 44, VKA_RB_VK_B8G8R8A8_SRGB = 50,
       VKA_RB_VK_A8B8G8R8_UNORM_PACK32 = 51, VKA_RB_VK_A8B8G8R8_SRGB_PACK32 = 57 };
// An image's format class from its VkFormat and whether the replay stores an sRGB texture in it (vka_fmt.srgb: 8-bit sRGB textures are stored as their UNORM twin)
static inline int vka_rb_format_class(int vk_format, int stored_srgb) {
    switch (vk_format) {
    case VKA_RB_VK_R8_UNORM: return VKA_RB_F_R8;
    case VKA_RB_VK_R8G8B8A8_UNORM: case VKA_RB_VK_A8B8G8R8_UNORM_PACK32: return stored_srgb ? VKA_RB_F_RGBA8_SRGB : VKA_RB_F_RGBA8;   // (A8B8G8R8_PACK32 is R, G, B, A in memory on a little-endian host)
    case VKA_RB_VK_R8G8B8A8_SRGB: case VKA_RB_VK_A8B8G8R8_SRGB_PACK32: return VKA_RB_F_RGBA8_SRGB;
    case VKA_RB_VK_B8G8R8A8_UNORM: return stored_srgb ? VKA_RB_F_BGRA8_SRGB : VKA_RB_F_BGRA8;
    case VKA_RB_VK_B8G8R8A8_SRGB: return VKA_RB_F_BGRA8_SRGB;
    default: return VKA_RB_F_OTHER;
    }
}
static inline uint32_t vka_rb_bpp(uint32_t format) { return format == VKA_RB_RGBA8 ? 4 : 1; }
// A request's own bounds, before anything is looked up
static inline int vka_rb_in_bounds(uint32_t w, uint32_t h, uint32_t format) {
    return (format == VKA_RB_R8 || format == VKA_RB_RGBA8) && w && h && w <= VKA_RB_MAX_SIDE && h <= VKA_RB_MAX_SIDE && (uint64_t)w * h * vka_rb_bpp(format) <= VKA_RB_MAX_BYTES;
}
// Whether an image of class fc can be copied out as the layout asked (R8 from R8 only, four bytes from the four 8-bit colour classes)
static inline int vka_rb_copyable(int fc, uint32_t format) { return format == VKA_RB_R8 ? fc == VKA_RB_F_R8 : fc >= VKA_RB_F_RGBA8 && fc <= VKA_RB_F_BGRA8_SRGB; }

#ifdef __cplusplus
}
#endif
