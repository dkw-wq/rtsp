#include "config_loader.hpp"

#include <algorithm>
#include <cctype>
#include <exception>

#include <yaml-cpp/yaml.h>

namespace rtsp {

namespace {

void assignPositiveInt(const YAML::Node& node, const char* key, int& target) {
    if (!node[key]) {
        return;
    }

    const int value = node[key].as<int>();
    if (value > 0) {
        target = value;
    }
}

void assignNonNegativeInt(const YAML::Node& node, const char* key, int& target) {
    if (!node[key]) {
        return;
    }

    const int value = node[key].as<int>();
    if (value >= 0) {
        target = value;
    }
}

} // namespace

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string displayDecodeBackend(const std::string& backend) {
    const std::string normalized = toLower(backend);
    return normalized == "cpu" ? "CPU" : toUpper(normalized);
}

bool parseLogLevel(const std::string& value, spdlog::level::level_enum& level) {
    const std::string normalized = toLower(value);

    if (normalized == "trace") {
        level = spdlog::level::trace;
    } else if (normalized == "debug") {
        level = spdlog::level::debug;
    } else if (normalized == "info") {
        level = spdlog::level::info;
    } else if (normalized == "warn" || normalized == "warning") {
        level = spdlog::level::warn;
    } else if (normalized == "error") {
        level = spdlog::level::err;
    } else if (normalized == "critical") {
        level = spdlog::level::critical;
    } else if (normalized == "off") {
        level = spdlog::level::off;
    } else {
        return false;
    }

    return true;
}

std::string describeFilters(const std::vector<std::string>& filters) {
    if (filters.empty()) {
        return "none";
    }

    std::string result;
    for (const std::string& filter : filters) {
        if (!result.empty()) {
            result += " -> ";
        }
        result += filter;
    }
    return result;
}

std::string describeLowLatency(const RtspConnectionOptions& options) {
    if (!options.lowLatency) {
        return "disabled";
    }

    return "enabled, max_delay_ms=" + std::to_string(options.maxDelayMs) +
           ", analyze_duration_ms=" + std::to_string(options.analyzeDurationMs) +
           ", probe_size_bytes=" + std::to_string(options.probeSizeBytes) +
           ", reorder_queue_size=" + std::to_string(options.reorderQueueSize);
}

AppConfig loadAppConfig(const std::string& path) {
    AppConfig config;

    try {
        YAML::Node root = YAML::LoadFile(path);
        if (root["rtsp_url"]) {
            config.rtspUrl = root["rtsp_url"].as<std::string>();
        }
        if (root["rtsp_urls"] && root["rtsp_urls"].IsSequence()) {
            config.rtspUrls.clear();
            for (const auto& urlNode : root["rtsp_urls"]) {
                const std::string url = urlNode.as<std::string>();
                if (!url.empty()) {
                    config.rtspUrls.push_back(url);
                }
            }
            config.rtspUrlsConfigured = !config.rtspUrls.empty();
            if (config.rtspUrlsConfigured) {
                config.rtspUrl = config.rtspUrls.front();
            }
        }
        if (root["audio_rtsp_url"]) {
            config.audioRtspUrl = root["audio_rtsp_url"].as<std::string>();
        }
        if (root["width"]) {
            config.width = root["width"].as<int>();
        }
        if (root["height"]) {
            config.height = root["height"].as<int>();
        }
        if (root["log_level"]) {
            config.logLevelName = root["log_level"].as<std::string>();
        }
        if (root["renderer"]) {
            config.rendererName = root["renderer"].as<std::string>();
        }
        if (root["hw_decode"]) {
            config.hwDecodeBackend = root["hw_decode"].as<std::string>();
        }
        if (root["rtsp"]) {
            const auto rtspConfig = root["rtsp"];
            if (rtspConfig["transport"]) {
                config.rtspOptions.transport = rtspConfig["transport"].as<std::string>();
            }
            assignPositiveInt(rtspConfig, "timeout_ms", config.rtspOptions.timeoutMs);
            assignNonNegativeInt(rtspConfig, "buffer_size", config.rtspOptions.bufferSize);

            if (rtspConfig["low_latency"]) {
                const auto lowLatencyConfig = rtspConfig["low_latency"];
                if (lowLatencyConfig.IsScalar()) {
                    config.rtspOptions.lowLatency = lowLatencyConfig.as<bool>();
                } else {
                    if (lowLatencyConfig["enabled"]) {
                        config.rtspOptions.lowLatency = lowLatencyConfig["enabled"].as<bool>();
                    }
                    assignNonNegativeInt(lowLatencyConfig, "max_delay_ms",
                                         config.rtspOptions.maxDelayMs);
                    assignNonNegativeInt(lowLatencyConfig, "analyze_duration_ms",
                                         config.rtspOptions.analyzeDurationMs);
                    assignNonNegativeInt(lowLatencyConfig, "probe_size_bytes",
                                         config.rtspOptions.probeSizeBytes);
                    assignNonNegativeInt(lowLatencyConfig, "reorder_queue_size",
                                         config.rtspOptions.reorderQueueSize);
                }
            }
        }
        if (root["opengl_filters"] && root["opengl_filters"].IsSequence()) {
            config.openglFilterNames.clear();
            for (const auto& filterNode : root["opengl_filters"]) {
                config.openglFilterNames.push_back(filterNode.as<std::string>());
            }
        } else if (root["opengl_filter"]) {
            config.openglFilterNames = {root["opengl_filter"].as<std::string>()};
        }
        if (root["jitter_buffer"]) {
            const auto jitterConfig = root["jitter_buffer"];
            if (jitterConfig["max_size"]) {
                const int configuredMaxSize = jitterConfig["max_size"].as<int>();
                if (configuredMaxSize > 0) {
                    config.jitterMaxSize = static_cast<size_t>(configuredMaxSize);
                }
            }
            if (jitterConfig["latency_ms"]) {
                const int configuredLatencyMs = jitterConfig["latency_ms"].as<int>();
                if (configuredLatencyMs >= 0) {
                    config.jitterLatencyMs = static_cast<uint32_t>(configuredLatencyMs);
                }
            }
        }
        if (root["audio"]) {
            const auto audioConfig = root["audio"];
            if (audioConfig["enabled"]) {
                config.audioOptions.enabled = audioConfig["enabled"].as<bool>();
            }
            assignNonNegativeInt(audioConfig, "target_latency_ms",
                                 config.audioOptions.targetLatencyMs);
            assignPositiveInt(audioConfig, "max_queue_ms", config.audioOptions.maxQueueMs);
            assignPositiveInt(audioConfig, "hard_reset_queue_ms",
                              config.audioOptions.hardResetQueueMs);
        }
        if (root["sync"]) {
            const auto syncConfig = root["sync"];
            if (syncConfig["enabled"]) {
                config.syncOptions.enabled = syncConfig["enabled"].as<bool>();
            }
            assignNonNegativeInt(syncConfig, "max_wait_ms", config.syncOptions.maxWaitMs);
            assignPositiveInt(syncConfig, "late_drop_ms", config.syncOptions.lateDropMs);
            if (syncConfig["audio_offset_ms"]) {
                config.syncOptions.audioOffsetMs = syncConfig["audio_offset_ms"].as<int>();
            }
        }
        if (root["face_detection"]) {
            const auto faceConfig = root["face_detection"];
            if (faceConfig["enabled"]) {
                config.faceDetectionOptions.enabled = faceConfig["enabled"].as<bool>();
            }
            if (faceConfig["backend"]) {
                config.faceDetectionOptions.backend = faceConfig["backend"].as<std::string>();
            }
            if (faceConfig["model"]) {
                config.faceDetectionOptions.modelPath = faceConfig["model"].as<std::string>();
            }
            assignPositiveInt(faceConfig, "input_width",
                              config.faceDetectionOptions.inputWidth);
            assignPositiveInt(faceConfig, "input_height",
                              config.faceDetectionOptions.inputHeight);
            assignPositiveInt(faceConfig, "detect_every_n_frames",
                              config.faceDetectionOptions.detectEveryNFrames);
            if (faceConfig["score_threshold"]) {
                const float value = faceConfig["score_threshold"].as<float>();
                if (value >= 0.0F && value <= 1.0F) {
                    config.faceDetectionOptions.scoreThreshold = value;
                }
            }
            if (faceConfig["nms_threshold"]) {
                const float value = faceConfig["nms_threshold"].as<float>();
                if (value >= 0.0F && value <= 1.0F) {
                    config.faceDetectionOptions.nmsThreshold = value;
                }
            }
        }
        if (root["reconnect"]) {
            const auto reconnectConfig = root["reconnect"];
            if (reconnectConfig["enabled"]) {
                config.reconnectOptions.enabled = reconnectConfig["enabled"].as<bool>();
            }
            if (reconnectConfig["initial_delay_ms"]) {
                const int configuredInitialDelay =
                    reconnectConfig["initial_delay_ms"].as<int>();
                if (configuredInitialDelay >= 0) {
                    config.reconnectOptions.initialDelayMs =
                        static_cast<uint32_t>(configuredInitialDelay);
                }
            }
            if (reconnectConfig["max_delay_ms"]) {
                const int configuredMaxDelay = reconnectConfig["max_delay_ms"].as<int>();
                if (configuredMaxDelay >= 0) {
                    config.reconnectOptions.maxDelayMs =
                        static_cast<uint32_t>(configuredMaxDelay);
                }
            }
        }
    } catch (const std::exception& e) {
        config.warning = e.what();
    }

    return config;
}

void applyCommandLineOverrides(AppConfig& config, int argc, char* argv[]) {
    if (argc > 2) {
        config.rtspUrls = {argv[1], argv[2]};
        config.rtspUrl = config.rtspUrls.front();
    } else if (argc > 1) {
        config.rtspUrl = argv[1];
        if (config.rtspUrlsConfigured && !config.rtspUrls.empty()) {
            config.rtspUrls[0] = config.rtspUrl;
        } else {
            config.rtspUrls = {config.rtspUrl};
        }
    }

    if (config.rtspUrls.empty()) {
        config.rtspUrls.push_back(config.rtspUrl);
    }
}

} // namespace rtsp
