#include "config_loader.hpp"
#include "jitter_buffer.hpp"
#include "rtsp_ffmpeg_utils.hpp"
#include "rtsp_frame_converter.hpp"
#include "rtsp_hardware_decoder.hpp"
#include "sync_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::shared_ptr<rtsp::MediaFrame> makeVideoFrame(double ptsSeconds,
                                                 bool keyFrame = false,
                                                 int recvAgeMs = 0) {
    auto frame = std::make_shared<rtsp::MediaFrame>();
    frame->type = rtsp::MediaFrame::Type::VIDEO;
    frame->pixelFormat = rtsp::MediaFrame::PixelFormat::NV12;
    frame->width = 640;
    frame->height = 360;
    frame->ptsSeconds = ptsSeconds;
    frame->keyFrame = keyFrame;
    frame->recvTime = std::chrono::duration_cast<std::chrono::microseconds>(
        (std::chrono::steady_clock::now() - std::chrono::milliseconds(recvAgeMs))
            .time_since_epoch());
    return frame;
}

std::filesystem::path tempYamlPath(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

void testJitterBufferImmediateKeyFrame() {
    rtsp::JitterBuffer buffer(4, 1000);
    const auto input = makeVideoFrame(1.0, true);
    require(buffer.push(input), "key frame push should succeed");

    std::shared_ptr<rtsp::MediaFrame> output;
    require(buffer.pop(output, 0), "key frame should release immediately");
    require(output == input, "jitter buffer should return the pushed key frame");
    require(buffer.empty(), "jitter buffer should be empty after pop");
}

void testJitterBufferDropsOldestWhenFull() {
    rtsp::JitterBuffer buffer(2, 0);
    const auto first = makeVideoFrame(1.0);
    const auto second = makeVideoFrame(2.0);
    const auto third = makeVideoFrame(3.0);

    buffer.push(first);
    buffer.push(second);
    buffer.push(third);

    const auto stats = buffer.getStats();
    require(stats.bufferSize == 2, "full jitter buffer should keep max size");
    require(stats.droppedFrames == 1, "full jitter buffer should count one dropped frame");

    std::shared_ptr<rtsp::MediaFrame> output;
    require(buffer.pop(output, 0), "first available frame should pop");
    require(output == second, "oldest frame should be dropped when buffer is full");
}

void testJitterBufferHoldsUntilLatency() {
    rtsp::JitterBuffer buffer(4, 50);
    const auto frame = makeVideoFrame(1.0, false);
    buffer.push(frame);

    std::shared_ptr<rtsp::MediaFrame> output;
    require(!buffer.pop(output, 0), "non-key frame should wait for latency target");
    require(buffer.size() == 1, "held frame should remain in buffer");
}

void testConfigLoaderParsesYaml() {
    const auto path = tempYamlPath("rtsp_player_config_loader_test.yaml");
    {
        std::ofstream yaml(path);
        yaml << "rtsp_url: \"rtsp://example.test/main\"\n"
             << "rtsp_urls:\n"
             << "  - \"rtsp://example.test/a\"\n"
             << "  - \"rtsp://example.test/b\"\n"
             << "audio_rtsp_url: \"rtsp://example.test/audio\"\n"
             << "width: 1280\n"
             << "height: 720\n"
             << "log_level: \"debug\"\n"
             << "renderer: \"vulkan\"\n"
             << "hw_decode: \"cuda\"\n"
             << "rtsp:\n"
             << "  transport: \"udp\"\n"
             << "  timeout_ms: 2500\n"
             << "  buffer_size: 131072\n"
             << "  low_latency:\n"
             << "    enabled: false\n"
             << "    max_delay_ms: 10\n"
             << "    analyze_duration_ms: 5\n"
             << "    probe_size_bytes: 4096\n"
             << "    reorder_queue_size: 1\n"
             << "jitter_buffer:\n"
             << "  max_size: 8\n"
             << "  latency_ms: 40\n"
             << "audio:\n"
             << "  enabled: false\n"
             << "  target_latency_ms: 70\n"
             << "  max_queue_ms: 900\n"
             << "  hard_reset_queue_ms: 1400\n"
             << "sync:\n"
             << "  enabled: true\n"
             << "  max_wait_ms: 12\n"
             << "  late_drop_ms: 200\n"
             << "  audio_offset_ms: -30\n"
             << "face_detection:\n"
             << "  enabled: true\n"
             << "  backend: \"onnx_cuda\"\n"
             << "  model: \"models/scrfd_test.onnx\"\n"
             << "  input_width: 320\n"
             << "  input_height: 320\n"
             << "  detect_every_n_frames: 3\n"
             << "  score_threshold: 0.65\n"
             << "  nms_threshold: 0.35\n"
             << "reconnect:\n"
             << "  enabled: false\n"
             << "  initial_delay_ms: 20\n"
             << "  max_delay_ms: 80\n"
             << "opengl_filters:\n"
             << "  - warm\n"
             << "  - contrast\n";
    }

    const auto config = rtsp::loadAppConfig(path.string());
    std::filesystem::remove(path);

    require(config.warning.empty(), "valid config should not produce warning");
    require(config.rtspUrl == "rtsp://example.test/a", "rtsp_urls first item should become primary URL");
    require(config.rtspUrls.size() == 2, "two RTSP URLs should be parsed");
    require(config.audioRtspUrl == "rtsp://example.test/audio", "audio RTSP URL should be parsed");
    require(config.width == 1280 && config.height == 720, "dimensions should be parsed");
    require(config.logLevelName == "debug", "log level should be parsed");
    require(config.rendererName == "vulkan", "renderer should be parsed");
    require(config.hwDecodeBackend == "cuda", "hardware decode backend should be parsed");
    require(config.rtspOptions.transport == "udp", "RTSP transport should be parsed");
    require(config.rtspOptions.timeoutMs == 2500, "RTSP timeout should be parsed");
    require(!config.rtspOptions.lowLatency, "low latency flag should be parsed");
    require(config.jitterMaxSize == 8, "jitter max size should be parsed");
    require(config.jitterLatencyMs == 40, "jitter latency should be parsed");
    require(!config.audioOptions.enabled, "audio enabled should be parsed");
    require(config.audioOptions.targetLatencyMs == 70, "audio target latency should be parsed");
    require(config.syncOptions.maxWaitMs == 12, "sync max wait should be parsed");
    require(config.syncOptions.audioOffsetMs == -30, "audio offset should be parsed");
    require(config.faceDetectionOptions.enabled, "face detection enabled should be parsed");
    require(config.faceDetectionOptions.backend == "onnx_cuda", "face backend should be parsed");
    require(config.faceDetectionOptions.modelPath == "models/scrfd_test.onnx",
            "face model path should be parsed");
    require(config.faceDetectionOptions.inputWidth == 320 &&
                config.faceDetectionOptions.inputHeight == 320,
            "face input size should be parsed");
    require(config.faceDetectionOptions.detectEveryNFrames == 3,
            "face detection cadence should be parsed");
    require(config.faceDetectionOptions.scoreThreshold > 0.64F &&
                config.faceDetectionOptions.scoreThreshold < 0.66F,
            "face score threshold should be parsed");
    require(config.faceDetectionOptions.nmsThreshold > 0.34F &&
                config.faceDetectionOptions.nmsThreshold < 0.36F,
            "face NMS threshold should be parsed");
    require(!config.reconnectOptions.enabled, "reconnect enabled should be parsed");
    require(config.reconnectOptions.maxDelayMs == 80, "reconnect max delay should be parsed");
    require(config.openglFilterNames.size() == 2, "OpenGL filters should be parsed");
}

void testCommandLineOverrideSingleUrl() {
    rtsp::AppConfig config;
    config.rtspUrls = {"rtsp://configured/a", "rtsp://configured/b"};
    config.rtspUrlsConfigured = true;

    char program[] = "rtsp_player";
    char url[] = "rtsp://override/one";
    char* argv[] = {program, url};
    rtsp::applyCommandLineOverrides(config, 2, argv);

    require(config.rtspUrl == "rtsp://override/one", "single command URL should override primary URL");
    require(config.rtspUrls.size() == 2, "single command URL should preserve configured stream count");
    require(config.rtspUrls[0] == "rtsp://override/one", "single command URL should replace first stream");
}

void testCommandLineOverrideTwoUrls() {
    rtsp::AppConfig config;
    config.rtspUrl = "rtsp://configured/main";
    config.rtspUrls = {"rtsp://configured/a"};
    config.rtspUrlsConfigured = true;

    char program[] = "rtsp_player";
    char firstUrl[] = "rtsp://override/one";
    char secondUrl[] = "rtsp://override/two";
    char* argv[] = {program, firstUrl, secondUrl};
    rtsp::applyCommandLineOverrides(config, 3, argv);

    require(config.rtspUrl == "rtsp://override/one", "two command URLs should set primary URL");
    require(config.rtspUrls.size() == 2, "two command URLs should replace configured streams");
    require(config.rtspUrls[0] == "rtsp://override/one", "first command URL should become stream 1");
    require(config.rtspUrls[1] == "rtsp://override/two", "second command URL should become stream 2");
}

void testConfigLoaderKeepsDefaultsForInvalidValues() {
    const auto path = tempYamlPath("rtsp_player_config_invalid_values_test.yaml");
    {
        std::ofstream yaml(path);
        yaml << "width: -1\n"
             << "height: 0\n"
             << "rtsp:\n"
             << "  timeout_ms: -50\n"
             << "jitter_buffer:\n"
             << "  max_size: 0\n"
             << "  latency_ms: -10\n"
             << "audio:\n"
             << "  max_queue_ms: 0\n"
             << "sync:\n"
             << "  late_drop_ms: 0\n"
             << "face_detection:\n"
             << "  score_threshold: 1.5\n"
             << "  nms_threshold: -0.1\n";
    }

    const auto config = rtsp::loadAppConfig(path.string());
    std::filesystem::remove(path);

    require(config.width == -1, "width currently accepts raw configured value");
    require(config.height == 0, "height currently accepts raw configured value");
    require(config.rtspOptions.timeoutMs == 5000, "invalid timeout should keep default");
    require(config.jitterMaxSize == 12, "invalid jitter size should keep default");
    require(config.jitterLatencyMs == 30, "invalid jitter latency should keep default");
    require(config.audioOptions.maxQueueMs == 800, "invalid audio max queue should keep default");
    require(config.syncOptions.lateDropMs == 250, "invalid late drop should keep default");
    require(config.faceDetectionOptions.scoreThreshold > 0.49F &&
                config.faceDetectionOptions.scoreThreshold < 0.51F,
            "invalid face score threshold should keep default");
    require(config.faceDetectionOptions.nmsThreshold > 0.39F &&
                config.faceDetectionOptions.nmsThreshold < 0.41F,
            "invalid face NMS threshold should keep default");
}

void testConfigLoaderReportsMalformedYaml() {
    const auto path = tempYamlPath("rtsp_player_config_malformed_test.yaml");
    {
        std::ofstream yaml(path);
        yaml << "rtsp_url: [unterminated\n";
    }

    const auto config = rtsp::loadAppConfig(path.string());
    std::filesystem::remove(path);

    require(!config.warning.empty(), "malformed YAML should produce a warning");
    require(config.rtspUrl == "rtsp://127.0.0.1:8554/webcam",
            "malformed YAML should keep default RTSP URL");
}

void testLogLevelParsing() {
    spdlog::level::level_enum level = spdlog::level::info;
    require(rtsp::parseLogLevel("TRACE", level), "TRACE should parse case-insensitively");
    require(level == spdlog::level::trace, "TRACE should map to trace level");
    require(rtsp::parseLogLevel("warning", level), "warning alias should parse");
    require(level == spdlog::level::warn, "warning should map to warn level");
    require(!rtsp::parseLogLevel("verbose", level), "unknown log level should fail");
}

void testFfmpegConnectionOptionNormalization() {
    rtsp::RtspConnectionOptions options;
    options.transport = "QuIc";
    options.timeoutMs = -10;
    options.bufferSize = -1;
    options.maxDelayMs = -2;
    options.analyzeDurationMs = -3;
    options.probeSizeBytes = -4;
    options.reorderQueueSize = -5;

    const auto normalized = rtsp::ffmpeg::normalizeConnectionOptions(options);
    require(normalized.transport == "tcp", "unknown transport should normalize to tcp");
    require(normalized.timeoutMs == 1, "timeout should be clamped to at least 1");
    require(normalized.bufferSize == 0, "buffer size should be clamped non-negative");
    require(normalized.maxDelayMs == 0, "max delay should be clamped non-negative");
    require(normalized.analyzeDurationMs == 0, "analyze duration should be clamped non-negative");
    require(normalized.probeSizeBytes == 0, "probe size should be clamped non-negative");
    require(normalized.reorderQueueSize == 0, "reorder queue should be clamped non-negative");
}

void testFfmpegTimestampHelpers() {
    AVFrame frame{};
    frame.best_effort_timestamp = AV_NOPTS_VALUE;
    frame.pts = 42;
    require(rtsp::ffmpeg::normalizedTimestamp(&frame) == 42,
            "normalized timestamp should use pts when best effort timestamp is missing");

    frame.best_effort_timestamp = 99;
    require(rtsp::ffmpeg::normalizedTimestamp(&frame) == 99,
            "normalized timestamp should prefer best effort timestamp");

    frame.best_effort_timestamp = -1;
    frame.pts = -1;
    require(rtsp::ffmpeg::normalizedTimestamp(&frame) == 0,
            "negative timestamp should normalize to zero");
}

void testFfmpegCudaDecoderNames() {
    require(std::string(rtsp::ffmpeg::cudaDecoderName(AV_CODEC_ID_H264)) == "h264_cuvid",
            "H264 should map to h264_cuvid");
    require(std::string(rtsp::ffmpeg::cudaDecoderName(AV_CODEC_ID_HEVC)) == "hevc_cuvid",
            "HEVC should map to hevc_cuvid");
    require(rtsp::ffmpeg::cudaDecoderName(AV_CODEC_ID_NONE) == nullptr,
            "unknown codec should not have CUDA decoder mapping");
}

void testHardwareDecoderContextState() {
    rtsp::HardwareDecoderContext hardware;
    hardware.setBackend("none");
    require(!hardware.requested(), "none backend should not request hardware decode");
    require(!hardware.active(), "new hardware context should not be active");
    require(hardware.status() == "off", "new hardware context should start off");

    hardware.setBackend("cuda");
    require(hardware.requested(), "cuda backend should request hardware decode");
    hardware.resetForSoftwareFallback();
    require(hardware.status() == "fallback-cpu", "software fallback should update status");
    hardware.resetForDisconnect();
    require(hardware.status() == "not-connected", "disconnect should keep requested backend status");

    const AVCodec* softwareCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (softwareCodec != nullptr) {
        hardware.setBackend("none");
        require(hardware.selectDecoder(AV_CODEC_ID_H264, softwareCodec) == softwareCodec,
                "non-CUDA backend should keep software decoder");
    }
}

void testNv12FrameConverterCopiesNv12Frame() {
    constexpr int width = 4;
    constexpr int height = 4;
    std::array<uint8_t, width * height> yPlane{};
    std::array<uint8_t, width * height / 2> uvPlane{};
    for (size_t index = 0; index < yPlane.size(); ++index) {
        yPlane[index] = static_cast<uint8_t>(index + 1);
    }
    for (size_t index = 0; index < uvPlane.size(); ++index) {
        uvPlane[index] = static_cast<uint8_t>(100 + index);
    }

    AVFrame frame{};
    frame.format = AV_PIX_FMT_NV12;
    frame.width = width;
    frame.height = height;
    frame.data[0] = yPlane.data();
    frame.data[1] = uvPlane.data();
    frame.linesize[0] = width;
    frame.linesize[1] = width;

    rtsp::Nv12FrameConverter converter;
    rtsp::MediaFrame mediaFrame;
    require(converter.copyFrameAsNv12(&frame, mediaFrame), "NV12 frame copy should succeed");
    require(mediaFrame.pixelFormat == rtsp::MediaFrame::PixelFormat::NV12,
            "copied frame should be marked NV12");
    require(mediaFrame.data.size() == width * height * 3 / 2,
            "copied NV12 frame should have compact 1.5x image size");
    require(std::equal(yPlane.begin(), yPlane.end(), mediaFrame.data.begin()),
            "Y plane bytes should be copied exactly");
    require(std::equal(uvPlane.begin(), uvPlane.end(), mediaFrame.data.begin() + yPlane.size()),
            "UV plane bytes should be copied exactly");
}

void testSyncWaitsForFutureVideoFrame() {
    rtsp::SyncOptions options;
    options.enabled = true;
    options.maxWaitMs = 16;
    options.lateDropMs = 250;
    rtsp::SingleStreamSyncController sync(options);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::AudioPlayer audioPlayer({false, 30, 800, 1500});
    rtsp::PlaybackStats stats;

    std::shared_ptr<rtsp::MediaFrame> pending = makeVideoFrame(10.0);
    std::shared_ptr<rtsp::MediaFrame> frame = pending;
    auto decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render, "first frame should establish sync base");

    pending = makeVideoFrame(10.100);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Wait, "future video frame should wait");
    require(decision.waitMs > 0 && decision.waitMs <= options.maxWaitMs,
            "sync wait should be clamped by max_wait_ms");
}

void testSyncDropsLateFrameAndUsesCatchUpFrame() {
    rtsp::SyncOptions options;
    options.enabled = true;
    options.maxWaitMs = 16;
    options.lateDropMs = 100;
    rtsp::SingleStreamSyncController sync(options);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::AudioPlayer audioPlayer({false, 30, 800, 1500});
    rtsp::PlaybackStats stats;

    std::shared_ptr<rtsp::MediaFrame> pending = makeVideoFrame(10.0);
    std::shared_ptr<rtsp::MediaFrame> frame = pending;
    auto decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render, "first frame should establish sync base");

    const auto catchUpFrame = makeVideoFrame(10.020);
    buffer.push(catchUpFrame);
    pending = makeVideoFrame(9.0);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);

    require(decision.type == rtsp::SyncDecision::Type::Render,
            "late frame should be replaced by catch-up frame when available");
    require(frame == catchUpFrame, "catch-up frame should become the frame to render");
    require(sync.droppedFrames() == 1, "late frame should increment sync dropped frame count");
    require(stats.syncDroppedFrames == 1, "playback stats should expose sync dropped frame count");
}

void runTest(const std::string& name, void (*test)()) {
    test();
    std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
    try {
        runTest("JitterBuffer releases key frames", testJitterBufferImmediateKeyFrame);
        runTest("JitterBuffer drops oldest frame when full", testJitterBufferDropsOldestWhenFull);
        runTest("JitterBuffer holds non-key frame until latency", testJitterBufferHoldsUntilLatency);
        runTest("ConfigLoader parses YAML", testConfigLoaderParsesYaml);
        runTest("ConfigLoader applies single URL override", testCommandLineOverrideSingleUrl);
        runTest("ConfigLoader applies two URL override", testCommandLineOverrideTwoUrls);
        runTest("ConfigLoader keeps defaults for invalid values",
                testConfigLoaderKeepsDefaultsForInvalidValues);
        runTest("ConfigLoader reports malformed YAML", testConfigLoaderReportsMalformedYaml);
        runTest("Log level parsing", testLogLevelParsing);
        runTest("FFmpeg normalizes connection options", testFfmpegConnectionOptionNormalization);
        runTest("FFmpeg timestamp helpers", testFfmpegTimestampHelpers);
        runTest("FFmpeg CUDA decoder names", testFfmpegCudaDecoderNames);
        runTest("Hardware decoder context state", testHardwareDecoderContextState);
        runTest("NV12 frame converter copies NV12 frame", testNv12FrameConverterCopiesNv12Frame);
        runTest("SyncController waits for future video frame", testSyncWaitsForFutureVideoFrame);
        runTest("SyncController drops late frame and uses catch-up frame",
                testSyncDropsLateFrameAndUsesCatchUpFrame);
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
