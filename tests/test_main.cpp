#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;
    for (const auto& c : sptest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = sptest::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  exception: %s\n", e.what());
            ++sptest::failures();
        }
        ++run;
        bool ok = sptest::failures() == before;
        if (!ok) ++failedCases;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", c.name);
    }
    std::printf("\n%d test cases, %d failed\n", run, failedCases);
    return failedCases ? 1 : 0;
}
