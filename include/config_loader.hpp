#pragma once

#include "audio_player.hpp"
#include "face_types.hpp"
#include "rtsp_client.hpp"
#include "sync_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/common.h>

namespace rtsp {

struct ReconnectOptions {
    bool enabled = true;
    uint32_t initialDelayMs = 1000;
    uint32_t maxDelayMs = 5000;
};

struct AppConfig {
    std::string rtspUrl = "rtsp://127.0.0.1:8554/webcam";
    std::vector<std::string> rtspUrls;
    std::string audioRtspUrl = "rtsp://127.0.0.1:8554/audio";
    int width = 1920;
    int height = 1080;
    std::string logLevelName = "info";
    std::string rendererName = "sdl";
    std::string hwDecodeBackend = "none";
    std::vector<std::string> openglFilterNames = {"none"};
    RtspConnectionOptions rtspOptions;
    AudioPlaybackOptions audioOptions;
    SyncOptions syncOptions;
    FaceDetectionOptions faceDetectionOptions;
    size_t jitterMaxSize = 12;
    uint32_t jitterLatencyMs = 30;
    ReconnectOptions reconnectOptions;
    std::string warning;
    bool rtspUrlsConfigured = false;
};

AppConfig loadAppConfig(const std::string& path);
void applyCommandLineOverrides(AppConfig& config, int argc, char* argv[]);

std::string toLower(std::string value);
std::string toUpper(std::string value);
std::string displayDecodeBackend(const std::string& backend);
bool parseLogLevel(const std::string& value, spdlog::level::level_enum& level);
std::string describeFilters(const std::vector<std::string>& filters);
std::string describeLowLatency(const RtspConnectionOptions& options);

} // namespace rtsp
