#include "app_logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace {

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

} // namespace

namespace rtsp {

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

void logConfig(const AppConfig& config) {
    SPDLOG_INFO("RTSP URL: {}", config.rtspUrl);
    SPDLOG_INFO("RTSP stream count: {}", config.rtspUrls.size());
    SPDLOG_INFO("Renderer backend: {}", config.rendererName);
    SPDLOG_INFO("Hardware decode: {}", config.hwDecodeBackend);
    SPDLOG_INFO("RTSP options: transport={}, timeout_ms={}, buffer_size={}, low_latency={}",
                config.rtspOptions.transport,
                config.rtspOptions.timeoutMs,
                config.rtspOptions.bufferSize,
                describeLowLatency(config.rtspOptions));
    SPDLOG_INFO("OpenGL shader pipeline: {}", describeFilters(config.openglFilterNames));
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

} // namespace rtsp
