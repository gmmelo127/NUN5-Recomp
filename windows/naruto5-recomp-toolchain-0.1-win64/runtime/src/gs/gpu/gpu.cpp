#include "gpu.hpp"
#include "ps2/system.hpp"
#include "menu/menu_overlay.hpp"

#include <atomic>
#include "menu/recomp_menu.hpp"

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

extern const char kGsShader[];
extern const char kBlitShader[];

namespace ps2::gs {
void swizzle_tables(std::vector<std::uint32_t> &out);

// Per-primitive switches: never call std::getenv() from draw()/note_raster().
namespace {
const char *const g_env_draw_raw = std::getenv("PS2_DRAW_RAW");
const char *const g_env_draw_frame = std::getenv("PS2_DRAW_FRAME");
const char *const g_env_draw_trace = std::getenv("PS2_DRAW_TRACE");
const char *const g_env_up_trace = std::getenv("PS2_UP_TRACE");
const bool g_env_hud_debug = std::getenv("PS2_HUD_DEBUG") != nullptr;
const bool g_env_round_sprite = std::getenv("PS2_ROUND_SPRITE") != nullptr;
const bool g_env_no_ui_native = std::getenv("PS2_NO_UI_NATIVE") != nullptr;
const char *const g_env_cp_trace = std::getenv("PS2_CP_TRACE");
const char *const g_env_native_dump = std::getenv("PS2_NATIVE_DUMP");
}
}

namespace ps2::gs::gpu {

namespace {

enum Pipeline { kUpload, kCopyRead, kCopyWrite, kLoadClut, kDisplay, kRaster, kResolve, kSnapshot, kDecode, kPipelines };
const char *const kEntryPoints[kPipelines] = {"upload", "copy_read", "copy_write", "load_clut", "display", "raster", "resolve", "snapshot", "decode"};

constexpr std::uint32_t kParamStride = 256;
constexpr std::uint32_t kClutSlots = 2048;
constexpr std::uint32_t kClutEntries = 512;
constexpr std::uint32_t kPrimWords = 28;
constexpr std::uint32_t kSetupWords = 25;
constexpr std::uint32_t kDecodedWords = 4u << 20;
constexpr std::uint32_t kDecodePrims = 64;

Mode g_mode = Mode::off;
std::uint64_t g_dispatches[kPipelines] = {}, g_submits = 0;

WGPUInstance g_instance = nullptr;
WGPUAdapter g_adapter = nullptr;
WGPUSurface g_surface = nullptr;
WGPUTextureFormat g_surface_format = WGPUTextureFormat_Undefined;
WGPUPresentMode g_present_mode = WGPUPresentMode_Fifo;
std::uint32_t g_surface_width = 0, g_surface_height = 0;
WGPURenderPipeline g_blit = nullptr;
WGPUBindGroup g_blit_group = nullptr;
WGPUBindGroupLayout g_blit_layout = nullptr;
WGPUBuffer g_blit_params = nullptr;
// Cheap second pass: blit at Resolution -> upscale to a maximized window.
WGPURenderPipeline g_up_pipeline = nullptr;
WGPUBindGroupLayout g_up_layout = nullptr;
WGPUBindGroup g_up_group = nullptr;
WGPUTexture g_present_tex = nullptr;
WGPUTextureView g_present_tex_view = nullptr;
std::uint32_t g_present_tex_w = 0, g_present_tex_h = 0;
bool g_present_upscale = false;
WGPUTexture g_menu_tex = nullptr;
WGPUTextureView g_menu_view = nullptr;
WGPUSampler g_menu_sampler = nullptr;
std::uint32_t g_menu_w = 0, g_menu_h = 0;
int g_overlay_kind = 0;  // 0 = hidden, 1 = menu, 2 = Master Mode hint
std::uint64_t g_overlay_key = 0;
std::vector<std::uint32_t> g_menu_pixels;
bool g_caps_fifo = true, g_caps_immediate = false, g_caps_mailbox = false;
std::uint32_t g_field = 0, g_last_width = 0, g_last_height = 0;
bool g_have_previous = false;
std::uint32_t g_scale = 1, g_copy_words = 1u << 20, g_field_words = 1u << 19;
std::uint32_t g_last_src_w = 0, g_last_src_h = 0;
// Last frame in which the Master Mode pause menu drew its "Map" icon
// (tbp 15360/PSMT8) in the pause-icon band. The world can use the same
// texture elsewhere, so the screen region is part of the test.
std::atomic<std::uint64_t> g_pause_overlay_frame{0};
std::uint64_t g_draws_this_frame = 0, g_draws_last_frame = 0;
float g_hud_xscale_cache = -1.0f;
bool g_menu_screen = false;
// Viewport chosen by the last present() call (surface pixels). submit() draws
// the blit pass into it, so the aspect selected there also sizes the output.
float g_present_vw = 0.0f, g_present_vh = 0.0f;

std::uint32_t field_base(std::uint32_t field) { return g_copy_words + (field & 1u) * g_field_words; }

bool g_timestamps = false;
WGPUQuerySet g_queries = nullptr;
WGPUBuffer g_query_resolve = nullptr, g_query_read = nullptr;
bool g_query_pending = false, g_query_ready = false;
std::uint64_t g_measured_ns = 0, g_measured = 0, g_total_passes = 0;
WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
WGPUBuffer g_vram = nullptr, g_tables = nullptr, g_params = nullptr, g_data = nullptr, g_clut = nullptr, g_scratch = nullptr, g_readback = nullptr, g_decoded = nullptr;
std::uint32_t g_decoded_used = 0;
bool g_rendered[512] = {};
bool g_decode = true;
// PS2_TEX_GRID selects the texture sampling domain: "sub" (default) samples on
// the supersampled grid, "native" keeps the old native-grid filter for
// compatibility. Anything else falls back to the default with a warning.
std::uint32_t g_tex_grid = 0;
// PS2_SPRITE_UV=center restores the old (pixel-centre) sprite texcoords; the
// default follows the GS and samples at the pixel index.
std::uint32_t g_sprite_uv = 0;
// PS2_TRI_UV=index re-enables the experimental "pixel index" triangle
// texcoords (Etapa B). The default is the pixel centre, which is what the
// reference (PCSX2) matches.
std::uint32_t g_tri_uv = 0;
// PS2_BILIN_STEP=native (experimental, default off) replaces the sub-texel
// bilinear step by a native-texel step on draws whose texture was NOT produced
// by a draw (in_g_rendered == 0). PS2_BILIN_MODE=A|B picks the tap value:
// A = centre sub-texel of the native texel, B = SCALE x SCALE average.
bool g_bilin_step_native = false;
// PS2_BILIN_STEP=writer: native step only where every page of the used UV rect
// was last written by an upload or by the savestate (not by a textured draw).
bool g_bilin_step_writer = false;
// PS2_BILIN_EXCLUDE=<tbp list>: never apply the native step to these textures
// (used with the writer criterion to isolate a render target).
std::vector<std::uint32_t> g_bilin_exclude;

// PS2_DUMP_VRAM_TEX=<tbp,...> + PS2_DUMP_VRAM_FRAME=<frame>: opt-in diagnostic
// readback of the GPU VRAM. Writes PPM (converted to PNG outside) and never
// touches the framebuffer.
struct TexDumpInfo {
    std::uint32_t tbp = 0, tbw = 0, psm = 0, tw = 0, th = 0, clut = 0, cpsm = 0, csa = 0;
    int u0 = 0, v0 = 0, u1 = -1, v1 = -1;
};
std::map<std::uint32_t, TexDumpInfo> g_dump_info;
bool g_dump_on = false;
std::uint64_t g_dump_frame = ~0ull;
std::vector<std::uint32_t> g_dump_tbps;
std::uint32_t g_dump_fb = ~0u;
bool g_tint = false;
std::int64_t g_dump_at_draw = -1;
// PS2_WRITER_EXACT=1: mark only the pages the destination bbox really touches
// (page rows/columns) instead of the whole [lo,hi) address interval.
bool g_writer_exact = false;
std::uint32_t g_bilin_mode = 0;
std::vector<std::uint32_t> g_bilin_tbps;
// Texture Filtering=Bilinear samples these render-target TBPs on the native
// texel grid (minimap + save effect). Other textures keep the sub grid.
std::vector<std::uint32_t> g_native_tbps = {14416u, 14450u, 14928u, 14944u,
                                            15072u, 15136u, 15200u};
// Small textures (fonts/icons/sprites) are magnified a lot on screen, so the
// supersampled sub-grid makes them look nearest. Up to this size they also use
// the native bilinear grid when Texture Filtering=Bilinear.
std::uint32_t g_native_max_tex = 64;
// Native-grid sampling also applies to draws whose texture is magnified by at
// least this factor on screen (fonts/icons/UI), leaving minified world textures
// on the sharp sub grid. 0 disables the criterion.
float g_native_mag = 1.5f;
// PS2_DRAW_SKIP / PS2_DRAW_ONLY: ranges of draw indices (commit order, same as
// the TSV "idx") to drop or to keep. Opt-in diagnostic, no other effect.
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_draw_skip, g_draw_only;
// Which writer last touched each 8 KiB VRAM page: >= 0 draw index, -1 upload,
// -2 local->local copy, -3 state load/unknown. Informative only (PS2_DRAW_LOG).
std::int64_t g_page_writer[512] = {};
std::int64_t g_batch_writer = -4;
bool g_batch_tme = false;
std::uint64_t g_params_size = 0, g_data_size = 0;
WGPUBindGroupLayout g_layout = nullptr;
WGPUPipelineLayout g_pipeline_layout = nullptr;
WGPUBindGroup g_group = nullptr;
WGPUComputePipeline g_pipelines[kPipelines] = {};

struct Command {
    Pipeline pipeline;
    std::uint32_t x, y;
    std::uint32_t param;
};

struct CommandInfo {
    std::size_t index;
    std::uint32_t values[8];
};
std::vector<CommandInfo> g_command_info;
std::FILE *g_profile_out = nullptr;
std::uint64_t g_profile_from = 0, g_profile_to = 0;
WGPUQuerySet g_profile_queries = nullptr;
WGPUBuffer g_profile_resolve = nullptr, g_profile_read = nullptr;

std::vector<Command> g_commands;
std::vector<std::array<std::uint32_t, kParamStride / 4>> g_param_records;
struct Arena {
    std::unique_ptr<std::uint32_t[]> words;
    std::size_t used = 0, capacity = 0;

    std::uint32_t *grow(std::size_t count) {
        if (used + count > capacity) {
            std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 20);
            while (next < used + count) next *= 2;
            auto bigger = std::make_unique_for_overwrite<std::uint32_t[]>(next);
            if (used) std::memcpy(bigger.get(), words.get(), used * 4);
            words = std::move(bigger);
            capacity = next;
        }
        std::uint32_t *at = words.get() + used;
        used += count;
        return at;
    }
    std::size_t size() const { return used; }
    bool empty() const { return !used; }
    void clear() { used = 0; }
    std::uint32_t *data() { return words.get(); }
    std::uint32_t &operator[](std::size_t i) { return words[i]; }
} g_arena;
std::uint32_t g_clut_slot = 0, g_clut_loads = 0, g_texflushes = 0;

struct Image {
    bool active = false;
    std::uint32_t dbp = 0, dbw = 0, dpsm = 0, dsax = 0, dsay = 0, rrw = 0, total = 0, done = 0, bpp = 32, lead_bits = 0, id = 0;
    std::vector<std::uint8_t> pending;
} g_image;
std::uint32_t g_image_ids = 0;

struct Range {
    std::uint64_t lo = ~0ull, hi = 0;
    bool overlaps(std::uint64_t a, std::uint64_t b) const { return lo < b && a < hi; }
    void add(std::uint64_t a, std::uint64_t b) {
        lo = std::min(lo, a);
        hi = std::max(hi, b);
    }
};

struct Batch {
    std::vector<std::uint32_t> prims;
    std::vector<std::uint32_t> setups;
    std::vector<std::uint32_t> boxes;
    std::uint64_t key = ~0ull;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    bool zte = false, zwrite = false;
    std::uint32_t texflushes = 0;
    std::uint32_t rows = 0; // PS2_DRAW_LOG rows buffered for this batch
    int rx0 = 0, ry0 = 0, rx1 = -2, ry1 = -2;
    Range fb_range, z_range;
} g_batch;

struct SetupCache {
    std::uint32_t generation = ~0u, clut = ~0u;
    std::uint64_t attr = ~0ull;
    std::uint32_t setup[kSetupWords] = {};
    std::uint32_t flags = 0;
    std::uint64_t key = 0;
    Range tex;
} g_setup_cache;

// --- PS2_DRAW_LOG=<frame> (opt-in, read only) ------------------------------
// One TSV row per draw of the selected frame. Rows are buffered at draw time
// and written when the batch is flushed, because only then is it known whether
// the texture was decoded (setup[24]). No rendering state is modified.
struct DrawRow {
    std::uint64_t idx = 0;
    std::uint32_t prim = 0, fst = 0, tme = 0;
    std::uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0;
    std::uint64_t tex1 = 0;
    std::uint64_t clamp_raw = 0;
    std::uint32_t fbp = 0, fbw = 0, fbpsm = 0;
    std::uint32_t abe = 0;
    std::uint64_t alpha_raw = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float dudx = 0.0f, dvdy = 0.0f;
    std::uint32_t flags = 0, in_g_rendered = 0;
    std::string last_writer;
    std::string fonte;
    float uniform = -1.0f;
    std::uint32_t f_bilinear = 0, sub_grid = 0, flags_final = 0, step_native = 0;
};
std::vector<DrawRow> g_rows;
std::size_t g_rows_start = 0;
std::FILE *g_draw_log = nullptr;
bool g_draw_log_given_up = false;
bool g_draw_log_on = false;
std::uint64_t g_draw_log_frame = ~0ull;
std::uint64_t g_draw_idx = 0;
std::uint64_t g_draw_frame_seen = ~0ull;

bool in_ranges(const std::vector<std::pair<std::uint32_t, std::uint32_t>> &r, std::uint64_t i) {
    for (const auto &e : r)
        if (i >= e.first && i <= e.second) return true;
    return false;
}

bool draw_skipped(std::uint64_t idx) {
    if (!g_draw_only.empty()) return !in_ranges(g_draw_only, idx);
    if (!g_draw_skip.empty()) return in_ranges(g_draw_skip, idx);
    return false;
}

const char *writer_name(std::int64_t w, char (&buf)[32]) {
    if (w >= 0) {
        std::snprintf(buf, sizeof buf, "draw:%lld", static_cast<long long>(w));
        return buf;
    }
    return w == -1 ? "upload" : w == -2 ? "local_copy" : w == -3 ? "state" : "draw:?";
}

void resolve_batch_rows(Batch &b, std::uint32_t prims) {
    if (prims == 0 || b.rows == 0) return;
    const std::size_t have = g_rows.size() - g_rows_start;
    const std::size_t count = std::min<std::size_t>(b.rows, have);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t *setup = b.setups.data() + b.prims[i * kPrimWords] * kSetupWords;
        DrawRow &row = g_rows[g_rows_start + i];
        row.fonte = setup[24] != 0xFFFFFFFFu ? "decoded" : "vram";
        row.sub_grid = (g_tex_grid && setup[24] == 0xFFFFFFFFu && (setup[0] & 2u)) ? 1u : 0u;
    }
    b.rows = 0;
}

// Defined after read_back(): the uniform-block column needs a VRAM readback.
void finalize_draw_log();
// ---------------------------------------------------------------------------

struct PreOp {
    bool clut = false;
    std::uint32_t rec[12] = {};
    Range range;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, image = 0;
};

bool uploads_overlap(const PreOp &a, const PreOp &b) {
    if (a.image && a.image == b.image) return false;
    if (!a.range.overlaps(b.range.lo, b.range.hi)) return false;
    if (a.rec[0] != b.rec[0] || a.rec[1] != b.rec[1] || a.rec[2] != b.rec[2]) return true;
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// Uploads and CLUT loads run before the pending primitive batch unless they touch memory it reads or writes.
std::vector<PreOp> g_pre;
std::vector<Range> g_batch_reads;

int g_tiles_per_row = 128;
std::vector<std::uint32_t> g_tile_count(128 * 128), g_tile_fill(128 * 128);
std::vector<std::uint32_t> g_touched;

WGPUStringView view(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

void page_size(std::uint32_t psm, std::uint32_t &pw, std::uint32_t &ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
}

Range region(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const auto page = [&](int x, int y) {
        return static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(std::max(x, 0)) / pw + static_cast<std::uint64_t>(std::max(y, 0)) / ph * per_row) * 8192;
    };
    Range r;
    r.add(std::min(page(x0, y0), static_cast<std::uint64_t>(bp) * 256), page(x1, y1) + 8192);
    return r;
}

std::uint32_t z_max(std::uint32_t psm) {
    switch (psm) {
    case PSMZ24: return 0xFFFFFFu;
    case PSMZ16: case PSMZ16S: return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

std::uint32_t push_params(const std::initializer_list<std::uint32_t> values) {
    std::array<std::uint32_t, kParamStride / 4> record{};
    std::size_t i = 0;
    for (std::uint32_t v : values) record[i++] = v;
    g_param_records.push_back(record);
    return static_cast<std::uint32_t>(g_param_records.size() - 1);
}

void dispatch_linear(Pipeline p, std::uint32_t items, std::uint32_t param) {
    const std::uint32_t groups = (items + 63) / 64;
    if (!groups) return;
    g_commands.push_back({p, std::min(groups, 1024u), (groups + 1023) / 1024, param});
}

constexpr std::uint32_t kCacheWords = 4u << 20;
constexpr std::size_t kCacheMinBytes = 256;

struct CacheEntry {
    std::uint64_t key;
    std::uint32_t offset, words;
    std::size_t bytes;
    std::uint64_t used;
};

WGPUBuffer g_cache = nullptr;
std::vector<std::uint32_t> g_cache_copy;
std::deque<CacheEntry> g_cache_ring;
std::unordered_multimap<std::uint64_t, CacheEntry *> g_cache_index;
std::unordered_map<std::uint64_t, std::uint32_t> g_cache_seen;
std::uint32_t g_cache_head = 0;
std::uint64_t g_serial = 1;

std::uint64_t content_key(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = static_cast<std::uint64_t>(n) * 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
    };
    const std::size_t step = std::max<std::size_t>(8, (n / 64) & ~static_cast<std::size_t>(7));
    std::uint64_t v;
    for (std::size_t i = 0; i + 8 <= n; i += step) {
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    v = 0;
    std::memcpy(&v, p + n - std::min<std::size_t>(n, 8), std::min<std::size_t>(n, 8));
    mix(v);
    return h;
}

bool cache_find(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint32_t &offset) {
    const auto [first, last] = g_cache_index.equal_range(key);
    for (auto it = first; it != last; ++it) {
        CacheEntry &e = *it->second;
        if (e.bytes != n || std::memcmp(g_cache_copy.data() + e.offset, bytes, n) != 0) continue;
        e.used = g_serial;
        offset = e.offset;
        return true;
    }
    return false;
}

bool cache_insert(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint32_t &offset) {
    const auto words = static_cast<std::uint32_t>((n + 3) / 4 + 1);
    if (words > kCacheWords / 4) return false;
    const auto evict_front = [] {
        CacheEntry &front = g_cache_ring.front();
        if (front.used == g_serial) return false;
        const auto [first, last] = g_cache_index.equal_range(front.key);
        for (auto it = first; it != last; ++it)
            if (it->second == &front) {
                g_cache_index.erase(it);
                break;
            }
        g_cache_ring.pop_front();
        return true;
    };
    if (g_cache_head + words > kCacheWords) {
        while (!g_cache_ring.empty() && g_cache_ring.front().offset >= g_cache_head)
            if (!evict_front()) return false;
        g_cache_head = 0;
    }
    while (!g_cache_ring.empty() && g_cache_ring.front().offset < g_cache_head + words)
        if (!evict_front()) return false;
    offset = g_cache_head;
    std::uint32_t *to = g_cache_copy.data() + offset;
    to[words - 1] = 0;
    to[words - 2] = 0;
    std::memcpy(to, bytes, n);
    wgpuQueueWriteBuffer(g_queue, g_cache, static_cast<std::uint64_t>(offset) * 4, to, static_cast<std::uint64_t>(words) * 4);
    g_cache_ring.push_back({key, offset, words, n, g_serial});
    g_cache_index.emplace(key, &g_cache_ring.back());
    g_cache_head += words;
    return true;
}

std::uint32_t arena_append(const void *bytes, std::size_t size) {
    const std::uint32_t offset = static_cast<std::uint32_t>(g_arena.size());
    const std::size_t words = (size + 3) / 4 + 1;
    std::uint32_t *to = g_arena.grow(words);
    to[words - 2] = 0;
    to[words - 1] = 0;
    std::memcpy(to, bytes, size);
    return offset;
}

float u2f(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

struct Span {
    std::int16_t ty, tx0, tx1;
};
std::vector<Span> g_spans;
std::vector<std::uint32_t> g_span_start;

void add_spans(const std::uint32_t *rec, std::uint32_t lo, std::uint32_t hi) {
    const int sc = static_cast<int>(g_scale);
    const int tx0 = static_cast<int>(lo & 0xFFFFu) * sc / 16, ty0 = static_cast<int>(lo >> 16) * sc / 16;
    const int tx1 = (static_cast<int>(hi & 0xFFFFu) * sc + sc - 1) / 16, ty1 = (static_cast<int>(hi >> 16) * sc + sc - 1) / 16;
    const auto whole = [&] {
        for (int ty = ty0; ty <= ty1; ++ty) g_spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(tx0), static_cast<std::int16_t>(tx1)});
    };
    // The edge-span math below is expensive (three slopes, two divisions and a
    // per-row scan in doubles). For a small bbox the whole rectangle is only a
    // handful of tiles, and binning it whole is cheaper than the math; the
    // raster shader still tests the real triangle per pixel. The pause/menu
    // scenes have thousands of small UI triangles, so this is a large win.
    static const int whole_tiles = [] {
        const char *e = std::getenv("PS2_SPAN_WHOLE_TILES");
        return e ? std::atoi(e) : 64;
    }();
    if (rec[1] != 0 || tx1 - tx0 < 2 ||
        (whole_tiles > 0 && (tx1 - tx0 + 1) * (ty1 - ty0 + 1) <= whole_tiles)) {
        whole();
        return;
    }
    const double x[3] = {u2f(rec[4]), u2f(rec[6]), u2f(rec[8])}, y[3] = {u2f(rec[5]), u2f(rec[7]), u2f(rec[9])};
    const double inv = u2f(rec[10]);
    static const int kEdges[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    double m[3], q[3];
    int kind[3];
    for (int e = 0; e < 3; ++e) {
        const int a = kEdges[e][0], b = kEdges[e][1];
        const double ca = (y[a] - y[b]) * inv, cb = (x[b] - x[a]) * inv, cc = (x[a] * y[b] - x[b] * y[a]) * inv;
        if (!std::isfinite(ca) || !std::isfinite(cb) || !std::isfinite(cc)) {
            whole();
            return;
        }
        kind[e] = ca > 0.0 ? 1 : ca < 0.0 ? -1 : 0;
        m[e] = kind[e] ? -cb / ca : cb;
        q[e] = kind[e] ? -cc / ca : cc;
    }
    const double span = 16.0 / static_cast<double>(sc);
    for (int ty = ty0; ty <= ty1; ++ty) {
        const double y0 = ty * span - 1.0, y1 = (ty + 1) * span + 1.0;
        double left = -1e30, right = 1e30;
        bool empty = false;
        for (int e = 0; e < 3; ++e) {
            const double a0 = m[e] * y0, a1 = m[e] * y1;
            if (kind[e] == 0) empty = empty || std::max(a0, a1) + q[e] < 0.0;
            else if (kind[e] > 0) left = std::max(left, std::min(a0, a1) + q[e]);
            else right = std::min(right, std::max(a0, a1) + q[e]);
        }
        left -= 1.0;
        right += 1.0;
        if (empty || left > right) continue;
        const int rx0 = std::max(tx0, static_cast<int>(std::floor(std::max(left, -1.0) / span)));
        const int rx1 = std::min(tx1, static_cast<int>(std::floor(std::min(right, 4096.0) / span)));
        if (rx0 <= rx1) g_spans.push_back({static_cast<std::int16_t>(ty), static_cast<std::int16_t>(rx0), static_cast<std::int16_t>(rx1)});
    }
}

bool reads_own_word(const std::uint32_t *rec, const std::uint32_t *setup) {
    const std::uint32_t fpsm = setup[3], tpsm = setup[14];
    if (rec[1] != 1 || (setup[0] & (16u | 16384u)) != 16u || setup[12] != setup[1] || setup[13] != setup[2]) return false;
    float f[6], uv[6];
    std::memcpy(f, rec + 4, sizeof f);
    std::memcpy(uv, rec + 15, sizeof uv);
    const float du = (uv[3] - uv[0]) / 16.0f, dv = (uv[4] - uv[1]) / 16.0f;
    if (f[2] == 0.0f || f[3] == 0.0f || std::fabs(du - f[2]) > 0.0005f * std::fabs(f[2]) || std::fabs(dv - f[3]) > 0.0005f * std::fabs(f[3])) return false;
    const int x0 = static_cast<int>(rec[2] & 0xFFFFu), y0 = static_cast<int>(rec[2] >> 16), x1 = static_cast<int>(rec[3] & 0xFFFFu), y1 = static_cast<int>(rec[3] >> 16);
    const auto texel = [](float a, float b, float origin, float span, int p) {
        return static_cast<int>(std::floor((a + (b - a) * ((static_cast<float>(p) - origin) / span)) / 16.0f));
    };
    for (const int y : {y0, y1})
        if (texel(uv[1], uv[4], f[1], f[3], y) != y) return false;
    const int ox = texel(uv[0], uv[3], f[0], f[2], x0) - x0;
    if (texel(uv[0], uv[3], f[0], f[2], x1) - x1 != ox) return false;
    const auto wide = [](std::uint32_t psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; };
    if (ox == 0) return tpsm == fpsm || (wide(fpsm) && wide(tpsm));
    const bool half = fpsm == PSMCT16 || fpsm == PSMCT16S;
    return half && tpsm == fpsm && (ox == 8 || ox == -8) && x1 - x0 < 8 && (x0 & 8) == (x1 & 8) && ((x0 ^ 8) == x0 + ox);
}

Range read_range(const std::uint32_t *rec, const std::uint32_t *setup, const Range &whole) {
    if (rec[1] != 1 || !(setup[0] & 16u)) return whole;
    float uv[6];
    std::memcpy(uv, rec + 15, sizeof uv);
    const int u0 = static_cast<int>(std::floor(std::min(uv[0], uv[3]) / 16.0f)) - 1, u1 = static_cast<int>(std::ceil(std::max(uv[0], uv[3]) / 16.0f)) + 1;
    const int v0 = static_cast<int>(std::floor(std::min(uv[1], uv[4]) / 16.0f)) - 1, v1 = static_cast<int>(std::ceil(std::max(uv[1], uv[4]) / 16.0f)) + 1;
    const int tw = std::max(1 << (setup[15] & 0xFFu), static_cast<int>(setup[18] >> 16) + 1), th = std::max(1 << (setup[15] >> 8), static_cast<int>(setup[19] >> 16) + 1);
    if (u0 < 0 || v0 < 0 || u1 >= tw || v1 >= th) return whole;
    return region(setup[12], setup[13], setup[14], u0, v0, u1, v1);
}

Range pages(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const auto page = [&](int x, int y) {
        return static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(std::max(x, 0)) / pw + static_cast<std::uint64_t>(std::max(y, 0)) / ph * per_row) * 8192;
    };
    Range r;
    r.add(page(x0, y0), page(x1, y1) + 8192);
    return r;
}

void mark_rendered(const Range &r) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192) g_rendered[(at >> 13) & 511u] = true;
}

void mark_written(const Range &r, std::int64_t w) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192) g_page_writer[(at >> 13) & 511u] = w;
}

void mark_written_exact(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1, std::int64_t w) {
    std::uint32_t pw = 64, ph = 32;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    for (int y = y0 / static_cast<int>(ph); y <= y1 / static_cast<int>(ph); ++y)
        for (int x = x0 / static_cast<int>(pw); x <= x1 / static_cast<int>(pw); ++x) {
            const std::uint64_t at = static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(x) + static_cast<std::uint64_t>(y) * per_row) * 8192;
            g_page_writer[(at >> 13) & 511u] = w;
        }
}

bool pages_static_exact(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw = 64, ph = 32;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    for (int y = y0 / static_cast<int>(ph); y <= y1 / static_cast<int>(ph); ++y)
        for (int x = x0 / static_cast<int>(pw); x <= x1 / static_cast<int>(pw); ++x) {
            const std::uint64_t at = static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(x) + static_cast<std::uint64_t>(y) * per_row) * 8192;
            const std::int64_t w = g_page_writer[(at >> 13) & 511u];
            if (w != -1 && w != -3) return false;
        }
    return true;
}

bool rendered(const Range &r) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192)
        if (g_rendered[(at >> 13) & 511u]) return true;
    return false;
}

struct Decode {
    std::uint32_t setup, offset, words;
};
std::vector<Decode> g_decodes;

void plan_decodes(std::uint32_t prims) {
    Batch &b = g_batch;
    g_decodes.clear();
    if (!g_decode || prims <= kDecodePrims) return;
    const std::uint32_t setups = static_cast<std::uint32_t>(b.setups.size() / kSetupWords);
    static const int kKey[] = {12, 13, 14, 15, 16, 17, 20, 22};
    for (std::uint32_t i = 0; i < setups; ++i) {
        std::uint32_t *w = b.setups.data() + i * kSetupWords;
        if (!(w[0] & 2u)) continue;
        bool shared = false;
        for (const Decode &d : g_decodes) {
            const std::uint32_t *o = b.setups.data() + d.setup * kSetupWords;
            bool same = (o[0] & 32768u) == (w[0] & 32768u);
            for (int k : kKey) same = same && o[k] == w[k];
            if (same) {
                w[24] = d.offset;
                shared = true;
                break;
            }
        }
        if (shared) continue;
        const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
        const std::uint32_t mu = (w[7] >> 24) & 3u, mv = (w[7] >> 26) & 3u;
        const auto inside = [](std::uint32_t mode, std::uint32_t region, std::uint32_t size) { return mode < 2 || ((region & 0x3FFu) < (1u << size) && (region >> 16) < (1u << size)); };
        if (!inside(mu, w[18], tw) || !inside(mv, w[19], th)) continue;
        const std::uint32_t words = 1u << (tw + th);
        if (g_decoded_used + words > kDecodedWords) continue;
        if (g_scale > 1 && rendered(pages(w[12], w[13], w[14], 0, 0, (1 << tw) - 1, (1 << th) - 1))) continue;
        w[24] = g_decoded_used;
        g_decodes.push_back({i, g_decoded_used, words});
        g_decoded_used += words;
    }
}

std::uint32_t snapshot_batch(std::uint32_t prims, std::uint32_t &lo, std::uint32_t &hi) {
    Batch &b = g_batch;
    lo = hi = 0;
    if (g_batch_reads.empty()) return 0;
    const Range fb = pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    const Range z = pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
    const auto written = [&](const Range &t) { return t.overlaps(fb.lo, fb.hi) || (b.zwrite && t.overlaps(z.lo, z.hi)); };
    bool any = false;
    for (const Range &t : g_batch_reads) any = any || written(t);
    if (!any) return 0;
    Range shared;
    for (std::uint32_t i = 0; i < prims; ++i) {
        const std::uint32_t *rec = b.prims.data() + i * kPrimWords;
        const std::uint32_t *setup = b.setups.data() + rec[0] * kSetupWords;
        if (!(setup[0] & 2u) || reads_own_word(rec, setup)) continue;
        const int tw = std::max(1 << (setup[15] & 0xFFu), static_cast<int>(setup[18] >> 16) + 1), th = std::max(1 << (setup[15] >> 8), static_cast<int>(setup[19] >> 16) + 1);
        const Range t = read_range(rec, setup, region(setup[12], setup[13], setup[14], 0, 0, tw - 1, th - 1));
        for (const Range *w : {&fb, b.zwrite ? &z : nullptr})
            if (w && t.overlaps(w->lo, w->hi)) shared.add(std::max(t.lo, w->lo), std::min(t.hi, w->hi));
    }
    if (shared.lo >= shared.hi) return 0;
    lo = static_cast<std::uint32_t>(std::min<std::uint64_t>(shared.lo, kMemorySize) / 4);
    hi = static_cast<std::uint32_t>(std::min<std::uint64_t>(shared.hi, kMemorySize) / 4);
    if (hi <= lo) return 0;
    const std::uint32_t words = (hi - lo) * g_scale * g_scale;
    dispatch_linear(kSnapshot, words, push_params({lo, words}));
    return words;
}

void note_raster(std::uint32_t prims, std::uint32_t tiles, std::uint32_t entries, std::uint32_t snapshot) {
    if (const char *tr = g_env_draw_trace) {
        unsigned long long a = 0, z = 0;
        if (std::sscanf(tr, "%llu,%llu", &a, &z) == 2 && frame_count() >= a && frame_count() <= z) {
            const std::uint32_t *st = g_batch.setups.data();
            std::fprintf(stderr, "[draw] prims %u bbox %d %d %d %d s0 %08X s7 %08X s8 %02X s9 %08X tex0 %08X %u %u %04X zn %d\n",
                         prims, g_batch.minx, g_batch.miny, g_batch.maxx, g_batch.maxy, st[0], st[7], st[8], st[9],
                         st[12], st[13], st[14], st[15], g_batch.zwrite ? 1 : 0);
            std::fprintf(stderr, "        cpsm %u csa %u clslot %u cbp %08X texa %08X\n",
                         st[16], st[17], st[22], static_cast<unsigned>(g_state.ctx[0].tex0 >> 37) & 0x3FFFu, st[20]);
        }
    }
    if (!g_profile_out) return;
    const Batch &b = g_batch;
    const std::uint32_t *first = b.setups.data();
    std::uint32_t textured = 0;
    for (std::uint32_t i = 0; i < prims; ++i) textured += (b.setups[b.prims[i * kPrimWords] * kSetupWords] & 2u) ? 1u : 0u;
    g_command_info.push_back({g_commands.size(), {prims, tiles, entries, snapshot, b.prims[1], first[3] | (first[14] << 8) | (first[6] << 16), first[0], textured}});
}

std::uint64_t g_flush_calls = 0, g_flush_prims = 0;
double g_batch_t_spans = 0, g_batch_t_plan = 0, g_batch_t_snap = 0, g_batch_t_fill = 0;
bool g_batch_stats = std::getenv("PS2_BATCH_STATS") != nullptr;
inline double batch_now() {
    return g_batch_stats ? std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0.0;
}

void flush_batch() {
    Batch &b = g_batch;
    if (b.prims.empty()) {
        b.key = ~0ull;
        return;
    }
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
    ++g_flush_calls;
    g_flush_prims += prims;
    const double t0 = batch_now();
    g_touched.clear();
    g_spans.clear();
    g_span_start.resize(prims + 1);
    for (std::uint32_t i = 0; i < prims; ++i) {
        g_span_start[i] = static_cast<std::uint32_t>(g_spans.size());
        add_spans(b.prims.data() + i * kPrimWords, b.boxes[i * 2], b.boxes[i * 2 + 1]);
        for (std::size_t k = g_span_start[i]; k < g_spans.size(); ++k) {
            const Span &sp = g_spans[k];
            std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_tiles_per_row + sp.tx0);
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx, ++t)
                if (!g_tile_count[t]++) g_touched.push_back(t);
        }
    }
    g_span_start[prims] = static_cast<std::uint32_t>(g_spans.size());
    const double t1 = batch_now();
    plan_decodes(prims);
    const double t2 = batch_now();
    resolve_batch_rows(b, prims);
    const std::uint32_t prim_offset = arena_append(b.prims.data(), b.prims.size() * 4);
    const std::uint32_t setup_offset = arena_append(b.setups.data(), b.setups.size() * 4);
    for (const Decode &d : g_decodes) dispatch_linear(kDecode, d.words, push_params({setup_offset + d.setup * kSetupWords, d.offset, d.words}));
    std::uint32_t total = 0;
    for (std::uint32_t t : g_touched) total += g_tile_count[t];
    const std::uint32_t tiles = static_cast<std::uint32_t>(g_touched.size());
    const std::uint32_t tile_offset = static_cast<std::uint32_t>(g_arena.size());
    const std::uint32_t list_offset = tile_offset + tiles * 2 + 2;
    const std::uint32_t list_words = (total + 1) / 2;
    std::uint32_t *grown = g_arena.grow(tiles * 2 + 2 + list_words);
    if (list_words) grown[tiles * 2 + 2 + list_words - 1] = 0;
    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < tiles; ++i) {
        const std::uint32_t t = g_touched[i];
        grown[i * 2] = (t % static_cast<std::uint32_t>(g_tiles_per_row)) | ((t / static_cast<std::uint32_t>(g_tiles_per_row)) << 16);
        grown[i * 2 + 1] = cursor;
        g_tile_fill[t] = cursor;
        cursor += g_tile_count[t];
    }
    grown[tiles * 2] = 0;
    grown[tiles * 2 + 1] = cursor;
    auto *halves = reinterpret_cast<std::uint16_t *>(grown + tiles * 2 + 2);
    for (std::uint32_t i = 0; i < prims; ++i) {
        const auto index = static_cast<std::uint16_t>(i);
        for (std::uint32_t k = g_span_start[i]; k < g_span_start[i + 1]; ++k) {
            const Span &sp = g_spans[k];
            std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_tiles_per_row + sp.tx0);
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx, ++t) halves[g_tile_fill[t]++] = index;
        }
    }
    for (std::uint32_t t : g_touched) g_tile_count[t] = 0;
    const double t3 = batch_now();
    std::uint32_t snapshot_lo, snapshot_hi;
    const std::uint32_t snapshot = snapshot_batch(prims, snapshot_lo, snapshot_hi);
    const double t4 = batch_now();
    const std::uint32_t param = push_params({tile_offset, prim_offset, setup_offset, tiles, list_offset, snapshot_lo, snapshot_hi, g_tex_grid, g_sprite_uv, g_tri_uv, g_bilin_mode, g_tint ? 1u : 0u});
    note_raster(prims, tiles, total, snapshot);
    g_commands.push_back({kRaster, std::min(tiles, 1024u), (tiles + 1023) / 1024, param});
    const Range fb_pages = pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    mark_rendered(fb_pages);
    // Overlays without a texture (TME=0, e.g. the 18850 fade) do not count as a
    // texture writer: they must not hide the real producer of a page.
    if (g_batch_tme) {
        if (g_writer_exact) mark_written_exact(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy, g_batch_writer);
        else mark_written(fb_pages, g_batch_writer);
    }
    if (b.zwrite) {
        const Range z_pages = pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
        mark_rendered(z_pages);
        if (g_batch_tme) {
            if (g_writer_exact) mark_written_exact(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy, g_batch_writer);
            else mark_written(z_pages, g_batch_writer);
        }
    }
    const double t5 = batch_now();
    g_batch_t_spans += t1 - t0;
    g_batch_t_plan += t2 - t1;
    g_batch_t_fill += t3 - t2;
    g_batch_t_snap += t4 - t3;
    b.prims.clear();
    b.setups.clear();
    b.boxes.clear();
    g_batch_reads.clear();
    b.key = ~0ull;
    b.minx = b.miny = 1 << 30;
    b.maxx = b.maxy = -1;
}

void batch_ranges() {
    Batch &b = g_batch;
    if (b.rx0 == b.minx && b.ry0 == b.miny && b.rx1 == b.maxx && b.ry1 == b.maxy) return;
    b.rx0 = b.minx;
    b.ry0 = b.miny;
    b.rx1 = b.maxx;
    b.ry1 = b.maxy;
    b.fb_range = region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    b.z_range = region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
}

bool batch_conflict(const Range &r, bool clut) {
    Batch &b = g_batch;
    if (b.prims.empty()) return false;
    batch_ranges();
    if (b.fb_range.overlaps(r.lo, r.hi)) return true;
    if (b.zte && b.z_range.overlaps(r.lo, r.hi)) return true;
    if (!clut)
        for (const Range &t : g_batch_reads)
            if (t.overlaps(r.lo, r.hi)) return true;
    return false;
}

void emit_pre() {
    std::size_t i = 0;
    while (i < g_pre.size()) {
        const bool clut = g_pre[i].clut;
        std::size_t j = i + 1;
        while (j < g_pre.size() && g_pre[j].clut == clut) {
            bool overlap = false;
            if (!clut)
                for (std::size_t k = i; k < j && !overlap; ++k) overlap = uploads_overlap(g_pre[k], g_pre[j]);
            if (overlap) break;
            ++j;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(j - i);
        std::uint32_t *records = g_arena.grow(count * 12u);
        const std::uint32_t offset = static_cast<std::uint32_t>(records - g_arena.data());
        std::uint32_t total = 0;
        for (std::size_t k = i; k < j; ++k) {
            std::memcpy(records + (k - i) * 12, g_pre[k].rec, sizeof g_pre[k].rec);
            if (!clut) {
                records[(k - i) * 12 + 11] = total;
                total += g_pre[k].rec[7];
            }
        }
        if (clut) g_commands.push_back({kLoadClut, 1, 1, push_params({offset, count})});
        else dispatch_linear(kUpload, total, push_params({offset, count, total}));
        i = j;
    }
    g_pre.clear();
}

void emit_all() {
    emit_pre();
    flush_batch();
}

void queue_pre(const PreOp &op) {
    if (batch_conflict(op.range, op.clut)) emit_all();
    g_pre.push_back(op);
}

std::uint64_t queue_upload(const std::uint8_t *bytes, std::size_t size, std::uint32_t lead_bits) {
    Image &im = g_image;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    if (bits <= lead_bits) return 0;
    const std::uint32_t pixels = static_cast<std::uint32_t>(std::min<std::uint64_t>((bits - lead_bits) / im.bpp, im.total - im.done));
    if (!pixels) return 0;
    const std::uint64_t used = lead_bits + static_cast<std::uint64_t>(pixels) * im.bpp;
    PreOp op;
    const auto n = static_cast<std::size_t>((used + 7) / 8);
    std::uint32_t offset = 0, source = 0;
    if (n >= kCacheMinBytes) {
        const std::uint64_t key = content_key(bytes, n);
        if (cache_find(key, bytes, n, offset) || (++g_cache_seen[key] >= 2 && cache_insert(key, bytes, n, offset))) source = 0x80000000u;
        if (g_cache_seen.size() > 65536) g_cache_seen.clear();
    }
    if (!source) offset = arena_append(bytes, n);
    const std::uint32_t rec[11] = {im.dbp, im.dbw, im.dpsm, im.dsax, im.dsay, im.rrw, im.done, pixels, offset, lead_bits, im.bpp | source};
    if (const char *tr = g_env_up_trace) {
        const std::uint64_t want = std::strtoull(tr, nullptr, 10);
        if (ps2::frame_count() == want)
            std::fprintf(stderr, "[up f%llu] dbp %u dbw %u psm %02X dsax %u dsay %u rrw %u done %u pixels %u bpp %u lead %u\n",
                         (unsigned long long)ps2::frame_count(), im.dbp, im.dbw, im.dpsm, im.dsax, im.dsay,
                         im.rrw, im.done, pixels, im.bpp, lead_bits);
    }
    std::memcpy(op.rec, rec, sizeof rec);
    const std::uint32_t first = im.done / im.rrw, last = (im.done + pixels - 1) / im.rrw;
    op.range = region(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay + first), static_cast<int>(im.dsax + im.rrw - 1),
                      static_cast<int>(im.dsay + last));
    op.x0 = im.dsax;
    op.x1 = im.dsax + im.rrw - 1;
    op.y0 = im.dsay + first;
    op.y1 = im.dsay + last;
    op.image = im.id;
    queue_pre(op);
    im.done += pixels;
    return used;
}

void finish_image_if_done() {
    Image &im = g_image;
    if (im.done < im.total) return;
    im.active = false;
    im.pending.clear();
    const std::uint32_t psm = im.dpsm;
    if (psm != PSMCT32 && psm != PSMCT16 && psm != PSMCT16S && psm != PSMT8 && psm != PSMT4 && psm != PSMZ32 && psm != PSMZ16 && psm != PSMZ16S) return;
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint32_t per_row = std::max<std::uint32_t>(1, im.dbw * 64 / pw), rows = im.rrw ? im.total / im.rrw : 0;
    for (std::uint32_t py = (im.dsay + ph - 1) / ph; py < (im.dsay + rows) / ph; ++py)
        for (std::uint32_t px = (im.dsax + pw - 1) / pw; px < (im.dsax + im.rrw) / pw; ++px) {
            const std::uint32_t page = ((im.dbp * 256 + (px + py * per_row) * 8192) >> 13) & 511u;
            g_rendered[page] = false;
            g_page_writer[page] = -1;
        }
}

void flush_image() {
    Image &im = g_image;
    if (!im.active || im.pending.empty()) return;
    const std::uint64_t used = queue_upload(im.pending.data(), im.pending.size(), im.lead_bits);
    if (!used) return;
    im.pending.erase(im.pending.begin(), im.pending.begin() + static_cast<std::ptrdiff_t>(used / 8));
    im.lead_bits = static_cast<std::uint32_t>(used % 8);
    finish_image_if_done();
}

void flush_all() {
    flush_image();
    emit_all();
}

void on_error(WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *, void *) {
    std::fprintf(stderr, "[gpu] error %d: %.*s\n", static_cast<int>(type), static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length), message.data);
}

void pump() {
#ifdef __EMSCRIPTEN__
    browser_yield();
    wgpuInstanceProcessEvents(g_instance);
#else
    if (g_device) wgpuDevicePoll(g_device, true, nullptr);
    else wgpuInstanceProcessEvents(g_instance);
#endif
}

void wait_until(const bool &flag) {
    while (!flag) pump();
}

WGPUBuffer make_buffer(std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage;
    return wgpuDeviceCreateBuffer(g_device, &d);
}

void rebuild_group() {
    if (g_group) wgpuBindGroupRelease(g_group);
    WGPUBindGroupEntry e[8] = {};
    WGPUBuffer buffers[8] = {g_vram, g_tables, g_params, g_data, g_clut, g_scratch, g_cache, g_decoded};
    for (int i = 0; i < 8; ++i) {
        e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[i].binding = static_cast<std::uint32_t>(i);
        e[i].buffer = buffers[i];
        e[i].size = i == 2 ? kParamStride : WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g_layout;
    d.entryCount = 8;
    d.entries = e;
    g_group = wgpuDeviceCreateBindGroup(g_device, &d);
}

void ensure_capacity() {
    bool changed = false;
    const std::uint64_t data_bytes = std::max<std::uint64_t>(g_arena.size() * 4, 16);
    if (data_bytes > g_data_size) {
        if (g_data) wgpuBufferRelease(g_data);
        g_data_size = std::max<std::uint64_t>(g_data_size * 2, (data_bytes + (4u << 20) - 1) & ~((4ull << 20) - 1));
        g_data = make_buffer(g_data_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    const std::uint64_t param_bytes = std::max<std::uint64_t>(g_param_records.size(), 1) * kParamStride;
    if (param_bytes > g_params_size) {
        if (g_params) wgpuBufferRelease(g_params);
        g_params_size = std::max<std::uint64_t>(g_params_size * 2, std::max<std::uint64_t>(param_bytes, 64 * 1024));
        g_params = make_buffer(g_params_size, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    if (changed || !g_group) rebuild_group();
}

void profile_submit() {
    constexpr std::uint32_t kMax = 2048;
    if (!g_profile_queries) {
        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qd.type = WGPUQueryType_Timestamp;
        qd.count = kMax * 2;
        g_profile_queries = wgpuDeviceCreateQuerySet(g_device, &qd);
        g_profile_resolve = make_buffer(kMax * 16, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
        g_profile_read = make_buffer(kMax * 16, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }
    ensure_capacity();
    wgpuQueueWriteBuffer(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    wgpuQueueWriteBuffer(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(g_commands.size(), kMax));
    for (std::uint32_t i = 0; i < g_commands.size(); ++i) {
        const Command &c = g_commands[i];
        WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
        writes.querySet = g_profile_queries;
        writes.beginningOfPassWriteIndex = i * 2;
        writes.endOfPassWriteIndex = i * 2 + 1;
        WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        if (i < n) cd.timestampWrites = &writes;
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
        wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    }
    if (n) {
        wgpuCommandEncoderResolveQuerySet(enc, g_profile_queries, 0, n * 2, g_profile_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_profile_resolve, 0, g_profile_read, 0, n * 16);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    if (n) {
        bool done = false;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
        cb.userdata1 = &done;
        wgpuBufferMapAsync(g_profile_read, WGPUMapMode_Read, 0, n * 16, cb);
        wait_until(done);
        const auto *t = static_cast<const std::uint64_t *>(wgpuBufferGetConstMappedRange(g_profile_read, 0, n * 16));
        std::size_t info = 0;
        for (std::uint32_t i = 0; t && i < n; ++i) {
            while (info < g_command_info.size() && g_command_info[info].index < i) ++info;
            std::fprintf(g_profile_out, "%llu %u %s %llu %u %u", static_cast<unsigned long long>(frame_count()), i, kEntryPoints[g_commands[i].pipeline],
                         static_cast<unsigned long long>(t[i * 2 + 1] > t[i * 2] ? t[i * 2 + 1] - t[i * 2] : 0), g_commands[i].x, g_commands[i].y);
            if (info < g_command_info.size() && g_command_info[info].index == i)
                for (std::uint32_t v : g_command_info[info].values) std::fprintf(g_profile_out, " %u", v);
            std::fprintf(g_profile_out, "\n");
        }
        wgpuBufferUnmap(g_profile_read);
    }
    g_commands.clear();
    g_command_info.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_decoded_used = 0;
    g_clut_loads = 0;
}

// Browsers copy writes above about 1 MiB through a slower path, so large uploads go in pieces.
void submit(WGPUTextureView present_view = nullptr, bool upscale = false) {
    if (g_commands.empty() && !present_view) return;
    if (g_profile_out && !present_view && g_timestamps && frame_count() >= g_profile_from && frame_count() < g_profile_to) {
        profile_submit();
        return;
    }
    ensure_capacity();
    wgpuQueueWriteBuffer(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    wgpuQueueWriteBuffer(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    if (g_query_ready) {
        const void *mapped = wgpuBufferGetConstMappedRange(g_query_read, 0, 16);
        if (mapped) {
            std::uint64_t t[2];
            std::memcpy(t, mapped, sizeof t);
            if (t[1] > t[0]) {
                g_measured_ns += t[1] - t[0];
                ++g_measured;
            }
        }
        wgpuBufferUnmap(g_query_read);
        g_query_ready = g_query_pending = false;
    }
    const bool measure = g_timestamps && !g_query_pending;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    writes.querySet = g_queries;
    writes.beginningOfPassWriteIndex = 0;
    writes.endOfPassWriteIndex = 1;
    WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    if (measure) cd.timestampWrites = &writes;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
    Pipeline current = kPipelines;
    ++g_submits;
    ++g_total_passes;
    for (const Command &c : g_commands) {
        ++g_dispatches[c.pipeline];
        if (c.pipeline != current) {
            wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
            current = c.pipeline;
        }
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    if (measure) {
        wgpuCommandEncoderResolveQuerySet(enc, g_queries, 0, 2, g_query_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_query_resolve, 0, g_query_read, 0, 16);
    }
    if (present_view) {
        const float w = static_cast<float>(g_surface_width), h = static_cast<float>(g_surface_height);
        const float aspect = g_present_vw > 0.0f && g_present_vh > 0.0f
                                 ? g_present_vw / g_present_vh
                                 : (g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f);
        const float vw = g_present_vw > 0.0f ? g_present_vw : std::min(w, h * aspect);
        const float vh = g_present_vh > 0.0f ? g_present_vh : vw / aspect;
        const auto draw_blit = [&](WGPUTextureView view, float x, float y, float dw, float dh) {
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = view;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.colorAttachmentCount = 1;
            rd.colorAttachments = &color;
            WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
            wgpuRenderPassEncoderSetViewport(rp, x, y, dw, dh, 0.0f, 1.0f);
            wgpuRenderPassEncoderSetPipeline(rp, g_blit);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, g_blit_group, 0, nullptr);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
        };
        if (upscale && g_present_tex_view && g_up_group && g_up_pipeline) {
            // Heavy blit at the configured size, then a cheap scale-to-window.
            draw_blit(g_present_tex_view, 0.0f, 0.0f,
                      static_cast<float>(g_present_tex_w), static_cast<float>(g_present_tex_h));
            const float uw = std::min(w, h * aspect), uh = uw / aspect;
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = present_view;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.colorAttachmentCount = 1;
            rd.colorAttachments = &color;
            WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
            wgpuRenderPassEncoderSetViewport(rp, (w - uw) * 0.5f, (h - uh) * 0.5f, uw, uh, 0.0f, 1.0f);
            wgpuRenderPassEncoderSetPipeline(rp, g_up_pipeline);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, g_up_group, 0, nullptr);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
        } else {
            draw_blit(present_view, (w - vw) * 0.5f, (h - vh) * 0.5f, vw, vh);
        }
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    if (measure) {
        g_query_pending = true;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *, void *) {
            if (status == WGPUMapAsyncStatus_Success) g_query_ready = true;
            else g_query_pending = false;
        };
        wgpuBufferMapAsync(g_query_read, WGPUMapMode_Read, 0, 16, cb);
    }
#ifdef __EMSCRIPTEN__
    wgpuInstanceProcessEvents(g_instance);
#else
    wgpuDevicePoll(g_device, false, nullptr);
#endif
    g_commands.clear();
    g_command_info.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_decoded_used = 0;
    g_clut_loads = 0;
}

void read_back(WGPUBuffer source, std::uint64_t bytes, void *out, std::uint64_t offset = 0) {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, source, offset, g_readback, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *flag, void *) {
        if (status != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[gpu] map failed %d\n", static_cast<int>(status));
        *static_cast<bool *>(flag) = true;
    };
    cb.userdata1 = &done;
    wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0, bytes, cb);
    wait_until(done);
    const void *mapped = wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
    if (mapped) std::memcpy(out, mapped, bytes);
    wgpuBufferUnmap(g_readback);
}

// --- PS2_DRAW_LOG uniformity column ----------------------------------------
// CPU mirror of locate() in gs.wgsl, so the used UV rect can be mapped back to
// the storage words and each native texel's SCALE x SCALE sub-texels checked
// for equality. Read-only: it only touches a readback buffer.
namespace {
constexpr std::uint32_t kTOff32 = 0u, kTOffZ32 = 2048u, kTOff16 = 4096u, kTOff16S = 8192u;
constexpr std::uint32_t kTOffZ16 = 12288u, kTOffZ16S = 16384u, kTOff8 = 20480u, kTOff4 = 28672u;

struct TexLoc {
    std::uint32_t word, shift, mask;
};

const std::vector<std::uint32_t> &swizzle_table() {
    static std::vector<std::uint32_t> table;
    if (table.empty()) ps2::gs::swizzle_tables(table);
    return table;
}

std::uint32_t page_addr(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t pw, std::uint32_t ph) {
    const std::uint32_t per_row = std::max(1u, bw * 64u / pw);
    return bp * 256u + ((x / pw) + (y / ph) * per_row) * 8192u;
}

TexLoc locate_texel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    const std::vector<std::uint32_t> &t = swizzle_table();
    std::uint32_t a = 0, shift = 0, mask = 0xFFFFFFFFu;
    switch (psm) {
    case 0x30u: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOffZ32 + (y & 31u) * 64u + (x & 63u)]; break;
    case 0x31u: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOffZ32 + (y & 31u) * 64u + (x & 63u)]; mask = 0xFFFFFFu; break;
    case 0x01u: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOff32 + (y & 31u) * 64u + (x & 63u)]; mask = 0xFFFFFFu; break;
    case 0x02u: a = page_addr(bp, bw, x, y, 64, 64) + t[kTOff16 + (y & 63u) * 64u + (x & 63u)]; shift = (a & 2u) * 8u; mask = 0xFFFFu; break;
    case 0x0Au: a = page_addr(bp, bw, x, y, 64, 64) + t[kTOff16S + (y & 63u) * 64u + (x & 63u)]; shift = (a & 2u) * 8u; mask = 0xFFFFu; break;
    case 0x32u: a = page_addr(bp, bw, x, y, 64, 64) + t[kTOffZ16 + (y & 63u) * 64u + (x & 63u)]; shift = (a & 2u) * 8u; mask = 0xFFFFu; break;
    case 0x3Au: a = page_addr(bp, bw, x, y, 64, 64) + t[kTOffZ16S + (y & 63u) * 64u + (x & 63u)]; shift = (a & 2u) * 8u; mask = 0xFFFFu; break;
    case 0x13u: a = page_addr(bp, bw, x, y, 128, 64) + t[kTOff8 + (y & 63u) * 128u + (x & 127u)]; shift = (a & 3u) * 8u; mask = 0xFFu; break;
    case 0x14u: {
        const std::uint32_t n = t[kTOff4 + (y & 127u) * 128u + (x & 127u)];
        a = page_addr(bp, bw, x, y, 128, 128) + n / 2u;
        shift = (a & 3u) * 8u + (n & 1u) * 4u;
        mask = 0xFu;
        break;
    }
    case 0x1Bu: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOff32 + (y & 31u) * 64u + (x & 63u)]; shift = 24u; mask = 0xFFu; break;
    case 0x24u: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOff32 + (y & 31u) * 64u + (x & 63u)]; shift = 24u; mask = 0xFu; break;
    case 0x2Cu: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOff32 + (y & 31u) * 64u + (x & 63u)]; shift = 28u; mask = 0xFu; break;
    default: a = page_addr(bp, bw, x, y, 64, 32) + t[kTOff32 + (y & 31u) * 64u + (x & 63u)]; break;
    }
    return TexLoc{(a & 0x3FFFFFu) >> 2u, shift, mask};
}

struct TexKey {
    std::uint32_t tbp, tbw, psm, tw, th;
    bool operator<(const TexKey &o) const {
        if (tbp != o.tbp) return tbp < o.tbp;
        if (tbw != o.tbw) return tbw < o.tbw;
        if (psm != o.psm) return psm < o.psm;
        if (tw != o.tw) return tw < o.tw;
        return th < o.th;
    }
};

// Fraction (0..100) of the native texels in [u0..u1]x[v0..v1] whose
// SCALE x SCALE sub-texels are all equal. -1 when the range cannot be read.
float uniform_fraction(const TexKey &k, int u0, int v0, int u1, int v1) {
    const int maxx = (1 << k.tw) - 1, maxy = (1 << k.th) - 1;
    u0 = std::clamp(u0, 0, maxx); u1 = std::clamp(u1, 0, maxx);
    v0 = std::clamp(v0, 0, maxy); v1 = std::clamp(v1, 0, maxy);
    if (u0 > u1 || v0 > v1) return -1.0f;
    std::uint32_t lo = ~0u, hi = 0;
    for (int y = v0; y <= v1; ++y)
        for (int x = u0; x <= u1; ++x) {
            const TexLoc l = locate_texel(k.psm, k.tbp, k.tbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            lo = std::min(lo, l.word);
            hi = std::max(hi, l.word);
        }
    const std::uint64_t words = static_cast<std::uint64_t>(hi - lo + 1) * g_scale * g_scale;
    if (words == 0 || words * 4 > static_cast<std::uint64_t>(g_copy_words) * 4) return -1.0f;
    std::vector<std::uint32_t> tex(words);
    read_back(g_vram, words * 4, tex.data(), static_cast<std::uint64_t>(lo) * g_scale * g_scale * 4);
    std::uint64_t total = 0, uniform = 0;
    for (int y = v0; y <= v1; ++y)
        for (int x = u0; x <= u1; ++x) {
            const TexLoc l = locate_texel(k.psm, k.tbp, k.tbw, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::uint32_t base = (l.word - lo) * g_scale * g_scale;
            const std::uint32_t first = (tex[base] >> l.shift) & l.mask;
            bool same = true;
            for (std::uint32_t s = 1; s < g_scale * g_scale && same; ++s) same = ((tex[base + s] >> l.shift) & l.mask) == first;
            ++total;
            uniform += same ? 1u : 0u;
        }
    return total ? 100.0f * static_cast<float>(uniform) / static_cast<float>(total) : -1.0f;
}
} // namespace

void finalize_draw_log() {
    if (!g_draw_log_on || g_rows.empty()) return;
    if (ps2::frame_count() < g_draw_log_frame) return;
    submit();
    if (const char *pd = std::getenv("PS2_PAGE_DUMP")) {
        const std::uint32_t want = static_cast<std::uint32_t>(std::strtoul(pd, nullptr, 0));
        std::uint32_t seen[8][3] = {};
        int shown = 0;
        for (const DrawRow &r : g_rows) {
            if (r.tbp0 != want || !r.tme || shown >= 8) continue;
            bool dup = false;
            for (int i = 0; i < shown; ++i)
                dup = dup || (seen[i][0] == r.psm && seen[i][1] == r.tw && seen[i][2] == r.th);
            if (dup) continue;
            seen[shown][0] = r.psm; seen[shown][1] = r.tw; seen[shown][2] = r.th;
            ++shown;
            const Range pg = pages(r.tbp0, r.tbw, r.psm, 0, 0, static_cast<int>(r.tw) - 1, static_cast<int>(r.th) - 1);
            std::fprintf(stderr, "[pages] tbp %u psm %02X %dx%d:", r.tbp0, r.psm, r.tw, r.th);
            for (std::uint64_t at = pg.lo; at < pg.hi; at += 8192)
                std::fprintf(stderr, " %lld", static_cast<long long>(g_page_writer[(at >> 13) & 511u]));
            std::fprintf(stderr, "\n");
        }
    }
    std::map<TexKey, std::array<int, 4>> rects;
    for (const DrawRow &r : g_rows) {
        if (!r.tme || r.u0 < 0) continue;
        auto it = rects.find(TexKey{r.tbp0, r.tbw, r.psm, r.tw, r.th});
        if (it == rects.end()) {
            rects.emplace(TexKey{r.tbp0, r.tbw, r.psm, r.tw, r.th}, std::array<int, 4>{r.u0, r.v0, r.u1, r.v1});
        } else {
            it->second[0] = std::min(it->second[0], r.u0);
            it->second[1] = std::min(it->second[1], r.v0);
            it->second[2] = std::max(it->second[2], r.u1);
            it->second[3] = std::max(it->second[3], r.v1);
        }
    }
    std::map<TexKey, float> frac;
    for (const auto &kv : rects) frac[kv.first] = uniform_fraction(kv.first, kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
    if (!g_draw_log && !g_draw_log_given_up) {
        char path[512];
        std::snprintf(path, sizeof path, "evidencias/ETAPA_E/T1/draws_%llu.tsv", static_cast<unsigned long long>(g_draw_log_frame));
        g_draw_log = std::fopen(path, "w");
        if (!g_draw_log) {
            std::fprintf(stderr, "[gpu] PS2_DRAW_LOG: cannot open %s\n", path);
            g_draw_log_given_up = true;
            g_rows.clear();
            return;
        }
        std::fprintf(g_draw_log,
                     "idx\tprim\tfst\ttme\ttbp0\ttbw\tpsm\ttw\tth\ttex1_raw\tmmag\tmmin\tlcm\tl\tk\tclamp_raw\t"
                     "fbp\tfbw\tfbpsm\tabe\talpha_raw\tbbox_screen\tuv_min\tuv_max\tdudx\tdvdy\tflags\t"
                     "in_g_rendered\tfonte\tlast_writer\tuniform_pct\tf_bilinear\tsub_grid\tflags_final\tstep_native\n");
    }
    if (!g_draw_log) { g_rows.clear(); return; }
    for (const DrawRow &r : g_rows) {
        const std::uint64_t tex1 = r.tex1;
        float u = -1.0f;
        if (r.tme) {
            auto it = frac.find(TexKey{r.tbp0, r.tbw, r.psm, r.tw, r.th});
            if (it != frac.end()) u = it->second;
        }
        std::fprintf(g_draw_log,
                     "%llu\t%u\t%u\t%u\t%u\t%u\t%02X\t%u\t%u\t%016llX\t%u\t%u\t%u\t%u\t%u\t%016llX\t"
                     "%u\t%u\t%02X\t%u\t%08llX\t%d,%d,%d,%d\t%d,%d\t%d,%d\t%.4f\t%.4f\t%08X\t%u\t%s\t%s\t%.1f\t%u\t%u\t%08X\t%u\n",
                     static_cast<unsigned long long>(r.idx), r.prim, r.fst, r.tme, r.tbp0, r.tbw, r.psm,
                     r.tw, r.th, static_cast<unsigned long long>(tex1), static_cast<std::uint32_t>((tex1 >> 5) & 3u),
                     static_cast<std::uint32_t>((tex1 >> 8) & 7u), static_cast<std::uint32_t>(tex1 & 1u),
                     static_cast<std::uint32_t>((tex1 >> 19) & 1u), static_cast<std::uint32_t>((tex1 >> 20) & 0xFu),
                     static_cast<unsigned long long>(r.clamp_raw), r.fbp, r.fbw, r.fbpsm, r.abe,
                     static_cast<unsigned long long>(r.alpha_raw), r.x0, r.y0, r.x1, r.y1, r.u0, r.v0, r.u1, r.v1,
                     r.dudx, r.dvdy, r.flags, r.in_g_rendered, r.fonte.empty() ? "?" : r.fonte.c_str(),
                     r.last_writer.c_str(), u, r.f_bilinear, r.sub_grid, r.flags_final, r.step_native);
    }
    g_rows.clear();
    g_rows_start = 0;
}

std::uint32_t dump_decode(std::uint32_t psm, std::uint32_t raw, const std::uint32_t *clut, std::uint32_t csa) {
    switch (psm) {
    case 0x13u: case 0x1Bu: return clut[raw & 255u];
    case 0x14u: case 0x24u: case 0x2Cu: return clut[(((csa & 15u) * 16u) + (raw & 15u)) & 511u];
    default: return raw;
    }
}

void write_ppm(const char *path, int w, int h, const std::vector<std::uint8_t> &px) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[dump] cannot open %s\n", path); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(px.data(), 1, px.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "[dump] %s %dx%d\n", path, w, h);
}

void dump_fb(std::uint32_t fbp) {
    const std::uint32_t n = g_scale * g_scale;
    const int W = 512, H = 512;
    std::uint32_t lo = ~0u, hi = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const TexLoc l = locate_texel(0x00u, fbp, 8, x, y);
            lo = std::min(lo, l.word);
            hi = std::max(hi, l.word);
        }
    std::vector<std::uint32_t> buf(static_cast<std::uint64_t>(hi - lo + 1) * n);
    read_back(g_vram, buf.size() * 4, buf.data(), static_cast<std::uint64_t>(lo) * n * 4);
    const int gw = W * g_scale, gh = H * g_scale;
    std::vector<std::uint8_t> img(static_cast<std::size_t>(gw) * gh * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const TexLoc l = locate_texel(0x00u, fbp, 8, x, y);
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::uint32_t c = buf[(l.word - lo) * n + k];
                const int px = x * g_scale + static_cast<int>(k % g_scale);
                const int py = y * g_scale + static_cast<int>(k / g_scale);
                const std::size_t o = (static_cast<std::size_t>(py) * gw + px) * 3;
                img[o] = c & 0xFF;
                img[o + 1] = (c >> 8) & 0xFF;
                img[o + 2] = (c >> 16) & 0xFF;
            }
        }
    char path[512];
    std::snprintf(path, sizeof path, "evidencias/ETAPA_F/texdump_fb%u_grid6x.ppm", fbp);
    write_ppm(path, gw, gh, img);
}

void dump_texture(const TexDumpInfo &d, const char *suffix) {
    const std::uint32_t n = g_scale * g_scale;
        std::uint32_t lo = ~0u, hi = 0;
        for (int y = 0; y < static_cast<int>(d.th); ++y)
            for (int x = 0; x < static_cast<int>(d.tw); ++x) {
                const TexLoc l = locate_texel(d.psm, d.tbp, d.tbw, x, y);
                lo = std::min(lo, l.word);
                hi = std::max(hi, l.word);
            }
        if (hi < lo) return;
        const std::uint64_t words = static_cast<std::uint64_t>(hi - lo + 1) * n;
        std::vector<std::uint32_t> buf(words);
        read_back(g_vram, words * 4, buf.data(), static_cast<std::uint64_t>(lo) * n * 4);
        std::vector<std::uint32_t> clut(512);
        read_back(g_clut, 512 * 4, clut.data(), static_cast<std::uint64_t>(d.clut) * 512 * 4);

        std::vector<std::uint8_t> nat(static_cast<std::size_t>(d.tw) * d.th * 3);
        for (int y = 0; y < static_cast<int>(d.th); ++y)
            for (int x = 0; x < static_cast<int>(d.tw); ++x) {
                const TexLoc l = locate_texel(d.psm, d.tbp, d.tbw, x, y);
                const std::uint32_t raw = (buf[(l.word - lo) * n] >> l.shift) & l.mask;
                const std::uint32_t c = dump_decode(d.psm, raw, clut.data(), d.csa);
                const std::size_t o = (static_cast<std::size_t>(y) * d.tw + x) * 3;
                nat[o] = c & 0xFF;
                nat[o + 1] = (c >> 8) & 0xFF;
                nat[o + 2] = (c >> 16) & 0xFF;
            }
        char path[512];
        std::snprintf(path, sizeof path, "evidencias/ETAPA_F/texdump_%u%s_native.ppm", d.tbp, suffix);
        write_ppm(path, d.tw, d.th, nat);

        const int gw = static_cast<int>(d.tw) * g_scale, gh = static_cast<int>(d.th) * g_scale;
        std::vector<std::uint8_t> grid(static_cast<std::size_t>(gw) * gh * 3);
        for (int y = 0; y < static_cast<int>(d.th); ++y)
            for (int x = 0; x < static_cast<int>(d.tw); ++x) {
                const TexLoc l = locate_texel(d.psm, d.tbp, d.tbw, x, y);
                for (std::uint32_t k = 0; k < n; ++k) {
                    const std::uint32_t raw = (buf[(l.word - lo) * n + k] >> l.shift) & l.mask;
                    const std::uint32_t c = dump_decode(d.psm, raw, clut.data(), d.csa);
                    const int px = x * g_scale + static_cast<int>(k % g_scale);
                    const int py = y * g_scale + static_cast<int>(k / g_scale);
                    const std::size_t o = (static_cast<std::size_t>(py) * gw + px) * 3;
                    grid[o] = c & 0xFF;
                    grid[o + 1] = (c >> 8) & 0xFF;
                    grid[o + 2] = (c >> 16) & 0xFF;
                }
            }
        std::snprintf(path, sizeof path, "evidencias/ETAPA_F/texdump_%u%s_grid6x.ppm", d.tbp, suffix);
        write_ppm(path, gw, gh, grid);

        const int u0 = std::clamp(d.u0, 0, static_cast<int>(d.tw) - 1);
        const int u1 = std::clamp(d.u1, 0, static_cast<int>(d.tw) - 1);
        const int v0 = std::clamp(d.v0, 0, static_cast<int>(d.th) - 1);
        const int v1 = std::clamp(d.v1, 0, static_cast<int>(d.th) - 1);
        if (u0 <= u1 && v0 <= v1) {
            const int cw = u1 - u0 + 1, ch = v1 - v0 + 1;
            std::vector<std::uint8_t> crop(static_cast<std::size_t>(cw) * ch * 3);
            for (int y = 0; y < ch; ++y)
                std::memcpy(&crop[static_cast<std::size_t>(y) * cw * 3],
                            &nat[(static_cast<std::size_t>(v0 + y) * d.tw + u0) * 3], static_cast<std::size_t>(cw) * 3);
            std::snprintf(path, sizeof path, "evidencias/ETAPA_F/texdump_%u%s_uv.ppm", d.tbp, suffix);
            write_ppm(path, cw, ch, crop);
        }
}

void finalize_vram_dump() {
    if (g_dump_fb != ~0u && (g_dump_frame == ~0ull || ps2::frame_count() >= g_dump_frame)) {
        submit();
        dump_fb(g_dump_fb);
        g_dump_fb = ~0u;
    }
    if (!g_dump_on || g_dump_info.empty()) return;
    submit();
    for (auto &kv : g_dump_info) dump_texture(kv.second, "");
    g_dump_info.clear();
}
// ---------------------------------------------------------------------------

std::uint32_t make_setup(const State &st, std::uint64_t attr, std::uint32_t *w) {
    const Context &c = st.ctx[(attr >> 9) & 1u];
    std::uint32_t flags = 0;
    if (attr & 0x8u) flags |= 1u;
    if (attr & 0x10u) flags |= 2u;
    if (attr & 0x20u) flags |= 4u;
    if (attr & 0x40u) flags |= 8u;
    if (attr & 0x100u) flags |= 16u;
    if ((c.zbuf >> 32) & 1u) flags |= 32u;
    if (c.test & 1u) flags |= 64u;
    if ((c.test >> 14) & 1u) flags |= 128u;
    if ((c.test >> 15) & 1u) flags |= 256u;
    if ((c.test >> 16) & 1u) flags |= 512u;
    if (st.pabe & 1u) flags |= 1024u;
    if (st.colclamp & 1u) flags |= 2048u;
    if (c.fba & 1u) flags |= 4096u;
    if ((c.tex0 >> 34) & 1u) flags |= 8192u;
    if ((c.tex1 >> 5) & 1u) flags |= 16384u;
    if ((st.texa >> 15) & 1u) flags |= 32768u;
    if (g_options.filter == 0) flags &= ~16384u;
    else if (g_options.filter == 1) flags |= 16384u;
    const std::uint32_t zpsm = 0x30u | (static_cast<std::uint32_t>(c.zbuf >> 24) & 0xFu);
    w[0] = flags;
    w[1] = (static_cast<std::uint32_t>(c.frame) & 0x1FFu) * 32u;
    w[2] = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    w[3] = static_cast<std::uint32_t>(c.frame >> 24) & 0x3Fu;
    w[4] = static_cast<std::uint32_t>(c.frame >> 32);
    w[5] = (static_cast<std::uint32_t>(c.zbuf) & 0x1FFu) * 32u;
    w[6] = zpsm;
    w[7] = (static_cast<std::uint32_t>(c.test >> 1) & 7u) | ((static_cast<std::uint32_t>(c.test >> 12) & 3u) << 4) |
           ((static_cast<std::uint32_t>(c.test >> 17) & 3u) << 8) | ((static_cast<std::uint32_t>(c.alpha) & 0xFFu) << 12) |
           ((static_cast<std::uint32_t>(c.tex0 >> 35) & 3u) << 20) | ((static_cast<std::uint32_t>(c.clamp) & 0xFu) << 24) |
           ((static_cast<std::uint32_t>(st.scanmsk) & 3u) << 28);
    w[8] = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    w[9] = static_cast<std::uint32_t>(c.alpha >> 32) & 0xFFu;
    w[10] = (static_cast<std::uint32_t>(c.scissor) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 16) & 0x7FFu) << 16);
    w[11] = (static_cast<std::uint32_t>(c.scissor >> 32) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 48) & 0x7FFu) << 16);
    w[12] = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    w[13] = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    w[14] = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    w[15] = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u) | (std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u) << 8);
    w[16] = static_cast<std::uint32_t>(c.tex0 >> 51) & 0xFu;
    w[17] = static_cast<std::uint32_t>(c.tex0 >> 56) & 0x1Fu;
    w[18] = (static_cast<std::uint32_t>(c.clamp >> 4) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 14) & 0x3FFu) << 16);
    w[19] = (static_cast<std::uint32_t>(c.clamp >> 24) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 34) & 0x3FFu) << 16);
    w[20] = (static_cast<std::uint32_t>(st.texa) & 0xFFu) | ((static_cast<std::uint32_t>(st.texa >> 32) & 0xFFu) << 8);
    w[21] = static_cast<std::uint32_t>(st.fogcol) & 0xFFFFFFu;
    w[22] = g_clut_slot;
    w[23] = z_max(zpsm);
    w[24] = 0xFFFFFFFFu;
    return flags;
}

bool texel_rect(const std::uint32_t *setup, const Vertex *v, int count, std::uint32_t prim, bool fst, int rect[4]) {
    const int tw = 1 << (setup[15] & 0xFFu), th = 1 << (setup[15] >> 8);
    float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
    for (int i = 0; i < count; ++i) {
        float u, t;
        if (fst) {
            u = static_cast<float>(v[i].u) / 16.0f;
            t = static_cast<float>(v[i].v) / 16.0f;
        } else {
            const float q = prim == 6 ? v[count - 1].q : v[i].q;
            if (!(q > 0.0f) || !(v[0].q > 0.0f) || !(v[count - 1].q > 0.0f)) return false;
            u = v[i].s / q * static_cast<float>(tw);
            t = v[i].t / q * static_cast<float>(th);
        }
        if (!std::isfinite(u) || !std::isfinite(t)) return false;
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, t);
        v1 = std::max(v1, t);
    }
    const auto axis = [](std::uint32_t mode, float lo, float hi, int size, std::uint32_t clamp, int &a, int &b) {
        a = static_cast<int>(std::floor(lo)) - 1;
        b = static_cast<int>(std::ceil(hi)) + 1;
        switch (mode) {
        case 0:
            if (a < 0 || b >= size) {
                a = 0;
                b = size - 1;
            }
            return true;
        case 1:
            a = std::clamp(a, 0, size - 1);
            b = std::clamp(b, 0, size - 1);
            return true;
        case 2: {
            const int minv = static_cast<int>(clamp & 0x3FFu), maxv = static_cast<int>(clamp >> 16);
            a = std::clamp(a, minv, std::max(minv, maxv));
            b = std::clamp(b, minv, std::max(minv, maxv));
            return true;
        }
        default: return false;
        }
    };
    int ua, ub, va, vb;
    if (!axis((setup[7] >> 24) & 3u, u0, u1, tw, setup[18], ua, ub) || !axis((setup[7] >> 26) & 3u, v0, v1, th, setup[19], va, vb)) return false;
    rect[0] = std::max(ua, 0);
    rect[1] = std::max(va, 0);
    rect[2] = std::max(ub, 0);
    rect[3] = std::max(vb, 0);
    return true;
}

bool shares_frame(const std::uint32_t *setup, const Batch &b) {
    const auto wide = [](std::uint32_t psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; };
    return setup[12] == b.fbp && setup[13] == b.fbw && (setup[14] == b.fpsm || (wide(setup[14]) && wide(b.fpsm)));
}

bool drawn_under(const Batch &b, const int rect[4]) {
    const auto hits = [&](int x0, int y0, int x1, int y1) { return rect[0] <= x1 && x0 <= rect[2] && rect[1] <= y1 && y0 <= rect[3]; };
    const std::size_t prims = b.boxes.size() / 2;
    if (prims > 64) return hits(b.minx, b.miny, b.maxx, b.maxy);
    for (std::size_t i = 0; i < prims; ++i) {
        const std::uint32_t lo = b.boxes[i * 2], hi = b.boxes[i * 2 + 1];
        if (hits(static_cast<int>(lo & 0xFFFFu), static_cast<int>(lo >> 16), static_cast<int>(hi & 0xFFFFu), static_cast<int>(hi >> 16))) return true;
    }
    return false;
}

void attrs(std::uint32_t *rec, int n, const Vertex &v, bool fst, const Vertex *color) {
    const Vertex &c = color ? *color : v;
    rec[12 + n] = c.r | (static_cast<std::uint32_t>(c.g) << 8) | (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
    rec[15 + n * 3] = f2u(fst ? static_cast<float>(v.u) : v.s);
    rec[16 + n * 3] = f2u(fst ? static_cast<float>(v.v) : v.t);
    rec[17 + n * 3] = f2u(fst ? 1.0f : v.q);
    rec[11] |= static_cast<std::uint32_t>(v.f) << (8 * n);
}

}

Mode mode() { return g_mode; }

namespace {

// Internal supersampling factor for a window width. The PS2 display source in
// this game is 512x512; scale 2 at <=1280 keeps 720p/960p light, scale 6 is the
// 4K cap (higher values are much slower without a matching visual gain).
std::uint32_t auto_scale(std::uint32_t width) {
    if (const char *internal = std::getenv("PS2_INTERNAL_SCALE"))
        return std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(internal)), 1u, 8u);
    if (width == 0) return 1;
    // The display source in this game is 512x448/512x512. Keep the internal
    // raster at least as wide as the output so the presentation pass
    // supersamples (downscales) instead of enlarging: that is what makes 2D
    // text/sprites look like the PCSX2 reference. 1080p now uses 4x
    // (2048x1792) instead of 3x (1536x1344), which was narrower than the
    // 1920-wide window and therefore upscaled.
    if (width <= 960u) return 2;    // 960x720 -> 1024x1024
    if (width <= 1440u) return 3;   // 1280x720 -> 1536x1536
    if (width <= 1920u) return 4;   // 1600x900 / 1920x1080 -> 2048x2048
    if (width <= 2560u) return 5;   // 2560x1440 -> 2560x2560
    return 6;                       // 4K -> 3072x3072 (upper cap)
}

bool create_gs_pipelines() {
    for (int i = 0; i < kPipelines; ++i) {
        if (g_pipelines[i]) wgpuComputePipelineRelease(g_pipelines[i]);
        g_pipelines[i] = nullptr;
    }
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kGsShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    if (!module) return false;
    bool ok = true;
    for (int i = 0; i < kPipelines; ++i) {
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = g_pipeline_layout;
        cd.compute.module = module;
        cd.compute.entryPoint = view(kEntryPoints[i]);
        WGPUConstantEntry constant = WGPU_CONSTANT_ENTRY_INIT;
        constant.key = view("SCALE");
        constant.value = static_cast<double>(g_scale);
        cd.compute.constantCount = 1;
        cd.compute.constants = &constant;
        g_pipelines[i] = wgpuDeviceCreateComputePipeline(g_device, &cd);
        if (!g_pipelines[i]) ok = false;
    }
    wgpuShaderModuleRelease(module);
    return ok;
}

} // namespace

bool init(Mode m) {
    if (m == Mode::off) return false;
#ifdef __EMSCRIPTEN__
    g_instance = wgpuCreateInstance(nullptr);
#else
    WGPUInstanceExtras extras = {};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    const char *backend = std::getenv("PS2_GPU_BACKEND");
    if (backend && std::strcmp(backend, "vulkan") == 0) extras.backends = WGPUInstanceBackend_Vulkan;
    else if (backend && std::strcmp(backend, "dx12") == 0) extras.backends = WGPUInstanceBackend_DX12;
    else extras.backends = WGPUInstanceBackend_Primary;
    WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
    id.nextInChain = &extras.chain;
    g_instance = wgpuCreateInstance(&id);
#endif
    if (!g_instance) return false;
    WGPUAdapter adapter = nullptr;
    bool adapter_done = false;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = std::getenv("PS2_GPU_LOW_POWER") ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    options.forceFallbackAdapter = std::getenv("PS2_GPU_FALLBACK") != nullptr;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView, void *out, void *done) {
        if (status == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter *>(out) = a;
        *static_cast<bool *>(done) = true;
    };
    acb.userdata1 = &adapter;
    acb.userdata2 = &adapter_done;
    wgpuInstanceRequestAdapter(g_instance, &options, acb);
    while (!adapter_done) pump();
    if (!adapter) {
        std::fprintf(stderr, "[gpu] no adapter\n");
        return false;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        std::fprintf(stderr, "[gpu] %.*s (%.*s)\n", static_cast<int>(info.device.length), info.device.data, static_cast<int>(info.description.length), info.description.data);
        wgpuAdapterInfoFreeMembers(info);
    }
    WGPULimits supported = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter, &supported);
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    limits.maxBufferSize = supported.maxBufferSize;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredLimits = &limits;
    const WGPUFeatureName timestamp_feature = WGPUFeatureName_TimestampQuery;
    g_timestamps = wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery) && !std::getenv("PS2_NO_GPU_TIMESTAMPS");
    if (g_timestamps) {
        dd.requiredFeatureCount = 1;
        dd.requiredFeatures = &timestamp_feature;
    }
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    bool device_done = false;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void *out, void *done) {
        if (status == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice *>(out) = d;
        else std::fprintf(stderr, "[gpu] device: %.*s\n", static_cast<int>(message.length), message.data);
        *static_cast<bool *>(done) = true;
    };
    dcb.userdata1 = &g_device;
    dcb.userdata2 = &device_done;
    wgpuAdapterRequestDevice(adapter, &dd, dcb);
    while (!device_done) pump();
    g_adapter = adapter;
    if (!g_device) return false;
    g_queue = wgpuDeviceGetQueue(g_device);

    if (const char *spec = std::getenv("PS2_GPU_PROFILE")) {
        char path[512] = {};
        unsigned long long a = 0, z = 0;
        if (std::sscanf(spec, "%llu,%llu,%511s", &a, &z, path) == 3) {
            g_profile_out = std::fopen(path, "w");
            g_profile_from = a;
            g_profile_to = z;
        }
    }
    if (std::getenv("PS2_GS_DEBUG")) std::fprintf(stderr, "[gpu] out %dx%d\n", g_options.out_width, g_options.out_height);
    // The Resolution chosen in Recomp Settings owns the internal scale. The
    // PS2_SCALE env is a developer override only when no resolution is set.
    if (g_options.out_width > 0) {
        g_scale = auto_scale(static_cast<std::uint32_t>(g_options.out_width));
    } else if (const char *scale = std::getenv("PS2_SCALE")) {
        g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 8u);
    }
    if (const char *internal = std::getenv("PS2_INTERNAL_SCALE"))
        g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(internal)), 1u, 8u);
    if (std::getenv("PS2_GS_DEBUG")) std::fprintf(stderr, "[gpu] scale %u\n", g_scale);
    g_copy_words = (1u << 20) * g_scale * g_scale;
    g_tiles_per_row = (2048 * static_cast<int>(g_scale) + 15) / 16;
    g_tile_count.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_tile_fill.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_field_words = (1u << 19) * g_scale * g_scale;
    g_vram = make_buffer(static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    std::vector<std::uint32_t> tables;
    swizzle_tables(tables);
    g_tables = make_buffer(tables.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(g_queue, g_tables, 0, tables.data(), tables.size() * 4);
    g_clut = make_buffer(static_cast<std::uint64_t>(kClutSlots) * kClutEntries * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    g_scratch = make_buffer((static_cast<std::uint64_t>(g_copy_words) + 2ull * g_field_words) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    g_cache = make_buffer(static_cast<std::uint64_t>(kCacheWords) * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    g_decoded = make_buffer(static_cast<std::uint64_t>(kDecodedWords) * 4, WGPUBufferUsage_Storage);
    if (const char *decode = std::getenv("PS2_DECODE")) g_decode = std::atoi(decode) != 0;
    {
        // Default is the supersampled grid. The native grid is selected only
        // for specific render-target textures (map/save effect) below: using
        // it globally softened the whole game and reintroduced the FMV strip
        // seams.
        const char *grid = std::getenv("PS2_TEX_GRID");
        g_tex_grid = 1;
        if (grid && std::strcmp(grid, "native") == 0) {
            g_tex_grid = 0;
        } else if (grid && std::strcmp(grid, "sub") != 0) {
            std::fprintf(stderr, "[gs] PS2_TEX_GRID=%s is not 'native' or 'sub'; using the default (sub)\n", grid);
        }
        g_sprite_uv = std::getenv("PS2_SPRITE_UV") && std::strcmp(std::getenv("PS2_SPRITE_UV"), "center") == 0 ? 1u : 0u;
        g_tri_uv = std::getenv("PS2_TRI_UV") && std::strcmp(std::getenv("PS2_TRI_UV"), "index") == 0 ? 1u : 0u;
        std::fprintf(stderr, "[gs] triangle texcoords: %s\n", g_tri_uv ? "pixel index (experimental, Etapa B)" : "pixel centre (default, pre-Etapa B)");
        std::fprintf(stderr, "[gs] sprite texcoords: %s\n", g_sprite_uv ? "centre (legacy)" : "pixel index (GS)");
        std::fprintf(stderr, "[gs] texture sampling: %s (internal scale %u)\n",
                     g_tex_grid ? "sub (supersampled grid)" : "native (compat)", g_scale);
    }
    // Experimental, opt-in: native-texel bilinear step on non-rendered textures.
    if (const char *step = std::getenv("PS2_BILIN_STEP")) {
        g_bilin_step_native = std::strcmp(step, "native") == 0;
        g_bilin_step_writer = std::strcmp(step, "writer") == 0;
        if (!g_bilin_step_native && !g_bilin_step_writer)
            std::fprintf(stderr, "[gs] PS2_BILIN_STEP=%s is not 'native' or 'writer'; ignored\n", step);
        if (g_bilin_step_writer) std::fprintf(stderr, "[gs] bilinear step: writer criterion\n");
        if (const char *mode = std::getenv("PS2_BILIN_MODE")) g_bilin_mode = (mode[0] == 'B' || mode[0] == 'b') ? 1u : 0u;
        std::fprintf(stderr, "[gs] bilinear step: native texel on in_g_rendered=0 draws (mode %s)\n",
                     g_bilin_mode ? "B (SCALExSCALE average)" : "A (centre sub-texel)");
    } else {
        // Default is true hardware bilinear. The native-texel/writer step is
        // an experimental compatibility path; enabling it by default made
        // ordinary textures look nearest and flicker as pages were rewritten.
        g_bilin_step_writer = false;
    }
    if (const char *tbp_list = std::getenv("PS2_BILIN_STEP_TBP")) {
        const char *p = tbp_list;
        while (*p) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p) break;
            g_bilin_tbps.push_back(static_cast<std::uint32_t>(v));
            if (!*end) break;
            p = end + 1;
        }
        std::fprintf(stderr, "[gs] bilinear step TBP list:");
        for (std::uint32_t v : g_bilin_tbps) std::fprintf(stderr, " %u", v);
        std::fprintf(stderr, "\n");
    } else {
        g_bilin_tbps.clear();
    }
    // Explicit Texture Filtering=Bilinear must mean real bilinear. The
    // native/writer step and the TBP list are developer overrides from the
    // old pixel-accuracy experiments; they must not turn a user-selected
    // Bilinear setting into nearest/pixelated output.
    if (g_options.filter == 1) {
        g_bilin_step_native = false;
        g_bilin_step_writer = false;
        g_bilin_tbps.clear();
        std::fprintf(stderr, "[gs] bilinear step disabled (Texture Filtering=Bilinear)\n");
    }
    g_native_tbps = {14416u, 14450u, 14928u, 14944u, 15072u, 15136u, 15200u};
    if (const char *native = std::getenv("PS2_NATIVE_TEX_TBP")) {
        g_native_tbps.clear();
        const char *p = native;
        while (*p) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p) break;
            g_native_tbps.push_back(static_cast<std::uint32_t>(v));
            if (!*end) break;
            p = end + 1;
        }
    }
    if (const char *mx = std::getenv("PS2_NATIVE_MAX_TEX"))
        g_native_max_tex = static_cast<std::uint32_t>(std::strtoul(mx, nullptr, 0));
    if (const char *mg = std::getenv("PS2_NATIVE_MAG"))
        g_native_mag = static_cast<float>(std::atof(mg));
    std::fprintf(stderr, "[gs] bilinear native-grid TBP list:");
    for (std::uint32_t v : g_native_tbps) std::fprintf(stderr, " %u", v);
    if (g_native_max_tex) std::fprintf(stderr, " | textures <= %ux%u", g_native_max_tex, g_native_max_tex);
    if (g_native_mag > 0.0f) std::fprintf(stderr, " | magnified >= %.2fx", static_cast<double>(g_native_mag));
    std::fprintf(stderr, "\n");
    if (const char *ex = std::getenv("PS2_BILIN_EXCLUDE")) {
        const char *p = ex;
        while (*p) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p) break;
            g_bilin_exclude.push_back(static_cast<std::uint32_t>(v));
            if (!*end) break;
            p = end + 1;
        }
        std::fprintf(stderr, "[gs] bilinear exclude:");
        for (std::uint32_t v : g_bilin_exclude) std::fprintf(stderr, " %u", v);
        std::fprintf(stderr, "\n");
    }
    if (const char *dfb = std::getenv("PS2_DUMP_VRAM_FB")) {
        g_dump_fb = static_cast<std::uint32_t>(std::strtoul(dfb, nullptr, 0));
        std::fprintf(stderr, "[gs] vram fb dump: fbp %u\n", g_dump_fb);
    }
    if (const char *we = std::getenv("PS2_WRITER_EXACT")) {
        g_writer_exact = std::atoi(we) != 0;
        std::fprintf(stderr, "[gs] writer exact pages: %s\n", g_writer_exact ? "on" : "off");
    }
    if (const char *df0 = std::getenv("PS2_DUMP_VRAM_FRAME")) g_dump_frame = std::strtoull(df0, nullptr, 10);
    if (const char *at = std::getenv("PS2_DUMP_VRAM_AT_DRAW")) {
        g_dump_at_draw = std::strtoll(at, nullptr, 10);
        std::fprintf(stderr, "[gs] vram dump at draw %lld\n", static_cast<long long>(g_dump_at_draw));
    }
    if (const char *tint = std::getenv("PS2_DEBUG_STEP_TINT")) {
        g_tint = std::atoi(tint) != 0;
        if (g_tint) std::fprintf(stderr, "[gs] debug step tint ON (25%% magenta on native-step draws)\n");
    }
    if (const char *dt = std::getenv("PS2_DUMP_VRAM_TEX")) {
        const char *p = dt;
        while (*p) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p) break;
            g_dump_tbps.push_back(static_cast<std::uint32_t>(v));
            if (!*end) break;
            p = end + 1;
        }
        g_dump_on = true;
        if (const char *df = std::getenv("PS2_DUMP_VRAM_FRAME")) g_dump_frame = std::strtoull(df, nullptr, 10);
        std::fprintf(stderr, "[gs] vram tex dump: frame %llu, tbpx:", static_cast<unsigned long long>(g_dump_frame));
        for (std::uint32_t v : g_dump_tbps) std::fprintf(stderr, " %u", v);
        std::fprintf(stderr, "\n");
    }
    const auto parse_ranges = [](const char *spec, std::vector<std::pair<std::uint32_t, std::uint32_t>> &out) {
        const char *p = spec;
        while (*p) {
            char *end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 10);
            if (end == p) break;
            unsigned long b = a;
            p = end;
            if (*p == '-') {
                const unsigned long c = std::strtoul(p + 1, &end, 10);
                if (end != p + 1) {
                    b = c;
                    p = end;
                }
            }
            out.emplace_back(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
            if (*p == ',') ++p;
        }
    };
    if (const char *v = std::getenv("PS2_DRAW_SKIP")) {
        parse_ranges(v, g_draw_skip);
        std::fprintf(stderr, "[gs] draw skip: %zu ranges\n", g_draw_skip.size());
    }
    if (const char *v = std::getenv("PS2_DRAW_ONLY")) {
        parse_ranges(v, g_draw_only);
        std::fprintf(stderr, "[gs] draw only: %zu ranges\n", g_draw_only.size());
    }
    if (const char *log = std::getenv("PS2_DRAW_LOG")) {
        g_draw_log_on = true;
        g_draw_log_frame = std::strtoull(log, nullptr, 10);
        std::fprintf(stderr, "[gs] draw log: frame %llu -> evidencias/ETAPA_E/T1/draws_%llu.tsv\n",
                     static_cast<unsigned long long>(g_draw_log_frame), static_cast<unsigned long long>(g_draw_log_frame));
    }
    std::fill(std::begin(g_page_writer), std::end(g_page_writer), static_cast<std::int64_t>(-3));
    g_cache_copy.assign(kCacheWords, 0);
    g_readback = make_buffer(static_cast<std::uint64_t>(g_copy_words) * 4, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    if (g_timestamps) {
        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qd.type = WGPUQueryType_Timestamp;
        qd.count = 2;
        g_queries = wgpuDeviceCreateQuerySet(g_device, &qd);
        g_query_resolve = make_buffer(16, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
        g_query_read = make_buffer(16, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }

    WGPUBindGroupLayoutEntry le[8] = {};
    const WGPUBufferBindingType types[8] = {WGPUBufferBindingType_Storage, WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Uniform,
                                            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage,
                                            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage};
    for (int i = 0; i < 8; ++i) {
        le[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le[i].binding = static_cast<std::uint32_t>(i);
        le[i].visibility = WGPUShaderStage_Compute;
        le[i].buffer.type = types[i];
        le[i].buffer.hasDynamicOffset = i == 2;
        le[i].buffer.minBindingSize = i == 2 ? kParamStride : 0;
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 8;
    ld.entries = le;
    g_layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g_layout;
    g_pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);

    if (!create_gs_pipelines()) return false;
    ensure_capacity();
    g_mode = m;
    return true;
}

static float present_aspect();

void draw(const State &st, const Vertex *vin, int count) {
    if (g_image.active && !g_image.pending.empty()) flush_image();
    if (const char *tr = g_env_draw_raw) {
        std::uint64_t at = st.prim;
        if (!(st.prmodecont & 1u)) at = (st.prim & 7u) | (st.prmode & ~7ull);
        const Context &cc = st.ctx[(at >> 9) & 1u];
        const char *fr = g_env_draw_frame;
        const bool frame_ok = !fr || ps2::frame_count() == std::strtoull(fr, nullptr, 10);
        const bool all = tr[0] == '*';
        if (frame_ok && (all || (cc.tex0 & 0x3FFFu) == std::strtoull(tr, nullptr, 16))) {
            std::fprintf(stderr, "[raw f%llu] prim %03llX pmc %llu tex0 %016llX tex1 %016llX tex2 %016llX texa %04llX clamp %016llX test %016llX alpha %016llX pabe %llu frame %016llX fba %llX xyoff %016llX scissor %016llX n %d\n",
                         (unsigned long long)ps2::frame_count(), (unsigned long long)st.prim,
                         (unsigned long long)(st.prmodecont & 1u), (unsigned long long)cc.tex0,
                         (unsigned long long)cc.tex1, (unsigned long long)cc.tex2, (unsigned long long)st.texa,
                         (unsigned long long)cc.clamp, (unsigned long long)cc.test, (unsigned long long)cc.alpha,
                         (unsigned long long)(st.pabe & 1u), (unsigned long long)cc.frame, (unsigned long long)(cc.fba & 1u),
                         (unsigned long long)cc.xyoffset, (unsigned long long)cc.scissor, count);
            for (int i = 0; i < count; ++i)
                std::fprintf(stderr, "      v%d x %u y %u z %u r %u g %u b %u a %u q %f s %f t %f u %u v %u f %u\n",
                             i, vin[i].x, vin[i].y, vin[i].z, vin[i].r, vin[i].g, vin[i].b, vin[i].a, vin[i].q, vin[i].s, vin[i].t, vin[i].u, vin[i].v, vin[i].f);
        }
    }
    ++g_draws_this_frame;
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    const std::uint32_t prim = static_cast<std::uint32_t>(st.prim) & 7u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    SetupCache &cache = g_setup_cache;
    if (cache.generation != g_setup_generation || cache.attr != attr || cache.clut != g_clut_slot) {
        cache.generation = g_setup_generation;
        cache.attr = attr;
        cache.clut = g_clut_slot;
        const std::uint32_t *w = cache.setup;
        cache.flags = make_setup(st, attr, cache.setup);
        cache.key = static_cast<std::uint64_t>(w[1]) | (static_cast<std::uint64_t>(w[2]) << 14) | (static_cast<std::uint64_t>(w[3]) << 20) |
                    (static_cast<std::uint64_t>(w[5]) << 26) | (static_cast<std::uint64_t>(w[6]) << 40);
        if (cache.flags & 2u) {
            const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
            const int tex_w = std::max(1 << tw, static_cast<int>(w[18] >> 16) + 1), tex_h = std::max(1 << th, static_cast<int>(w[19] >> 16) + 1);
            cache.tex = region(w[12], w[13], w[14], 0, 0, tex_w - 1, tex_h - 1);
        }
    }
    const std::uint32_t *setup = cache.setup;
    const std::uint32_t flags = cache.flags;
    const bool fst = flags & 16u, iip = flags & 1u;
    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    const int scax0 = static_cast<int>(setup[10] & 0xFFFFu), scax1 = static_cast<int>(setup[10] >> 16);
    const int scay0 = static_cast<int>(setup[11] & 0xFFFFu), scay1 = static_cast<int>(setup[11] >> 16);
    const auto sx = [&](const Vertex &p) { return (static_cast<float>(p.x) - static_cast<float>(ofx)) / 16.0f; };
    const auto sy = [&](const Vertex &p) { return (static_cast<float>(p.y) - static_cast<float>(ofy)) / 16.0f; };
    const std::uint32_t draw_tbp = static_cast<std::uint32_t>(c.tex0 & 0x3FFFu);
    const std::uint32_t draw_psm = static_cast<std::uint32_t>((c.tex0 >> 20) & 0x3Fu);
    if (draw_tbp == 15360u && draw_psm == 0x13u && count > 0) {
        float px0 = 1e30f, px1 = -1e30f, py0 = 1e30f, py1 = -1e30f;
        for (int i = 0; i < count; ++i) {
            const float x = sx(vin[i]), y = sy(vin[i]);
            px0 = std::min(px0, x); px1 = std::max(px1, x);
            py0 = std::min(py0, y); py1 = std::max(py1, y);
        }
        // Pause icons are drawn in the last quarter of the frame, after the 3D
        // scene; a world object that shares the texture is drawn much earlier.
        const bool late = g_draws_last_frame > 2000 &&
                          g_draws_this_frame * 4 > g_draws_last_frame * 3;
        if (late && px0 >= 200.0f && px1 <= 310.0f && py0 >= 15.0f && py1 <= 130.0f)
            g_pause_overlay_frame.store(ps2::frame_count(), std::memory_order_relaxed);
    }
    const float samples = static_cast<float>(g_scale);
    const auto first = [&](float lo) { return static_cast<int>(std::floor(lo)); };

    // 2D overlay (HUD) aspect correction. The game draws the HUD in
    // screen-space pixels; the presentation stretches that buffer by
    // aspect/(4:3). Scaling the HUD's X back by the inverse keeps circles round
    // and icons at their original size, anchored to the nearest screen edge so
    // corner widgets stay in the corner. Only small, screen-space, uploaded
    // (not render-target) draws in the top/bottom bands are touched.
    if (g_hud_xscale_cache < 0.0f) {
        float value = 1.0f;
        const char *off = std::getenv("PS2_HUD_ASPECT");
        const char *ha = off && std::strcmp(off, "0") != 0 ? off : nullptr;
        const char *hs = std::getenv("PS2_HUD_SCALE");
        if (off && std::strcmp(off, "0") == 0) {
            value = 1.0f;
        } else if (ha) {
            // HUD Aspect Ratio is the high-level choice and wins over a manual
            // HUD Scale (otherwise a saved 0.57 made every aspect option look
            // identical).
            const float v = std::atof(ha);
            if (v > 0.3f && v <= 1.0f) value = v;
        } else if (hs) {
            const float v = std::atof(hs);
            if (v > 0.3f && v <= 1.0f) value = v;
        }
        if (value == 1.0f && !(off && std::strcmp(off, "0") == 0) && !ha) {
            const float aspect = present_aspect();
            if (aspect > 0.5f) {
                const float k = aspect / (4.0f / 3.0f);
                value = k > 1.001f ? 1.0f / k : 1.0f;
            }
        }
        g_hud_xscale_cache = value;
    }
    const float hud_xscale = g_hud_xscale_cache;
    Vertex ui[3];
    const Vertex *v = vin;
    bool hud_applied = false;
    int omnx = 1 << 30, omny = 1 << 30, omxx = -(1 << 30), omxy = -(1 << 30);
    const bool ui_prim = prim == 6 || prim == 3 || prim == 4 || prim == 5;
    if (hud_xscale < 1.0f && ui_prim && count > 0 && count <= 3) {
        const bool tme0 = (cache.flags & 2u) != 0;
        const std::uint32_t tbw0 = (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu;
        const std::uint32_t psm0 = (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu;
        const bool strip0 = fst && prim == 6 && tbw0 == 1u && psm0 == 0u &&
                            (setup[15] & 0xFFu) == 6u && ((setup[15] >> 8) & 0xFFu) == 9u;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (int i = 0; i < count; ++i) {
            const float x = sx(vin[i]), y = sy(vin[i]);
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        const float w = x1 - x0, h = y1 - y0;
        const bool on_screen = x0 >= -4.0f && x1 <= 516.0f;
        const bool small = w > 2.0f && h > 1.0f && w < 0.8f * 512.0f && h < 0.7f * 448.0f;
        const bool screen_space = ofx == 0 && ofy == 0;
        const bool ortho = v[0].q > 0.99f && v[0].q < 1.01f;
        // HUD art is CLUT (PSMT8/PSMT4); direct-colour screen-space quads are
        // effects (the blue beam/ring) and must keep filling the wide frame.
        const bool clut = psm0 == 0x13u || psm0 == 0x14u || psm0 == 0x1Bu || psm0 == 0x24u || psm0 == 0x2Cu;
        // The save-effect ring is a direct-colour 256x256 screen-space quad with
        // the same 128 px scissor as the HUD; small HUD art is at most 128x128.
        const bool hud_sized = (setup[15] & 0xFFu) <= 7u && ((setup[15] >> 8) & 0xFFu) <= 7u;
        // The health bar and the minimap are 3D UI art drawn through the game's
        // UI camera (xyoff 0x7000). Their texture ids are stable, and the map
        // uses a narrow q range; targeting those avoids touching the world.
        const std::uint32_t ui_tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
        const bool in_g0 = tme0 && rendered(cache.tex);
        // Left HUD: portrait frame/health bar (tbp 15200/15136/14928/14944)
        // and the left-side icons that share tbp 15072 with the minimap.
        // Right HUD: the minimap icons (15072) and the rendered minimap
        // texture (14416). 15424/15360 are NOT corrected: they are also used
        // by nearby world geometry, and moving them left a black triangle in
        // the wall while walking. 15616/15872/16224 are 3D village textures.
        const bool hud_left = ((ui_tbp == 15200u || ui_tbp == 15136u || ui_tbp == 14928u ||
                               ui_tbp == 14944u || ui_tbp == 15072u) &&
                              x1 <= 400.0f);
        const bool ui_map = ui_tbp == 15072u && x0 >= 370.0f;
        const bool ui_map_rt = in_g0 && ui_tbp == 14416u && x0 >= 390.0f && x1 <= 500.0f && y0 >= 50.0f && y1 <= 410.0f;
        const bool edge_2d = (hud_left || (ui_map && !in_g0) || ui_map_rt) && small && on_screen;
        // The game draws the 2D menu/HUD layer through its UI camera (xyoffset
        // 0x7000) and some screen-space quads. Those are the pieces that look
        // stretched when the presentation widens; the perspective 3D world
        // (xyoffset 0, q != 1) must keep filling 16:9/21:9. Scale the whole UI
        // camera layer back to the native 4:3 width, centered. This also fixes
        // large UI (Mode Select signs/text, hit counter, names) that the old
        // small-HUD-only heuristic missed.
        // Only the small screen-space HUD widgets are rescaled here; large 3D
        // UI quads (health bars, signs) must keep their original screen-space
        // geometry, otherwise the wide presentation splits/duplicates them.
        // Mode Select / Master Mode pause are handled by a 4:3 pillarbox in
        // present() instead.
        const bool hud_2d = screen_space && !in_g0 && clut && tme0 && !strip0 && on_screen && small;
        const bool hud_2d_dc = screen_space && !in_g0 && hud_sized && tme0 && !strip0 && on_screen && small;
        if (hud_2d || hud_2d_dc || edge_2d) {
            if (g_env_hud_debug) {
                static int shown_map = 0;
                if (shown_map++ < 200)
                    std::fprintf(stderr, "[hud-map] tbp %u bbox %.1f,%.1f-%.1f,%.1f anchor %s xscale %.4f zte %d zwrite %d ofx %d\n",
                                 ui_tbp, x0, y0, x1, y1,
                                 (ui_map || ui_map_rt) ? "512" : hud_left ? "0" : "auto",
                                 hud_xscale, (c.test & 1u) ? 1 : 0, g_batch.zwrite ? 1 : 0, ofx);
            }
            float anchor;
            if (hud_left) {
                anchor = 0.0f;              // portrait/health at the left edge
            } else if (ui_map || ui_map_rt) {
                anchor = 512.0f;            // minimap on the right edge
            } else if (screen_space) {
                const float gx = 0.5f * (static_cast<float>(scax0) + static_cast<float>(scax1));
                anchor = gx < 0.42f * 512.0f ? 0.0f : gx > 0.58f * 512.0f ? 512.0f : 256.0f;
            } else if (y0 >= 300.0f) {
                anchor = 256.0f;            // bottom text
            } else {
                anchor = 0.0f;              // health bar, left edge
            }
            for (int i = 0; i < count; ++i) {
                ui[i] = vin[i];
                const float x = sx(vin[i]);
                const float nx = anchor + (x - anchor) * hud_xscale;
                const long long fixed = std::llround(static_cast<double>(nx) * 16.0 + static_cast<double>(ofx));
                ui[i].x = static_cast<std::uint32_t>(std::clamp<long long>(fixed, 0, 0xFFFFFFFFll));
            }
            v = ui;
            hud_applied = true;
            // Original sprite footprint, with the exact integer formula used
            // by the raster switch below, so batching/page tracking is unchanged.
            {
                float rx0 = 1e30f, ry0 = 1e30f, rx1 = -1e30f, ry1 = -1e30f;
                for (int i = 0; i < count; ++i) {
                    rx0 = std::min(rx0, sx(vin[i])); ry0 = std::min(ry0, sy(vin[i]));
                    rx1 = std::max(rx1, sx(vin[i])); ry1 = std::max(ry1, sy(vin[i]));
                }
                if (prim == 6) {
                    omnx = std::max(scax0, static_cast<int>(std::ceil(rx0)));
                    omxx = std::min(scax1, static_cast<int>(std::ceil(rx1)) - 1);
                    omny = std::max(scay0, static_cast<int>(std::ceil(ry0)));
                    omxy = std::min(scay1, static_cast<int>(std::ceil(ry1)) - 1);
                } else {
                    omnx = std::max(scax0, static_cast<int>(std::floor(rx0)));
                    omxx = std::min(scax1, static_cast<int>(std::ceil(rx1)) - 1);
                    omny = std::max(scay0, static_cast<int>(std::floor(ry0)));
                    omxy = std::min(scay1, static_cast<int>(std::ceil(ry1)) - 1);
                }
            }
            if (g_env_hud_debug) {
                static int shown = 0;
                if (shown++ < 12)
                    std::fprintf(stderr, "[hud] prim %u tbp %u psm %u tw %u th %u bbox %.1f,%.1f-%.1f,%.1f anchor %.0f xscale %.4f ofx %d ofy %d q %.6f\n",
                                 prim, static_cast<unsigned>(c.tex0 & 0x3FFFu), psm0, 1u << (setup[15] & 0xFFu), 1u << ((setup[15] >> 8) & 0xFFu),
                                 x0, y0, x1, y1, anchor, hud_xscale, ofx, ofy, v[0].q);
            }
        }
    }

    std::uint32_t rec[kPrimWords] = {};
    int minx, miny, maxx, maxy;
    switch (prim) {
    case 3: case 4: case 5: {
        if (count < 3) return;
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]), x2 = sx(v[2]), y2 = sy(v[2]);
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (area == 0.0f) return;
        minx = std::max(scax0, first(std::min({x0, x1, x2})));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) - 1);
        miny = std::max(scay0, first(std::min({y0, y1, y2})));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) - 1);
        rec[1] = 0;
        const float xy[6] = {x0, y0, x1, y1, x2, y2};
        for (int i = 0; i < 6; ++i) rec[4 + i] = f2u(xy[i]);
        rec[10] = f2u(1.0f / area);
        const float sg = area > 0.0f ? 1.0f : -1.0f;
        const auto inclusive = [&](float ax, float ay, float bx, float by) {
            const float gx = (ay - by) * sg, gy = (bx - ax) * sg;
            return gx > 0.0f || (gx == 0.0f && gy > 0.0f);
        };
        rec[27] = (inclusive(x1, y1, x2, y2) ? 1u : 0u) | (inclusive(x2, y2, x0, y0) ? 2u : 0u) | (inclusive(x0, y0, x1, y1) ? 4u : 0u);
        for (int i = 0; i < 3; ++i) attrs(rec, i, v[i], fst, iip ? nullptr : &v[2]);
        rec[24] = v[0].z;
        rec[25] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        rec[26] = f2u(static_cast<float>(static_cast<std::int64_t>(v[2].z) - static_cast<std::int64_t>(v[0].z)));
        break;
    }
    case 6: {
        Vertex va = v[0], vb = v[1];
        if (fst && g_env_round_sprite) round_sprite_offset(va, vb, static_cast<std::uint32_t>(ofx), static_cast<std::uint32_t>(ofy), (flags & 16384u) != 0);
        const float x0 = sx(va), y0 = sy(va), x1 = sx(vb), y1 = sy(vb);
        minx = std::max(scax0, static_cast<int>(std::ceil(std::min(x0, x1))));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) - 1);
        miny = std::max(scay0, static_cast<int>(std::ceil(std::min(y0, y1))));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) - 1);
        rec[1] = 1;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1 - x0);
        rec[7] = f2u(y1 - y0);
        attrs(rec, 0, va, fst, nullptr);
        attrs(rec, 1, vb, fst, nullptr);
        rec[24] = vb.z;
        break;
    }
    case 1: case 2: {
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
        minx = std::max(scax0, static_cast<int>(std::floor(std::min(x0, x1))) - 1);
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) + 1);
        miny = std::max(scay0, static_cast<int>(std::floor(std::min(y0, y1))) - 1);
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) + 1);
        rec[1] = 2;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1);
        rec[7] = f2u(y1);
        rec[8] = static_cast<std::uint32_t>(steps);
        attrs(rec, 0, v[0], fst, iip ? nullptr : &v[1]);
        attrs(rec, 1, v[1], fst, nullptr);
        rec[24] = v[0].z;
        rec[25] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        break;
    }
    case 0: {
        const int x = static_cast<int>(sx(v[0])), y = static_cast<int>(sy(v[0]));
        minx = maxx = x;
        miny = maxy = y;
        if (x < scax0 || x > scax1 || y < scay0 || y > scay1) return;
        rec[1] = 3;
        rec[4] = static_cast<std::uint32_t>(x);
        rec[5] = static_cast<std::uint32_t>(y);
        attrs(rec, 0, v[0], fst, nullptr);
        rec[24] = v[0].z;
        break;
    }
    default:
        return;
    }
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, 2047);
    maxy = std::min(maxy, 2047);
    if (minx > maxx || miny > maxy) return;

    // --- opt-in instrumentation: PS2_DRAW_LOG / PS2_BILIN_STEP ------------
    std::int64_t draw_id = -4;
    const std::uint64_t frame_now = ps2::frame_count();
    if (frame_now != g_draw_frame_seen) {
        if (g_draw_log_on && frame_now + 10 >= g_draw_log_frame && frame_now <= g_draw_log_frame + 10)
            std::fprintf(stderr, "[gs] draw frame %llu (target %llu)\n", static_cast<unsigned long long>(frame_now),
                         static_cast<unsigned long long>(g_draw_log_frame));
        g_draw_frame_seen = frame_now;
        g_draw_idx = 0;
    }
    const std::uint64_t idx_now = g_draw_idx++;
    if (draw_skipped(idx_now)) return;
    const bool log_now = g_draw_log_on && frame_now == g_draw_log_frame;
    if (log_now) draw_id = static_cast<std::int64_t>(idx_now);

    const bool tme = (cache.flags & 2u) != 0;
    // rendered() scans every 8 KiB page of the texture and is only needed by
    // the experimental native-step modes and the draw log. With ordinary
    // Bilinear it was pure overhead on every textured draw (~10k/frame).
    bool in_g = false;
    if (tme && (g_bilin_step_native || log_now)) in_g = rendered(cache.tex);
    const std::uint32_t tbp0_now = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;

    if (g_dump_at_draw >= 0 && (g_dump_frame == ~0ull || frame_now == g_dump_frame) &&
        static_cast<std::int64_t>(idx_now) == g_dump_at_draw && tme) {
        flush_all();
        submit();
        TexDumpInfo d;
        d.tbp = tbp0_now;
        d.tbw = (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu;
        d.psm = (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu;
        d.tw = 1u << (cache.setup[15] & 0xFFu);
        d.th = 1u << (cache.setup[15] >> 8);
        d.clut = cache.setup[22];
        d.cpsm = cache.setup[16];
        d.csa = cache.setup[17];
        int rect[4];
        if (texel_rect(cache.setup, v, count, prim, fst, rect)) {
            d.u0 = rect[0]; d.v0 = rect[1]; d.u1 = rect[2]; d.v1 = rect[3];
        }
        char sfx[32];
        std::snprintf(sfx, sizeof sfx, "at%llu", static_cast<unsigned long long>(idx_now));
        dump_texture(d, sfx);
        g_dump_at_draw = -1;
    }
    // The FMV player uploads each frame as eight 64x512 PSMCT32 strips
    // (tbw 1) and draws them 1:1 as sprites. The native-texel bilinear step is
    // meant for supersampled/magnified textures; on these strips it pulls the
    // texture's wrapped edge column into the last pixel and every strip boundary
    // becomes a vertical line. Keep the native step off that layout.
    const std::uint32_t now_tbw = (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu;
    const std::uint32_t now_psm = (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu;
    const bool video_strip = fst && prim == 6 && now_tbw == 1u && now_psm == 0u &&
                             (cache.setup[15] & 0xFFu) == 6u && ((cache.setup[15] >> 8) & 0xFFu) == 9u;
    bool wants_native = false;
    if (!video_strip && tme) {
        // Texture Filtering=Bilinear: the HUD/minimap camera (xyoff 0x7000/
        // 0x7200) draws text, icons, the life bar and the map. Sampling those
        // on the supersampled grid gives one-subtexel ramps and the enlarged
        // image looks hard/pixelated. Use the native texel grid for the whole
        // UI camera; the 3D world and the FMV strips keep the sub grid.
        const bool ui_camera = ofx == 0x7000 && ofy == 0x7200 &&
                               !g_env_no_ui_native;
        if (g_options.filter == 1 && ui_camera) wants_native = true;
        if (g_options.filter == 1 && !wants_native) {
            wants_native = std::find(g_native_tbps.begin(), g_native_tbps.end(), tbp0_now) != g_native_tbps.end();
            if (!wants_native && g_native_max_tex) {
                const std::uint32_t tw = 1u << (cache.setup[15] & 0xFFu);
                const std::uint32_t th = 1u << (cache.setup[15] >> 8);
                if (tw <= g_native_max_tex && th <= g_native_max_tex) wants_native = true;
            }
            if (!wants_native && g_native_mag > 0.0f) {
                int ur[4];
                if (texel_rect(cache.setup, v, count, prim, fst, ur)) {
                    const float uw = static_cast<float>(std::max(1, ur[2] - ur[0] + 1));
                    const float vh = static_cast<float>(std::max(1, ur[3] - ur[1] + 1));
                    const float sw = static_cast<float>(std::max(1, maxx - minx + 1));
                    const float sh = static_cast<float>(std::max(1, maxy - miny + 1));
                    const float mag = std::max(sw / uw, sh / vh);
                    if (mag >= g_native_mag) wants_native = true;
                }
            }
        }
        if (!wants_native && g_bilin_step_native) wants_native = !in_g;
        if (!wants_native && !g_bilin_tbps.empty())
            wants_native = std::find(g_bilin_tbps.begin(), g_bilin_tbps.end(), tbp0_now) != g_bilin_tbps.end();
    }
    if (g_dump_on && frame_now == g_dump_frame && tme &&
        std::find(g_dump_tbps.begin(), g_dump_tbps.end(), tbp0_now) != g_dump_tbps.end() &&
        g_dump_info.find(tbp0_now) == g_dump_info.end()) {
        TexDumpInfo d;
        d.tbp = tbp0_now;
        d.tbw = (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu;
        d.psm = (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu;
        d.tw = 1u << (cache.setup[15] & 0xFFu);
        d.th = 1u << (cache.setup[15] >> 8);
        d.clut = cache.setup[22];
        d.cpsm = cache.setup[16];
        d.csa = cache.setup[17];
        int rect[4];
        if (texel_rect(cache.setup, v, count, prim, fst, rect)) {
            d.u0 = rect[0]; d.v0 = rect[1]; d.u1 = rect[2]; d.v1 = rect[3];
        }
        g_dump_info[tbp0_now] = d;
    }
    const bool excluded = std::find(g_bilin_exclude.begin(), g_bilin_exclude.end(), tbp0_now) != g_bilin_exclude.end();
    if (!video_strip && g_bilin_step_writer && tme && !excluded && !wants_native) {
        int wr[4];
        if (texel_rect(cache.setup, v, count, prim, fst, wr)) {
            const std::uint32_t ntbw = (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu;
            const std::uint32_t ntpsm = (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu;
            if (g_writer_exact) {
                wants_native = pages_static_exact(tbp0_now, ntbw, ntpsm, wr[0], wr[1], wr[2], wr[3]);
            } else {
                const Range used = pages(tbp0_now, ntbw, ntpsm, wr[0], wr[1], wr[2], wr[3]);
                bool stat = true;
                for (std::uint64_t at = used.lo; at < used.hi; at += 8192) {
                    const std::int64_t w = g_page_writer[(at >> 13) & 511u];
                    if (w != -1 && w != -3) {
                        stat = false;
                        break;
                    }
                }
                wants_native = stat;
            }
        }
    }
    if (const char *nd = g_env_native_dump) {
        if (frame_now == std::strtoull(nd, nullptr, 10) && tme) {
            int ur[4] = {-1, -1, -1, -1};
            texel_rect(cache.setup, v, count, prim, fst, ur);
            std::fprintf(stderr,
                         "[nd] idx %llu prim %u fst %u tbp %u tbw %u psm %02X tw %u th %u "
                         "box %d,%d..%d,%d uv %d,%d..%d,%d xy %d,%d native %u\n",
                         static_cast<unsigned long long>(idx_now), prim, fst ? 1u : 0u,
                         tbp0_now, (static_cast<std::uint32_t>(c.tex0) >> 14) & 0x3Fu,
                         (static_cast<std::uint32_t>(c.tex0) >> 20) & 0x3Fu,
                         1u << (cache.setup[15] & 0xFFu), 1u << (cache.setup[15] >> 8),
                         minx, miny, maxx, maxy, ur[0], ur[1], ur[2], ur[3],
                         ofx, ofy, wants_native ? 1u : 0u);
        }
    }

    std::uint32_t tex_flags = cache.setup[0];
    if (wants_native) tex_flags |= 0x20000u;
    else tex_flags &= ~0x20000u;
    cache.setup[0] = tex_flags;

    if (log_now) {
        DrawRow r;
        r.idx = static_cast<std::uint64_t>(draw_id);
        r.prim = prim;
        r.fst = fst ? 1u : 0u;
        r.tme = (cache.flags & 2u) ? 1u : 0u;
        const std::uint32_t t0 = static_cast<std::uint32_t>(c.tex0);
        r.tbp0 = t0 & 0x3FFFu;
        r.tbw = (t0 >> 14) & 0x3Fu;
        r.psm = (t0 >> 20) & 0x3Fu;
        r.tw = 1u << (cache.setup[15] & 0xFFu);
        r.th = 1u << (cache.setup[15] >> 8);
        r.tex1 = c.tex1;
        r.clamp_raw = c.clamp;
        r.fbp = cache.setup[1];
        r.fbw = cache.setup[2];
        r.fbpsm = cache.setup[3];
        r.abe = (cache.flags & 8u) ? 1u : 0u;
        r.alpha_raw = c.alpha;
        r.x0 = minx; r.y0 = miny; r.x1 = maxx; r.y1 = maxy;
        int rect[4] = {-1, -1, -1, -1};
        if (cache.flags & 2u) texel_rect(cache.setup, v, count, prim, fst, rect);
        r.u0 = rect[0]; r.v0 = rect[1]; r.u1 = rect[2]; r.v1 = rect[3];
        const int tw = 1 << (cache.setup[15] & 0xFFu), th = 1 << (cache.setup[15] >> 8);
        const auto tex_uv = [&](const Vertex &p, float &uu, float &vv) {
            if (fst) {
                uu = static_cast<float>(p.u) / 16.0f;
                vv = static_cast<float>(p.v) / 16.0f;
                return true;
            }
            const float q = prim == 6 ? v[count - 1].q : p.q;
            if (!(q > 0.0f) || !(v[0].q > 0.0f)) return false;
            uu = p.s / q * static_cast<float>(tw);
            vv = p.t / q * static_cast<float>(th);
            return true;
        };
        float gu = 0.0f, gv = 0.0f;
        if (prim >= 3 && prim <= 5 && count >= 3) {
            float au, av, bu, bv, cu, cv;
            if (tex_uv(v[0], au, av) && tex_uv(v[1], bu, bv) && tex_uv(v[2], cu, cv)) {
                const float ax = sx(v[0]), ay = sy(v[0]), bx = sx(v[1]), by = sy(v[1]), cx = sx(v[2]), cy = sy(v[2]);
                const float det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
                if (det != 0.0f) {
                    gu = ((bu - au) * (cy - ay) - (cu - au) * (by - ay)) / det;
                    gv = ((cv - av) * (bx - ax) - (bv - av) * (cx - ax)) / det;
                }
            }
        } else if (count >= 2) {
            float au, av, bu, bv;
            if (tex_uv(v[0], au, av) && tex_uv(v[count - 1], bu, bv)) {
                const float dx = sx(v[count - 1]) - sx(v[0]), dy = sy(v[count - 1]) - sy(v[0]);
                if (dx != 0.0f) gu = (bu - au) / dx;
                if (dy != 0.0f) gv = (bv - av) / dy;
            }
        }
        r.dudx = std::fabs(gu);
        r.dvdy = std::fabs(gv);
        r.flags = cache.flags;
        r.f_bilinear = (cache.flags & 16384u) ? 1u : 0u;
        r.flags_final = tex_flags;
        r.step_native = wants_native ? 1u : 0u;
        r.in_g_rendered = in_g ? 1u : 0u;
        if (cache.flags & 2u) {
            char wb[32];
            std::int64_t found = -5;
            bool mixed = false;
            for (std::uint64_t at = cache.tex.lo; at < cache.tex.hi; at += 8192) {
                const std::int64_t w = g_page_writer[(at >> 13) & 511u];
                if (found == -5) found = w;
                else if (w != found) mixed = true;
            }
            r.last_writer = mixed ? "mixed" : found == -5 ? "-" : writer_name(found, wb);
        } else {
            r.last_writer = "-";
        }
        g_rows.push_back(r);
        ++g_batch.rows;
    }
    // ---------------------------------------------------------------------

    Batch &b = g_batch;
    const bool zte = flags & 512u;
    const std::uint64_t key = cache.key;
    if (b.key != ~0ull && b.key != key) emit_all();
    const Range &tex = cache.tex;
    if (flags & 2u) {
        if (!b.prims.empty()) {
            batch_ranges();
            const auto written = [&](const Range &t) { return t.overlaps(b.fb_range.lo, b.fb_range.hi) || (b.zwrite && t.overlaps(b.z_range.lo, b.z_range.hi)); };
            int rect[4];
            if (written(tex)) {
                if (!texel_rect(setup, v, count, prim, fst, rect)) {
                    emit_all();
                } else {
                    const Range used = region(setup[12], setup[13], setup[14], rect[0], rect[1], rect[2], rect[3]);
                    const bool small = rect[2] - rect[0] < 64 && rect[3] - rect[1] < 64;
                    const bool cached = b.texflushes == g_texflushes && small && shares_frame(setup, b);
                    const bool frame = !cached && (shares_frame(setup, b) ? drawn_under(b, rect) : used.overlaps(b.fb_range.lo, b.fb_range.hi));
                    if (frame || (b.zwrite && used.overlaps(b.z_range.lo, b.z_range.hi))) emit_all();
                }
            }
        }
    }
    if (b.prims.empty()) {
        b.texflushes = g_texflushes;
        b.key = key;
        b.fbp = setup[1];
        b.fbw = setup[2];
        b.fpsm = setup[3];
        b.zbp = setup[5];
        b.zpsm = setup[6];
        b.zte = false;
        b.zwrite = false;
        b.rx1 = -2;
    }
    b.zte = b.zte || zte;
    b.zwrite = b.zwrite || (zte && !(flags & 32u));
    const std::size_t setups = b.setups.size() / kSetupWords;
    std::uint32_t setup_index;
    if (setups && std::memcmp(b.setups.data() + (setups - 1) * kSetupWords, setup, kSetupWords * 4) == 0) {
        setup_index = static_cast<std::uint32_t>(setups - 1);
    } else {
        setup_index = static_cast<std::uint32_t>(setups);
        b.setups.insert(b.setups.end(), setup, setup + kSetupWords);
    }
    if (flags & 2u) {
        if (g_batch_reads.empty() || g_batch_reads.back().lo != tex.lo || g_batch_reads.back().hi != tex.hi) {
            bool known = false;
            for (const Range &t : g_batch_reads) known = known || (t.lo <= tex.lo && tex.hi <= t.hi);
            if (!known) g_batch_reads.push_back(tex);
        }
    }
    if (!hud_applied) {
        omnx = minx; omny = miny; omxx = maxx; omxy = maxy;
    }
    rec[0] = setup_index;
    rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
    rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
    b.prims.insert(b.prims.end(), rec, rec + kPrimWords);
    g_batch_tme = tme;
    if (b.rows == 1) g_rows_start = g_rows.size() - 1;
    g_batch_writer = draw_id;
    b.boxes.push_back(static_cast<std::uint32_t>(omnx) | (static_cast<std::uint32_t>(omny) << 16));
    b.boxes.push_back(static_cast<std::uint32_t>(omxx) | (static_cast<std::uint32_t>(omxy) << 16));
    b.minx = std::min(b.minx, omnx);
    b.miny = std::min(b.miny, omny);
    b.maxx = std::max(b.maxx, omxx);
    b.maxy = std::max(b.maxy, omxy);
    if (b.prims.size() >= kPrimWords * 65536) emit_all();
}

void image_begin(const State &s) {
    flush_image();
    Image &im = g_image;
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    im = Image{};
    im.dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    im.dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    im.dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    im.dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    im.dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    im.rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    im.total = im.rrw * (static_cast<std::uint32_t>(reg >> 32) & 0xFFFu);
    im.bpp = bits_per_pixel(im.dpsm);
    im.id = ++g_image_ids ? g_image_ids : ++g_image_ids;
    im.active = im.total != 0;
}

void image_data(const std::uint8_t *data, std::size_t bytes) {
    Image &im = g_image;
    if (!im.active) return;
    if (im.pending.empty() && !im.lead_bits) {
        const std::uint64_t used = queue_upload(data, bytes, 0);
        const std::size_t whole = static_cast<std::size_t>(used / 8);
        if (im.done < im.total && whole < bytes) im.pending.assign(data + whole, data + bytes);
        im.lead_bits = static_cast<std::uint32_t>(used % 8);
        finish_image_if_done();
        return;
    }
    im.pending.insert(im.pending.end(), data, data + bytes);
}

void local_copy(const State &s) {
    flush_all();
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    const std::uint32_t w = static_cast<std::uint32_t>(reg) & 0xFFFu, h = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!w || !h) return;
    const Range dst_pages = pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                  static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                                  (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1);
    mark_rendered(dst_pages);
    mark_written(dst_pages, -2);
    const std::uint32_t read = push_params({static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                                            static_cast<std::uint32_t>(pos) & 0x7FFu, static_cast<std::uint32_t>(pos >> 16) & 0x7FFu, w, h});
    const std::uint32_t write = push_params({static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                             static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, static_cast<std::uint32_t>(pos >> 48) & 0x7FFu, w, h,
                                             static_cast<std::uint32_t>(pos >> 59) & 3u});
    if (const char *tr = g_env_cp_trace) {
        if (ps2::frame_count() == std::strtoull(tr, nullptr, 10))
            std::fprintf(stderr, "[cp f%llu] src tbp %u tbw %u psm %02X (%u,%u) -> dst dbp %u dbw %u psm %02X (%u,%u) %ux%u dir %u\n",
                         (unsigned long long)ps2::frame_count(),
                         static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                         static_cast<std::uint32_t>(pos) & 0x7FFu, static_cast<std::uint32_t>(pos >> 16) & 0x7FFu,
                         static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                         static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, static_cast<std::uint32_t>(pos >> 48) & 0x7FFu,
                         w, h, static_cast<std::uint32_t>(pos >> 59) & 3u);
    }
    dispatch_linear(kCopyRead, w * h * g_scale * g_scale, read);
    dispatch_linear(kCopyWrite, w * h * g_scale * g_scale, write);
}

void load_clut(std::uint64_t tex0, std::uint64_t texclut) {
    flush_image();
    if (g_clut_loads + 2 >= kClutSlots) {
        emit_all();
        submit();
    }
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = (static_cast<std::uint32_t>(tex0 >> 51) & 0xFu) == 0 ? PSMCT32 : static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t next = (g_clut_slot + 1) % kClutSlots;
    const std::uint32_t tc = static_cast<std::uint32_t>(texclut);
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu, csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t cbw = tc & 0x3Fu, cou = (tc >> 6) & 0x3Fu, cov = (tc >> 12) & 0x3FFu;
    PreOp op;
    op.clut = true;
    const std::uint32_t rec[10] = {g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, (psm == PSMT8 || psm == PSMT8H) ? 1u : 0u, cbw, cou, cov};
    std::memcpy(op.rec, rec, sizeof rec);
    op.range = csm ? region(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov))
                   : region(cbp, 1, cpsm, 0, 0, 15, 15);
    queue_pre(op);
    g_clut_slot = next;
    ++g_clut_loads;
}

void texflush() { ++g_texflushes; }

void flush() {
    if (g_mode == Mode::off) return;
    flush_all();
    submit();
}

void wait_idle() {
    flush_all();
    submit();
    bool done = false;
    WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    cb.userdata1 = &done;
    wgpuQueueOnSubmittedWorkDone(g_queue, cb);
    wait_until(done);
}

struct PendingDownload {
    WGPUBuffer buffer = nullptr;
    std::uint64_t capacity = 0;
    std::uint8_t *memory = nullptr;
    std::uint32_t offset = 0, bytes = 0, serial = 0;
    bool busy = false, ready = false;
} g_pending;

void collect_download() {
    if (!g_pending.busy || !g_pending.ready) return;
    const void *mapped = wgpuBufferGetConstMappedRange(g_pending.buffer, 0, g_pending.bytes);
    if (mapped) std::memcpy(g_pending.memory + g_pending.offset, mapped, g_pending.bytes);
    wgpuBufferUnmap(g_pending.buffer);
    g_pending.busy = g_pending.ready = false;
}

bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial) {
    collect_download();
    if (g_pending.busy) return false;
    if (g_pending.capacity < bytes) {
        if (g_pending.buffer) wgpuBufferRelease(g_pending.buffer);
        g_pending.capacity = bytes;
        g_pending.buffer = make_buffer(bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    }
    flush_all();
    if (g_scale != 1) dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
    submit();
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, g_scale == 1 ? g_vram : g_scratch, offset, g_pending.buffer, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    g_pending.memory = memory;
    g_pending.offset = offset;
    g_pending.bytes = bytes;
    g_pending.serial = serial;
    g_pending.busy = true;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *, void *) {
        if (status == WGPUMapAsyncStatus_Success) g_pending.ready = true;
        else g_pending.busy = false;
    };
    wgpuBufferMapAsync(g_pending.buffer, WGPUMapMode_Read, 0, bytes, cb);
    return true;
}

bool finish_download(std::uint32_t serial) {
    if (!g_pending.busy || g_pending.serial != serial) return false;
    while (g_pending.busy && !g_pending.ready) pump();
    const bool done = g_pending.ready;
    collect_download();
    return done;
}

void download(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes) {
    flush_all();
    if (g_scale == 1) {
        submit();
        read_back(g_vram, bytes, memory + offset, offset);
        return;
    }
    dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
    submit();
    read_back(g_scratch, bytes, memory + offset, offset);
}

void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut) {
    flush_all();
    submit();
    scale = g_scale;
    vram.resize(static_cast<std::size_t>(kMemorySize / 4) * g_scale * g_scale);
    read_back(g_vram, vram.size() * 4, vram.data());
    clut.resize(kClutEntries);
    read_back(g_clut, kClutEntries * 4, clut.data(), static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4);
}

void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut) {
    wait_idle();
    const std::size_t words = kMemorySize / 4;
    const std::uint32_t from = scale * scale, to = g_scale * g_scale;
    std::fill(std::begin(g_rendered), std::end(g_rendered), from == to && to > 1);
    std::fill(std::begin(g_page_writer), std::end(g_page_writer), static_cast<std::int64_t>(-3));
    if (from == to && vram.size() == words * to) {
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, vram.data(), vram.size() * 4);
    } else {
        std::vector<std::uint32_t> layers(words * to);
        for (std::size_t w = 0; w < words; ++w)
            for (std::uint32_t k = 0; k < to; ++k) layers[w * to + k] = vram[w * from];
        wgpuQueueWriteBuffer(g_queue, g_vram, 0, layers.data(), layers.size() * 4);
    }
    if (clut.size() >= kClutEntries) wgpuQueueWriteBuffer(g_queue, g_clut, static_cast<std::uint64_t>(g_clut_slot) * kClutEntries * 4, clut.data(), kClutEntries * 4);
    wait_idle();
}

void rebuild_blit_group();

std::uint32_t preferred_scale(std::uint32_t width) { return auto_scale(width); }

bool set_scale(std::uint32_t new_scale) {
    new_scale = std::clamp<std::uint32_t>(new_scale, 1u, 8u);
    if (!g_device || new_scale == g_scale) return new_scale == g_scale;

    // Preserve the GS VRAM (textures/framebuffer) across the layout change. The
    // save-state path already knows how to convert between scales.
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t old_scale = g_scale;
    read_state(vram, old_scale, clut);
    wait_idle();

    g_scale = new_scale;
    g_copy_words = (1u << 20) * g_scale * g_scale;
    g_tiles_per_row = (2048 * static_cast<int>(g_scale) + 15) / 16;
    g_tile_count.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_tile_fill.assign(static_cast<std::size_t>(g_tiles_per_row) * g_tiles_per_row, 0);
    g_field_words = (1u << 19) * g_scale * g_scale;

    if (g_vram) wgpuBufferRelease(g_vram);
    g_vram = make_buffer(static_cast<std::uint64_t>(kMemorySize) * g_scale * g_scale,
                         WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    if (g_scratch) wgpuBufferRelease(g_scratch);
    g_scratch = make_buffer((static_cast<std::uint64_t>(g_copy_words) + 2ull * g_field_words) * 4,
                            WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    if (g_readback) wgpuBufferRelease(g_readback);
    g_readback = make_buffer(static_cast<std::uint64_t>(g_copy_words) * 4,
                             WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);

    if (!create_gs_pipelines()) return false;
    rebuild_group();
    rebuild_blit_group();

    std::fill(std::begin(g_rendered), std::end(g_rendered), false);
    std::fill(std::begin(g_page_writer), std::end(g_page_writer), static_cast<std::int64_t>(-1));
    g_decoded_used = 0;
    g_field = 0;
    g_have_previous = false;
    g_last_width = g_last_height = 0;
    g_present_vw = g_present_vh = 0.0f;

    write_state(vram, old_scale, clut);
    wait_idle();
    if (std::getenv("PS2_GS_DEBUG"))
        std::fprintf(stderr, "[gpu] scale %u -> %u (internal %ux%u)\n", old_scale, g_scale,
                     g_scale * 512u, g_scale * 512u);
    return true;
}

std::uint32_t scale() { return g_scale; }

bool pause_overlay_active() {
    const std::uint64_t frame = ps2::frame_count();
    const std::uint64_t last = g_pause_overlay_frame.load(std::memory_order_relaxed);
    return last != 0 && frame >= last && frame - last <= 3;
}

std::uint32_t internal_width() { return g_last_src_w ? g_last_src_w * g_scale : 512u * g_scale; }
std::uint32_t internal_height() { return g_last_src_h ? g_last_src_h * g_scale : 512u * g_scale; }

std::uint32_t present_width() {
    return g_present_vw > 0.0f ? static_cast<std::uint32_t>(g_present_vw) : g_surface_width;
}

std::uint32_t present_height() {
    return g_present_vh > 0.0f ? static_cast<std::uint32_t>(g_present_vh) : g_surface_height;
}

void set_texture_grid(bool sub) {
    const std::uint32_t next = sub ? 1u : 0u;
    if (next == g_tex_grid) return;
    g_tex_grid = next;
    std::fprintf(stderr, "[gs] texture sampling: %s\n", sub ? "sub (supersampled grid)" : "native (bilinear)");
}

void refresh_hud_scale() { g_hud_xscale_cache = -1.0f; }

void stats() {
    if (g_mode == Mode::off) return;
    if (g_batch_stats)
        std::fprintf(stderr, "[batch] flushes %llu prims %llu avg %.2f | spans %.1f ms plan %.1f ms fill %.1f ms snapshot %.1f ms\n",
                     (unsigned long long)g_flush_calls, (unsigned long long)g_flush_prims,
                     g_flush_calls ? static_cast<double>(g_flush_prims) / static_cast<double>(g_flush_calls) : 0.0,
                     g_batch_t_spans * 1000.0, g_batch_t_plan * 1000.0,
                     g_batch_t_fill * 1000.0, g_batch_t_snap * 1000.0);
    std::fprintf(stderr, "[gpu] %llu submits, dispatches:", static_cast<unsigned long long>(g_submits));
    for (int i = 0; i < kPipelines; ++i) std::fprintf(stderr, " %s %llu", kEntryPoints[i], static_cast<unsigned long long>(g_dispatches[i]));
    std::fprintf(stderr, "\n");
}

void configure_surface() {
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = g_device;
    config.format = g_surface_format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = std::max(g_surface_width, 1u);
    config.height = std::max(g_surface_height, 1u);
    config.presentMode = g_present_mode;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(g_surface, &config);
}

void rebuild_blit_group() {
    if (!g_blit_layout || !g_blit_params || !g_menu_view) return;
    if (g_blit_group) wgpuBindGroupRelease(g_blit_group);
    WGPUBindGroupEntry be[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    be[0].binding = 0;
    be[0].buffer = g_scratch;
    be[0].size = WGPU_WHOLE_SIZE;
    be[1].binding = 1;
    be[1].buffer = g_blit_params;
    be[1].size = 32;
    be[2].binding = 2;
    be[2].textureView = g_menu_view;
    be[3].binding = 3;
    be[3].sampler = g_menu_sampler;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = g_blit_layout;
    bd.entryCount = 4;
    bd.entries = be;
    g_blit_group = wgpuDeviceCreateBindGroup(g_device, &bd);
}

void create_menu_texture(std::uint32_t w, std::uint32_t h) {
    if (g_menu_view) { wgpuTextureViewRelease(g_menu_view); g_menu_view = nullptr; }
    if (g_menu_tex) { wgpuTextureRelease(g_menu_tex); g_menu_tex = nullptr; }
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{std::max(w, 1u), std::max(h, 1u), 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    g_menu_tex = wgpuDeviceCreateTexture(g_device, &td);
    g_menu_view = g_menu_tex ? wgpuTextureCreateView(g_menu_tex, nullptr) : nullptr;
    g_menu_w = std::max(w, 1u);
    g_menu_h = std::max(h, 1u);
    g_overlay_kind = 0;
    g_overlay_key = 0;
    rebuild_blit_group();
}

// Fixed-size render target for the heavy blit. It is then scaled to the
// window by the cheap fs_up pass so a maximized 4K window does not run FXAA
// and the 8-tap source filter at 4K.
void ensure_present_texture(std::uint32_t w, std::uint32_t h) {
    w = std::max(w, 1u);
    h = std::max(h, 1u);
    if (g_present_tex && g_present_tex_w == w && g_present_tex_h == h && g_up_group) return;
    if (g_up_group) { wgpuBindGroupRelease(g_up_group); g_up_group = nullptr; }
    if (g_present_tex_view) { wgpuTextureViewRelease(g_present_tex_view); g_present_tex_view = nullptr; }
    if (g_present_tex) { wgpuTextureRelease(g_present_tex); g_present_tex = nullptr; }
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
               WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{w, h, 1};
    td.format = g_surface_format;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    g_present_tex = wgpuDeviceCreateTexture(g_device, &td);
    g_present_tex_view = g_present_tex ? wgpuTextureCreateView(g_present_tex, nullptr) : nullptr;
    g_present_tex_w = w;
    g_present_tex_h = h;
    if (!g_present_tex_view || !g_up_layout || !g_menu_sampler) return;
    WGPUBindGroupEntry be[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    be[0].binding = 4;
    be[0].textureView = g_present_tex_view;
    be[1].binding = 5;
    be[1].sampler = g_menu_sampler;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = g_up_layout;
    bd.entryCount = 2;
    bd.entries = be;
    g_up_group = wgpuDeviceCreateBindGroup(g_device, &bd);
}

// The overlay is generated at the presentation viewport aspect, capped at
// 2560x1440 so a 4K menu costs at most a 1440p upload. The UI canvas itself
// keeps a 16:9 design and is letterboxed, so text never stretches.
bool ensure_menu_texture(float vw, float vh) {
    auto &m = menu::menu();
    const bool open = m.is_open();
    const bool hint = !open && ps2::recomp_menu_available();
    const int kind = open ? 1 : hint ? 2 : 0;
    if (kind == 0 || vw < 1.0f || vh < 1.0f) return false;
    // Fixed 1920x1080 canvas. The shader fits it into the viewport, so the
    // CPU never has to rescale the overlay (that pass was ~20 ms at 720p).
    const std::uint32_t tw = 1920, th = 1080;
    if (tw != g_menu_w || th != g_menu_h) {
        create_menu_texture(tw, th);
    }
    const std::uint64_t key = open ? m.revision() : 1u;
    if (kind != g_overlay_kind || key != g_overlay_key) {
        const auto render_start = std::chrono::steady_clock::now();
        if (open) menu::render_overlay(g_menu_pixels, static_cast<int>(tw), static_cast<int>(th));
        else menu::render_hint(g_menu_pixels, static_cast<int>(tw), static_cast<int>(th), ps2::mode_select_active());
        const auto render_end = std::chrono::steady_clock::now();
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = g_menu_tex;
        dst.mipLevel = 0;
        dst.origin = WGPUOrigin3D{0, 0, 0};
        dst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.offset = 0;
        layout.bytesPerRow = tw * 4u;
        layout.rowsPerImage = th;
        WGPUExtent3D extent{tw, th, 1};
        wgpuQueueWriteTexture(g_queue, &dst, g_menu_pixels.data(),
                              static_cast<std::size_t>(tw) * th * 4u, &layout, &extent);
        const auto upload_end = std::chrono::steady_clock::now();
        if (std::getenv("PS2_MENU_STATS")) {
            const double render_ms = std::chrono::duration<double, std::milli>(render_end - render_start).count();
            const double upload_ms = std::chrono::duration<double, std::milli>(upload_end - render_end).count();
            std::fprintf(stderr, "[menu] %s %ux%u render %.2f ms upload %.2f ms\n",
                         open ? "menu" : "hint", tw, th, render_ms, upload_ms);
        }
        g_overlay_kind = kind;
        g_overlay_key = key;
    }
    return true;
}

void set_vsync(bool on) {
    g_options.vsync = on;
    if (!g_surface) return;
    WGPUPresentMode mode = WGPUPresentMode_Fifo;
    if (!on) {
        if (g_caps_mailbox) mode = WGPUPresentMode_Mailbox;
        else if (g_caps_immediate) mode = WGPUPresentMode_Immediate;
    }
    if (mode == g_present_mode) return;
    g_present_mode = mode;
    configure_surface();
}

bool attach_surface(const SurfaceTarget &target) {
    if (g_mode != Mode::primary || !g_device) return false;
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = view(target.selector ? target.selector : "#canvas");
    sd.nextInChain = &canvas.chain;
#else
#if defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND hwnd = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    if (target.kind != SurfaceTarget::Windows) return false;
    hwnd.hinstance = target.display;
    hwnd.hwnd = reinterpret_cast<void *>(static_cast<std::uintptr_t>(target.window));
    sd.nextInChain = &hwnd.chain;
#else
    WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
    WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    if (target.kind == SurfaceTarget::X11) {
        xlib.display = target.display;
        xlib.window = target.window;
        sd.nextInChain = &xlib.chain;
    } else if (target.kind == SurfaceTarget::Wayland) {
        wayland.display = target.display;
        wayland.surface = target.surface;
        sd.nextInChain = &wayland.chain;
    } else {
        return false;
    }
#endif
#endif
    g_surface = wgpuInstanceCreateSurface(g_instance, &sd);
    if (!g_surface) return false;
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps) != WGPUStatus_Success || !caps.formatCount) {
        wgpuSurfaceRelease(g_surface);
        g_surface = nullptr;
        return false;
    }
    g_surface_format = caps.formats[0];
    for (std::size_t i = 0; i < caps.formatCount; ++i)
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            g_surface_format = caps.formats[i];
            break;
        }
    g_caps_fifo = false;
    g_caps_immediate = false;
    g_caps_mailbox = false;
    for (std::size_t i = 0; i < caps.presentModeCount; ++i) {
        if (caps.presentModes[i] == WGPUPresentMode_Fifo) g_caps_fifo = true;
        if (caps.presentModes[i] == WGPUPresentMode_Immediate) g_caps_immediate = true;
        if (caps.presentModes[i] == WGPUPresentMode_Mailbox) g_caps_mailbox = true;
    }
    if (!g_caps_fifo) g_caps_fifo = true;  // Fifo is mandatory in WebGPU
    g_present_mode = WGPUPresentMode_Fifo;
    if (!g_options.vsync) {
        if (g_caps_mailbox) g_present_mode = WGPUPresentMode_Mailbox;
        else if (g_caps_immediate) g_present_mode = WGPUPresentMode_Immediate;
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    g_surface_width = target.width;
    g_surface_height = target.height;
    configure_surface();

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kBlitShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    WGPUBindGroupLayoutEntry le[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                      WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Fragment;
    le[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Fragment;
    le[1].buffer.type = WGPUBufferBindingType_Uniform;
    le[2].binding = 2;
    le[2].visibility = WGPUShaderStage_Fragment;
    le[2].texture.sampleType = WGPUTextureSampleType_Float;
    le[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    le[3].binding = 3;
    le[3].visibility = WGPUShaderStage_Fragment;
    le[3].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 4;
    ld.entries = le;
    WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &layout;
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = g_surface_format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = view("fs");
    fragment.targetCount = 1;
    fragment.targets = &color;
    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = pipeline_layout;
    rd.vertex.module = module;
    rd.vertex.entryPoint = view("vs");
    rd.fragment = &fragment;
    g_blit = wgpuDeviceCreateRenderPipeline(g_device, &rd);
    // Cheap upscale pipeline: samples the fixed-size blit texture, no FXAA
    // and no multi-tap source filtering.
    WGPUBindGroupLayoutEntry ue[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                      WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    ue[0].binding = 4;
    ue[0].visibility = WGPUShaderStage_Fragment;
    ue[0].texture.sampleType = WGPUTextureSampleType_Float;
    ue[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    ue[1].binding = 5;
    ue[1].visibility = WGPUShaderStage_Fragment;
    ue[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor uld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    uld.entryCount = 2;
    uld.entries = ue;
    g_up_layout = wgpuDeviceCreateBindGroupLayout(g_device, &uld);
    WGPUPipelineLayoutDescriptor upl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    upl.bindGroupLayoutCount = 1;
    upl.bindGroupLayouts = &g_up_layout;
    WGPUPipelineLayout up_layout = wgpuDeviceCreatePipelineLayout(g_device, &upl);
    WGPUColorTargetState ucolor = WGPU_COLOR_TARGET_STATE_INIT;
    ucolor.format = g_surface_format;
    WGPUFragmentState ufrag = WGPU_FRAGMENT_STATE_INIT;
    ufrag.module = module;
    ufrag.entryPoint = view("fs_up");
    ufrag.targetCount = 1;
    ufrag.targets = &ucolor;
    WGPURenderPipelineDescriptor urd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    urd.layout = up_layout;
    urd.vertex.module = module;
    urd.vertex.entryPoint = view("vs");
    urd.fragment = &ufrag;
    g_up_pipeline = wgpuDeviceCreateRenderPipeline(g_device, &urd);
    wgpuPipelineLayoutRelease(up_layout);
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pipeline_layout);
    g_blit_params = make_buffer(32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUSamplerDescriptor smd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    smd.addressModeU = WGPUAddressMode_ClampToEdge;
    smd.addressModeV = WGPUAddressMode_ClampToEdge;
    smd.addressModeW = WGPUAddressMode_ClampToEdge;
    smd.magFilter = WGPUFilterMode_Linear;
    smd.minFilter = WGPUFilterMode_Linear;
    smd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    smd.maxAnisotropy = 1;
    g_menu_sampler = wgpuDeviceCreateSampler(g_device, &smd);
    // The layout must outlive the pipeline: the overlay texture is recreated
    // when the window resizes, which means rebuilding the bind group.
    g_blit_layout = layout;
    create_menu_texture(1, 1);
    std::fprintf(stderr, "[gpu] present mode %s (vsync %s)\n",
                 g_present_mode == WGPUPresentMode_Fifo ? "fifo" :
                 g_present_mode == WGPUPresentMode_Mailbox ? "mailbox" : "immediate",
                 g_options.vsync ? "on" : "off");
    return g_blit != nullptr;
}

void resize_surface(std::uint32_t width, std::uint32_t height) {
    if (!g_surface || (width == g_surface_width && height == g_surface_height)) return;
    g_surface_width = width;
    g_surface_height = height;
    configure_surface();
}

bool has_surface() { return g_surface != nullptr; }

// Vertical averaging at presentation, in fractions of a native line. The
// framebuffer holds two half-line-interleaved fields; leaving this at 0 makes
// every element the game draws per field appear twice, half a line apart.
static std::uint32_t vert_blur() {
    static const std::uint32_t v = [] {
        const char *e = std::getenv("PS2_PRESENT_VBLUR");
        const double d = e ? std::atof(e) : 0.0;
        return static_cast<std::uint32_t>(std::clamp(d, 0.0, 1.0) * 256.0 + 0.5);
    }();
    return v;
}

// Target presentation aspect. PS2_PRESENT_AR accepts the named ratios, a
// "W:H" pair (e.g. 2560:1080) or a plain float (1.891); without it the old
// --widescreen / 4:3 defaults apply.
static float present_aspect() {
    float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (const char *ar = std::getenv("PS2_PRESENT_AR")) {
        if (std::strcmp(ar, "4:3") == 0) aspect = 4.0f / 3.0f;
        else if (std::strcmp(ar, "16:9") == 0) aspect = 16.0f / 9.0f;
        else if (std::strcmp(ar, "21:9") == 0) aspect = 21.0f / 9.0f;
        else if (std::strcmp(ar, "32:9") == 0) aspect = 32.0f / 9.0f;
        else if (std::strcmp(ar, "fit") == 0) return 1.0f;
        else {
            float a = 0.0f, b = 0.0f;
            if (std::sscanf(ar, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) aspect = a / b;
            else {
                const float v = std::atof(ar);
                if (v > 0.5f && v < 8.0f) aspect = v;
            }
        }
    }
    return aspect;
}

bool present(const DisplaySource &src, int mode) {
    g_last_src_w = src.width;
    g_last_src_h = src.height;
    g_menu_screen = ps2::menu_screen_native_aspect();
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr, "[gpu] window scale %u internal %ux%u surface %ux%u\n",
                     g_scale, src.width * g_scale, src.height * g_scale, g_surface_width, g_surface_height);
    }
    if (!g_surface) return false;
    if (mode == 2 && g_field == 0) mode = 0;
    flush_all();
    // mode 0: normal. mode 1: new game frame, present the half-way blend of the
    // previous and current frame. mode 2: no new game frame, show the frame
    // already blended/presented by mode 1.
    const bool draw = mode == 0 || mode == 1;
    // Reading the field buffer we are about to overwrite can catch a framebuffer
    // that is still being updated (one field written, the other from the previous
    // frame), which shows up as a duplicated/offset image. PS2_PRESENT_DELAY=1
    // shows the last *completed* field buffer instead.
    static const bool delay_present = std::getenv("PS2_PRESENT_DELAY") != nullptr;
    std::uint32_t current = draw ? field_base(g_field) : field_base(g_field - 1);
    std::uint32_t previous = draw ? field_base(g_field + 1) : field_base(g_field - 2);
    if (delay_present && g_have_previous) {
        current = field_base(g_field - 1);
        previous = field_base(g_field - 2);
    }
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    if (std::getenv("PS2_GS_DEBUG")) {
        const DisplayRegs dr = display_regs();
        static int once = 0;
        if (once < 6) std::fprintf(stderr, "[display] pmode %04llX smode2 %04llX dispfb %016llX %016llX display %016llX %016llX\n",
            (unsigned long long)dr.pmode, (unsigned long long)dr.smode2, (unsigned long long)dr.dispfb[0],
            (unsigned long long)dr.dispfb[1], (unsigned long long)dr.display[0], (unsigned long long)dr.display[1]);
        if (once++ < 6) std::fprintf(stderr, "[present] %ux%u halved %d scale %u dbx %u dby %u fbp %04X fbw %u psm %02X out %ux%u\n",
            src.width, src.height, src.halved ? 1 : 0, g_scale, src.dbx, src.dby, src.fbp / 32u, src.fbw, src.psm, g_surface_width, g_surface_height);
    }
    if (std::getenv("PS2_GS_DEBUG")) {
        static int last = -1;
        const int st = (src.halved ? 1 : 0) | (src.bob ? 2 : 0) | (g_have_previous ? 4 : 0);
        if (st != last) { last = st; std::fprintf(stderr, "[present] halved %d bob %d prev %d\n", src.halved ? 1 : 0, src.bob ? 1 : 0, g_have_previous ? 1 : 0); }
    }
    const bool native_blend = src.halved && g_have_previous && g_last_width == src.width && g_last_height == src.height && !weave_off;
    const bool blend = (mode == 1 || mode == 3) ? g_have_previous : native_blend;
    if (draw) {
        const std::uint32_t dpmode = src.halved ? 1u : (src.bob ? 2u : 0u);
        const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, dpmode, current, src.field});
        g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    }
    const float sw = static_cast<float>(g_surface_width), sh = static_cast<float>(g_surface_height);
    float aspect = present_aspect();
    if (const char *ar = std::getenv("PS2_PRESENT_AR"); ar && std::strcmp(ar, "fit") == 0)
        aspect = static_cast<float>(src.width) / static_cast<float>(src.height);
    // 2D menus (Mode Select / Master Mode pause) are native 4:3; pillarbox
    // them instead of stretching their signs, icons and text to 21:9/32:9.
    // With the recomp overlay open, use the chosen aspect so the menu itself
    // can use the full window.
    if (g_menu_screen && !menu::menu().is_open()) aspect = 4.0f / 3.0f;
    float vw = std::min(sw, sh * aspect), vh = vw / aspect;
    // The game image must not grow with a maximized window. Keep the
    // presented area at the configured Resolution (centered, black around it)
    // so the blit/FXAA cost stays fixed instead of following a 4K window.
    if (g_options.out_width > 0 && g_options.out_height > 0 &&
        !std::getenv("PS2_NO_PRESENT_CAP")) {
        const float cap_w = static_cast<float>(g_options.out_width);
        const float cap_h = static_cast<float>(g_options.out_height);
        if (vw > cap_w) { vw = cap_w; vh = vw / aspect; }
        if (vh > cap_h) { vh = cap_h; vw = vh * aspect; }
    }
    g_present_vw = vw;
    g_present_vh = vh;
    const bool need_upscale =
        g_surface_width > static_cast<std::uint32_t>(vw) + 1u ||
        g_surface_height > static_cast<std::uint32_t>(vh) + 1u;
    if (need_upscale)
        ensure_present_texture(static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh));
    g_present_upscale = need_upscale && g_present_tex_view && g_up_group;
    const bool menu_visible = ensure_menu_texture(vw, vh);
    const bool menu_dim = menu_visible && menu::menu().is_open();
    const std::uint32_t blit[8] = {src.width * g_scale, src.height * g_scale, current, previous, blend ? 1u : 0u,
                                   static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh),
                                   (std::getenv("PS2_PRESENT_NEAREST") ? 1u : 0u) | (g_scale << 8) | (vert_blur() << 16) |
                                       (menu_visible ? (1u << 24) : 0u) | (menu_dim ? (1u << 25) : 0u) |
                                       (g_options.aa ? (1u << 26) : 0u) |
                                       ((static_cast<std::uint32_t>(g_options.aa) & 3u) << 27)};
    wgpuQueueWriteBuffer(g_queue, g_blit_params, 0, blit, sizeof blit);
    if (draw) {
        ++g_field;
        g_have_previous = true;
        g_last_width = src.width;
        g_last_height = src.height;
    }
    // Opt-in: render the exact presentation pass to an offscreen texture and
    // write it as PPM. The X11 window capture is stale with the Vulkan
    // swapchain, so this is the only reliable way to see what is presented.
    if (const char *dump = std::getenv("PS2_PRESENT_DUMP")) {
        static bool done = false;
        const char *ff = std::getenv("PS2_PRESENT_DUMP_FRAME");
        bool want = true;
        if (ff) {
            const std::uint64_t at = std::strtoull(ff, nullptr, 10);
            // Exact by default. PS2_PRESENT_DUMP_LATE=1 accepts the first
            // present at or after the frame, which is what tests want when the
            // render thread can skip a counter value.
            want = std::getenv("PS2_PRESENT_DUMP_LATE") ? ps2::frame_count() >= at : ps2::frame_count() == at;
        }
        if (!done && want) {
            done = true;
            const std::uint32_t w = g_surface_width, h = g_surface_height;
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
            td.dimension = WGPUTextureDimension_2D;
            td.size = WGPUExtent3D{w, h, 1};
            td.format = g_surface_format;
            td.mipLevelCount = 1;
            td.sampleCount = 1;
            WGPUTexture tex = wgpuDeviceCreateTexture(g_device, &td);
            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
            submit(view, false);
            const std::uint32_t bpr = ((w * 4u + 255u) / 256u) * 256u;
            WGPUBuffer buf = make_buffer(static_cast<std::uint64_t>(bpr) * h,
                                         WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            src.texture = tex;
            WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            dst.buffer = buf;
            dst.layout.bytesPerRow = bpr;
            dst.layout.rowsPerImage = h;
            WGPUExtent3D ext{w, h, 1};
            wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
            wgpuQueueSubmit(g_queue, 1, &cmd);
            wgpuCommandBufferRelease(cmd);
            wgpuCommandEncoderRelease(enc);
            bool mapped = false;
            WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *flag, void *) {
                if (status != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[present] dump map failed %d\n", static_cast<int>(status));
                *static_cast<bool *>(flag) = true;
            };
            cb.userdata1 = &mapped;
            wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, static_cast<std::uint64_t>(bpr) * h, cb);
            wait_until(mapped);
            const void *pixels = wgpuBufferGetConstMappedRange(buf, 0, static_cast<std::uint64_t>(bpr) * h);
            if (pixels) {
                if (std::FILE *f = std::fopen(dump, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                    const bool bgra = g_surface_format == WGPUTextureFormat_BGRA8Unorm;
                    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 3u);
                    const auto *bytes = static_cast<const std::uint8_t *>(pixels);
                    for (std::uint32_t y = 0; y < h; ++y) {
                        const std::uint8_t *p = bytes + static_cast<std::size_t>(y) * bpr;
                        for (std::uint32_t x = 0; x < w; ++x) {
                            row[x * 3 + 0] = p[x * 4 + (bgra ? 2 : 0)];
                            row[x * 3 + 1] = p[x * 4 + 1];
                            row[x * 3 + 2] = p[x * 4 + (bgra ? 0 : 2)];
                        }
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[present] dumped %s (%ux%u)\n", dump, w, h);
                }
            }
            wgpuBufferUnmap(buf);
            wgpuBufferRelease(buf);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(tex);
            return true;
        }
    }
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        submit();
        configure_surface();
        return true;
    }
    WGPUTextureView target = wgpuTextureCreateView(st.texture, nullptr);
    submit(target, g_present_upscale);
#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(g_surface);
#endif
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(st.texture);
    if (g_draws_this_frame) {
        g_draws_last_frame = g_draws_this_frame;
        g_draws_this_frame = 0;
    }
    return true;
}

double busy_ms() {
    if (!g_measured) return 0.0;
    return static_cast<double>(g_measured_ns) / static_cast<double>(g_measured) * static_cast<double>(g_total_passes) / 1e6;
}

struct Copy {
    WGPUBuffer buffer = nullptr;
    std::uint64_t size = 0, order = 0;
    std::uint32_t width = 0, height = 0;
    bool busy = false, ready = false;
};
Copy g_copies[3];
std::uint64_t g_copy_order = 0;

bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
#ifdef __EMSCRIPTEN__
    wgpuInstanceProcessEvents(g_instance);
#else
    wgpuDevicePoll(g_device, false, nullptr);
#endif
    Copy *newest = nullptr;
    for (Copy &c : g_copies)
        if (c.ready && (!newest || c.order > newest->order)) newest = &c;
    if (newest) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(newest->width) * newest->height * 4;
        const void *mapped = wgpuBufferGetConstMappedRange(newest->buffer, 0, bytes);
        if (mapped) {
            rgba.resize(static_cast<std::size_t>(newest->width) * newest->height);
            std::memcpy(rgba.data(), mapped, bytes);
            width = newest->width;
            height = newest->height;
        }
        for (Copy &c : g_copies)
            if (c.ready && c.order <= newest->order) {
                wgpuBufferUnmap(c.buffer);
                c.ready = c.busy = false;
            }
        if (!mapped) newest = nullptr;
    }
    Copy *slot = nullptr;
    for (Copy &c : g_copies)
        if (!c.busy) slot = &c;
    if (slot) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(src.width) * src.height * g_scale * g_scale * 4;
        if (slot->size < bytes) {
            if (slot->buffer) wgpuBufferRelease(slot->buffer);
            slot->size = bytes;
            slot->buffer = make_buffer(bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        }
        flush_all();
        const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : (src.bob ? 2u : 0u), 0u, src.field});
        g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
        submit();
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_scratch, 0, slot->buffer, 0, bytes);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        slot->busy = true;
        slot->order = ++g_copy_order;
        slot->width = src.width * g_scale;
        slot->height = src.height * g_scale;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *copy, void *) {
            Copy &c = *static_cast<Copy *>(copy);
            if (status == WGPUMapAsyncStatus_Success) c.ready = true;
            else c.busy = false;
        };
        cb.userdata1 = slot;
        wgpuBufferMapAsync(slot->buffer, WGPUMapMode_Read, 0, bytes, cb);
    }
    return newest != nullptr;
}

bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    g_menu_screen = ps2::menu_screen_native_aspect();
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr, "[gpu] headless scale %u internal %ux%u\n", g_scale, src.width * g_scale, src.height * g_scale);
    }
    flush_all();
    finalize_draw_log();
    finalize_vram_dump();
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : (src.bob ? 2u : 0u), 0u, src.field});
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    submit();
    width = src.width * g_scale;
    height = src.height * g_scale;
    rgba.resize(static_cast<std::size_t>(width) * height);
    read_back(g_scratch, rgba.size() * 4, rgba.data());
    return true;
}

}
