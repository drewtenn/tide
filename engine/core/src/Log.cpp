#include "tide/core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <mutex>
#include <string>

namespace tide::log {

namespace {

std::shared_ptr<spdlog::logger>& logger_slot() {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}

std::once_flag& init_flag() {
    static std::once_flag flag;
    return flag;
}

void init_default(std::string_view pattern) {
    auto& slot = logger_slot();
    slot = spdlog::stdout_color_mt("tide");
    slot->set_pattern(std::string(pattern));
#if !defined(NDEBUG)
    slot->set_level(spdlog::level::debug);
#else
    slot->set_level(spdlog::level::info);
#endif
}

} // namespace

void init(std::string_view pattern) {
    std::call_once(init_flag(), [pattern] { init_default(pattern); });
}

spdlog::logger& engine() {
    std::call_once(init_flag(), [] { init_default("[%H:%M:%S.%e] [%^%l%$] [%n] %v"); });
    return *logger_slot();
}

} // namespace tide::log
