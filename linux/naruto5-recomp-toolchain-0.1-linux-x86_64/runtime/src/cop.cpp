#include "hw.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>

namespace ps2 {

std::uint32_t cfc1(Cpu &c, std::uint32_t reg) {
    if (reg == 0) return 0x2E30u;
    if (reg == 31) return c.fcr31;
    return 0;
}

void ctc1(Cpu &c, std::uint32_t reg, std::uint32_t value) {
    if (reg == 31) c.fcr31 = value;
}

std::uint32_t cfc2(Cpu &c, std::uint32_t reg) {
    Vu0Regs &v = c.vu0;
    switch (reg) {
    case 16: return v.status;
    case 17: return v.mac;
    case 18: return v.clip;
    case 20: return v.r;
    case 21: return v.i;
    case 22: return v.q;
    case 26: return v.tpc;
    case 27: return v.cmsar0;
    case 29: return 0;
    default:
        std::fprintf(stderr, "[vu0] cfc2 %u\n", reg);
        return 0;
    }
}

void ctc2(Cpu &c, std::uint32_t reg, std::uint32_t value) {
    Vu0Regs &v = c.vu0;
    switch (reg) {
    case 16: v.status = (v.status & 0x3Fu) | (value & 0xFC0u); break;
    case 17: v.mac = value; break;
    case 18: v.clip = value & 0xFFFFFFu; break;
    case 20: v.r = value & 0x7FFFFFu; break;
    case 21: v.i = value; break;
    case 22: v.q = value; break;
    case 27: v.cmsar0 = value & 0xFFFFu; break;
    case 28: break;
    case 31:
        std::fprintf(stderr, "[vu1] start at 0x%X through CMSAR1 (not implemented)\n", value);
        break;
    default:
        std::fprintf(stderr, "[vu0] ctc2 %u = 0x%08X\n", reg, value);
        break;
    }
}

bool vu0_busy(Cpu &) { return false; }
void vu0_wait(Cpu &) {}
std::uint8_t *vu0_data() { return g_vu0_mem; }

bool cpcond0(Cpu &) { return true; }

}
