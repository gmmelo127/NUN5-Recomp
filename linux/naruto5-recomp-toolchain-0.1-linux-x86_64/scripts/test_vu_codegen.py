#!/usr/bin/env python3
"""Exercise native and Wasm VU code generation without a game disc.

Requires a C++20 compiler with std::format and Node with Wasm SIMD/tail calls.
Run from any directory: python3 scripts/test_vu_codegen.py [c++ compiler]
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CXX = sys.argv[1] if len(sys.argv) > 1 else 'clang++'

GENERATOR = r'''
#define main vurecomp_main
#include "recomp/vurecomp.cpp"
#undef main
#include "runtime/src/vu_jit.hpp"
#include <cstddef>

int main(int argc, char **argv) {
    const fs::path out = argv[1];
    std::ofstream native(out / "native.cpp");
    native << "#include <stdexcept>\n#include <cstdio>\n#include \"ps2/vu_ops.hpp\"\n"
              "namespace ps2::vu { void interpret(int, std::uint32_t) { throw std::runtime_error(\"unexpected fallback\"); } }\n"
              "using namespace ps2; using namespace ps2::vuops;\n";
    constexpr std::uint32_t nop = 0x8000033C, unop = 0x000002FF, end = 0x40000000;
    const auto addi = [](unsigned it, unsigned is, unsigned imm) { return (8u << 25) | (it << 16) | (is << 11) | imm; };
    const auto branch = [](unsigned op, unsigned is, unsigned it, unsigned from, unsigned to) {
        return (op << 25) | (is << 11) | (it << 16) | (((to - from - 8u) / 8u) & 0x7FFu);
    };
    for (int test = 0; test < 8; ++test) {
        Program p{static_cast<std::uint64_t>(test), 0, std::vector<std::uint8_t>(16384)};
        const auto put = [&](unsigned pc, std::uint32_t lo, std::uint32_t hi = unop) {
            std::memcpy(p.code.data() + pc, &lo, 4);
            std::memcpy(p.code.data() + pc + 4, &hi, 4);
        };
        for (unsigned pc = 0; pc < 128; pc += 8) put(pc, nop);
        if (test < 3 || test == 5) {
            // A preceding VI write delays a comparison, but FCAND does not.
            put(0, test == 2 ? (0x12u << 25) | 1u : addi(1, 0, 1));
            put(8, branch(test == 5 ? 0x2C : test == 2 ? 0x29 : 0x28, test == 1 ? 0 : 1, test == 0 ? 3 : 0, 8, 40));
            put(24, addi(2, 0, 1), unop | end);
            put(40, addi(2, 0, 2), unop | end);
        } else if (test == 3) {
            // The instruction executed before PC 40 is a taken branch's
            // delay slot at PC 8, rather than the instruction at PC 32.
            put(0, branch(0x20, 0, 0, 0, 40));
            put(8, addi(1, 0, 1));
            put(32, addi(1, 0, 3));
            put(40, branch(0x28, 1, 3, 40, 72));
            put(56, addi(2, 0, 1), unop | end);
            put(72, addi(2, 0, 2), unop | end);
        } else if (test >= 6) {
            // JR/JALR read the operand without the delay; JALR also overwrites it.
            put(0, addi(1, 0, 1));
            put(8, ((test == 6 ? 0x24u : 0x25u) << 25) | (1u << 11) | (1u << 16), unop | end);
        } else {
            // Flags remain observable after exit in a program that reads them.
            put(0, nop, (0xFu << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 0x28u | end);
            put(8, (0x16u << 25) | (2u << 16));
        }
        std::string name;
        native << generate(p, name);
        std::vector<std::uint8_t> wasm;
        if (!ps2::vu::jit::translate(p.code.data(), 0, {0x1000}, wasm)) return 1;
        std::ofstream(out / std::format("{}.wasm", test), std::ios::binary).write(reinterpret_cast<const char *>(wasm.data()), wasm.size());
    }
    native << "int main() {\n";
    for (int test = 0; test < 8; ++test) {
        native << "{ Vu0Regs r{}; std::uint8_t mem[16384]{}; r.vi[1]=2; r.vi[3]=2; r.clip=1;"
                  "for (int i=0;i<4;++i) r.vf[1].w[i]=0xBF800000; r.vf[0].w[3]=0x3F800000;\n";
        if (test == 5) native << "r.vi[1]=0xFFFF;\n";
        native << std::format("vu1_{:016X}(r, mem);\n", test);
        native << (test >= 6 ? "if (r.tpc != 8) return 1;\n" : test == 4 ? "if (r.mac != 0xF0 || r.status != 0x82) return 1;\n" : "if (r.vi[2] != 2) return 1;\n");
        native << "}\n";
    }
    native << "puts(\"8 native VU regressions passed\"); }\n";
    std::ofstream offsets(out / "offsets.json");
    offsets << std::format("{{\"vi\":{},\"vf\":{},\"clip\":{},\"mac\":{},\"status\":{},\"tpc\":{}}}", offsetof(ps2::Vu0Regs, vi), offsetof(ps2::Vu0Regs, vf), offsetof(ps2::Vu0Regs, clip), offsetof(ps2::Vu0Regs, mac), offsetof(ps2::Vu0Regs, status), offsetof(ps2::Vu0Regs, tpc));
}
'''

WASM_TEST = r'''
const fs = require('node:fs');
const path = require('node:path');
const dir = process.argv[2];
const off = JSON.parse(fs.readFileSync(path.join(dir, 'offsets.json')));
for (let test = 0; test < 8; ++test) {
  const memory = new WebAssembly.Memory({initial: 1, maximum: 65536, shared: true});
  const view = new DataView(memory.buffer);
  const r = 0x3000;
  view.setUint16(r + off.vi + 2, test === 5 ? 0xFFFF : 2, true);
  view.setUint16(r + off.vi + 6, 2, true);
  view.setUint32(r + off.clip, 1, true);
  view.setUint32(r + off.vf + 12, 0x3F800000, true);
  for (let i = 0; i < 4; ++i) view.setUint32(r + off.vf + 16 + 4*i, 0xBF800000, true);
  const mod = new WebAssembly.Module(fs.readFileSync(path.join(dir, `${test}.wasm`)));
  const env = {memory};
  for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.kind !== 'function') continue;
    env[imp.name] = () => { throw Error(`Unexpected import: ${imp.name}`); };
  }
  env.clip = () => 1;
  env.status = () => 0;
  new WebAssembly.Instance(mod, {env}).exports.run(r, 0x4000);
  if (test >= 6) {
    if (view.getUint32(r + off.tpc, true) !== 8) throw Error(`Register jump regression ${test}`);
  } else if (test === 4) {
    if (view.getUint32(r + off.mac, true) !== 0xF0 || view.getUint32(r + off.status, true) !== 0x82)
      throw Error('Exit flags were dropped');
  } else if (view.getUint16(r + off.vi + 4, true) !== 2) {
    throw Error(`Branch VI regression ${test}`);
  }
}
console.log('8 Wasm VU regressions passed');
'''


def run(*args):
    subprocess.run(args, check=True, cwd=ROOT)


with tempfile.TemporaryDirectory(prefix='vu-codegen-') as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'generator.cpp').write_text(GENERATOR)
    run(CXX, '-std=c++20', '-O2', '-I.', '-Iruntime/include',
        str(tmp / 'generator.cpp'), 'runtime/src/vu_jit.cpp', '-o', str(tmp / 'generator'))
    run(str(tmp / 'generator'), str(tmp))
    run(CXX, '-std=c++20', '-O2', '-Iruntime/include', str(tmp / 'native.cpp'), '-o', str(tmp / 'native'))
    run(str(tmp / 'native'))
    (tmp / 'wasm.cjs').write_text(WASM_TEST)
    run('node', str(tmp / 'wasm.cjs'), str(tmp))
