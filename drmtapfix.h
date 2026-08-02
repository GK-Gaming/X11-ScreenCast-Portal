#include <sys/wait.h>
#include <sys/socket.h>

#define HELPER_PROTO_MAGIC   0x544D5244u  /* "DRMT" as a little-endian u32 */
#define HELPER_PROTO_VERSION 1u

#define CMD_GRAB       0x01u
#define CMD_GET_CURSOR 0x02u
#define CMD_QUIT       0xFFu

struct drmtap_ctx {
    /* DRM device */
    int drm_fd;
    char device_path[256];
    char driver_name[64];
    /* Render node of THIS device, resolved lazily by drmtap_render_node().
     * Empty until first asked (the device has none, or it was never queried). */
    char render_node[256];

    /* Selected display */
    uint32_t crtc_id;

    /* 1 = context from drmtap_open_render(): render node only, no KMS.
     * Grab entry points reject it; only drmtap_convert_dmabuf() applies. */
    int is_render_only;

    /* Cached resources for hotplug detection */
    uint64_t cached_topology_hash;  /* per-connector state fold; hotplug/modeset signal */

    /* Helper binary */
    char helper_path[512];
    int helper_pid;
    int helper_fd;          /* socket to helper */

    /* Error handling */
    char error_msg[512];

    /* Debug */
    int debug;

    /* ── Persistent fast-grab state (double-buffer cache) ── */
    /* Cache up to 4 buffer slots indexed by fb_id.
     * Compositor typically uses 2-3 buffers (double/triple buffering).
     * Each slot keeps its GEM handle + mmap alive across frames,
     * eliminating GetFB2 + PrimeHandleToFD + mmap/munmap per frame. */
    #define DRMTAP_FAST_SLOTS 4
    struct {
        uint32_t fb_id;         /* KMS framebuffer id (0 = slot unused) */
        uint32_t gem_handle;    /* GEM handle from GetFB2 import */
        int      prime_fd;      /* DMA-BUF fd */
        void    *mmap_ptr;      /* persistent mmap */
        size_t   mmap_size;     /* mapped region size */
        uint32_t width, height, stride;
        /* The framebuffer OBJECT width, kept alongside `width` (which is the
         * SCANNED-OUT width and may be narrower on a padded scanout). Transfer and
         * allocation geometry must use this one: a virtio transfer box describes the
         * buffer being made coherent, not the visible sub-region of it. */
        uint32_t fb_width;
        uint32_t format;
        uint64_t modifier;
        /* Full plane layout captured at cache-miss (GetFB2) time. Restaged
         * into ctx->fb2_* on every cache HIT so the EGL detile / CCS import
         * sees THIS fb's planes, not whatever the last GetFB2 left there. */
        int      fb2_num_planes;
        uint32_t fb2_pitches[4];
        uint32_t fb2_offsets[4];
    } fast_slots[4];
    uint32_t fast_plane_id;         /* cached primary plane id */
    uint32_t fast_last_fb_id;       /* fb_id from last capture (change detect) */
    int      fast_initialized;      /* 1 = plane found, slots ready */
    /* 1 = this device's scanout refused a CPU mmap, so the fast path serves
     * every frame through the EGL fd fallback and caches no slot. Sticky per
     * context: whether a scanout BO is CPU-mappable is a property of the driver
     * and the placement (amdgpu GFX9+ keeps it in VRAM), not of one frame, so
     * retrying the mmap on every frame only buys a failing syscall. Also makes
     * the per-frame "miss" honest in the log: nothing was ever cached to hit. */
    int      fast_no_cpu_map;

    /* Set once drmtap_grab_mapped_fast has established that this process cannot
     * export the scanout itself (drmModeGetFB2 returns handles[0]==0 without
     * CAP_SYS_ADMIN). Unlike drmtap_grab_mapped, the fast path has no helper
     * fallback -- caching a persistent CPU mapping per framebuffer is its whole
     * purpose, and a helper hands over a fresh fd per grab, so there is nothing
     * stable to cache. It therefore cannot work for an unprivileged caller on
     * any GPU. Sticky so the diagnosis is stated once instead of once per frame:
     * reported as pages of "CACHE MISS ... cold start" with the real reason only
     * in drmtap_error(), it read as a broken cache (issue #36). */
    int      fast_no_privilege;

    /* Set once the CRTC mode and the scanout framebuffer have been found to
     * disagree on width, so the reason a frame is narrower than the fb (or is
     * deliberately NOT narrowed) is stated once per context instead of per
     * frame. See drmtap_scanout_width() in drm_grab.c. */
    int      logged_scanout_crop;

    /* 1 = DRM_CLIENT_CAP_ATOMIC has been requested on drm_fd. Set LAZILY, only when
     * a scanout framebuffer turns out to be wider than its mode, because the plane
     * SRC_W/CRTC_W properties that settle whether that is pitch padding or a scaling
     * plane are hidden from a non-atomic client (measured: the same plane reports
     * SRC_W with the cap and nothing without it). Per-fd, needs no privilege, no
     * atomic commit is ever made, and no other client is affected. Sticky so the
     * cap is requested once rather than per frame. */
    int      atomic_cap_tried;

    /* Memoized scanout-width decision for a PADDED scanout. Reading the plane rect
     * costs a plane-resource sweep plus one drmModeGetProperty per property, and the
     * answer only changes when the CRTC, the mode, the framebuffer width or the
     * layout changes -- not per page flip. Keyed on all four so a modeset invalidates
     * it (a mode change with an identical fb width and modifier would otherwise be
     * served the stale answer). The cheap drmModeGetCrtc gate still runs per grab: it
     * is the ioctl the code always did, and it supplies hdisplay for this key. */
    uint32_t sw_key_crtc;
    uint32_t sw_key_hdisplay;
    uint32_t sw_key_fb_width;
    uint64_t sw_key_modifier;
    uint32_t sw_cached_width;
    int      sw_cached;

    /* Caller-supplied destination for converted pixels (drmtap_set_output_buffer).
     * NULL means "use the ctx-owned deswizzle_buf". Set once by the caller, who
     * owns the memory and must keep it alive; libdrmtap never frees or reallocates
     * it, and refuses a frame that would not fit rather than writing short. Every
     * path that produces converted pixels resolves its destination through
     * drmtap_ensure_out(), so the EGL detile and the CPU conversions behave
     * identically -- a caller must not have to know which one ran. */
    void   *user_out;
    size_t  user_out_len;

    /* Deswizzle shadow buffer (for read-only mmap'd DMA-BUFs).
     * Grow-once and reused across grabs; capped at DRMTAP_MAX_FB_BYTES;
     * freed in drmtap_close(). */
    void *deswizzle_buf;
    size_t deswizzle_buf_size;

    /* Helper-mode (V2) pixel receive buffer. Same model as deswizzle_buf:
     * ctx-owned, grow-once, reused across grabs, capped, freed in
     * drmtap_close() — never a per-frame malloc/free. */
    void *pixel_buf;
    size_t pixel_buf_size;

    /* Cached FB2 multi-plane info (for EGL CCS import) */
    uint32_t fb2_pitches[4];
    uint32_t fb2_offsets[4];
    int      fb2_num_planes;  /* number of active planes (1..4) */

    /* HDR state of the frame currently being processed. Set per grab from the
     * connector HDR_OUTPUT_METADATA (helper sends it on the wire; direct mode
     * reads it itself) and consumed by the conversion path to decide whether to
     * tone-map (DRMTAP_EOTF_PQ) or do a plain bit-depth reduction. */
    uint32_t cur_hdr_eotf;     /* DRMTAP_EOTF_* */
    uint32_t cur_hdr_max_nits; /* peak luminance, 0 = unknown */
};


typedef struct {
    uint32_t magic;    /* HELPER_PROTO_MAGIC */
    uint16_t version;  /* HELPER_PROTO_VERSION */
    uint16_t type;     /* CMD_GRAB / CMD_GET_CURSOR / CMD_QUIT */
    uint32_t length;   /* total frame length in bytes (== sizeof(helper_cmd_grab_t)) */
    uint32_t crtc_id;  /* target CRTC id (0 = auto-select first active) */
} helper_cmd_grab_t;   /* 16 bytes, naturally aligned, no padding */

/* Build a command frame with the header fields set for this build. */
static inline helper_cmd_grab_t wire_cmd(uint16_t type, uint32_t crtc_id) {
    helper_cmd_grab_t c;
    memset(&c, 0, sizeof(c));
    c.magic = HELPER_PROTO_MAGIC;
    c.version = HELPER_PROTO_VERSION;
    c.type = type;
    c.length = (uint32_t)sizeof(helper_cmd_grab_t);
    c.crtc_id = crtc_id;
    return c;
}


// Kill the helper process
void drmtap_helper_stop(drmtap_ctx *ctx) {
    if (ctx->helper_fd >= 0) {
        /* Send a full-size quit command (best effort). The helper reads one whole
         * fixed-size command frame per iteration, so a 1-byte quit would be a
         * short read that the helper now rejects; send the complete struct. */
        helper_cmd_grab_t hcmd = wire_cmd(CMD_QUIT, 0);
        ssize_t n = send(ctx->helper_fd, &hcmd, sizeof(hcmd), MSG_NOSIGNAL);
        (void)n;

        close(ctx->helper_fd);
        ctx->helper_fd = -1;
    }

    if (ctx->helper_pid > 0) {
        /* Give helper 100ms to exit, then SIGKILL */
        int status;
        usleep(100000);
        if (waitpid(ctx->helper_pid, &status, WNOHANG) == 0) {
            kill(ctx->helper_pid, SIGKILL);
            waitpid(ctx->helper_pid, &status, 0);
        }
        ctx->helper_pid = -1;
    }
}
