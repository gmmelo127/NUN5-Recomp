#include "profile.hpp"

#include "ps2/system.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <functional>
#include <link.h>
#include <map>
#include <string>
#include <sys/time.h>
#include <ucontext.h>

namespace {

constexpr std::size_t kSlots = 1u << 23;
constexpr std::size_t kOutside = 1u << 16;
std::uint32_t g_hits[kSlots];
std::uint32_t g_caller_hits[kSlots];
std::uintptr_t g_outside_pc[kOutside];
std::uint32_t g_outside_hits[kOutside];
std::uintptr_t g_base = 0;
std::uint64_t g_samples = 0, g_lost = 0;
std::uint64_t g_from = 0;

void sample(int, siginfo_t *, void *context) {
    const auto *uc = static_cast<const ucontext_t *>(context);
    const std::uintptr_t pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    ++g_samples;
    const std::uintptr_t offset = (pc - g_base) >> 4;
    if (pc >= g_base && offset < kSlots) {
        ++g_hits[offset];
        return;
    }
    const std::uintptr_t caller = *reinterpret_cast<const std::uintptr_t *>(uc->uc_mcontext.gregs[REG_RSP]);
    const std::uintptr_t coff = (caller - g_base) >> 4;
    if (caller >= g_base && coff < kSlots) ++g_caller_hits[coff];
    const std::uintptr_t key = pc >> 4;
    for (std::size_t i = 0, h = (key * 0x9E3779B97F4A7C15ull) >> 48; i < 64; ++i, h = (h + 1) & (kOutside - 1)) {
        if (g_outside_pc[h] == key) { ++g_outside_hits[h]; return; }
        if (!g_outside_pc[h]) { g_outside_pc[h] = key; g_outside_hits[h] = 1; return; }
    }
    ++g_lost;
}

int find_base(dl_phdr_info *info, std::size_t, void *) {
    if (!info->dlpi_name || !info->dlpi_name[0]) {
        g_base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

void arm() {
    struct sigaction sa{};
    sa.sa_sigaction = sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval timer{};
    timer.it_interval.tv_usec = 1000;
    timer.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &timer, nullptr);
}

}

void start_profile(std::uint64_t from_frame) {
    dl_iterate_phdr(find_base, nullptr);
    g_from = from_frame;
    if (!g_from) {
        arm();
        return;
    }
    auto previous = ps2::g_options.on_vblank;
    ps2::g_options.on_vblank = [previous]() {
        if (ps2::frame_count() == g_from) arm();
        if (previous) previous();
    };
}

void write_profile(const char *path) {
    itimerval timer{};
    setitimer(ITIMER_PROF, &timer, nullptr);
    std::FILE *f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "# samples %llu lost %llu\n", static_cast<unsigned long long>(g_samples), static_cast<unsigned long long>(g_lost));
    for (std::size_t i = 0; i < kSlots; ++i)
        if (g_hits[i]) std::fprintf(f, "%zx %u\n", i << 4, g_hits[i]);
    for (std::size_t i = 0; i < kSlots; ++i)
        if (g_caller_hits[i]) std::fprintf(f, "^ %zx %u\n", i << 4, g_caller_hits[i]);
    std::map<std::string, std::uint64_t> outside;
    for (std::size_t i = 0; i < kOutside; ++i) {
        if (!g_outside_pc[i]) continue;
        Dl_info info{};
        std::string name = "?";
        if (dladdr(reinterpret_cast<void *>(g_outside_pc[i] << 4), &info)) {
            if (info.dli_sname) name = info.dli_sname;
            else if (info.dli_fname) name = info.dli_fname;
        }
        outside[name] += g_outside_hits[i];
    }
    for (const auto &[name, hits] : outside) std::fprintf(f, "@ %llu %s\n", static_cast<unsigned long long>(hits), name.c_str());
    std::fclose(f);
}
