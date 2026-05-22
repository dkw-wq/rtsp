#pragma once

#include "config_loader.hpp"

namespace rtsp {

int runSingleStream(const AppConfig& config);
int runMultiStream(const AppConfig& config);

} // namespace rtsp
