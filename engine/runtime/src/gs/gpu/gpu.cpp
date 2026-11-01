#include "gpu.hpp"
#include "ps2/system.hpp"

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
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern const char kGsShader[];
extern const char kBlitShader[];

namespace ps2::gs {
void swizzle_tables(std::vector<std::uint32_t> &out);
}

namespace ps2::gs::gpu {

namespace {

enum Pipeline { kUpload, kCopyRead, kCopyWrite, kLoadClut, kDisplay, kRaster, kResolve, kSnapshot, kDecode, kPreBatch, kRasterSprite, kPipelines };
const char *const kEntryPoints[kPipelines] = {"upload", "copy_read", "copy_write", "load_clut", "display", "raster", "resolve", "snapshot", "decode", "pre_batch", "raster_sprite"};

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
WGPUBuffer g_blit_params = nullptr;
std::uint32_t g_field = 0, g_last_width = 0, g_last_height = 0;
bool g_have_previous = false;
std::uint32_t g_scale = 1, g_copy_words = 1u << 20, g_field_words = 1u << 19;

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
bool g_sprite_fast = false;
bool g_skip_raster = false;
bool g_render_tile_pass = false;
bool g_render_tile_all = false;
std::uint64_t g_render_tile_frame = 0;
std::size_t g_render_tile_command = 0;
bool g_primitive_pass = false;
bool g_primitive_pass_skip = false;
bool g_primitive_pass_compute = false;
bool g_primitive_pass_all = false;
std::uint32_t g_primitive_pass_matches = 0;
#ifndef __EMSCRIPTEN__
WGPUBuffer g_primitive_probe = nullptr;
#endif
std::uint64_t g_params_size = 0, g_data_size = 0;
WGPUBindGroupLayout g_layout = nullptr;
WGPUPipelineLayout g_pipeline_layout = nullptr;
WGPUBindGroup g_group = nullptr;
WGPUComputePipeline g_pipelines[kPipelines] = {};
WGPURenderPipeline g_tile_pipeline = nullptr;
WGPURenderPipeline g_primitive_pipeline = nullptr;
WGPUTexture g_tile_texture = nullptr;
WGPUTextureView g_tile_view = nullptr;

struct Command {
    Pipeline pipeline;
    std::uint32_t x, y;
    std::uint32_t param;
    bool tile_pass = false;
    std::uint32_t instances = 0;
    bool primitive_pass = false;
    bool primitive_probe = false;
};

struct CommandInfo {
    std::size_t index;
    std::uint32_t values[8];
};
std::vector<CommandInfo> g_command_info;
std::FILE *g_profile_out = nullptr;
std::uint64_t g_profile_from = 0, g_profile_to = 0;
std::FILE *g_access_out = nullptr;
std::uint64_t g_access_from = 0, g_access_to = 0;
std::FILE *g_fastpath_trace = nullptr;
std::uint64_t g_fastpath_trace_from = 0, g_fastpath_trace_to = 0;
std::FILE *g_prim_trace = nullptr;
std::uint64_t g_prim_trace_frame = 0;
std::size_t g_prim_trace_command = 0;
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

struct PageMask {
    std::array<std::uint64_t, 8> bits = {};
    void all() { bits.fill(~0ull); }
    void add(std::uint64_t physical_page) { bits[(physical_page & 511u) >> 6] |= 1ull << (physical_page & 63u); }
    bool overlaps(const PageMask &other) const {
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i] & other.bits[i]) return true;
        return false;
    }
};

struct CommandAccess {
    std::vector<Range> reads, writes;
    struct Resource { std::vector<Range> reads, writes; } clut, decoded, scratch;
};
std::vector<CommandAccess> g_command_access;

CommandAccess *access_for_last_command() {
    if (!g_access_out || g_commands.empty()) return nullptr;
    g_command_access.resize(g_commands.size());
    return &g_command_access.back();
}

void dump_access_profile() {
    if (!g_access_out || frame_count() < g_access_from || frame_count() >= g_access_to) return;
    const auto print_ranges = [](std::FILE *out, const std::vector<Range> &ranges) {
        if (ranges.empty()) {
            std::fputc('-', out);
            return;
        }
        for (std::size_t i = 0; i < ranges.size(); ++i)
            std::fprintf(out, "%s%llu:%llu", i ? ";" : "", static_cast<unsigned long long>(ranges[i].lo), static_cast<unsigned long long>(ranges[i].hi));
    };
    for (std::size_t i = 0; i < g_commands.size(); ++i) {
        const CommandAccess empty;
        const CommandAccess &access = i < g_command_access.size() ? g_command_access[i] : empty;
        std::fprintf(g_access_out, "%llu %zu %s %u %u ", static_cast<unsigned long long>(frame_count()), i,
                     kEntryPoints[g_commands[i].pipeline], g_commands[i].x, g_commands[i].y);
        print_ranges(g_access_out, access.reads);
        std::fputc(' ', g_access_out);
        print_ranges(g_access_out, access.writes);
        for (const CommandAccess::Resource *resource : {&access.clut, &access.decoded, &access.scratch}) {
            std::fputc(' ', g_access_out);
            print_ranges(g_access_out, resource->reads);
            std::fputc(' ', g_access_out);
            print_ranges(g_access_out, resource->writes);
        }
        std::fputc('\n', g_access_out);
    }
    std::fflush(g_access_out);
}

struct Batch {
    std::vector<std::uint32_t> prims;
    std::vector<std::uint32_t> setups;
    std::vector<std::uint32_t> boxes;
    std::uint64_t key = ~0ull;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    bool zte = false, zwrite = false;
    std::uint32_t texflushes = 0;
    int rx0 = 0, ry0 = 0, rx1 = -2, ry1 = -2;
    Range fb_range, z_range;
    PageMask fb_mask, z_mask;
} g_batch;

struct SetupCache {
    std::uint32_t generation = ~0u, clut = ~0u;
    std::uint64_t attr = ~0ull;
    std::uint32_t setup[kSetupWords] = {};
    std::uint32_t flags = 0;
    std::uint64_t key = 0;
    Range tex;
    PageMask tex_mask;
} g_setup_cache;

struct PreOp {
    bool clut = false;
    std::uint32_t rec[12] = {};
    Range range;
    PageMask mask;
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
std::vector<PageMask> g_batch_read_masks;
bool g_reorder_pre = false;
bool g_fuse_pre = false;
bool g_conflict_diag = false;
std::uint64_t g_conflict_checks = 0, g_conflict_coarse = 0, g_conflict_precise = 0, g_conflict_false = 0, g_conflict_missed = 0;
bool g_cpu_diag = false;
bool g_chunk_writes = false;
bool g_fastpath_diag = false;
bool g_split_fastpath = false;
std::uint64_t g_fastpath_splits = 0;
struct FastpathStats {
    std::uint64_t total = 0, no_snapshot = 0, standard_frame = 0, no_blend_or_date = 0;
    std::uint64_t alpha_keep = 0, no_fb_mask = 0, strict = 0, textured_strict = 0;
    std::uint64_t broad = 0, textured_broad = 0, standard_alpha_blend = 0, identity_blend = 0;
    std::uint64_t batches = 0, broad_batches = 0, broad_batch_prims = 0, broad_no_z_batches = 0, broad_no_z_prims = 0;
    std::array<std::uint64_t, 4> alpha_fail_modes{};
    std::array<std::uint64_t, 256> blend_modes{};
} g_fastpath_stats;
struct CpuTiming {
    std::uint64_t count = 0, commands = 0, data_bytes = 0, param_bytes = 0;
    double ensure_ms = 0, data_ms = 0, params_ms = 0, encode_ms = 0, queue_ms = 0, poll_ms = 0;
} g_cpu_timing;
std::uint64_t g_reordered_pre_ops = 0;

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

PageMask page_mask_region(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
                          int x0, int y0, int x1, int y1) {
    PageMask mask;
    // Unusual wrapping rectangles stay conservative until their exact GS page
    // traversal is modelled; the common in-range rectangles are enumerated.
    if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || x1 >= 2048 || y1 >= 2048) {
        mask.all();
        return mask;
    }
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const std::uint64_t base = static_cast<std::uint64_t>(bp) * 256;
    for (int py = y0 / static_cast<int>(ph); py <= y1 / static_cast<int>(ph); ++py)
        for (int px = x0 / static_cast<int>(pw); px <= x1 / static_cast<int>(pw); ++px) {
            const std::uint64_t start = base + (static_cast<std::uint64_t>(px) + static_cast<std::uint64_t>(py) * per_row) * 8192;
            mask.add(start / 8192);
            mask.add((start + 8191) / 8192);
        }
    return mask;
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
bool g_upload_diag = false;
bool g_texture_diag = false;
bool g_decode_cache = false;
std::unordered_set<std::string> g_decode_seen;
std::unordered_map<std::string, std::uint32_t> g_decode_offsets;
std::array<std::uint64_t, kClutSlots> g_clut_versions{};
std::uint64_t g_decode_diag_total = 0, g_decode_diag_hits = 0, g_decode_diag_words = 0, g_decode_diag_hit_words = 0;
std::uint64_t g_decode_cache_hits = 0, g_decode_cache_hit_words = 0;
using UploadEpochs = std::vector<std::pair<std::uint16_t, std::uint64_t>>;
std::unordered_map<std::string, UploadEpochs> g_upload_signatures;
std::array<std::uint64_t, 512> g_vram_epochs = {};
std::uint64_t g_upload_diag_total = 0, g_upload_diag_repeated = 0, g_upload_diag_safe = 0, g_vram_epoch = 0;

template <typename F> void for_vram_page(const Range &r, F visit) {
    const std::uint64_t first = r.lo >> 13;
    const std::uint64_t count = std::min<std::uint64_t>((r.hi + 8191) / 8192 - first, 512);
    for (std::uint64_t i = 0; i < count; ++i) visit(static_cast<std::uint16_t>((first + i) & 511u));
}

void diag_vram_write(const Range &r) {
    if (!g_upload_diag && !g_texture_diag && !g_decode_cache) return;
    const std::uint64_t epoch = ++g_vram_epoch;
    for_vram_page(r, [&](std::uint16_t page) { g_vram_epochs[page] = epoch; });
}
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
    if (rec[1] != 0 || tx1 - tx0 < 2) {
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

bool rendered(const Range &r) {
    for (std::uint64_t at = r.lo; at < r.hi; at += 8192)
        if (g_rendered[(at >> 13) & 511u]) return true;
    return false;
}

struct Decode {
    std::uint32_t setup, offset, words;
};
std::vector<Decode> g_decodes;

std::string decode_key(const std::uint32_t *w) {
    std::string key;
    key.reserve(256);
    const auto append = [&key](auto value) {
        key.append(reinterpret_cast<const char *>(&value), sizeof value);
    };
    for (int index : {12, 13, 14, 15, 16, 17, 20}) append(w[index]);
    append(w[0] & 32768u);
    const std::uint32_t psm = w[14];
    if (psm == PSMT8 || psm == PSMT8H || psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) {
        append(w[22]);
        append(g_clut_versions[w[22]]);
    }
    const std::uint32_t tw = w[15] & 0xFFu, th = w[15] >> 8;
    const Range texture = region(w[12], w[13], psm, 0, 0, (1 << tw) - 1, (1 << th) - 1);
    for_vram_page(texture, [&](std::uint16_t page) {
        append(page);
        append(g_vram_epochs[page]);
    });
    return key;
}

void diag_decode_candidate(const std::string &key, std::uint32_t words) {
    if (!g_texture_diag) return;
    ++g_decode_diag_total;
    g_decode_diag_words += words;
    if (!g_decode_seen.insert(key).second) {
        ++g_decode_diag_hits;
        g_decode_diag_hit_words += words;
    }
    if (g_decode_seen.size() > 65536) g_decode_seen.clear();
}

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
        if (g_scale > 1 && rendered(pages(w[12], w[13], w[14], 0, 0, (1 << tw) - 1, (1 << th) - 1))) continue;
        std::string key;
        if (g_texture_diag || g_decode_cache) key = decode_key(w);
        diag_decode_candidate(key, words);
        if (g_decode_cache) {
            const auto hit = g_decode_offsets.find(key);
            if (hit != g_decode_offsets.end()) {
                w[24] = hit->second;
                ++g_decode_cache_hits;
                g_decode_cache_hit_words += words;
                continue;
            }
        }
        if (g_decoded_used + words > kDecodedWords) continue;
        w[24] = g_decoded_used;
        g_decodes.push_back({i, g_decoded_used, words});
        if (g_decode_cache) g_decode_offsets.emplace(std::move(key), g_decoded_used);
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
    if (CommandAccess *access = access_for_last_command()) {
        access->reads.push_back({static_cast<std::uint64_t>(lo) * 4, static_cast<std::uint64_t>(hi) * 4});
        access->scratch.writes.push_back({0, static_cast<std::uint64_t>(words) * 4});
    }
    return words;
}

void note_raster(std::uint32_t prims, std::uint32_t tiles, std::uint32_t entries, std::uint32_t snapshot) {
    if (!g_profile_out) return;
    const Batch &b = g_batch;
    const std::uint32_t *first = b.setups.data();
    std::uint32_t textured = 0;
    for (std::uint32_t i = 0; i < prims; ++i) textured += (b.setups[b.prims[i * kPrimWords] * kSetupWords] & 2u) ? 1u : 0u;
    g_command_info.push_back({g_commands.size(), {prims, tiles, entries, snapshot, b.prims[1], first[3] | (first[14] << 8) | (first[6] << 16), first[0], textured}});
}

void flush_batch() {
    Batch &b = g_batch;
    if (b.prims.empty()) {
        b.key = ~0ull;
        return;
    }
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
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
    plan_decodes(prims);
    const std::uint32_t prim_offset = arena_append(b.prims.data(), b.prims.size() * 4);
    const std::uint32_t setup_offset = arena_append(b.setups.data(), b.setups.size() * 4);
    for (const Decode &d : g_decodes) {
        dispatch_linear(kDecode, d.words, push_params({setup_offset + d.setup * kSetupWords, d.offset, d.words}));
        if (CommandAccess *access = access_for_last_command()) {
            const std::uint32_t *setup = b.setups.data() + d.setup * kSetupWords;
            access->reads.push_back(pages(setup[12], setup[13], setup[14], 0, 0, (1 << (setup[15] & 0xFFu)) - 1, (1 << (setup[15] >> 8)) - 1));
            access->decoded.writes.push_back({static_cast<std::uint64_t>(d.offset) * 4, static_cast<std::uint64_t>(d.offset + d.words) * 4});
            if (setup[14] == PSMT8 || setup[14] == PSMT8H || setup[14] == PSMT4 || setup[14] == PSMT4HL || setup[14] == PSMT4HH)
                access->clut.reads.push_back({static_cast<std::uint64_t>(setup[22]) * kClutEntries * 4,
                                               static_cast<std::uint64_t>(setup[22] + 1) * kClutEntries * 4});
        }
    }
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
    auto *halves = reinterpret_cast<unsigned char *>(grown + tiles * 2 + 2);
    for (std::uint32_t i = 0; i < prims; ++i) {
        const auto index = static_cast<std::uint16_t>(i);
        for (std::uint32_t k = g_span_start[i]; k < g_span_start[i + 1]; ++k) {
            const Span &sp = g_spans[k];
            std::uint32_t t = static_cast<std::uint32_t>(sp.ty * g_tiles_per_row + sp.tx0);
            for (int tx = sp.tx0; tx <= sp.tx1; ++tx, ++t) std::memcpy(halves + 2u * g_tile_fill[t]++, &index, 2);
        }
    }
    for (std::uint32_t t : g_touched) g_tile_count[t] = 0;
    std::uint32_t snapshot_lo, snapshot_hi;
    const std::uint32_t snapshot = snapshot_batch(prims, snapshot_lo, snapshot_hi);
    if (g_prim_trace && frame_count() == g_prim_trace_frame && g_commands.size() == g_prim_trace_command) {
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *rec = b.prims.data() + i * kPrimWords;
            const std::uint32_t *setup = b.setups.data() + rec[0] * kSetupWords;
            const std::uint32_t flags = setup[0];
            const std::uint32_t values[] = {
                i, rec[1], flags, setup[3], setup[6], setup[4],
                (flags & 8u) ? (setup[7] >> 12) & 0xFFu : 256u,
                (flags & 512u) ? (setup[7] >> 8) & 3u : 4u,
                (flags & 512u) != 0u, (flags & 32u) == 0u,
                (flags & 64u) != 0u, setup[7] & 7u, (setup[7] >> 4) & 3u,
                (flags & 2u) ? setup[14] : 256u,
                (flags & 2u) ? 1u << (setup[15] & 0xFFu) : 0u,
                (flags & 2u) ? 1u << (setup[15] >> 8) : 0u,
                setup[10] & 0xFFFFu, setup[11] & 0xFFFFu, setup[10] >> 16, setup[11] >> 16,
            };
            std::fprintf(g_prim_trace, "%llu,%zu", static_cast<unsigned long long>(frame_count()), g_commands.size());
            for (std::uint32_t value : values) std::fprintf(g_prim_trace, ",%u", value);
            std::fputc('\n', g_prim_trace);
        }
        std::fflush(g_prim_trace);
    }
    if (g_fastpath_trace && frame_count() >= g_fastpath_trace_from && frame_count() < g_fastpath_trace_to) {
        std::uint32_t broad = 0, textured = 0, flags_or = 0, flags_and = ~0u;
        std::uint32_t blend_mode = ~0u, ztest_mode = ~0u, tex_psm = ~0u, tex_width = ~0u, tex_height = ~0u;
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *setup = b.setups.data() + b.prims[i * kPrimWords] * kSetupWords;
            const std::uint32_t flags = setup[0];
            const bool common = !snapshot && (setup[3] == PSMCT32 || setup[3] == PSMCT24) && !(flags & 128u) &&
                                (!(flags & 64u) || !((setup[7] >> 4) & 3u)) && !setup[4] && b.prims[i * kPrimWords + 1] <= 1u;
            broad += common;
            textured += (flags & 2u) != 0u;
            flags_or |= flags;
            flags_and &= flags;
            const std::uint32_t blend = (flags & 8u) ? ((setup[7] >> 12) & 0xFFu) : 256u;
            const std::uint32_t ztest = (flags & 512u) ? ((setup[7] >> 8) & 3u) : 4u;
            const std::uint32_t texture = (flags & 2u) ? setup[14] : 256u;
            const std::uint32_t width = (flags & 2u) ? (1u << (setup[15] & 0xFFu)) : 0u;
            const std::uint32_t height = (flags & 2u) ? (1u << (setup[15] >> 8)) : 0u;
            if (i == 0) {
                blend_mode = blend;
                ztest_mode = ztest;
                tex_psm = texture;
                tex_width = width;
                tex_height = height;
            } else {
                if (blend_mode != blend) blend_mode = 257u;
                if (ztest_mode != ztest) ztest_mode = 5u;
                if (tex_psm != texture) tex_psm = 257u;
                if (tex_width != width) tex_width = 0u;
                if (tex_height != height) tex_height = 0u;
            }
        }
        std::fprintf(g_fastpath_trace,
                     "%llu,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                     static_cast<unsigned long long>(frame_count()), g_commands.size(), prims, tiles, total, snapshot,
                     b.fpsm, b.zpsm, b.fbp, b.zbp, b.zte ? 1u : 0u, b.zwrite ? 1u : 0u,
                     b.minx, b.miny, b.maxx, b.maxy, broad, textured, flags_or, flags_and,
                     blend_mode, ztest_mode, tex_psm, tex_width, tex_height);
    }
    if (g_fastpath_diag) {
        ++g_fastpath_stats.batches;
        bool whole_broad = true, whole_broad_no_z = true;
        for (std::uint32_t i = 0; i < prims; ++i) {
            const std::uint32_t *setup = b.setups.data() + b.prims[i * kPrimWords] * kSetupWords;
            const std::uint32_t flags = setup[0];
            const bool standard_frame = setup[3] == PSMCT32 || setup[3] == PSMCT24;
            const bool alpha_keep = !(flags & 64u) || !((setup[7] >> 4) & 3u);
            const bool common = !snapshot && standard_frame && !(flags & 128u) && alpha_keep && !setup[4] && b.prims[i * kPrimWords + 1] <= 1u;
            whole_broad = whole_broad && common;
            whole_broad_no_z = whole_broad_no_z && common && !(flags & 512u);
            if (flags & 64u) ++g_fastpath_stats.alpha_fail_modes[(setup[7] >> 4) & 3u];
            if (flags & 8u) ++g_fastpath_stats.blend_modes[(setup[7] >> 12) & 0xFFu];
            ++g_fastpath_stats.total;
            if (snapshot) continue;
            ++g_fastpath_stats.no_snapshot;
            if (!standard_frame) continue;
            ++g_fastpath_stats.standard_frame;
            if (common) {
                ++g_fastpath_stats.broad;
                if (flags & 2u) ++g_fastpath_stats.textured_broad;
                const std::uint32_t blend = (setup[7] >> 12) & 0xFFu;
                if ((flags & 8u) && blend == 0x44u) ++g_fastpath_stats.standard_alpha_blend;
                if ((flags & 8u) && (blend == 0x0Au || blend == 0x2Au)) ++g_fastpath_stats.identity_blend;
            }
            if (flags & (8u | 128u)) continue;
            ++g_fastpath_stats.no_blend_or_date;
            if ((flags & 64u) && ((setup[7] >> 4) & 3u)) continue;
            ++g_fastpath_stats.alpha_keep;
            if (setup[4]) continue;
            ++g_fastpath_stats.no_fb_mask;
            if (b.prims[i * kPrimWords + 1] > 1u) continue;
            ++g_fastpath_stats.strict;
            if (flags & 2u) ++g_fastpath_stats.textured_strict;
        }
        if (whole_broad) {
            ++g_fastpath_stats.broad_batches;
            g_fastpath_stats.broad_batch_prims += prims;
        }
        if (whole_broad_no_z) {
            ++g_fastpath_stats.broad_no_z_batches;
            g_fastpath_stats.broad_no_z_prims += prims;
        }
    }
    const std::uint32_t param = push_params({tile_offset, prim_offset, setup_offset, tiles, list_offset, snapshot_lo, snapshot_hi});
    note_raster(prims, tiles, total, snapshot);
    bool sprite_batch = g_sprite_fast && tiles >= 128;
    for (std::uint32_t i = 0; sprite_batch && i < prims; ++i)
        sprite_batch = b.prims[i * kPrimWords + 1] == 1u;
    const bool tile_pass = g_render_tile_pass && (g_render_tile_all ||
                            (frame_count() == g_render_tile_frame && g_commands.size() == g_render_tile_command));
    bool primitive_pass = g_primitive_pass && g_scale == 1 && frame_count() >= 6350 &&
                          (g_primitive_pass_all || g_primitive_pass_matches == 0) &&
                          (g_primitive_pass_all || (prims == 77 && tiles == 122)) && snapshot == 0 &&
                          b.fpsm == PSMCT32 && b.zpsm == PSMZ16S && b.minx >= 0 && b.miny >= 0 &&
                          b.maxx < 512 && b.maxy < 448;
    for (std::uint32_t i = 0; primitive_pass && i < prims; ++i) {
        const std::uint32_t *rec = b.prims.data() + i * kPrimWords;
        const std::uint32_t *setup = b.setups.data() + rec[0] * kSetupWords;
        primitive_pass = rec[1] == 0u && setup[0] == 27207u && setup[3] == PSMCT32 &&
                         setup[6] == PSMZ16S && setup[4] == 0u && setup[14] == PSMT8 &&
                         (setup[7] & 7u) == 1u && ((setup[7] >> 8u) & 3u) == 2u;
    }
    if (primitive_pass) {
        ++g_primitive_pass_matches;
#ifdef __EMSCRIPTEN__
        if (g_primitive_pass_matches == 1)
#endif
        std::fprintf(stderr, "PS2_PRIMITIVE_PASS frame=%llu command=%zu prims=%u tiles=%u\n",
                     static_cast<unsigned long long>(frame_count()), g_commands.size(), prims, tiles);
    }
    if (tile_pass && !g_render_tile_all)
        std::fprintf(stderr, "PS2_RASTER_PASS frame=%llu command=%zu prims=%u tiles=%u\n",
                     static_cast<unsigned long long>(frame_count()), g_commands.size(), prims, tiles);
    g_commands.push_back({sprite_batch ? kRasterSprite : kRaster, std::min(tiles, 1024u), (tiles + 1023) / 1024,
                          param, tile_pass, primitive_pass && !g_primitive_pass_compute ? prims : tiles,
                          primitive_pass && !g_primitive_pass_compute, primitive_pass && g_primitive_pass_matches == 1});
    const Range frame_pages = pages(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    const Range depth_pages = b.zte ? pages(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy) : Range{};
    if (CommandAccess *access = access_for_last_command()) {
        access->reads = g_batch_reads;
        access->reads.push_back(frame_pages);
        if (b.zte) access->reads.push_back(depth_pages);
        access->writes.push_back(frame_pages);
        if (b.zwrite) access->writes.push_back(depth_pages);
        if (snapshot) access->scratch.reads.push_back({0, static_cast<std::uint64_t>(snapshot) * 4});
        for (std::size_t s = 0; s < b.setups.size(); s += kSetupWords) {
            const std::uint32_t *setup = b.setups.data() + s;
            if (!(setup[0] & 2u)) continue;
            if (setup[24] != 0xFFFFFFFFu) {
                const std::uint64_t words = 1ull << ((setup[15] & 0xFFu) + (setup[15] >> 8));
                access->decoded.reads.push_back({static_cast<std::uint64_t>(setup[24]) * 4,
                                                   (static_cast<std::uint64_t>(setup[24]) + words) * 4});
            } else if (setup[14] == PSMT8 || setup[14] == PSMT8H || setup[14] == PSMT4 || setup[14] == PSMT4HL || setup[14] == PSMT4HH) {
                access->clut.reads.push_back({static_cast<std::uint64_t>(setup[22]) * kClutEntries * 4,
                                               static_cast<std::uint64_t>(setup[22] + 1) * kClutEntries * 4});
            }
        }
    }
    mark_rendered(frame_pages);
    diag_vram_write(frame_pages);
    if (b.zwrite) {
        mark_rendered(depth_pages);
        diag_vram_write(depth_pages);
    }
    b.prims.clear();
    b.setups.clear();
    b.boxes.clear();
    g_batch_reads.clear();
    g_batch_read_masks.clear();
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
    if (g_conflict_diag) {
        b.fb_mask = page_mask_region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
        b.z_mask = page_mask_region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
    }
}

bool batch_conflict(const PreOp &op) {
    Batch &b = g_batch;
    if (b.prims.empty()) return false;
    batch_ranges();
    const Range &r = op.range;
    if (!g_conflict_diag) {
        if (b.fb_range.overlaps(r.lo, r.hi)) return true;
        if (b.zte && b.z_range.overlaps(r.lo, r.hi)) return true;
        if (!op.clut)
            for (const Range &t : g_batch_reads)
                if (t.overlaps(r.lo, r.hi)) return true;
        return false;
    }
    bool coarse = b.fb_range.overlaps(r.lo, r.hi) || (b.zte && b.z_range.overlaps(r.lo, r.hi));
    if (!op.clut)
        for (const Range &t : g_batch_reads)
            coarse = coarse || t.overlaps(r.lo, r.hi);
    if (g_conflict_diag) {
        ++g_conflict_checks;
        const bool precise_fb = b.fb_mask.overlaps(op.mask);
        const bool precise_z = b.zte && b.z_mask.overlaps(op.mask);
        bool precise_tex = false;
        if (!op.clut)
            for (const PageMask &mask : g_batch_read_masks)
                precise_tex = precise_tex || mask.overlaps(op.mask);
        const bool precise = precise_fb || precise_z || precise_tex;
        g_conflict_coarse += coarse;
        g_conflict_precise += precise;
        g_conflict_false += coarse && !precise;
        g_conflict_missed += !coarse && precise;
    }
    return coarse;
}

void emit_pre() {
    if (g_reorder_pre) {
        // Keep upload order and CLUT order. An upload may cross only CLUT reads
        // of disjoint VRAM, so the resulting command stream is equivalent.
        for (std::size_t i = 1; i < g_pre.size(); ++i) {
            if (g_pre[i].clut) continue;
            std::size_t j = i;
            while (j && g_pre[j - 1].clut && !g_pre[j].range.overlaps(g_pre[j - 1].range.lo, g_pre[j - 1].range.hi)) {
                std::swap(g_pre[j], g_pre[j - 1]);
                --j;
                ++g_reordered_pre_ops;
            }
        }
    }
    std::size_t i = 0;
    while (i < g_pre.size()) {
        // One workgroup can order these small operations with storage barriers.
        // Larger uploads keep the existing multi-workgroup dispatch path.
        if (g_fuse_pre && (g_pre[i].clut || g_pre[i].rec[7] <= 256u)) {
            std::size_t j = i;
            while (j < g_pre.size() && j - i < 16 && (g_pre[j].clut || g_pre[j].rec[7] <= 256u)) ++j;
            const std::uint32_t count = static_cast<std::uint32_t>(j - i);
            std::uint32_t *records = g_arena.grow(count * 12u);
            const std::uint32_t offset = static_cast<std::uint32_t>(records - g_arena.data());
            for (std::size_t k = i; k < j; ++k) {
                std::memcpy(records + (k - i) * 12u, g_pre[k].rec, sizeof g_pre[k].rec);
                records[(k - i) * 12u + 11u] = g_pre[k].clut ? 1u : 0u;
            }
            g_commands.push_back({kPreBatch, 1, 1, push_params({offset, count})});
            if (CommandAccess *access = access_for_last_command())
                for (std::size_t k = i; k < j; ++k) {
                    (g_pre[k].clut ? access->reads : access->writes).push_back(g_pre[k].range);
                    if (g_pre[k].clut) {
                        access->clut.reads.push_back({static_cast<std::uint64_t>(g_pre[k].rec[0]) * kClutEntries * 4,
                                                      static_cast<std::uint64_t>(g_pre[k].rec[0] + 1) * kClutEntries * 4});
                        access->clut.writes.push_back({static_cast<std::uint64_t>(g_pre[k].rec[1]) * kClutEntries * 4,
                                                       static_cast<std::uint64_t>(g_pre[k].rec[1] + 1) * kClutEntries * 4});
                    }
                }
            i = j;
            continue;
        }
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
        if (CommandAccess *access = access_for_last_command())
            for (std::size_t k = i; k < j; ++k) {
                (clut ? access->reads : access->writes).push_back(g_pre[k].range);
                if (clut) {
                    access->clut.reads.push_back({static_cast<std::uint64_t>(g_pre[k].rec[0]) * kClutEntries * 4,
                                                  static_cast<std::uint64_t>(g_pre[k].rec[0] + 1) * kClutEntries * 4});
                    access->clut.writes.push_back({static_cast<std::uint64_t>(g_pre[k].rec[1]) * kClutEntries * 4,
                                                   static_cast<std::uint64_t>(g_pre[k].rec[1] + 1) * kClutEntries * 4});
                }
            }
        i = j;
    }
    g_pre.clear();
}

void emit_all() {
    emit_pre();
    flush_batch();
}

void queue_pre(const PreOp &op) {
    if (batch_conflict(op)) emit_all();
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
    std::memcpy(op.rec, rec, sizeof rec);
    const std::uint32_t first = im.done / im.rrw, last = (im.done + pixels - 1) / im.rrw;
    op.range = region(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay + first), static_cast<int>(im.dsax + im.rrw - 1),
                      static_cast<int>(im.dsay + last));
    op.x0 = im.dsax;
    op.x1 = im.dsax + im.rrw - 1;
    op.y0 = im.dsay + first;
    op.y1 = im.dsay + last;
    if (g_conflict_diag) op.mask = page_mask_region(im.dbp, im.dbw, im.dpsm, op.x0, op.y0, op.x1, op.y1);
    op.image = im.id;
    queue_pre(op);
    if (g_upload_diag) {
        std::string signature(reinterpret_cast<const char *>(rec), 8 * sizeof(std::uint32_t));
        signature.append(reinterpret_cast<const char *>(&lead_bits), sizeof lead_bits);
        signature.append(reinterpret_cast<const char *>(&im.bpp), sizeof im.bpp);
        signature.append(reinterpret_cast<const char *>(bytes), n);
        ++g_upload_diag_total;
        auto [it, inserted] = g_upload_signatures.try_emplace(std::move(signature));
        if (!inserted) ++g_upload_diag_repeated;
        bool safe = !inserted;
        if (safe) {
            for (const auto &[page, epoch] : it->second)
                if (g_vram_epochs[page] != epoch) { safe = false; break; }
        }
        if (safe) {
            ++g_upload_diag_safe;
        } else {
            diag_vram_write(op.range);
            it->second.clear();
            for_vram_page(op.range, [&](std::uint16_t page) { it->second.emplace_back(page, g_vram_epochs[page]); });
        }
    } else if (g_texture_diag || g_decode_cache) {
        diag_vram_write(op.range);
    }
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
        for (std::uint32_t px = (im.dsax + pw - 1) / pw; px < (im.dsax + im.rrw) / pw; ++px)
            g_rendered[((im.dbp * 256 + (px + py * per_row) * 8192) >> 13) & 511u] = false;
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

void write_staged(WGPUBuffer buffer, const void *source, std::size_t bytes) {
    if (!bytes) return;
    if (!g_chunk_writes) {
        wgpuQueueWriteBuffer(g_queue, buffer, 0, source, bytes);
        return;
    }
    constexpr std::size_t kChunk = 1u << 20;
    const auto *data = static_cast<const std::uint8_t *>(source);
    for (std::size_t offset = 0; offset < bytes; offset += kChunk)
        wgpuQueueWriteBuffer(g_queue, buffer, offset, data + offset, std::min(kChunk, bytes - offset));
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
    write_staged(g_data, g_arena.data(), g_arena.size() * 4);
    write_staged(g_params, g_param_records.data(), g_param_records.size() * kParamStride);
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
        std::uint64_t pipeline_ns[kPipelines] = {};
        std::uint32_t pipeline_commands[kPipelines] = {};
        struct RasterCost { std::uint64_t ns = 0; std::uint32_t command = 0, prims = 0, tiles = 0; };
        std::array<RasterCost, 5> top_rasters = {};
        for (std::uint32_t i = 0; t && i < n; ++i) {
            while (info < g_command_info.size() && g_command_info[info].index < i) ++info;
            const std::uint64_t elapsed_ns = t[i * 2 + 1] > t[i * 2] ? t[i * 2 + 1] - t[i * 2] : 0;
            pipeline_ns[g_commands[i].pipeline] += elapsed_ns;
            ++pipeline_commands[g_commands[i].pipeline];
            if ((g_commands[i].pipeline == kRaster || g_commands[i].pipeline == kRasterSprite) && elapsed_ns > top_rasters.back().ns) {
                RasterCost candidate{elapsed_ns, i};
                if (info < g_command_info.size() && g_command_info[info].index == i) {
                    candidate.prims = g_command_info[info].values[0];
                    candidate.tiles = g_command_info[info].values[1];
                }
                for (std::size_t position = 0; position < top_rasters.size(); ++position) {
                    if (candidate.ns > top_rasters[position].ns) {
                        for (std::size_t shift = top_rasters.size() - 1; shift > position; --shift)
                            top_rasters[shift] = top_rasters[shift - 1];
                        top_rasters[position] = candidate;
                        break;
                    }
                }
            }
            std::fprintf(g_profile_out, "%llu %u %s %llu %u %u", static_cast<unsigned long long>(frame_count()), i, kEntryPoints[g_commands[i].pipeline],
                         static_cast<unsigned long long>(elapsed_ns), g_commands[i].x, g_commands[i].y);
            if (info < g_command_info.size() && g_command_info[info].index == i)
                for (std::uint32_t v : g_command_info[info].values) std::fprintf(g_profile_out, " %u", v);
            std::fprintf(g_profile_out, "\n");
        }
#ifdef __EMSCRIPTEN__
        if (t) {
            std::fprintf(stderr, "PS2_GPU_PROFILE_SUMMARY frame=%llu commands=%u", static_cast<unsigned long long>(frame_count()), n);
            for (int i = 0; i < kPipelines; ++i)
                if (pipeline_commands[i]) std::fprintf(stderr, " %s=%u:%.3fms", kEntryPoints[i], pipeline_commands[i], pipeline_ns[i] / 1e6);
            std::fputc('\n', stderr);
            for (const RasterCost &cost : top_rasters)
                if (cost.ns) std::fprintf(stderr, "PS2_GPU_PROFILE_TOP frame=%llu command=%u raster_ms=%.3f prims=%u tiles=%u\n",
                                          static_cast<unsigned long long>(frame_count()), cost.command, cost.ns / 1e6, cost.prims, cost.tiles);
        }
#endif
        wgpuBufferUnmap(g_profile_read);
    }
    g_commands.clear();
    g_command_info.clear();
    g_command_access.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_decoded_used = 0;
    g_decode_offsets.clear();
    g_clut_loads = 0;
}

// Browsers copy writes above about 1 MiB through a slower path, so large uploads go in pieces.
void submit(WGPUTextureView present_view = nullptr) {
    if (g_commands.empty() && !present_view) return;
    if (g_profile_out && !g_render_tile_pass && !g_primitive_pass && !g_commands.empty() && g_timestamps && frame_count() >= g_profile_from && frame_count() < g_profile_to) {
        profile_submit();
        if (!present_view) return;
    }
    const auto cpu_start = std::chrono::steady_clock::now();
    ensure_capacity();
    const auto cpu_ensured = std::chrono::steady_clock::now();
    write_staged(g_data, g_arena.data(), g_arena.size() * 4);
    const auto cpu_data_written = std::chrono::steady_clock::now();
    write_staged(g_params, g_param_records.data(), g_param_records.size() * kParamStride);
    const auto cpu_written = std::chrono::steady_clock::now();
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
    const bool measure = g_timestamps && !g_query_pending && !g_render_tile_pass && !g_primitive_pass;
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
        // Diagnostic upper bound only: the picture is intentionally invalid.
        if (g_skip_raster && (c.pipeline == kRaster || c.pipeline == kRasterSprite)) continue;
        if (c.primitive_pass && g_primitive_pass_skip) {
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
#ifndef __EMSCRIPTEN__
            if (c.primitive_probe) wgpuCommandEncoderCopyBufferToBuffer(enc, g_vram, 0, g_primitive_probe, 0, kMemorySize);
#endif
            pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
            current = kPipelines;
            continue;
        }
        if (c.tile_pass || c.primitive_pass) {
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = g_tile_view;
            color.loadOp = WGPULoadOp_Load;
            color.storeOp = WGPUStoreOp_Discard;
            color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
            WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rd.colorAttachmentCount = 1;
            rd.colorAttachments = &color;
            WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
            wgpuRenderPassEncoderSetPipeline(rp, c.primitive_pass ? g_primitive_pipeline : g_tile_pipeline);
            const std::uint32_t offset = c.param * kParamStride;
            wgpuRenderPassEncoderSetBindGroup(rp, 0, g_group, 1, &offset);
            wgpuRenderPassEncoderDraw(rp, 6, c.instances, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
#ifndef __EMSCRIPTEN__
            if (c.primitive_probe) wgpuCommandEncoderCopyBufferToBuffer(enc, g_vram, 0, g_primitive_probe, 0, kMemorySize);
#endif
            pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
            current = kPipelines;
            ++g_dispatches[c.pipeline];
            continue;
        }
        ++g_dispatches[c.pipeline];
        if (c.pipeline != current) {
            wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
            current = c.pipeline;
        }
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
#ifndef __EMSCRIPTEN__
        if (c.primitive_probe) {
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            wgpuCommandEncoderCopyBufferToBuffer(enc, g_vram, 0, g_primitive_probe, 0, kMemorySize);
            pass = wgpuCommandEncoderBeginComputePass(enc, &cd);
            current = kPipelines;
        }
#endif
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    if (measure) {
        wgpuCommandEncoderResolveQuerySet(enc, g_queries, 0, 2, g_query_resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, g_query_resolve, 0, g_query_read, 0, 16);
    }
    if (present_view) {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = present_view;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
        WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rd.colorAttachmentCount = 1;
        rd.colorAttachments = &color;
        WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
        const float w = static_cast<float>(g_surface_width), h = static_cast<float>(g_surface_height);
        const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(w, h * aspect), vh = vw / aspect;
        wgpuRenderPassEncoderSetViewport(rp, (w - vw) * 0.5f, (h - vh) * 0.5f, vw, vh, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetPipeline(rp, g_blit);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, g_blit_group, 0, nullptr);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
    }
    const auto cpu_encoded = std::chrono::steady_clock::now();
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    const auto cpu_queued = std::chrono::steady_clock::now();
#ifndef __EMSCRIPTEN__
    if (g_primitive_probe && std::any_of(g_commands.begin(), g_commands.end(), [](const Command &c) { return c.primitive_probe; })) {
        bool done = false;
        WGPUBufferMapCallbackInfo probe_cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        probe_cb.mode = WGPUCallbackMode_AllowProcessEvents;
        probe_cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *flag, void *) {
            if (status != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "PS2_PRIMITIVE_PROBE map failed %d\n", static_cast<int>(status));
            *static_cast<bool *>(flag) = true;
        };
        probe_cb.userdata1 = &done;
        wgpuBufferMapAsync(g_primitive_probe, WGPUMapMode_Read, 0, kMemorySize, probe_cb);
        wait_until(done);
        const auto *bytes = static_cast<const std::uint8_t *>(wgpuBufferGetConstMappedRange(g_primitive_probe, 0, kMemorySize));
        if (bytes) {
            std::uint64_t hash = 14695981039346656037ull;
            for (std::size_t i = 0; i < kMemorySize; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
            std::fprintf(stderr, "PS2_PRIMITIVE_PROBE mode=%s hash=%016llx\n",
                         g_primitive_pass_skip ? "skip" : g_primitive_pass_compute ? "compute" : "direct",
                         static_cast<unsigned long long>(hash));
        }
        wgpuBufferUnmap(g_primitive_probe);
    }
#endif
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
    if (g_cpu_diag) {
        const auto cpu_polled = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        ++g_cpu_timing.count;
        g_cpu_timing.commands += g_commands.size();
        g_cpu_timing.data_bytes += g_arena.size() * 4;
        g_cpu_timing.param_bytes += g_param_records.size() * kParamStride;
        g_cpu_timing.ensure_ms += milliseconds(cpu_start, cpu_ensured);
        g_cpu_timing.data_ms += milliseconds(cpu_ensured, cpu_data_written);
        g_cpu_timing.params_ms += milliseconds(cpu_data_written, cpu_written);
        g_cpu_timing.encode_ms += milliseconds(cpu_written, cpu_encoded);
        g_cpu_timing.queue_ms += milliseconds(cpu_encoded, cpu_queued);
        g_cpu_timing.poll_ms += milliseconds(cpu_queued, cpu_polled);
        if (g_cpu_timing.count == 20) {
            std::fprintf(stderr, "[gpu-cpu] 20 submits, %.1f commands, data %.2f MiB, params %.2f MiB, ensure %.2f ms, data-write %.2f ms, params-write %.2f ms, encode %.2f ms, queue %.2f ms, poll %.2f ms (per submit)\n",
                         static_cast<double>(g_cpu_timing.commands) / g_cpu_timing.count,
                         static_cast<double>(g_cpu_timing.data_bytes) / g_cpu_timing.count / (1024 * 1024),
                         static_cast<double>(g_cpu_timing.param_bytes) / g_cpu_timing.count / (1024 * 1024),
                         g_cpu_timing.ensure_ms / g_cpu_timing.count, g_cpu_timing.data_ms / g_cpu_timing.count,
                         g_cpu_timing.params_ms / g_cpu_timing.count, g_cpu_timing.encode_ms / g_cpu_timing.count,
                         g_cpu_timing.queue_ms / g_cpu_timing.count, g_cpu_timing.poll_ms / g_cpu_timing.count);
            g_cpu_timing = {};
        }
    }
    dump_access_profile();
    g_commands.clear();
    g_command_info.clear();
    g_command_access.clear();
    ++g_serial;
    g_param_records.clear();
    g_arena.clear();
    g_decoded_used = 0;
    g_decode_offsets.clear();
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

bool init(Mode m) {
    if (m == Mode::off) return false;
#ifdef __EMSCRIPTEN__
    g_instance = wgpuCreateInstance(nullptr);
#else
    WGPUInstanceExtras extras = {};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = WGPUInstanceBackend_Primary;
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
    g_timestamps = wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery);
    if (const char *timestamps = std::getenv("PS2_GPU_TIMESTAMPS")) g_timestamps = g_timestamps && std::atoi(timestamps) != 0;
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
    if (const char *spec = std::getenv("PS2_GPU_ACCESS_PROFILE")) {
        char path[512] = {};
        unsigned long long a = 0, z = 0;
        if (std::sscanf(spec, "%llu,%llu,%511s", &a, &z, path) == 3) {
            g_access_out = std::fopen(path, "w");
            g_access_from = a;
            g_access_to = z;
        }
    }
    if (const char *spec = std::getenv("PS2_GPU_FASTPATH_TRACE")) {
        char path[512] = {};
        unsigned long long a = 0, z = 0;
        if (std::sscanf(spec, "%llu,%llu,%511s", &a, &z, path) == 3) {
            g_fastpath_trace = std::fopen(path, "w");
            g_fastpath_trace_from = a;
            g_fastpath_trace_to = z;
            if (g_fastpath_trace)
                std::fputs("frame,command,prims,tiles,tile_entries,snapshot,fbpsm,zpsm,fbp,zbp,zte,zwrite,minx,miny,maxx,maxy,broad_prims,textured_prims,flags_or,flags_and,blend_mode,ztest_mode,tex_psm,tex_width,tex_height\n", g_fastpath_trace);
        }
    }
    if (const char *spec = std::getenv("PS2_GPU_PRIM_TRACE")) {
        char path[512] = {};
        unsigned long long frame = 0;
        std::size_t command = 0;
        if (std::sscanf(spec, "%llu,%zu,%511s", &frame, &command, path) == 3) {
            g_prim_trace = std::fopen(path, "w");
            g_prim_trace_frame = frame;
            g_prim_trace_command = command;
            if (g_prim_trace)
                std::fputs("frame,command,primitive,type,flags,fbpsm,zpsm,fbmsk,blend,ztest,zte,zwrite,ate,atst,afail,texpsm,texw,texh,scax0,scay0,scax1,scay1\n", g_prim_trace);
        }
    }
    if (const char *scale = std::getenv("PS2_SCALE")) g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 3u);
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
    g_upload_diag = std::getenv("PS2_GPU_UPLOAD_DIAG") != nullptr;
    g_texture_diag = std::getenv("PS2_GPU_TEXTURE_DIAG") != nullptr;
    g_decode_cache = std::getenv("PS2_DECODE_CACHE") != nullptr;
    g_reorder_pre = std::getenv("PS2_REORDER_PRE") != nullptr;
    g_fuse_pre = std::getenv("PS2_FUSE_PRE") != nullptr;
    g_conflict_diag = std::getenv("PS2_GPU_CONFLICT_DIAG") != nullptr;
    g_cpu_diag = std::getenv("PS2_GPU_CPU_DIAG") != nullptr;
    g_chunk_writes = std::getenv("PS2_CHUNK_WRITES") != nullptr;
    g_fastpath_diag = std::getenv("PS2_GPU_FASTPATH_DIAG") != nullptr;
    g_split_fastpath = std::getenv("PS2_FASTPATH_SPLIT") != nullptr;
    g_sprite_fast = std::getenv("PS2_SPRITE_FAST") != nullptr;
    g_skip_raster = std::getenv("PS2_GPU_SKIP_RASTER") != nullptr;
    if (const char *spec = std::getenv("PS2_PRIMITIVE_PASS")) {
        g_primitive_pass = true;
        g_primitive_pass_skip = std::strcmp(spec, "skip") == 0 || std::strcmp(spec, "all-skip") == 0;
        g_primitive_pass_compute = std::strcmp(spec, "compute") == 0 || std::strcmp(spec, "all-compute") == 0;
        g_primitive_pass_all = std::strncmp(spec, "all", 3) == 0;
    }
    if (const char *spec = std::getenv("PS2_RASTER_PASS")) {
        unsigned long long frame = 0;
        std::size_t command = 0;
        if (std::strcmp(spec, "all") == 0) {
            g_render_tile_pass = true;
            g_render_tile_all = true;
        } else if (std::sscanf(spec, "%llu,%zu", &frame, &command) == 2) {
            g_render_tile_pass = true;
            g_render_tile_frame = frame;
            g_render_tile_command = command;
        }
    }
    g_cache_copy.assign(kCacheWords, 0);
    g_readback = make_buffer(static_cast<std::uint64_t>(g_copy_words) * 4, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
#ifndef __EMSCRIPTEN__
    if (g_primitive_pass) g_primitive_probe = make_buffer(kMemorySize, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
#endif
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
        if (g_render_tile_pass || g_primitive_pass) {
            le[i].visibility |= WGPUShaderStage_Fragment;
            if (i == 2 || i == 3) le[i].visibility |= WGPUShaderStage_Vertex;
        }
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

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kGsShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    for (int i = 0; i < kPipelines; ++i) {
        if (i == kRasterSprite && !g_sprite_fast) continue;
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
        if (!g_pipelines[i]) return false;
    }
    if (g_render_tile_pass || g_primitive_pass) {
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = WGPUTextureFormat_RGBA8Unorm;
        color.writeMask = 0;
        WGPUConstantEntry constant = WGPU_CONSTANT_ENTRY_INIT;
        constant.key = view("SCALE");
        constant.value = static_cast<double>(g_scale);
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module;
        fragment.entryPoint = view(g_render_tile_pass ? "raster_fs" : "primitive_fs");
        fragment.constantCount = 1;
        fragment.constants = &constant;
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rd.layout = g_pipeline_layout;
        rd.vertex.module = module;
        rd.vertex.entryPoint = view(g_render_tile_pass ? "raster_vs" : "primitive_vs");
        rd.vertex.constantCount = 1;
        rd.vertex.constants = &constant;
        rd.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(g_device, &rd);
        if (!pipeline) return false;
        if (g_render_tile_pass) g_tile_pipeline = pipeline;
        else g_primitive_pipeline = pipeline;
        if (g_render_tile_pass && g_primitive_pass) {
            fragment.entryPoint = view("primitive_fs");
            rd.vertex.entryPoint = view("primitive_vs");
            g_primitive_pipeline = wgpuDeviceCreateRenderPipeline(g_device, &rd);
            if (!g_primitive_pipeline) return false;
        }
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.dimension = WGPUTextureDimension_2D;
        td.size = WGPUExtent3D{2048u * g_scale, 2048u * g_scale, 1u};
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.usage = WGPUTextureUsage_RenderAttachment;
        g_tile_texture = wgpuDeviceCreateTexture(g_device, &td);
        if (!g_tile_texture) return false;
        g_tile_view = wgpuTextureCreateView(g_tile_texture, nullptr);
        if (!g_tile_view) return false;
    }
    wgpuShaderModuleRelease(module);
    ensure_capacity();
    g_mode = m;
    return true;
}

void draw(const State &st, const Vertex *v, int count) {
    if (g_image.active && !g_image.pending.empty()) flush_image();
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
            if (g_conflict_diag) cache.tex_mask = page_mask_region(w[12], w[13], w[14], 0, 0, tex_w - 1, tex_h - 1);
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
    const float samples = static_cast<float>(g_scale);
    const auto first = [&](float lo) { return static_cast<int>(std::floor(std::ceil(lo * samples) / samples)); };

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
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        minx = std::max(scax0, static_cast<int>(std::ceil(std::min(x0, x1))));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) - 1);
        miny = std::max(scay0, static_cast<int>(std::ceil(std::min(y0, y1))));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) - 1);
        rec[1] = 1;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1 - x0);
        rec[7] = f2u(y1 - y0);
        attrs(rec, 0, v[0], fst, nullptr);
        attrs(rec, 1, v[1], fst, nullptr);
        rec[24] = v[1].z;
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

    Batch &b = g_batch;
    if (g_split_fastpath && !b.prims.empty()) {
        const std::uint32_t *first = b.setups.data() + b.prims[0] * kSetupWords;
        const auto candidate = [](const std::uint32_t *w, std::uint32_t type) {
            const std::uint32_t flags = w[0];
            return (flags & 2u) && (w[3] == PSMCT32 || w[3] == PSMCT24) && !(flags & 128u) &&
                   (!(flags & 64u) || !((w[7] >> 4) & 3u)) && !w[4] && type <= 1u;
        };
        if (candidate(first, b.prims[1]) != candidate(setup, rec[1])) {
            emit_all();
            ++g_fastpath_splits;
        }
    }
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
            if (!known) {
                g_batch_reads.push_back(tex);
                if (g_conflict_diag) g_batch_read_masks.push_back(cache.tex_mask);
            }
        }
    }
    rec[0] = setup_index;
    rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
    rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
    b.prims.insert(b.prims.end(), rec, rec + kPrimWords);
    b.boxes.push_back(rec[2]);
    b.boxes.push_back(rec[3]);
    b.minx = std::min(b.minx, minx);
    b.miny = std::min(b.miny, miny);
    b.maxx = std::max(b.maxx, maxx);
    b.maxy = std::max(b.maxy, maxy);
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
    const Range copied_pages = pages(static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                     static_cast<int>(pos >> 32) & 0x7FF, static_cast<int>(pos >> 48) & 0x7FF, (static_cast<int>(pos >> 32) & 0x7FF) + static_cast<int>(w) - 1,
                                     (static_cast<int>(pos >> 48) & 0x7FF) + static_cast<int>(h) - 1);
    const Range source_pages = pages(static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                                     static_cast<int>(pos) & 0x7FF, static_cast<int>(pos >> 16) & 0x7FF, (static_cast<int>(pos) & 0x7FF) + static_cast<int>(w) - 1,
                                     (static_cast<int>(pos >> 16) & 0x7FF) + static_cast<int>(h) - 1);
    mark_rendered(copied_pages);
    diag_vram_write(copied_pages);
    const std::uint32_t read = push_params({static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                                            static_cast<std::uint32_t>(pos) & 0x7FFu, static_cast<std::uint32_t>(pos >> 16) & 0x7FFu, w, h});
    const std::uint32_t write = push_params({static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                             static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, static_cast<std::uint32_t>(pos >> 48) & 0x7FFu, w, h,
                                             static_cast<std::uint32_t>(pos >> 59) & 3u});
    dispatch_linear(kCopyRead, w * h * g_scale * g_scale, read);
    if (CommandAccess *access = access_for_last_command()) {
        access->reads.push_back(source_pages);
        access->scratch.writes.push_back({0, static_cast<std::uint64_t>(w) * h * g_scale * g_scale * 4});
    }
    dispatch_linear(kCopyWrite, w * h * g_scale * g_scale, write);
    if (CommandAccess *access = access_for_last_command()) {
        access->scratch.reads.push_back({0, static_cast<std::uint64_t>(w) * h * g_scale * g_scale * 4});
        access->writes.push_back(copied_pages);
    }
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
    if (g_conflict_diag)
        op.mask = csm ? page_mask_region(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov))
                      : page_mask_region(cbp, 1, cpsm, 0, 0, 15, 15);
    queue_pre(op);
    g_clut_slot = next;
    if (g_texture_diag || g_decode_cache) ++g_clut_versions[next];
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
    if (g_scale != 1) {
        dispatch_linear(kResolve, bytes / 4, push_params({offset / 4}));
        if (CommandAccess *access = access_for_last_command()) {
            access->reads.push_back({offset, static_cast<std::uint64_t>(offset) + bytes});
            access->scratch.writes.push_back({offset, static_cast<std::uint64_t>(offset) + bytes});
        }
    }
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
    if (CommandAccess *access = access_for_last_command()) {
        access->reads.push_back({offset, static_cast<std::uint64_t>(offset) + bytes});
        access->scratch.writes.push_back({offset, static_cast<std::uint64_t>(offset) + bytes});
    }
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
    g_upload_signatures.clear();
    g_decode_seen.clear();
    g_decode_offsets.clear();
    g_vram_epochs.fill(0);
    g_clut_versions.fill(0);
    g_vram_epoch = g_upload_diag_total = g_upload_diag_repeated = g_upload_diag_safe = 0;
    const std::size_t words = kMemorySize / 4;
    const std::uint32_t from = scale * scale, to = g_scale * g_scale;
    std::fill(std::begin(g_rendered), std::end(g_rendered), from == to && to > 1);
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

void stats() {
    if (g_mode == Mode::off) return;
    std::fprintf(stderr, "[gpu] %llu submits, dispatches:", static_cast<unsigned long long>(g_submits));
    for (int i = 0; i < kPipelines; ++i) std::fprintf(stderr, " %s %llu", kEntryPoints[i], static_cast<unsigned long long>(g_dispatches[i]));
    std::fprintf(stderr, "\n");
    if (g_upload_diag)
        std::fprintf(stderr, "[gpu] upload signatures: %llu total, %llu repeated, %llu unchanged VRAM pages (conservative candidates)\n",
                     static_cast<unsigned long long>(g_upload_diag_total), static_cast<unsigned long long>(g_upload_diag_repeated),
                     static_cast<unsigned long long>(g_upload_diag_safe));
    if (g_texture_diag)
        std::fprintf(stderr, "[gpu] decode shadow cache: %llu candidates, %llu unchanged-page-and-clut hits, %llu words, %llu hit words\n",
                     static_cast<unsigned long long>(g_decode_diag_total), static_cast<unsigned long long>(g_decode_diag_hits),
                     static_cast<unsigned long long>(g_decode_diag_words), static_cast<unsigned long long>(g_decode_diag_hit_words));
    if (g_decode_cache)
        std::fprintf(stderr, "[gpu] within-submit decode cache: %llu hits, %llu hit words\n",
                     static_cast<unsigned long long>(g_decode_cache_hits), static_cast<unsigned long long>(g_decode_cache_hit_words));
    if (g_reorder_pre) std::fprintf(stderr, "[gpu] reordered pre-op pairs: %llu\n", static_cast<unsigned long long>(g_reordered_pre_ops));
    if (g_conflict_diag)
        std::fprintf(stderr, "[gpu] pre-op conflicts: checks %llu coarse %llu page-mask %llu coarse-only %llu mask-only %llu\n",
                     static_cast<unsigned long long>(g_conflict_checks), static_cast<unsigned long long>(g_conflict_coarse),
                     static_cast<unsigned long long>(g_conflict_precise), static_cast<unsigned long long>(g_conflict_false),
                     static_cast<unsigned long long>(g_conflict_missed));
    if (g_fastpath_diag)
        std::fprintf(stderr, "[gpu] fastpath primitives: total %llu no-snapshot %llu standard-frame %llu no-blend-date %llu alpha-keep %llu no-fb-mask %llu strict-triangle-sprite %llu textured-strict %llu\n",
                     static_cast<unsigned long long>(g_fastpath_stats.total),
                     static_cast<unsigned long long>(g_fastpath_stats.no_snapshot),
                     static_cast<unsigned long long>(g_fastpath_stats.standard_frame),
                     static_cast<unsigned long long>(g_fastpath_stats.no_blend_or_date),
                     static_cast<unsigned long long>(g_fastpath_stats.alpha_keep),
                     static_cast<unsigned long long>(g_fastpath_stats.no_fb_mask),
                     static_cast<unsigned long long>(g_fastpath_stats.strict),
                     static_cast<unsigned long long>(g_fastpath_stats.textured_strict));
    if (g_fastpath_diag) {
        std::fprintf(stderr, "[gpu] broad graphics candidates: %llu total, %llu textured, %llu with alpha blend 44, %llu with identity blend 0a/2a\n",
                     static_cast<unsigned long long>(g_fastpath_stats.broad),
                     static_cast<unsigned long long>(g_fastpath_stats.textured_broad),
                     static_cast<unsigned long long>(g_fastpath_stats.standard_alpha_blend),
                     static_cast<unsigned long long>(g_fastpath_stats.identity_blend));
        std::fprintf(stderr, "[gpu] broad whole batches: %llu/%llu, %llu primitives; without Z test %llu batches, %llu primitives\n",
                     static_cast<unsigned long long>(g_fastpath_stats.broad_batches),
                     static_cast<unsigned long long>(g_fastpath_stats.batches),
                     static_cast<unsigned long long>(g_fastpath_stats.broad_batch_prims),
                     static_cast<unsigned long long>(g_fastpath_stats.broad_no_z_batches),
                     static_cast<unsigned long long>(g_fastpath_stats.broad_no_z_prims));
        std::fprintf(stderr, "[gpu] alpha-test fail modes:");
        for (std::size_t i = 0; i < g_fastpath_stats.alpha_fail_modes.size(); ++i)
            std::fprintf(stderr, " %zu=%llu", i, static_cast<unsigned long long>(g_fastpath_stats.alpha_fail_modes[i]));
        std::fprintf(stderr, "\n[gpu] blend equation modes:");
        for (std::size_t i = 0; i < g_fastpath_stats.blend_modes.size(); ++i)
            if (g_fastpath_stats.blend_modes[i])
                std::fprintf(stderr, " %02zx=%llu", i, static_cast<unsigned long long>(g_fastpath_stats.blend_modes[i]));
        std::fprintf(stderr, "\n");
    }
    if (g_split_fastpath)
        std::fprintf(stderr, "[gpu] fastpath candidate boundaries: %llu\n", static_cast<unsigned long long>(g_fastpath_splits));
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

bool attach_surface(const SurfaceTarget &target) {
    if (g_mode != Mode::primary || !g_device) return false;
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = view(target.selector ? target.selector : "#canvas");
    sd.nextInChain = &canvas.chain;
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
    g_present_mode = WGPUPresentMode_Fifo;
    for (const WGPUPresentMode wanted : {WGPUPresentMode_Immediate, WGPUPresentMode_Mailbox})
        for (std::size_t i = 0; i < caps.presentModeCount; ++i)
            if (caps.presentModes[i] == wanted) g_present_mode = wanted;
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    g_surface_width = target.width;
    g_surface_height = target.height;
    configure_surface();

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kBlitShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    WGPUBindGroupLayoutEntry le[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Fragment;
    le[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Fragment;
    le[1].buffer.type = WGPUBufferBindingType_Uniform;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2;
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
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pipeline_layout);
    g_blit_params = make_buffer(32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry be[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    be[0].binding = 0;
    be[0].buffer = g_scratch;
    be[0].size = WGPU_WHOLE_SIZE;
    be[1].binding = 1;
    be[1].buffer = g_blit_params;
    be[1].size = 32;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = layout;
    bd.entryCount = 2;
    bd.entries = be;
    g_blit_group = wgpuDeviceCreateBindGroup(g_device, &bd);
    wgpuBindGroupLayoutRelease(layout);
    return g_blit != nullptr;
}

void resize_surface(std::uint32_t width, std::uint32_t height) {
    if (!g_surface || (width == g_surface_width && height == g_surface_height)) return;
    g_surface_width = width;
    g_surface_height = height;
    configure_surface();
}

bool has_surface() { return g_surface != nullptr; }

void note_display_access(const DisplaySource &src, std::uint32_t offset_words) {
    if (CommandAccess *access = access_for_last_command()) {
        access->reads.push_back(pages(src.fbp, src.fbw, src.psm, src.dbx, src.dby,
                                      src.dbx + static_cast<int>(src.width) - 1,
                                      src.dby + static_cast<int>(src.height) - 1));
        const std::uint64_t lo = static_cast<std::uint64_t>(offset_words) * 4;
        const std::uint64_t bytes = static_cast<std::uint64_t>(src.width) * src.height * g_scale * g_scale * 4;
        access->scratch.writes.push_back({lo, lo + bytes});
    }
}

bool present(const DisplaySource &src) {
    if (!g_surface) return false;
    flush_all();
    const std::uint32_t current = field_base(g_field), previous = field_base(g_field + 1);
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    const bool blend = src.halved && g_have_previous && g_last_width == src.width && g_last_height == src.height && !weave_off;
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u, current});
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    note_display_access(src, current);
    const float sw = static_cast<float>(g_surface_width), sh = static_cast<float>(g_surface_height);
    const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float vw = std::min(sw, sh * aspect), vh = vw / aspect;
    const std::uint32_t blit[8] = {src.width * g_scale, src.height * g_scale, current, previous, blend ? 1u : 0u, static_cast<std::uint32_t>(vw), static_cast<std::uint32_t>(vh), 0};
    wgpuQueueWriteBuffer(g_queue, g_blit_params, 0, blit, sizeof blit);
    ++g_field;
    g_have_previous = true;
    g_last_width = src.width;
    g_last_height = src.height;
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        submit();
        configure_surface();
        return true;
    }
    WGPUTextureView target = wgpuTextureCreateView(st.texture, nullptr);
    submit(target);
#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(g_surface);
#endif
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(st.texture);
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
        const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u});
        g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
        note_display_access(src, 0);
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
    flush_all();
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u});
    g_commands.push_back({kDisplay, (src.width * g_scale + 7) / 8, (src.height * g_scale + 7) / 8, param});
    note_display_access(src, 0);
    submit();
    width = src.width * g_scale;
    height = src.height * g_scale;
    rgba.resize(static_cast<std::size_t>(width) * height);
    read_back(g_scratch, rgba.size() * 4, rgba.data());
    return true;
}

}
