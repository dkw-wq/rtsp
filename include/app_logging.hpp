#pragma once

#include "config_loader.hpp"

#include <filesystem>

namespace rtsp {

std::filesystem::path initializeLogging();
void logConfig(const AppConfig& config);

} // namespace rtsp
