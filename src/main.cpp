#include "rtsp_client.hpp"
#include "video_renderer.hpp"
#include "jitter_buffer.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace {

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
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

} // namespace

// 全局变量用于控制主循环
static bool g_running = true;

int main(int argc, char* argv[]) {
    // 设置日志
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    // 加载配置
    std::string rtspUrl = "rtsp://127.0.0.1:8554/webcam";
    int width = 1920;
    int height = 1080;
    std::string logLevelName = "info";
    std::string rendererName = "sdl";
    std::vector<std::string> openglFilterNames = {"none"};
    size_t jitterMaxSize = 30;
    uint32_t jitterLatencyMs = 100;
    bool reconnectEnabled = true;
    uint32_t reconnectInitialDelayMs = 1000;
    uint32_t reconnectMaxDelayMs = 5000;
    std::string configWarning;

    // 尝试加载配置文件
    try {
        YAML::Node config = YAML::LoadFile("config/config.yaml");
        if (config["rtsp_url"]) {
            rtspUrl = config["rtsp_url"].as<std::string>();
        }
        if (config["width"]) {
            width = config["width"].as<int>();
        }
        if (config["height"]) {
            height = config["height"].as<int>();
        }
        if (config["log_level"]) {
            logLevelName = config["log_level"].as<std::string>();
        }
        if (config["renderer"]) {
            rendererName = config["renderer"].as<std::string>();
        }
        if (config["opengl_filters"] && config["opengl_filters"].IsSequence()) {
            openglFilterNames.clear();
            for (const auto& filterNode : config["opengl_filters"]) {
                openglFilterNames.push_back(filterNode.as<std::string>());
            }
        } else if (config["opengl_filter"]) {
            openglFilterNames = {config["opengl_filter"].as<std::string>()};
        }
        if (config["jitter_buffer"]) {
            const auto jitterConfig = config["jitter_buffer"];
            if (jitterConfig["max_size"]) {
                int configuredMaxSize = jitterConfig["max_size"].as<int>();
                if (configuredMaxSize > 0) {
                    jitterMaxSize = static_cast<size_t>(configuredMaxSize);
                }
            }
            if (jitterConfig["latency_ms"]) {
                int configuredLatencyMs = jitterConfig["latency_ms"].as<int>();
                if (configuredLatencyMs >= 0) {
                    jitterLatencyMs = static_cast<uint32_t>(configuredLatencyMs);
                }
            }
        }
        if (config["reconnect"]) {
            const auto reconnectConfig = config["reconnect"];
            if (reconnectConfig["enabled"]) {
                reconnectEnabled = reconnectConfig["enabled"].as<bool>();
            }
            if (reconnectConfig["initial_delay_ms"]) {
                int configuredInitialDelay = reconnectConfig["initial_delay_ms"].as<int>();
                if (configuredInitialDelay >= 0) {
                    reconnectInitialDelayMs = static_cast<uint32_t>(configuredInitialDelay);
                }
            }
            if (reconnectConfig["max_delay_ms"]) {
                int configuredMaxDelay = reconnectConfig["max_delay_ms"].as<int>();
                if (configuredMaxDelay >= 0) {
                    reconnectMaxDelayMs = static_cast<uint32_t>(configuredMaxDelay);
                }
            }
        }
    } catch (const std::exception& e) {
        configWarning = e.what();
    }

    spdlog::level::level_enum parsedLogLevel = spdlog::level::info;
    if (!parseLogLevel(logLevelName, parsedLogLevel)) {
        SPDLOG_WARN("Unknown log level '{}', using info", logLevelName);
    }
    spdlog::set_level(parsedLogLevel);

    SPDLOG_INFO("RTSP Player starting...");
    if (!configWarning.empty()) {
        SPDLOG_WARN("Config file not found or invalid, using defaults: {}", configWarning);
    }

    // 如果有命令行参数，使用命令行参数
    if (argc > 1) {
        rtspUrl = argv[1];
    }

    SPDLOG_INFO("RTSP URL: {}", rtspUrl);
    SPDLOG_INFO("Renderer backend: {}", rendererName);
    SPDLOG_INFO("OpenGL shader pipeline: {}", describeFilters(openglFilterNames));
    SPDLOG_INFO("Jitter buffer: max_size={}, latency_ms={}", jitterMaxSize, jitterLatencyMs);
    SPDLOG_INFO("Reconnect: enabled={}, initial_delay_ms={}, max_delay_ms={}",
                reconnectEnabled, reconnectInitialDelayMs, reconnectMaxDelayMs);

    // 创建组件
    auto rtspClient = std::make_unique<rtsp::RtspClient>();
    auto jitterBuffer = std::make_unique<rtsp::JitterBuffer>(jitterMaxSize, jitterLatencyMs);
    std::unique_ptr<rtsp::VideoRenderer> renderer;
    const std::string rendererBackend = toLower(rendererName);
    if (rendererBackend == "opengl" || rendererBackend == "gl") {
        renderer = rtsp::createOpenGlVideoRenderer(openglFilterNames);
    } else {
        if (rendererBackend != "sdl") {
            SPDLOG_WARN("Unknown renderer backend '{}', using sdl", rendererName);
        }
        renderer = rtsp::createSdlVideoRenderer();
    }

    // 设置帧回调
    rtspClient->setFrameCallback([&jitterBuffer](const std::shared_ptr<rtsp::MediaFrame>& frame) {
        jitterBuffer->push(frame);
    });

    // 设置错误回调
    rtspClient->setErrorCallback([](const std::string& error) {
        SPDLOG_ERROR("RTSP Error: {}", error);
    });

    auto waitBeforeReconnect = [&](uint32_t delayMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        while (g_running && std::chrono::steady_clock::now() < deadline) {
            if (renderer->isInitialized() && !renderer->handleEvents()) {
                g_running = false;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return g_running;
    };

    auto connectStream = [&](bool reconnecting) {
        uint32_t delayMs = reconnectInitialDelayMs;

        while (g_running) {
            if (reconnecting) {
                SPDLOG_INFO("Reconnecting to RTSP stream...");
            }

            if (rtspClient->connect(rtspUrl)) {
                const int newWidth = rtspClient->getWidth();
                const int newHeight = rtspClient->getHeight();
                SPDLOG_INFO("Video resolution: {}x{}", newWidth, newHeight);

                if (!renderer->isInitialized() ||
                    newWidth != renderer->getWidth() ||
                    newHeight != renderer->getHeight()) {
                    if (!renderer->initialize(newWidth, newHeight, "RTSP Player")) {
                        SPDLOG_ERROR("Failed to initialize renderer");
                        rtspClient->disconnect();
                        return false;
                    }
                }

                width = newWidth;
                height = newHeight;
                jitterBuffer->clear();
                rtspClient->start();
                SPDLOG_INFO("Streaming started, press ESC or Q to quit");
                return true;
            }

            if (!reconnectEnabled) {
                SPDLOG_ERROR("Failed to connect to RTSP stream");
                return false;
            }

            SPDLOG_WARN("RTSP connect failed, retrying in {} ms", delayMs);
            if (!waitBeforeReconnect(delayMs)) {
                return false;
            }

            const uint32_t nextDelay = delayMs == 0 ? 1 : delayMs * 2;
            delayMs = std::min(nextDelay, reconnectMaxDelayMs);
        }

        return false;
    };

    if (!connectStream(false)) {
        return -1;
    }

    // 主渲染循环
    std::shared_ptr<rtsp::MediaFrame> frame;
    rtsp::PlaybackStats playbackStats;
    uint64_t renderedFramesSinceFpsUpdate = 0;
    auto lastFpsUpdateTime = std::chrono::steady_clock::now();

    while (g_running && renderer->handleEvents()) {
        if (!rtspClient->isRunning()) {
            SPDLOG_WARN("RTSP receive loop stopped");
            rtspClient->stop();
            rtspClient->disconnect();
            jitterBuffer->clear();

            playbackStats = {};
            renderedFramesSinceFpsUpdate = 0;
            lastFpsUpdateTime = std::chrono::steady_clock::now();

            if (!reconnectEnabled || !connectStream(true)) {
                break;
            }

            continue;
        }

        // 从jitter buffer获取帧
        if (jitterBuffer->pop(frame, 10)) {
            const auto now = std::chrono::steady_clock::now();
            if (frame->recvTime.count() > 0) {
                const auto frameRecvTime = std::chrono::steady_clock::time_point(frame->recvTime);
                playbackStats.latencyMs = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - frameRecvTime).count());
            }

            const auto jitterStats = jitterBuffer->getStats();
            playbackStats.decodedFrames = jitterStats.totalFrames;
            playbackStats.droppedFrames = jitterStats.droppedFrames;
            playbackStats.jitterBufferSize = jitterStats.bufferSize;

            renderer->setPlaybackStats(playbackStats);
            if (renderer->render(frame)) {
                ++renderedFramesSinceFpsUpdate;
            }

            const auto elapsed = now - lastFpsUpdateTime;
            if (elapsed >= std::chrono::seconds(1)) {
                const double elapsedSeconds =
                    std::chrono::duration<double>(elapsed).count();
                playbackStats.fps =
                    static_cast<double>(renderedFramesSinceFpsUpdate) / elapsedSeconds;
                renderedFramesSinceFpsUpdate = 0;
                lastFpsUpdateTime = now;
            }
        }

        // 短暂休眠避免CPU占用过高
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 停止接收
    rtspClient->stop();
    rtspClient->disconnect();

    SPDLOG_INFO("RTSP Player stopped");
    return 0;
}
