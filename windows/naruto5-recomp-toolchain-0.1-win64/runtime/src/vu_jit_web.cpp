#include "vu_jit_web.hpp"

#include "vu_jit.hpp"

#include <cstdlib>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ps2::vu::jit {

bool g_dry = false;
std::int64_t g_escape = -1;
std::vector<std::uint32_t> *g_kicks = nullptr;

int mode() {
    static const int value = [] {
#ifdef __EMSCRIPTEN__
        const char *env = std::getenv("PS2_VU_JIT");
        if (!env) return 1;
        if (std::strcmp(env, "all") == 0) return 2;
        if (std::strcmp(env, "check") == 0) return 3;
        return std::strcmp(env, "0") == 0 ? 0 : 1;
#else
        return 0;
#endif
    }();
    return value;
}

#ifdef __EMSCRIPTEN__

namespace {

struct State {
    vuops::FlagPipe fl;
};

State g_state;

}

EM_JS_DEPS(ps2jit, "$addFunction");

EM_JS(int, ps2jit_instantiate, (const std::uint8_t *bytes, int size), {
    try {
        const module = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + size));
        const env = {memory: wasmMemory};
        for (const entry of WebAssembly.Module.imports(module))
            if (entry.kind === 'function') env[entry.name] = wasmExports['ps2jit_' + entry.name];
        const instance = new WebAssembly.Instance(module, {env: env});
        return addFunction(instance.exports.run, 'vii');
    } catch (e) {
        console.warn('VU JIT failed', e);
        return 0;
    }
});

ProgramFn compile(const std::uint8_t *code, std::uint32_t entry) {
    std::vector<std::uint8_t> bytes;
    const Layout layout{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_state.fl))};
    if (!translate(code, entry, layout, bytes)) return nullptr;
    const int index = ps2jit_instantiate(bytes.data(), static_cast<int>(bytes.size()));
    if (!index) return nullptr;
    return reinterpret_cast<ProgramFn>(static_cast<std::uintptr_t>(index));
}

#else

ProgramFn compile(const std::uint8_t *, std::uint32_t) { return nullptr; }

#endif

}

#ifdef __EMSCRIPTEN__

using namespace ps2;

extern "C" {

EMSCRIPTEN_KEEPALIVE std::uint32_t ps2jit_mac(vuops::FlagPipe *fl) { return fl->mac(); }
EMSCRIPTEN_KEEPALIVE std::uint32_t ps2jit_status(vuops::FlagPipe *fl) { return fl->status(); }
EMSCRIPTEN_KEEPALIVE std::uint32_t ps2jit_clip(vuops::FlagPipe *fl) { return fl->clip(); }
EMSCRIPTEN_KEEPALIVE void ps2jit_force_mac(vuops::FlagPipe *fl, Vu0Regs *r) { fl->force_mac(r->mac, r->status); }
EMSCRIPTEN_KEEPALIVE void ps2jit_efu(Vu0Regs *r, unsigned index, unsigned fs, unsigned fsf) { vuops::efu(*r, index, fs, fsf); }
EMSCRIPTEN_KEEPALIVE void ps2jit_kick(std::uint32_t address) {
    if (vu::jit::g_dry) {
        vu::jit::g_kicks->push_back(address);
        return;
    }
    vu::kick(1, address);
}
EMSCRIPTEN_KEEPALIVE std::uint32_t ps2jit_top() { return vu::top(); }
EMSCRIPTEN_KEEPALIVE std::uint32_t ps2jit_itop() { return vu::itop(); }
EMSCRIPTEN_KEEPALIVE void ps2jit_interpret(Vu0Regs *r, std::uint32_t pc) {
    vu::jit::g_state.fl.flush_q(*r);
    if (vu::jit::g_dry) {
        vu::jit::g_escape = pc;
        return;
    }
    vu::interpret(1, pc);
}

}

#endif
