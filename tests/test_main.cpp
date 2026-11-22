#include <cstring>

#include "TestFramework.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& t : bstest::registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = bstest::failures();
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        t.fn();
        std::printf("[ %s ] %s\n", bstest::failures() == before ? " OK " : "FAIL", t.name);
        ++run;
    }
    std::printf("\n%d test(s) run, %d check(s) failed\n", run, bstest::failures());
    return bstest::failures() == 0 ? 0 : 1;
}
