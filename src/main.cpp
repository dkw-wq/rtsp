#include "app_logging.hpp"
#include "config_loader.hpp"
#include "player_runner.hpp"

#include <spdlog/spdlog.h>

int main(int argc, char* argv[]) {
    rtsp::initializeLogging();

    auto config = rtsp::loadAppConfig("config/config.yaml");
    rtsp::applyCommandLineOverrides(config, argc, argv);

    spdlog::level::level_enum parsedLogLevel = spdlog::level::info;
    if (!rtsp::parseLogLevel(config.logLevelName, parsedLogLevel)) {
        SPDLOG_WARN("Unknown log level '{}', using info", config.logLevelName);
    }
    spdlog::set_level(parsedLogLevel);

    SPDLOG_INFO("RTSP Player starting...");
    if (!config.warning.empty()) {
        SPDLOG_WARN("Config file not found or invalid, using defaults: {}", config.warning);
    }
    rtsp::logConfig(config);

    if (config.rtspUrls.size() > 1) {
        return rtsp::runMultiStream(config);
    }

    return rtsp::runSingleStream(config);
}
