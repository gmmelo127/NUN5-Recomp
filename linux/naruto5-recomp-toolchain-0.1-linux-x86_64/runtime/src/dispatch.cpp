#include "kernel.hpp"

#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <bit>
#include <cstdlib>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace ps2 {

namespace {

std::vector<UnitInfo> g_units;
std::vector<const UnitInfo *> g_index;
std::uint32_t g_origin = 0;
std::uint32_t g_unit_size = 1;
unsigned g_unit_shift = 0;
bool g_unit_pow2 = true;
std::set<std::uint32_t> g_misses;
std::set<std::string> g_module_misses;
std::uint32_t g_trace_last_pc = 0;
bool g_idle_reported = false;

// Debug hooks are checked on every dispatched block; cache the environment
// lookups once instead of scanning the environment thousands of times a frame.
const bool g_trace_scene = std::getenv("PS2_TRACE_SCENE") != nullptr;
const bool g_trace_mode_state = std::getenv("PS2_TRACE_MODE_STATE") != nullptr;
const bool g_trace_mode = std::getenv("PS2_TRACE_MODE") != nullptr;
const bool g_trace_loader = std::getenv("PS2_TRACE_LOADER") != nullptr;
const bool g_trace_window = std::getenv("PS2_TRACE_WINDOW") != nullptr;
const bool g_debug_sched = std::getenv("PS2_DEBUG_SCHED") != nullptr;

struct PcTrace {
    bool on = false;
    int thread = 0;
    unsigned long long from = 0, to = 0;
};

const PcTrace &pc_trace() {
    static const PcTrace t = [] {
        PcTrace x;
        const char *th = std::getenv("PS2_TRACE_THREAD");
        const char *fr = std::getenv("PS2_TRACE_FROM");
        const char *to = std::getenv("PS2_TRACE_TO");
        if (th && fr && to) {
            x.thread = std::atoi(th);
            x.from = std::strtoull(fr, nullptr, 10);
            x.to = std::strtoull(to, nullptr, 10);
            x.on = true;
        }
        return x;
    }();
    return t;
}
std::uint32_t g_pc_ring[32];
unsigned g_pc_ring_at = 0;

const std::set<std::uint32_t> &trace_at() {
    static const std::set<std::uint32_t> s = [] {
        std::set<std::uint32_t> x;
        if (const char *p = std::getenv("PS2_TRACE_AT")) {
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s", p);
            for (char *t = std::strtok(buf, ","); t; t = std::strtok(nullptr, ","))
                x.insert(static_cast<std::uint32_t>(std::strtoul(t, nullptr, 16)));
        }
        return x;
    }();
    return s;
}

const bool g_trace_at_on = !trace_at().empty();
const bool g_pc_trace_on = pc_trace().on;

constexpr std::uint32_t kModuleMagic = 0x32524E53u;

struct Instance {
    std::uint32_t base;
    const ModuleInfo *module;
    // Fixed-base code windows are shared by several overlays (BTL, ADV and ETC
    // all load at 0x006C6D00), so the base alone does not say which image is in
    // place. The module header carries the version (base+4) and the product
    // name (base+0x20); keep both and compare them before serving a cached
    // instance.
    std::uint32_t header_version;
    std::uint32_t header_name;
    // Overlays at a shared base do not always cover the same bytes: ETC is
    // much smaller than BTL and only rewrites [0, size), leaving the rest of
    // the previous image intact (the game relies on this and keeps calling
    // BTL code above the ETC end). tail_start is the first offset still owned
    // by this module and over_* identify the overlay that took the head.
    std::uint32_t tail_start;
    std::uint32_t over_version;
    std::uint32_t over_name;
    std::uint32_t tail_check_begin;
};

const ModuleInfo *g_modules = nullptr;
std::uint32_t g_module_count = 0;
std::uint32_t g_module_span = 0;
std::vector<Instance> g_instances;

bool loaded_at(const ModuleInfo &m, std::uint32_t base) {
    if (m.fixed_base) {
        if (base != m.fixed_base) return false;
    } else if (ld32(base + 0x2Cu) != m.size) {
        return false;
    }
    std::uint32_t bad = 0;
    const std::uint32_t tolerance = m.fixed_base ? 8u : 0u;
    for (std::uint32_t k = 0; k < m.check_count; ++k)
        if (ld32(base + m.checks[2 * k]) != m.checks[2 * k + 1]) {
            if (!m.fixed_base || ++bad > tolerance) return false;
        }
    return true;
}

// The word a call checks turns with each call, so every translated code word
// is proven again within check_count calls: one word per call keeps calls
// cheap while even a one word change of the image is caught in time.
std::uint32_t g_spot_cursor = 0;

// Proves that the part of the image at or above `from` is still in place. The
// checks array is sorted by offset; `first` receives the index of the first
// check at or above `from` so later spot checks can stay in the tail.
bool tail_loaded(const ModuleInfo &m, std::uint32_t base, std::uint32_t from, std::uint32_t *first = nullptr) {
    std::uint32_t first_idx = m.check_count;
    for (std::uint32_t k = 0; k < m.check_count; ++k) {
        const std::uint32_t off = m.checks[2 * k];
        if (off < from) continue;
        if (first_idx == m.check_count) first_idx = k;
        if (ld32(base + off) != m.checks[2 * k + 1]) return false;
    }
    if (first_idx == m.check_count) return false;  // nothing left to prove
    if (first) *first = first_idx;
    return true;
}

// Size of the overlay that currently owns the head of a fixed base window.
std::uint32_t current_size_at(std::uint32_t base, const ModuleInfo *skip) {
    std::uint32_t size = 0;
    for (std::uint32_t k = 0; k < g_module_count; ++k) {
        const ModuleInfo &m = g_modules[k];
        if (!m.fixed_base || m.fixed_base != base || &m == skip) continue;
        if (m.size > size && loaded_at(m, base)) size = m.size;
    }
    return size;
}

const Instance *in_place(Instance &i) {
    static const bool full = std::getenv("PS2_WINDOW_FULL") != nullptr;
    if (full || !i.module->fixed_base) return loaded_at(*i.module, i.base) ? &i : nullptr;
    // A different overlay may have been copied over this window since the
    // instance was cached. Check the header first: the spot check samples code
    // words and can easily land on a word the new image shares.
    const std::uint32_t ver = ld32(i.base + 4);
    const std::uint32_t name = ld32(i.base + 0x20);
    if (ver == i.header_version && name == i.header_name) {
        i.tail_start = 0;
        if (!i.module->check_count) return &i;
        const std::uint32_t c = g_spot_cursor++ % i.module->check_count;
        if (ld32(i.base + i.module->checks[2 * c]) == i.module->checks[2 * c + 1]) return &i;
        return loaded_at(*i.module, i.base) ? &i : nullptr;
    }
    // Another overlay took the head of this window. Keep serving the tail it
    // did not overwrite, as long as the image above it still proves to be ours.
    if (!i.tail_start || ver != i.over_version || name != i.over_name) {
        const std::uint32_t occ = current_size_at(i.base, i.module);
        if (!occ || occ >= i.module->size) return nullptr;
        std::uint32_t first = 0;
        if (!tail_loaded(*i.module, i.base, occ, &first)) return nullptr;
        i.tail_start = occ;
        i.over_version = ver;
        i.over_name = name;
        i.tail_check_begin = first;
    }
    const std::uint32_t n = i.module->check_count - i.tail_check_begin;
    if (!n) return &i;
    const std::uint32_t c = i.tail_check_begin + (g_spot_cursor++ % n);
    if (ld32(i.base + i.module->checks[2 * c]) == i.module->checks[2 * c + 1]) return &i;
    return tail_loaded(*i.module, i.base, i.tail_start) ? &i : nullptr;
}

Instance make_instance(std::uint32_t base, const ModuleInfo *m) {
    return {base, m, ld32(base + 4), ld32(base + 0x20), 0u, 0u, 0u, 0u};
}

const UnitInfo *module_unit(const ModuleInfo &m, std::uint32_t offset);

bool module_has_entry(const ModuleInfo &m, std::uint32_t pc, std::uint32_t base) {
    if (pc < base) return false;
    const std::uint32_t off = pc - base;
    if ((off & 3u) != 0) return false;
    const UnitInfo *u = module_unit(m, off);
    return u && off - u->base < u->size && u->entries[(off - u->base) >> 2] != 0;
}

const Instance *instance_for(std::uint32_t pc) {
    // Validação de PC: rejeitar valores extremamente inválidos
    if (pc < 0x1000) {
        std::fprintf(stderr, "[ee] PC inválido 0x%08X detectado\n", pc);
        return nullptr;
    }
    
    for (std::size_t k = 0; k < g_instances.size();) {
        Instance &i = g_instances[k];
        if (pc < i.base) { ++k; continue; }
        const std::uint32_t off = pc - i.base;
        if (off >= i.module->size) { ++k; continue; }
        if (!module_has_entry(*i.module, pc, i.base)) { ++k; continue; }
        if (!in_place(i)) {
            g_instances.erase(g_instances.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        if (off >= i.tail_start) return &i;
        ++k;
    }
    if (!g_module_count || pc >= kRamSize) return nullptr;
    // A code window rewritten by the game holds fixed base modules, each an
    // image of one content. The one whose check words are in place serves it.
    for (std::uint32_t k = 0; k < g_module_count; ++k) {
        const ModuleInfo &m = g_modules[k];
        if (m.fixed_base && pc >= m.fixed_base && pc - m.fixed_base < m.size && module_has_entry(m, pc, m.fixed_base) &&
            loaded_at(m, m.fixed_base)) {
            g_instances.push_back(make_instance(m.fixed_base, &m));
            return &g_instances.back();
        }
    }
    // No image is fully in place, but a smaller overlay may only have replaced
    // the head of the window. Serve the untouched tail from the larger image.
    for (std::uint32_t k = 0; k < g_module_count; ++k) {
        const ModuleInfo &m = g_modules[k];
        if (!m.fixed_base || pc < m.fixed_base || pc - m.fixed_base >= m.size) continue;
        if (!module_has_entry(m, pc, m.fixed_base)) continue;
        const std::uint32_t occ = current_size_at(m.fixed_base, nullptr);
        if (!occ || pc - m.fixed_base < occ) continue;
        std::uint32_t first = 0;
        if (!tail_loaded(m, m.fixed_base, occ, &first)) continue;
        g_instances.push_back(make_instance(m.fixed_base, &m));
        Instance &i = g_instances.back();
        i.tail_start = occ;
        i.over_version = ld32(i.base + 4);
        i.over_name = ld32(i.base + 0x20);
        i.tail_check_begin = first;
        return &i;
    }
    for (std::uint32_t at = pc & ~15u; pc - at < g_module_span; at -= 16) {
        if (ld32(at) == kModuleMagic) {
            for (std::uint32_t k = 0; k < g_module_count; ++k)
                if (pc - at < g_modules[k].size && module_has_entry(g_modules[k], pc, at) && loaded_at(g_modules[k], at)) {
                    g_instances.push_back(make_instance(at, &g_modules[k]));
                    return &g_instances.back();
                }
            return nullptr;
        }
        if (at < 16) break;
    }
    return nullptr;
}

const UnitInfo *module_unit(const ModuleInfo &m, std::uint32_t offset) {
    for (std::uint32_t k = 0; k < m.count; ++k)
        if (offset - m.units[k].base < m.units[k].size) return &m.units[k];
    return nullptr;
}

const UnitInfo *unit_for(std::uint32_t pc) {
    if (pc < g_origin) return nullptr;
    const std::uint32_t slot = g_unit_pow2 ? ((pc - g_origin) >> g_unit_shift) : ((pc - g_origin) / g_unit_size);
    if (slot >= g_index.size()) return nullptr;
    return g_index[slot];
}

void record_miss(std::uint32_t pc, std::uint32_t ra = 0) {
    if (!pc) {
        std::fprintf(stderr, "[ee] PC inválido 0x%08X (ra 0x%08X)\n", pc, ra);
        halt("PC inválido");
        return;
    }
    if (const Instance *m = instance_for(pc)) {
        char token[96];
        std::snprintf(token, sizeof token, "%s:%x", m->module->name, pc - m->base);
        if (!g_module_misses.insert(token).second || g_options.misses_path.empty()) return;
        std::ofstream(g_options.misses_path, std::ios::app) << token << "\n";
        return;
    }
    if (!g_misses.insert(pc).second || g_options.misses_path.empty()) return;
    std::ofstream out(g_options.misses_path, std::ios::app);
    out << std::hex << pc << "\n";
}

}

void register_units(const UnitInfo *units, std::uint32_t count) {
    g_units.assign(units, units + count);
    if (g_units.empty()) return;
    std::sort(g_units.begin(), g_units.end(), [](const UnitInfo &a, const UnitInfo &b) { return a.base < b.base; });
    g_origin = g_units.front().base;
    g_unit_size = g_units.front().size;
    g_unit_pow2 = g_unit_size && !(g_unit_size & (g_unit_size - 1));
    g_unit_shift = g_unit_pow2 ? static_cast<unsigned>(std::countr_zero(g_unit_size)) : 0;
    g_index.assign((g_units.back().base - g_origin) / g_unit_size + 1, nullptr);
    for (const UnitInfo &u : g_units) g_index[(u.base - g_origin) / g_unit_size] = &u;
}

void register_modules(const ModuleInfo *modules, std::uint32_t count) {
    g_modules = modules;
    g_module_count = count;
    g_instances.clear();
    for (std::uint32_t k = 0; k < count; ++k) g_module_span = std::max(g_module_span, modules[k].size);
}

bool has_code(std::uint32_t pc) {
    const UnitInfo *u = unit_for(pc);
    if (u && u->entries[(pc - u->base) >> 2] != 0) return true;
    const Instance *m = instance_for(pc);
    const UnitInfo *mu = m ? module_unit(*m->module, pc - m->base) : nullptr;
    return mu && mu->entries[(pc - m->base - mu->base) >> 2] != 0;
}

void dump_recent_pcs() {
    std::fprintf(stderr, "[sched]   recent pcs:");
    for (unsigned i = 0; i < 32; ++i) std::fprintf(stderr, " %08X", g_pc_ring[(g_pc_ring_at + i) & 31u]);
    std::fprintf(stderr, "\n");
}

void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {
    const std::uint32_t pc = c.pc;
    if (pc & kUnwind) return;
    if (pc == kHandlerReturn || pc == kIdle) {
        if (pc == kIdle && !g_idle_reported && g_debug_sched) {
            g_idle_reported = true;
            std::fprintf(stderr, "[sched] pc became kIdle: thread %d handler %d prev 0x%08X ra 0x%08X sp 0x%08X\n",
                         kernel::current_thread(), kernel::in_handler() ? 1 : 0, g_trace_last_pc, c.r[31].w[0], c.r[29].w[0]);
            std::fprintf(stderr, "[sched]   recent pcs:");
            for (unsigned i = 0; i < 32; ++i) std::fprintf(stderr, " %08X", g_pc_ring[(g_pc_ring_at + i) & 31u]);
            std::fprintf(stderr, "\n");
        }
        return;
    }
    g_trace_last_pc = pc;
    g_pc_ring[g_pc_ring_at++ & 31u] = pc;
    if (g_pc_trace_on) {
        const PcTrace &pt = pc_trace();
        if (kernel::current_thread() == pt.thread) {
            const unsigned long long f = ps2::frame_count();
            if (f >= pt.from && f <= pt.to)
                std::fprintf(stderr, "[pc] f%llu t%d pc %08X ra %08X sp %08X a0 %08X a1 %08X b %d\n", f, pt.thread, pc, c.r[31].w[0],
                             c.r[29].w[0], c.r[4].w[0], c.r[5].w[0], c.budget);
        }
    }
    
    // Validação de PC adicional para prevenir crashes
    if (pc < 0x1000) {
        std::fprintf(stderr, "[ee] PC inválido no dispatch: 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
        halt("PC inválido");
        c.pc = pc | kUnwind;
        return;
    }
    if (g_trace_at_on && trace_at().count(pc))
        std::fprintf(stderr, "[at] f%llu t%d pc %08X ra %08X a0 %08X a1 %08X a2 %08X a3 %08X\n",
                     ps2::frame_count(), kernel::current_thread(), pc, c.r[31].w[0], c.r[4].w[0], c.r[5].w[0],
                     c.r[6].w[0], c.r[7].w[0]);
    if (g_trace_scene && pc == 0x006D4330u)
        std::fprintf(stderr, "[scene] f%llu t%d state %d ra %08X\n", ps2::frame_count(), kernel::current_thread(),
                     static_cast<int>(ld32(c.r[4].w[0] + 12u)), c.r[31].w[0]);
    if (g_trace_mode_state) {
        static std::uint32_t last_mo = 0xFFFFFFFFu, last_st = 0xFFFFFFFFu, last_m8 = 0xFFFFFFFFu,
                             last_m12 = 0xFFFFFFFFu, last_m16 = 0xFFFFFFFFu, last_flag = 0xFFFFFFFFu;
        const std::uint32_t mo = ld32(0x00617EF0u - 484u);
        const std::uint32_t st = mo ? ld32(mo) : 0xFFFFFFFFu;
        const std::uint32_t mgr = ld32(0x00617EF0u - 496u);
        const std::uint32_t m8 = mgr ? ld32(mgr + 8u) : 0xFFFFFFFFu;
        const std::uint32_t m12 = mgr ? ld32(mgr + 12u) : 0xFFFFFFFFu;
        const std::uint32_t m16 = mgr ? ld32(mgr + 16u) : 0xFFFFFFFFu;
        const std::uint32_t flag = ld32(0x00618074u);
        if (mo != last_mo || st != last_st || m8 != last_m8 || m12 != last_m12 || m16 != last_m16 || flag != last_flag) {
            std::fprintf(stderr, "[ms] f%llu t%d pc %08X obj %08X st %u mgr8 %u mgr12 %u mgr16 %u flag %u\n",
                         ps2::frame_count(), kernel::current_thread(), pc, mo, st, m8, m12, m16, flag);
            last_mo = mo; last_st = st; last_m8 = m8; last_m12 = m12; last_m16 = m16; last_flag = flag;
        }
    }
    if (g_trace_mode && (pc == 0x001F0260u || pc == 0x001F0314u || pc == 0x001F037Cu || pc == 0x001F02FCu ||
                                           pc == 0x001F1260u || pc == 0x001F1324u || pc == 0x001F1488u || pc == 0x001F12E8u)) {
        const std::uint32_t mo = ld32(c.r[28].w[0] - 484u);
        std::fprintf(stderr, "[mode] f%llu t%d pc %08X obj %08X state %d ra %08X\n", ps2::frame_count(),
                     kernel::current_thread(), pc, mo, mo ? static_cast<int>(ld32(mo)) : -1, c.r[31].w[0]);
    }
    if (g_trace_loader && (pc == 0x001FA5D0u || pc == 0x001C3150u)) {
        char name[17] = {};
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t ch = static_cast<std::uint8_t>(ld8(0x006C6D00u + 0x20u + static_cast<std::uint32_t>(i)));
            name[i] = ch ? static_cast<char>(ch) : '\0';
            if (!ch) break;
        }
        std::fprintf(stderr, "[loader] f%llu t%d pc %08X id %u gp %08X cache %08X resident ver %u name %s\n",
                     ps2::frame_count(), kernel::current_thread(), pc, c.r[4].w[0], c.r[28].w[0],
                     ld32(c.r[28].w[0] - 496u), ld32(0x006C6D00u + 4u), name);
    }
    if (g_trace_window) {
        static std::uint32_t last_ver = 0xFFFFFFFFu, last_name = 0;
        const std::uint32_t ver = ld32(0x006C6D00u + 4u), nm = ld32(0x006C6D00u + 0x20u);
        if (ver != last_ver || nm != last_name) {
            char name[17] = {};
            for (int i = 0; i < 16; ++i) {
                const std::uint8_t ch = static_cast<std::uint8_t>(ld8(0x006C6D00u + 0x20u + static_cast<std::uint32_t>(i)));
                name[i] = ch ? static_cast<char>(ch) : '\0';
                if (!ch) break;
            }
            std::fprintf(stderr, "[window] f%llu t%d prev %08X pc %08X ra %08X ver %u name %s\n",
                         ps2::frame_count(), kernel::current_thread(), g_pc_ring[(g_pc_ring_at - 2) & 31u], pc, c.r[31].w[0], ver, name);
            last_ver = ver;
            last_name = nm;
        }
    }
    const UnitInfo *u = unit_for(pc);
    const std::uint16_t id = u ? u->entries[(pc - u->base) >> 2] : 0;
    if (!id) {
        if (const Instance *m = instance_for(pc)) {
            const std::uint32_t base = m->base;
            const UnitInfo *mu = module_unit(*m->module, pc - base);
            const std::uint16_t entry = mu ? mu->entries[(pc - base - mu->base) >> 2] : 0;
            if (entry) {
                c.base = base;
                mu->function(c, entry, depth);
                return;
            }
        }
        record_miss(pc, c.r[31].w[0]);
        // Exploration mode: pretend the unknown function returned 0 and
        // continue after the call. This lets one headless run discover many
        // indirect entry points instead of stopping at the first one. It is
        // only enabled by port.sh during exploration, never for normal play.
        if (std::getenv("PS2_MISS_CONTINUE")) {
            c.r[2].w[0] = 0;
            c.pc = c.r[31].w[0];
            std::fprintf(stderr, "[ee] skip missing 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
            return;
        }
        char reason[64];
        std::snprintf(reason, sizeof reason, "missing code at 0x%08X (ra 0x%08X)", pc, c.r[31].w[0]);
        std::fprintf(stderr, "[ee] no translated code at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
        dump_recent_pcs();
        for (int i = 1; i < 32; i += 4)
            std::fprintf(stderr, "[ee]   r%-2d %08X %08X %08X %08X\n", i, c.r[i].w[0], c.r[i + 1].w[0], c.r[i + 2].w[0], i + 3 < 32 ? c.r[i + 3].w[0] : 0u);
        halt(reason);
        c.pc = pc | kUnwind;
        return;
    }
    u->function(c, id, depth);
}

void call(Cpu &c, std::uint32_t target, std::uint32_t depth) {
    c.pc = target;
    if (depth >= kMaxDepth) return;
    dispatch(c, 0, depth);
}

void bad_entry(Cpu &c, std::uint32_t unit_base) {
    std::fprintf(stderr, "[ee] bad entry into unit 0x%08X for pc 0x%08X\n", unit_base, c.pc);
    halt("bad entry");
    c.pc |= kUnwind;
}

void stop(Cpu &c, std::uint32_t pc, const char *what) {
    std::fprintf(stderr, "[ee] stopped at 0x%08X: %s\n", pc, what);
    halt(what);
    c.pc = pc | kUnwind;
}

void probe(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X ra %08X v0 %08X\n", pc, c.r[4].w[0], c.r[5].w[0], c.r[6].w[0],
                 c.r[7].w[0], c.r[31].w[0], c.r[2].w[0]);
}

void trace_loader(const char *where, std::uint32_t id, std::uint32_t gp, std::uint32_t name_ptr) {
    static const bool on = std::getenv("PS2_TRACE_LOADER") != nullptr;
    if (!on) return;
    char name[17] = {};
    if (name_ptr) {
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t ch = static_cast<std::uint8_t>(ld8(name_ptr + static_cast<std::uint32_t>(i)));
            name[i] = ch ? static_cast<char>(ch) : '\0';
            if (!ch) break;
        }
    }
    char res[17] = {};
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t ch = static_cast<std::uint8_t>(ld8(0x006C6D00u + 0x20u + static_cast<std::uint32_t>(i)));
        res[i] = ch ? static_cast<char>(ch) : '\0';
        if (!ch) break;
    }
    std::fprintf(stderr, "[loader] f%llu t%d %s id %u gp %08X cache %08X resident %s%s%s\n",
                 ps2::frame_count(), kernel::current_thread(), where, id, gp,
                 gp ? ld32(gp - 496u) : 0u, res, name[0] ? " -> " : "", name);
}

void breakpoint(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] break at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

void trap(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] trap at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

extern "C" {
static unsigned long long g_scene_updates = 0;
static unsigned long long g_scene_last_frame = 0;
unsigned long long ps2_scene_last_frame() { return g_scene_last_frame; }
void ps2_note_scene_update() {
    ++g_scene_updates;
    g_scene_last_frame = ps2::frame_count();
    static const bool on = std::getenv("PS2_TRACE_SCENE_UPD") != nullptr;
    if (on)
        std::fprintf(stderr, "[sceneupd] f%llu t%d ra %08X a0 %08X a1 %08X a2 %08X a3 %08X f12 %.6f f13 %.6f f14 %.6f\n",
                     static_cast<unsigned long long>(ps2::frame_count()), kernel::current_thread(), g_cpu.r[31].w[0],
                     g_cpu.r[4].w[0], g_cpu.r[5].w[0], g_cpu.r[6].w[0], g_cpu.r[7].w[0], g_cpu.f[12], g_cpu.f[13], g_cpu.f[14]);
}
unsigned long long ps2_scene_updates() { return g_scene_updates; }
}
}

