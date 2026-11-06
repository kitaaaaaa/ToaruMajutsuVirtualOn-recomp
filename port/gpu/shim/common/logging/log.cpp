// vox: replaces shadPS4's spdlog logger behind the same log.h: a small sink on stdout.
// Level from VOXGPU_LOG (trace|debug|info|warning|error|critical; default warning).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"

namespace Common::Log {
namespace {
constexpr const char* LevelNames[] = {"trace", "debug", "info", "warning", "error", "critical"};

Level InitialLevel() {
    const char* value = std::getenv("VOXGPU_LOG");
    if (value) {
        for (int i = 0; i < 6; ++i) {
            if (!std::strcmp(value, LevelNames[i])) {
                return static_cast<Level>(i);
            }
        }
    }
    return Level::Warning;
}

std::array<Level, NUM_LOG_CLASSES> InitialLevels() {
    std::array<Level, NUM_LOG_CLASSES> levels;
    levels.fill(InitialLevel());
    return levels;
}
} // namespace

std::array<Level, NUM_LOG_CLASSES> g_class_levels = InitialLevels();

void Flush() {
    std::fflush(stdout);
}

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args) {
    static std::mutex mutex;
    const std::string message = fmt::vformat(format, args);
    const char* base = std::strrchr(file, '\\');
    if (const char* slash = std::strrchr(file, '/'); slash && (!base || slash > base)) {
        base = slash;
    }
    const auto name = NameOf(log_class);
    std::scoped_lock lock{mutex};
    std::fprintf(stdout, "GPU [%.*s] <%s> %s:%d %s: %s\n", static_cast<int>(name.size()),
                 name.data(), LevelNames[static_cast<int>(level)], base ? base + 1 : file, line,
                 func, message.c_str());
    std::fflush(stdout);
}

} // namespace Common::Log
