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
    std::string openglFilterName = "none";
    size_t jitterMaxSize = 30;
    uint32_t jitterLatencyMs = 100;
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
        if (config["opengl_filter"]) {
            openglFilterName = config["opengl_filter"].as<std::string>();
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
    SPDLOG_INFO("OpenGL filter: {}", openglFilterName);
    SPDLOG_INFO("Jitter buffer: max_size={}, latency_ms={}", jitterMaxSize, jitterLatencyMs);

    // 创建组件
    auto rtspClient = std::make_unique<rtsp::RtspClient>();
    auto jitterBuffer = std::make_unique<rtsp::JitterBuffer>(jitterMaxSize, jitterLatencyMs);
    std::unique_ptr<rtsp::VideoRenderer> renderer;
    const std::string rendererBackend = toLower(rendererName);
    if (rendererBackend == "opengl" || rendererBackend == "gl") {
        renderer = rtsp::createOpenGlVideoRenderer(openglFilterName);
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

    // 连接RTSP流
    if (!rtspClient->connect(rtspUrl)) {
        SPDLOG_ERROR("Failed to connect to RTSP stream");
        return -1;
    }

    // 获取视频参数
    width = rtspClient->getWidth();
    height = rtspClient->getHeight();

    SPDLOG_INFO("Video resolution: {}x{}", width, height);

    // 初始化渲染器
    if (!renderer->initialize(width, height, "RTSP Player")) {
        SPDLOG_ERROR("Failed to initialize renderer");
        rtspClient->disconnect();
        return -1;
    }

    // 开始接收流
    rtspClient->start();

    SPDLOG_INFO("Streaming started, press ESC or Q to quit");

    // 主渲染循环
    std::shared_ptr<rtsp::MediaFrame> frame;
    rtsp::PlaybackStats playbackStats;
    uint64_t renderedFramesSinceFpsUpdate = 0;
    auto lastFpsUpdateTime = std::chrono::steady_clock::now();

    while (g_running && renderer->handleEvents()) {
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
