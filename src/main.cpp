#include "os/kernel.hpp"

#ifdef __EMSCRIPTEN__
int main() { return 0; }

#else
#include <iostream>
#include <fstream>
#include <vector>
#include <iterator>

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return { std::istreambuf_iterator<char>(f), {} };
}

int main() {
    os::Kernel kernel;
    auto ttf = slurp("assets/fonts/Monaco.ttf");
    if (ttf.empty())
        std::cerr << "[warn] Monaco.ttf not found text will be blank\n";
    kernel.boot(8, std::move(ttf));
    std::cout << "JadeOS booted (native smoke-test).\n";
    constexpr uint64_t TEST_TICKS = 200;
    for (uint64_t i = 0; i < TEST_TICKS && kernel.is_running(); ++i)
        kernel.tick(1000);
    std::cout << "Native test: " << TEST_TICKS << " ticks OK, "
              << kernel.cpu().total_cycles() << " CPU cycles.\n";
}
#endif
