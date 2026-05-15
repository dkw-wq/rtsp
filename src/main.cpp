#include "config_loader.hpp"
#include "face_analyzer.hpp"
#include "stream_session.hpp"
#include "sync_controller.hpp"
#include "video_renderer.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

static bool g_running = true;

std::string makeTimestampedLogFileName() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << "rtsp_player_" << std::put_time(&localTime, "%Y%m%d_%H%M%S") << '_'
           << std::setw(3) << std::setfill('0') << milliseconds.count() << ".log";
    return stream.str();
}

std::filesystem::path initializeLogging() {
    const auto logDir = std::filesystem::path("logs");
    const auto logFile = logDir / makeTimestampedLogFileName();

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    try {
        std::filesystem::create_directories(logDir);
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            logFile.string(), true);
        auto logger = std::make_shared<spdlog::logger>(
            "rtsp_player", spdlog::sinks_init_list{consoleSink, fileSink});
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::flush_on(spdlog::level::info);
        SPDLOG_INFO("Log file: {}", logFile.string());
        spdlog::default_logger()->flush();
        return logFile;
    } catch (const spdlog::spdlog_ex& ex) {
        auto logger = std::make_shared<spdlog::logger>("rtsp_player", consoleSink);
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::flush_on(spdlog::level::info);
        SPDLOG_WARN("Failed to open log file '{}': {}", logFile.string(), ex.what());
    } catch (const std::filesystem::filesystem_error& ex) {
        auto logger = std::make_shared<spdlog::logger>("rtsp_player", consoleSink);
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::flush_on(spdlog::level::info);
        SPDLOG_WARN("Failed to create log directory '{}': {}", logDir.string(), ex.what());
    }

    return {};
}

bool handleEventsDuringDelay(rtsp::VideoRenderer& renderer, uint32_t delayMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    while (g_running && std::chrono::steady_clock::now() < deadline) {
        if (renderer.isInitialized() && !renderer.handleEvents()) {
            g_running = false;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return g_running;
}

bool connectStreamWithRetry(rtsp::StreamSession& stream,
                            const rtsp::ReconnectOptions& reconnect,
                            rtsp::VideoRenderer& renderer,
                            bool reconnecting,
                            bool startAfterConnect) {
    uint32_t delayMs = reconnect.initialDelayMs;
    while (g_running) {
        if (reconnecting) {
            SPDLOG_INFO("Reconnecting RTSP stream {}...", stream.streamIndex() + 1);
        }

        if (stream.connect()) {
            if (startAfterConnect) {
                stream.start();
            }
            return true;
        }

        if (!reconnect.enabled) {
            SPDLOG_ERROR("Failed to connect RTSP stream {}", stream.streamIndex() + 1);
            return false;
        }

        SPDLOG_WARN("RTSP stream {} connect failed, retrying in {} ms",
                    stream.streamIndex() + 1, delayMs);
        if (!handleEventsDuringDelay(renderer, delayMs)) {
            return false;
        }

        const uint32_t nextDelay = delayMs == 0 ? 1 : delayMs * 2;
        delayMs = std::min(nextDelay, reconnect.maxDelayMs);
    }

    return false;
}

std::unique_ptr<rtsp::VideoRenderer> createRenderer(const rtsp::AppConfig& config,
                                                    bool forceVulkan) {
    const std::string rendererBackend = rtsp::toLower(config.rendererName);
    if (forceVulkan || rendererBackend == "vulkan" || rendererBackend == "vk") {
        return rtsp::createVulkanVideoRenderer();
    }
    if (rendererBackend == "opengl" || rendererBackend == "gl") {
        return rtsp::createOpenGlVideoRenderer(config.openglFilterNames);
    }

    if (rendererBackend != "sdl") {
        SPDLOG_WARN("Unknown renderer backend '{}', using sdl", config.rendererName);
    }
    return rtsp::createSdlVideoRenderer();
}

void logConfig(const rtsp::AppConfig& config) {
    SPDLOG_INFO("RTSP URL: {}", config.rtspUrl);
    SPDLOG_INFO("RTSP stream count: {}", config.rtspUrls.size());
    SPDLOG_INFO("Renderer backend: {}", config.rendererName);
    SPDLOG_INFO("Hardware decode: {}", config.hwDecodeBackend);
    SPDLOG_INFO("RTSP options: transport={}, timeout_ms={}, buffer_size={}, low_latency={}",
                config.rtspOptions.transport,
                config.rtspOptions.timeoutMs,
                config.rtspOptions.bufferSize,
                rtsp::describeLowLatency(config.rtspOptions));
    SPDLOG_INFO("OpenGL shader pipeline: {}", rtsp::describeFilters(config.openglFilterNames));
    SPDLOG_INFO("Jitter buffer: max_size={}, latency_ms={}",
                config.jitterMaxSize,
                config.jitterLatencyMs);
    SPDLOG_INFO("Audio: enabled={}, target_latency_ms={}, max_queue_ms={}, hard_reset_queue_ms={}",
                config.audioOptions.enabled,
                config.audioOptions.targetLatencyMs,
                config.audioOptions.maxQueueMs,
                config.audioOptions.hardResetQueueMs);
    SPDLOG_INFO("A/V sync: enabled={}, max_wait_ms={}, late_drop_ms={}, audio_offset_ms={}",
                config.syncOptions.enabled,
                config.syncOptions.maxWaitMs,
                config.syncOptions.lateDropMs,
                config.syncOptions.audioOffsetMs);
    SPDLOG_INFO("Reconnect: enabled={}, initial_delay_ms={}, max_delay_ms={}",
                config.reconnectOptions.enabled,
                config.reconnectOptions.initialDelayMs,
                config.reconnectOptions.maxDelayMs);
    SPDLOG_INFO("Face detection: enabled={}, backend={}, model={}, input={}x{}, every_n_frames={}",
                config.faceDetectionOptions.enabled,
                config.faceDetectionOptions.backend,
                config.faceDetectionOptions.modelPath,
                config.faceDetectionOptions.inputWidth,
                config.faceDetectionOptions.inputHeight,
                config.faceDetectionOptions.detectEveryNFrames);
}

int runSingleStream(const rtsp::AppConfig& config);

int runMultiStream(const rtsp::AppConfig& config) {
    const size_t requestedStreamCount = std::min<size_t>(config.rtspUrls.size(), 2);
    const std::string rendererBackend = rtsp::toLower(config.rendererName);
    const bool usesOpenGlRenderer = rendererBackend == "opengl" || rendererBackend == "gl";
    const bool usesVulkanRenderer = rendererBackend == "vulkan" || rendererBackend == "vk";
    if (!usesOpenGlRenderer && !usesVulkanRenderer) {
        SPDLOG_WARN("Multi-stream display supports OpenGL/Vulkan; using Vulkan instead of '{}'",
                    config.rendererName);
    }
    if (config.rtspUrls.size() > requestedStreamCount) {
        SPDLOG_WARN("Only the first {} RTSP streams are used in this build",
                    requestedStreamCount);
    }

    auto renderer = createRenderer(config, !usesOpenGlRenderer);
    auto audioPlayer = std::make_unique<rtsp::AudioPlayer>(config.audioOptions);
    auto faceAnalyzer = std::make_unique<rtsp::FaceAnalyzer>();
    faceAnalyzer->initialize(config.faceDetectionOptions);
    std::vector<std::unique_ptr<rtsp::StreamSession>> streams;
    streams.reserve(requestedStreamCount);

    for (size_t index = 0; index < requestedStreamCount; ++index) {
        rtsp::StreamSessionOptions options;
        options.url = config.rtspUrls[index];
        options.connectionOptions = config.rtspOptions;
        options.hardwareDecodeBackend = config.hwDecodeBackend;
        options.audioEnabled = false;
        options.videoEnabled = true;
        options.hardwareFrameOutput = false;
        options.jitterMaxSize = config.jitterMaxSize;
        options.jitterLatencyMs = config.jitterLatencyMs;
        options.streamIndex = index;
        streams.push_back(std::make_unique<rtsp::StreamSession>(options));
    }

    std::unique_ptr<rtsp::StreamSession> audioStream;
    const bool useSeparateAudio = config.audioOptions.enabled && !config.audioRtspUrl.empty();
    if (useSeparateAudio) {
        rtsp::StreamSessionOptions options;
        options.url = config.audioRtspUrl;
        options.connectionOptions = config.rtspOptions;
        options.hardwareDecodeBackend = "none";
        options.audioEnabled = true;
        options.videoEnabled = false;
        options.hardwareFrameOutput = false;
        options.forwardAudioToPlayer = true;
        options.audioPlayer = audioPlayer.get();
        options.jitterMaxSize = config.jitterMaxSize;
        options.jitterLatencyMs = config.jitterLatencyMs;
        options.streamIndex = requestedStreamCount;
        audioStream = std::make_unique<rtsp::StreamSession>(options);
    }

    if (streams.empty()) {
        SPDLOG_ERROR("No RTSP streams configured");
        return -1;
    }

    if (!connectStreamWithRetry(*streams.front(), config.reconnectOptions, *renderer, false, true)) {
        return -1;
    }

    for (size_t index = 1; index < streams.size();) {
        auto& stream = streams[index];
        if (stream->connect()) {
            stream->start();
            ++index;
            continue;
        }

        SPDLOG_WARN("Optional RTSP stream {} unavailable: {}; continuing with single-stream display",
                    index + 1,
                    config.rtspUrls[index]);
        stream->stopAndDisconnect();
        streams.erase(streams.begin() + static_cast<std::ptrdiff_t>(index));
    }

    if (streams.empty()) {
        SPDLOG_ERROR("No RTSP streams are available");
        return -1;
    }

    if (requestedStreamCount > 1 && streams.size() == 1) {
        SPDLOG_INFO("Dual-view fallback active: switching to single-stream pipeline");
        streams.front()->stopAndDisconnect();

        auto fallbackConfig = config;
        fallbackConfig.rtspUrl = config.rtspUrls.front();
        fallbackConfig.rtspUrls = {fallbackConfig.rtspUrl};
        fallbackConfig.rtspUrlsConfigured = false;
        return runSingleStream(fallbackConfig);
    }

    for (size_t index = 0; index < streams.size(); ++index) {
        if (!streams[index]->isRunning()) {
            SPDLOG_ERROR("RTSP stream {} is not running after initial connection", index + 1);
            return -1;
        }
    }

    if (audioStream) {
        if (audioStream->connect()) {
            audioStream->start();
            SPDLOG_INFO("Separate audio RTSP started: {}", config.audioRtspUrl);
        } else {
            SPDLOG_WARN("Separate audio RTSP unavailable: {}; continuing video-only",
                        config.audioRtspUrl);
        }
    }

    int windowWidth = 0;
    int windowHeight = 0;
    for (const auto& stream : streams) {
        windowWidth += std::max(stream->client().getWidth(), 1);
        windowHeight = std::max(windowHeight, std::max(stream->client().getHeight(), 1));
    }
    windowWidth = std::clamp(windowWidth, 640, 1920);
    windowHeight = std::clamp(windowHeight, 360, 1080);
    const std::string windowTitle =
        streams.size() > 1 ? "RTSP Player - Dual View" : "RTSP Player";
    if (!renderer->initialize(windowWidth, windowHeight, windowTitle)) {
        SPDLOG_ERROR("Failed to initialize multi-stream renderer");
        return -1;
    }

    SPDLOG_INFO("{} RTSP stream(s) started, press ESC or Q to quit", streams.size());

    while (g_running && renderer->handleEvents()) {
        bool hasNewVideoFrame = false;
        bool waitingForSync = false;

        for (size_t index = 0; index < streams.size(); ++index) {
            auto& stream = *streams[index];
            if (!stream.isRunning()) {
                SPDLOG_WARN("RTSP stream {} receive loop stopped", index + 1);
                stream.stopAndDisconnect();
                stream.resetBufferedFrames();
                if (index == 0) {
                    audioPlayer->reset();
                }

                if (!config.reconnectOptions.enabled ||
                    !connectStreamWithRetry(stream, config.reconnectOptions, *renderer, true, true)) {
                    g_running = false;
                    break;
                }
                continue;
            }

            std::shared_ptr<rtsp::MediaFrame> nextFrame;
            const bool syncOrderedPlayback =
                index == 0 && config.syncOptions.enabled && audioPlayer->hasClock();
            if (!stream.pendingFrame &&
                stream.jitterBuffer().pop(nextFrame, index == 0 ? 1 : 0)) {
                stream.pendingFrame = nextFrame;
                stream.noteInputFrame();
                if (!syncOrderedPlayback) {
                    while (stream.jitterBuffer().pop(nextFrame, 0)) {
                        if (stream.pendingFrame) {
                            ++stream.stats.syncDroppedFrames;
                        }
                        stream.pendingFrame = nextFrame;
                        stream.noteInputFrame();
                    }
                }
            }

            if (stream.pendingFrame) {
                if (index == 0 && config.syncOptions.enabled && audioPlayer->hasClock()) {
                    const int targetDelayMs =
                        config.audioOptions.targetLatencyMs + config.syncOptions.audioOffsetMs;
                    const auto now = std::chrono::steady_clock::now();
                    const auto targetTime =
                        rtsp::frameTargetTimeByReceiveTime(stream.pendingFrame, targetDelayMs);

                    if (now < targetTime) {
                        const int waitMs = std::clamp(
                            static_cast<int>(std::ceil(
                                std::chrono::duration<double, std::milli>(targetTime - now).count())),
                            1,
                            std::max(config.syncOptions.maxWaitMs, 1));
                        stream.stats.avSyncDiffMs = waitMs;
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        waitingForSync = true;
                        break;
                    }

                    const auto lateMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - targetTime).count();
                    if (config.syncOptions.lateDropMs > 0 &&
                        lateMs > static_cast<int64_t>(config.syncOptions.lateDropMs)) {
                        bool foundNewerFrame = false;
                        while (stream.jitterBuffer().pop(nextFrame, 0)) {
                            stream.pendingFrame = nextFrame;
                            stream.noteInputFrame();
                            ++stream.stats.syncDroppedFrames;
                            foundNewerFrame = true;
                        }

                        if (foundNewerFrame) {
                            const auto updatedTargetTime =
                                rtsp::frameTargetTimeByReceiveTime(stream.pendingFrame, targetDelayMs);
                            const auto updatedLateMs =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - updatedTargetTime).count();
                            if (updatedLateMs > static_cast<int64_t>(config.syncOptions.lateDropMs)) {
                                ++stream.stats.syncDroppedFrames;
                                stream.pendingFrame.reset();
                                continue;
                            }
                        }
                    }
                }

                stream.latestFrame = stream.pendingFrame;
                faceAnalyzer->submitFrame(stream.latestFrame, index);
                stream.pendingFrame.reset();
                hasNewVideoFrame = true;
            }

            stream.refreshStats();
            if (index == 0) {
                const auto audioStats = audioPlayer->getStats();
                stream.stats.audioActive = audioStats.active;
                stream.stats.audioQueueMs = audioStats.queuedMs;
            }
        }

        if (!g_running) {
            break;
        }
        if (waitingForSync) {
            continue;
        }

        std::vector<std::shared_ptr<rtsp::MediaFrame>> frames;
        frames.reserve(streams.size());
        bool hasAnyFrame = false;
        for (const auto& stream : streams) {
            frames.push_back(stream->latestFrame);
            hasAnyFrame = hasAnyFrame || static_cast<bool>(stream->latestFrame);
        }

        if (hasAnyFrame && hasNewVideoFrame) {
            renderer->setPlaybackStats(streams.front()->stats);
            renderer->setFaceOverlays(faceAnalyzer->latestResults());
            renderer->render(frames);

            for (auto& stream : streams) {
                stream->updateInputFpsIfDue();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (auto& stream : streams) {
        stream->stopAndDisconnect();
    }
    if (audioStream) {
        audioStream->stopAndDisconnect();
    }
    audioPlayer->reset();

    SPDLOG_INFO("Dual RTSP Player stopped");
    return 0;
}

int runSingleStream(const rtsp::AppConfig& config) {
    const std::string rendererBackend = rtsp::toLower(config.rendererName);
    const bool usesOpenGlRenderer = rendererBackend == "opengl" || rendererBackend == "gl";
    const bool usesVulkanRenderer = rendererBackend == "vulkan" || rendererBackend == "vk";

    auto renderer = createRenderer(config, false);
    auto audioPlayer = std::make_unique<rtsp::AudioPlayer>(config.audioOptions);
    auto faceAnalyzer = std::make_unique<rtsp::FaceAnalyzer>();
    faceAnalyzer->initialize(config.faceDetectionOptions);

    rtsp::StreamSessionOptions options;
    options.url = config.rtspUrl;
    options.connectionOptions = config.rtspOptions;
    options.hardwareDecodeBackend = config.hwDecodeBackend;
    options.audioEnabled = config.audioOptions.enabled;
    options.videoEnabled = true;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    options.hardwareFrameOutput =
        (usesOpenGlRenderer || usesVulkanRenderer) && !config.faceDetectionOptions.enabled;
    if (config.faceDetectionOptions.enabled && (usesOpenGlRenderer || usesVulkanRenderer)) {
        SPDLOG_INFO("Hardware frame passthrough disabled while face detection is enabled");
    }
#else
    options.hardwareFrameOutput = false;
#endif
    options.forwardAudioToPlayer = true;
    options.audioPlayer = audioPlayer.get();
    options.jitterMaxSize = config.jitterMaxSize;
    options.jitterLatencyMs = config.jitterLatencyMs;
    options.streamIndex = 0;
    auto stream = std::make_unique<rtsp::StreamSession>(options);

    renderer->setCommandCallback([&stream](rtsp::RendererCommand command) {
        if (command != rtsp::RendererCommand::ToggleRecording) {
            return;
        }

        if (stream->client().isRecording()) {
            stream->client().stopRecording();
        } else {
            stream->client().startRecording();
        }
    });

    auto connectAndInitialize = [&](bool reconnecting) {
        uint32_t delayMs = config.reconnectOptions.initialDelayMs;
        while (g_running) {
            if (reconnecting) {
                SPDLOG_INFO("Reconnecting to RTSP stream...");
            }

            if (stream->connect()) {
                const int newWidth = stream->client().getWidth();
                const int newHeight = stream->client().getHeight();
                SPDLOG_INFO("Video resolution: {}x{}", newWidth, newHeight);

                if (!renderer->isInitialized() ||
                    newWidth != renderer->getWidth() ||
                    newHeight != renderer->getHeight()) {
                    if (!renderer->initialize(newWidth, newHeight, "RTSP Player")) {
                        SPDLOG_ERROR("Failed to initialize renderer");
                        stream->client().disconnect();
                        return false;
                    }
                }

                audioPlayer->reset();
                stream->start();
                SPDLOG_INFO("Streaming started, press ESC or Q to quit");
                return true;
            }

            if (!config.reconnectOptions.enabled) {
                SPDLOG_ERROR("Failed to connect to RTSP stream");
                return false;
            }

            SPDLOG_WARN("RTSP connect failed, retrying in {} ms", delayMs);
            if (!handleEventsDuringDelay(*renderer, delayMs)) {
                return false;
            }

            const uint32_t nextDelay = delayMs == 0 ? 1 : delayMs * 2;
            delayMs = std::min(nextDelay, config.reconnectOptions.maxDelayMs);
        }

        return false;
    };

    if (!connectAndInitialize(false)) {
        return -1;
    }

    std::shared_ptr<rtsp::MediaFrame> frame;
    rtsp::SingleStreamSyncController sync(config.syncOptions);
    uint64_t renderedFramesSinceFpsUpdate = 0;
    auto lastFpsUpdateTime = std::chrono::steady_clock::now();

    while (g_running && renderer->handleEvents()) {
        if (!stream->isRunning()) {
            SPDLOG_WARN("RTSP receive loop stopped");
            stream->stopAndDisconnect();
            stream->resetBufferedFrames();
            stream->resetStats();
            audioPlayer->reset();
            sync.reset();
            renderedFramesSinceFpsUpdate = 0;
            lastFpsUpdateTime = std::chrono::steady_clock::now();

            if (!config.reconnectOptions.enabled || !connectAndInitialize(true)) {
                break;
            }

            continue;
        }

        if (!stream->pendingFrame) {
            std::shared_ptr<rtsp::MediaFrame> nextFrame;
            if (stream->jitterBuffer().pop(nextFrame, 1)) {
                stream->pendingFrame = nextFrame;
            }
        }

        if (stream->pendingFrame) {
            frame = stream->pendingFrame;
            rtsp::updateFrameLatency(frame, stream->stats);
            stream->refreshStats();

            const auto decision = sync.synchronize(
                frame,
                stream->pendingFrame,
                stream->jitterBuffer(),
                *audioPlayer,
                stream->stats);

            if (decision.type == rtsp::SyncDecision::Type::Wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(decision.waitMs));
                if (!renderer->handleEvents()) {
                    g_running = false;
                    break;
                }
                continue;
            }
            if (decision.type == rtsp::SyncDecision::Type::WaitingForNewerFrame) {
                continue;
            }

            stream->refreshStats();
            stream->stats.syncDroppedFrames = sync.droppedFrames();
            renderer->setPlaybackStats(stream->stats);
            faceAnalyzer->submitFrame(frame, 0);
            renderer->setFaceOverlays(faceAnalyzer->latestResults());
            if (renderer->render(frame)) {
                ++renderedFramesSinceFpsUpdate;
            }
            stream->pendingFrame.reset();

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - lastFpsUpdateTime;
            if (elapsed >= std::chrono::seconds(1)) {
                const double elapsedSeconds =
                    std::chrono::duration<double>(elapsed).count();
                stream->stats.fps =
                    static_cast<double>(renderedFramesSinceFpsUpdate) / elapsedSeconds;
                renderedFramesSinceFpsUpdate = 0;
                lastFpsUpdateTime = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stream->stopAndDisconnect();
    audioPlayer->reset();

    SPDLOG_INFO("RTSP Player stopped");
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    initializeLogging();

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
    logConfig(config);

    if (config.rtspUrls.size() > 1) {
        return runMultiStream(config);
    }

    return runSingleStream(config);
}
