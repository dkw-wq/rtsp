#include "rtsp_client.hpp"
#include "video_renderer.hpp"
#include "jitter_buffer.hpp"
#include "audio_player.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

namespace {

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string toUpper(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
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

std::string describeLowLatency(const rtsp::RtspConnectionOptions& options) {
    if (!options.lowLatency) {
        return "disabled";
    }

    return "enabled, max_delay_ms=" + std::to_string(options.maxDelayMs) +
           ", analyze_duration_ms=" + std::to_string(options.analyzeDurationMs) +
           ", probe_size_bytes=" + std::to_string(options.probeSizeBytes) +
           ", reorder_queue_size=" + std::to_string(options.reorderQueueSize);
}

struct SyncOptions {
    bool enabled = true;
    int maxWaitMs = 16;
    int lateDropMs = 250;
};

struct SyncState {
    bool initialized = false;
    double videoBaseSeconds = 0.0;
    double audioBaseSeconds = 0.0;
    bool audioBaseInitialized = false;
    std::chrono::steady_clock::time_point wallBaseTime{};
};

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
    std::string hwDecodeBackend = "none";
    std::vector<std::string> openglFilterNames = {"none"};
    rtsp::RtspConnectionOptions rtspOptions;
    rtsp::AudioPlaybackOptions audioOptions;
    SyncOptions syncOptions;
    size_t jitterMaxSize = 12;
    uint32_t jitterLatencyMs = 30;
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
        if (config["hw_decode"]) {
            hwDecodeBackend = config["hw_decode"].as<std::string>();
        }
        if (config["rtsp"]) {
            const auto rtspConfig = config["rtsp"];
            if (rtspConfig["transport"]) {
                rtspOptions.transport = rtspConfig["transport"].as<std::string>();
            }
            assignPositiveInt(rtspConfig, "timeout_ms", rtspOptions.timeoutMs);
            assignNonNegativeInt(rtspConfig, "buffer_size", rtspOptions.bufferSize);

            if (rtspConfig["low_latency"]) {
                const auto lowLatencyConfig = rtspConfig["low_latency"];
                if (lowLatencyConfig.IsScalar()) {
                    rtspOptions.lowLatency = lowLatencyConfig.as<bool>();
                } else {
                    if (lowLatencyConfig["enabled"]) {
                        rtspOptions.lowLatency = lowLatencyConfig["enabled"].as<bool>();
                    }
                    assignNonNegativeInt(lowLatencyConfig, "max_delay_ms", rtspOptions.maxDelayMs);
                    assignNonNegativeInt(lowLatencyConfig, "analyze_duration_ms", rtspOptions.analyzeDurationMs);
                    assignNonNegativeInt(lowLatencyConfig, "probe_size_bytes", rtspOptions.probeSizeBytes);
                    assignNonNegativeInt(lowLatencyConfig, "reorder_queue_size", rtspOptions.reorderQueueSize);
                }
            }
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
        if (config["audio"]) {
            const auto audioConfig = config["audio"];
            if (audioConfig["enabled"]) {
                audioOptions.enabled = audioConfig["enabled"].as<bool>();
            }
            assignNonNegativeInt(audioConfig, "target_latency_ms", audioOptions.targetLatencyMs);
            assignPositiveInt(audioConfig, "max_queue_ms", audioOptions.maxQueueMs);
            assignPositiveInt(audioConfig, "hard_reset_queue_ms", audioOptions.hardResetQueueMs);
        }
        if (config["sync"]) {
            const auto syncConfig = config["sync"];
            if (syncConfig["enabled"]) {
                syncOptions.enabled = syncConfig["enabled"].as<bool>();
            }
            assignNonNegativeInt(syncConfig, "max_wait_ms", syncOptions.maxWaitMs);
            assignPositiveInt(syncConfig, "late_drop_ms", syncOptions.lateDropMs);
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

    const std::string rendererBackend = toLower(rendererName);
    const bool usesOpenGlRenderer = rendererBackend == "opengl" || rendererBackend == "gl";
    const bool usesVulkanRenderer = rendererBackend == "vulkan" || rendererBackend == "vk";

    SPDLOG_INFO("RTSP URL: {}", rtspUrl);
    SPDLOG_INFO("Renderer backend: {}", rendererName);
    SPDLOG_INFO("Hardware decode: {}", hwDecodeBackend);
    SPDLOG_INFO("RTSP options: transport={}, timeout_ms={}, buffer_size={}, low_latency={}",
                rtspOptions.transport, rtspOptions.timeoutMs, rtspOptions.bufferSize,
                describeLowLatency(rtspOptions));
    SPDLOG_INFO("OpenGL shader pipeline: {}", describeFilters(openglFilterNames));
    SPDLOG_INFO("Jitter buffer: max_size={}, latency_ms={}", jitterMaxSize, jitterLatencyMs);
    SPDLOG_INFO("Audio: enabled={}, target_latency_ms={}, max_queue_ms={}, hard_reset_queue_ms={}",
                audioOptions.enabled, audioOptions.targetLatencyMs,
                audioOptions.maxQueueMs, audioOptions.hardResetQueueMs);
    SPDLOG_INFO("A/V sync: enabled={}, max_wait_ms={}, late_drop_ms={}",
                syncOptions.enabled, syncOptions.maxWaitMs, syncOptions.lateDropMs);
    SPDLOG_INFO("Reconnect: enabled={}, initial_delay_ms={}, max_delay_ms={}",
                reconnectEnabled, reconnectInitialDelayMs, reconnectMaxDelayMs);

    // 创建组件
    auto rtspClient = std::make_unique<rtsp::RtspClient>();
    rtspClient->setConnectionOptions(rtspOptions);
    rtspClient->setAudioEnabled(audioOptions.enabled);
    rtspClient->setHardwareDecode(hwDecodeBackend);
    auto jitterBuffer = std::make_unique<rtsp::JitterBuffer>(jitterMaxSize, jitterLatencyMs);
    auto audioPlayer = std::make_unique<rtsp::AudioPlayer>(audioOptions);
    std::unique_ptr<rtsp::VideoRenderer> renderer;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    rtspClient->setHardwareFrameOutput(usesOpenGlRenderer || usesVulkanRenderer);
#else
    rtspClient->setHardwareFrameOutput(false);
#endif
    if (usesOpenGlRenderer) {
        renderer = rtsp::createOpenGlVideoRenderer(openglFilterNames);
    } else if (usesVulkanRenderer) {
        renderer = rtsp::createVulkanVideoRenderer();
    } else {
        if (rendererBackend != "sdl") {
            SPDLOG_WARN("Unknown renderer backend '{}', using sdl", rendererName);
        }
        renderer = rtsp::createSdlVideoRenderer();
    }

    // 设置帧回调
    rtspClient->setFrameCallback([&jitterBuffer, &audioPlayer](const std::shared_ptr<rtsp::MediaFrame>& frame) {
        if (!frame) {
            return;
        }

        if (frame->type == rtsp::MediaFrame::Type::AUDIO) {
            audioPlayer->pushFrame(frame);
            return;
        }

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
                audioPlayer->reset();
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
    std::shared_ptr<rtsp::MediaFrame> pendingVideoFrame;
    rtsp::PlaybackStats playbackStats;
    playbackStats.decoderBackend = displayDecodeBackend(rtspClient->getDecodeBackend());
    playbackStats.hardwareDecodeStatus = rtspClient->getHardwareDecodeStatus();
    uint64_t renderedFramesSinceFpsUpdate = 0;
    uint64_t syncDroppedFrames = 0;
    SyncState syncState;
    auto lastFpsUpdateTime = std::chrono::steady_clock::now();
    auto updateFrameLatency = [](const std::shared_ptr<rtsp::MediaFrame>& currentFrame,
                                 rtsp::PlaybackStats& stats) {
        if (!currentFrame || currentFrame->recvTime.count() <= 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto frameRecvTime = std::chrono::steady_clock::time_point(currentFrame->recvTime);
        stats.latencyMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - frameRecvTime).count());
    };
    auto dropReadyVideoFrames = [&jitterBuffer]() {
        uint64_t dropped = 0;
        std::shared_ptr<rtsp::MediaFrame> staleFrame;
        while (jitterBuffer->pop(staleFrame, 0)) {
            ++dropped;
        }
        return dropped;
    };

    while (g_running && renderer->handleEvents()) {
        if (!rtspClient->isRunning()) {
            SPDLOG_WARN("RTSP receive loop stopped");
            rtspClient->stop();
            rtspClient->disconnect();
            jitterBuffer->clear();
            audioPlayer->reset();
            pendingVideoFrame.reset();

            playbackStats = {};
            playbackStats.decoderBackend = "CPU";
            playbackStats.hardwareDecodeStatus = rtspClient->getHardwareDecodeStatus();
            renderedFramesSinceFpsUpdate = 0;
            syncDroppedFrames = 0;
            syncState = {};
            lastFpsUpdateTime = std::chrono::steady_clock::now();

            if (!reconnectEnabled || !connectStream(true)) {
                break;
            }

            continue;
        }

        if (!pendingVideoFrame) {
            std::shared_ptr<rtsp::MediaFrame> nextFrame;
            if (jitterBuffer->pop(nextFrame, 1)) {
                pendingVideoFrame = nextFrame;
            }
        }

        if (pendingVideoFrame) {
            frame = pendingVideoFrame;
            updateFrameLatency(frame, playbackStats);

            const auto jitterStats = jitterBuffer->getStats();
            playbackStats.decoderBackend = displayDecodeBackend(rtspClient->getDecodeBackend());
            playbackStats.hardwareDecodeStatus = rtspClient->getHardwareDecodeStatus();
            playbackStats.decodedFrames = jitterStats.totalFrames;
            playbackStats.droppedFrames = jitterStats.droppedFrames;
            playbackStats.syncDroppedFrames = syncDroppedFrames;
            playbackStats.jitterBufferSize = jitterStats.bufferSize;
            const auto audioStats = audioPlayer->getStats();
            playbackStats.audioActive = audioStats.active;
            playbackStats.audioQueueMs = audioStats.queuedMs;

            if (syncOptions.enabled &&
                audioStats.sampleRate > 0 &&
                !audioStats.active &&
                audioStats.queuedMs < audioStats.targetLatencyMs) {
                ++syncDroppedFrames;
                syncDroppedFrames += dropReadyVideoFrames();
                playbackStats.syncDroppedFrames = syncDroppedFrames;
                pendingVideoFrame.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!renderer->handleEvents()) {
                    g_running = false;
                    break;
                }
                continue;
            }

            if (syncOptions.enabled && frame->ptsSeconds > 0.0) {
                if (!syncState.initialized) {
                    syncState.initialized = true;
                    syncState.videoBaseSeconds = frame->ptsSeconds;
                    syncState.wallBaseTime = std::chrono::steady_clock::now();
                    SPDLOG_INFO("Video sync base: video={:.3f}s",
                                syncState.videoBaseSeconds);
                }

                const auto wallElapsedSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - syncState.wallBaseTime).count();
                double videoDelayMs =
                    ((frame->ptsSeconds - syncState.videoBaseSeconds) - wallElapsedSeconds) * 1000.0;

                if (audioPlayer->hasClock()) {
                    const double audioClock = audioPlayer->clockSeconds();
                    if (!syncState.audioBaseInitialized) {
                        syncState.audioBaseInitialized = true;
                        syncState.audioBaseSeconds = audioClock;
                        SPDLOG_INFO("A/V sync base: video={:.3f}s, audio={:.3f}s",
                                    syncState.videoBaseSeconds, syncState.audioBaseSeconds);
                    }

                    auto frameAudioDiffMs = [&syncState, audioClock](
                                                const std::shared_ptr<rtsp::MediaFrame>& currentFrame) {
                        return ((currentFrame->ptsSeconds - syncState.videoBaseSeconds) -
                                (audioClock - syncState.audioBaseSeconds)) * 1000.0;
                    };

                    double avDiffMs = frameAudioDiffMs(frame);
                    bool waitingForNewerFrame = false;

                    while (syncOptions.lateDropMs > 0 &&
                           avDiffMs < -static_cast<double>(syncOptions.lateDropMs)) {
                        ++syncDroppedFrames;

                        std::shared_ptr<rtsp::MediaFrame> catchUpFrame;
                        if (!jitterBuffer->pop(catchUpFrame, 0)) {
                            pendingVideoFrame.reset();
                            waitingForNewerFrame = true;
                            break;
                        }

                        pendingVideoFrame = catchUpFrame;
                        frame = pendingVideoFrame;
                        updateFrameLatency(frame, playbackStats);
                        avDiffMs = frameAudioDiffMs(frame);
                    }

                    playbackStats.avSyncDiffMs = static_cast<int32_t>(std::lround(avDiffMs));
                    playbackStats.syncDroppedFrames = syncDroppedFrames;

                    if (waitingForNewerFrame) {
                        continue;
                    }

                    if (avDiffMs > 1.0 && syncOptions.maxWaitMs > 0) {
                        const int waitMs =
                            std::clamp(static_cast<int>(std::ceil(avDiffMs)), 1, syncOptions.maxWaitMs);
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        if (!renderer->handleEvents()) {
                            g_running = false;
                            break;
                        }

                        continue;
                    }
                } else {
                    playbackStats.avSyncDiffMs = 0;
                    syncState.audioBaseInitialized = false;

                    if (videoDelayMs > 1.0 && syncOptions.maxWaitMs > 0) {
                        const int waitMs =
                            std::clamp(static_cast<int>(std::ceil(videoDelayMs)), 1, syncOptions.maxWaitMs);
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        if (!renderer->handleEvents()) {
                            g_running = false;
                            break;
                        }

                        continue;
                    }

                    bool waitingForNewerFrame = false;
                    while (syncOptions.lateDropMs > 0 &&
                           videoDelayMs < -static_cast<double>(syncOptions.lateDropMs)) {
                        ++syncDroppedFrames;

                        std::shared_ptr<rtsp::MediaFrame> catchUpFrame;
                        if (!jitterBuffer->pop(catchUpFrame, 0)) {
                            pendingVideoFrame.reset();
                            waitingForNewerFrame = true;
                            break;
                        }

                        pendingVideoFrame = catchUpFrame;
                        frame = pendingVideoFrame;
                        updateFrameLatency(frame, playbackStats);

                        const auto updatedWallElapsedSeconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - syncState.wallBaseTime).count();
                        videoDelayMs =
                            ((frame->ptsSeconds - syncState.videoBaseSeconds) -
                             updatedWallElapsedSeconds) * 1000.0;
                    }

                    playbackStats.syncDroppedFrames = syncDroppedFrames;
                    if (waitingForNewerFrame) {
                        continue;
                    }
                }
            } else {
                playbackStats.avSyncDiffMs = 0;
                if (!syncOptions.enabled || !audioPlayer->hasClock()) {
                    syncState = {};
                }
            }

            const auto updatedJitterStats = jitterBuffer->getStats();
            playbackStats.decodedFrames = updatedJitterStats.totalFrames;
            playbackStats.droppedFrames = updatedJitterStats.droppedFrames;
            playbackStats.jitterBufferSize = updatedJitterStats.bufferSize;
            playbackStats.syncDroppedFrames = syncDroppedFrames;
            renderer->setPlaybackStats(playbackStats);
            if (renderer->render(frame)) {
                ++renderedFramesSinceFpsUpdate;
            }
            pendingVideoFrame.reset();

            const auto now = std::chrono::steady_clock::now();
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
    audioPlayer->reset();

    SPDLOG_INFO("RTSP Player stopped");
    return 0;
}
