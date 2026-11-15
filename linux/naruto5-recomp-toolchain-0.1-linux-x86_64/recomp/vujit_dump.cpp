#include "vu_jit.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: vujit_dump <recorded programs dir> <out dir>\n");
        return 1;
    }
    std::filesystem::create_directories(argv[2]);
    int count = 0, failed = 0;
    for (const auto &e : std::filesystem::directory_iterator(argv[1])) {
        if (e.path().extension() != ".bin") continue;
        unsigned long long key = 0;
        unsigned entry = 0;
        if (std::sscanf(e.path().stem().string().c_str(), "%llx_%x", &key, &entry) != 2) continue;
        std::vector<std::uint8_t> code(16384);
        std::ifstream(e.path(), std::ios::binary).read(reinterpret_cast<char *>(code.data()), 16384);
        std::vector<std::uint8_t> out;
        if (!ps2::vu::jit::translate(code.data(), entry, {0x1000}, out)) {
            ++failed;
            continue;
        }
        std::ofstream(std::filesystem::path(argv[2]) / (e.path().stem().string() + ".wasm"), std::ios::binary).write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        ++count;
    }
    std::printf("%d modules, %d failed\n", count, failed);
    return failed != 0;
}
