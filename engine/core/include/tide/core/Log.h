#pragma once

#include <spdlog/spdlog.h>

#include <string_view>

namespace tide::log {

void init(std::string_view pattern = "[%H:%M:%S.%e] [%^%l%$] [%n] %v");

spdlog::logger& engine();

} // namespace tide::log

#define TIDE_LOG_TRACE(...) ::tide::log::engine().trace(__VA_ARGS__)
#define TIDE_LOG_DEBUG(...) ::tide::log::engine().debug(__VA_ARGS__)
#define TIDE_LOG_INFO(...) ::tide::log::engine().info(__VA_ARGS__)
#define TIDE_LOG_WARN(...) ::tide::log::engine().warn(__VA_ARGS__)
#define TIDE_LOG_ERROR(...) ::tide::log::engine().error(__VA_ARGS__)
#define TIDE_LOG_FATAL(...) ::tide::log::engine().critical(__VA_ARGS__)
