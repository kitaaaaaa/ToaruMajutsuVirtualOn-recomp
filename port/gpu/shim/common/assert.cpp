// vox: GPU-side assertion failures stop the process with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include "common/assert.h"
#include "common/logging/log.h"

void assert_fail_impl() {
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see the log above)\n", stderr);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 23);
    __builtin_unreachable();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    __builtin_unreachable();
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
