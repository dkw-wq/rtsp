#include "config_loader.hpp"
#include "jitter_buffer.hpp"
#include "rtsp_ffmpeg_utils.hpp"
#include "rtsp_frame_converter.hpp"
#include "rtsp_hardware_decoder.hpp"
#include "sync_controller.hpp"
#include "video_renderer.hpp"

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
#include <SDL2/SDL.h>
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

void testJitterBufferRejectsZeroCapacity() {
    rtsp::JitterBuffer buffer(0, 0);
    const auto input = makeVideoFrame(1.0);
    require(buffer.push(input), "frame push should succeed");
    require(buffer.size() == 1, "zero capacity should be clamped to one frame");

    std::shared_ptr<rtsp::MediaFrame> output;
    require(buffer.pop(output, 0), "frame should be available with zero latency");
    require(output == input, "jitter buffer should return the pushed frame");
    require(buffer.empty(), "jitter buffer should be empty after pop");
}

void testJitterBufferWaitsForLatencyIncludingKeyFrames() {
    rtsp::JitterBuffer buffer(4, 30, false);
    const auto input = makeVideoFrame(1.0, true);
    require(buffer.push(input), "key frame push should succeed");

    std::shared_ptr<rtsp::MediaFrame> output;
    require(!buffer.pop(output, 0), "key frame should respect latency");
    const auto started = std::chrono::steady_clock::now();
    require(buffer.pop(output, 100), "pop should wake when latency expires");
    const auto waited = std::chrono::steady_clock::now() - started;
    require(waited >= std::chrono::milliseconds(15),
            "pop should wait for the frame release deadline");
    require(output == input, "waiting pop should return the key frame");
}

void testJitterBufferMeasuresArrivalJitterAndAdapts() {
    rtsp::JitterBuffer buffer(4, 10, true, 18);
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    const auto first = makeVideoFrame(1.000);
    const auto second = makeVideoFrame(1.033);
    const auto third = makeVideoFrame(1.066);
    first->pts = (uint64_t{1} << 33);
    second->pts = first->pts + 3000;
    third->pts = second->pts + 3000;
    first->recvTime = now - std::chrono::milliseconds(200);
    second->recvTime = now - std::chrono::milliseconds(147);
    third->recvTime = now - std::chrono::milliseconds(94);

    buffer.push(first);
    buffer.push(second);
    buffer.push(third);
    const auto stats = buffer.getStats();
    require(stats.avgJitter > 2.3 && stats.avgJitter < 2.5,
            "jitter should measure arrival deviation in milliseconds, independent of raw PTS");
    require(stats.targetLatencyMs == 18,
            "adaptive latency should increase but respect its configured cap");
}

void testJitterBufferReordersAndDropsTooLateFrames() {
    rtsp::JitterBuffer buffer(4, 30, false);
    const auto newest = makeVideoFrame(3.0);
    const auto oldest = makeVideoFrame(1.0);
    const auto middle = makeVideoFrame(2.0);
    newest->recvTime = oldest->recvTime = middle->recvTime =
        std::chrono::duration_cast<std::chrono::microseconds>(
            (std::chrono::steady_clock::now() - std::chrono::milliseconds(100))
                .time_since_epoch());
    buffer.push(newest);
    buffer.push(oldest);
    buffer.push(middle);

    std::shared_ptr<rtsp::MediaFrame> output;
    require(buffer.pop(output, 0) && output == oldest, "oldest PTS should release first");
    require(buffer.pop(output, 0) && output == middle, "middle PTS should release second");
    require(buffer.pop(output, 0) && output == newest, "newest PTS should release last");

    const auto late = makeVideoFrame(2.5);
    require(!buffer.push(late), "frame older than last released PTS should be dropped");
    const auto stats = buffer.getStats();
    require(stats.lateDroppedFrames == 1 && stats.overflowDroppedFrames == 0,
            "late reordering drops should have their own counter");
    buffer.clear();
    const auto resetStats = buffer.getStats();
    require(resetStats.totalFrames == 0 && resetStats.droppedFrames == 0 &&
                resetStats.avgJitter == 0.0 && resetStats.bufferSize == 0,
            "clear should start fresh per-connection statistics");

    rtsp::JitterBuffer smallBuffer(2, 0, false);
    require(smallBuffer.push(makeVideoFrame(2.0)), "first frame should be retained");
    require(smallBuffer.push(makeVideoFrame(3.0)), "second frame should be retained");
    require(!smallBuffer.push(makeVideoFrame(1.0)),
            "an older frame should be discarded when the reorder window is full");
    require(smallBuffer.getStats().overflowDroppedFrames == 1,
            "full reorder window should count its own overflow drop");
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
             << "  adaptive: false\n"
             << "  max_latency_ms: 120\n"
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
             << "multi_stream:\n"
             << "  max_streams: 40\n"
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
    require(!config.jitterAdaptive && config.jitterMaxLatencyMs == 120,
            "adaptive jitter options should be parsed");
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
    require(config.maxStreams == 40, "multi-stream limit should be parsed");
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
    require(config.rtspUrls.size() == 1, "single command URL should replace configured stream list");
    require(config.rtspUrls[0] == "rtsp://override/one", "single command URL should become stream 1");
}

void testCommandLineOverrideMultipleUrls() {
    rtsp::AppConfig config;
    config.rtspUrl = "rtsp://configured/main";
    config.rtspUrls = {"rtsp://configured/a"};
    config.rtspUrlsConfigured = true;

    char program[] = "rtsp_player";
    char firstUrl[] = "rtsp://override/one";
    char secondUrl[] = "rtsp://override/two";
    char thirdUrl[] = "rtsp://override/three";
    char* argv[] = {program, firstUrl, secondUrl, thirdUrl};
    rtsp::applyCommandLineOverrides(config, 4, argv);

    require(config.rtspUrl == "rtsp://override/one", "command URLs should set primary URL");
    require(config.rtspUrls.size() == 3, "all command URLs should replace configured streams");
    require(config.rtspUrls[0] == "rtsp://override/one", "first command URL should become stream 1");
    require(config.rtspUrls[1] == "rtsp://override/two", "second command URL should become stream 2");
    require(config.rtspUrls[2] == "rtsp://override/three", "third command URL should become stream 3");
}

void testCommandLineOverrideFaceDetection() {
    rtsp::AppConfig config;
    config.faceDetectionOptions.enabled = true;

    char program[] = "rtsp_player";
    char faceDetectionOption[] = "--face-detection=off";
    char firstUrl[] = "rtsp://override/one";
    char secondUrl[] = "rtsp://override/two";
    char* argv[] = {program, faceDetectionOption, firstUrl, secondUrl};
    rtsp::applyCommandLineOverrides(config, 4, argv);

    require(!config.faceDetectionOptions.enabled,
            "command line should disable face detection globally");
    require(config.rtspUrls.size() == 2,
            "face detection option should not be treated as a stream URL");
    require(config.rtspUrls[0] == "rtsp://override/one", "first URL should remain stream 1");
    require(config.rtspUrls[1] == "rtsp://override/two", "second URL should remain stream 2");

    char enableFaceDetectionOption[] = "--enable-face-detection";
    char* enableArgv[] = {program, enableFaceDetectionOption};
    rtsp::applyCommandLineOverrides(config, 2, enableArgv);
    require(config.faceDetectionOptions.enabled,
            "command line should enable face detection globally");
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
    require(decision.catchUpFrames == 1, "catch-up frame should be counted for input statistics");
}

void testVideoTimestampRecoveryAndReset() {
    rtsp::VideoTimestampTracker timestamps;
    constexpr double duration = 0.04;
    const auto check = [&](double candidate, bool present, double expected) {
        require(std::abs(timestamps.stabilize(candidate, present, duration) - expected) < 1e-9,
                "video timestamp should preserve valid progress or synthesize one frame interval");
    };
    check(10.0, true, 10.0);
    check(0.0, false, 10.04);
    check(10.04, true, 10.08);
    check(1.0, true, 10.12);
    check(20.0, true, 10.16);
    check(10.20, true, 10.20);

    timestamps.reset();
    check(100.0, true, 100.0);
    timestamps.reset();
    check(0.0, false, 0.0);
    check(0.0, false, 0.04);

    timestamps.reset();
    timestamps.stabilize(1.0, true, 0.1);
    require(std::abs(timestamps.stabilize(1.3, true, 0.1) - 1.3) < 1e-9,
            "low frame rates should allow a timestamp gap of up to four frame intervals");
}

void testAudioTimingPrebufferClockAndReset() {
    rtsp::AudioPlaybackTiming timing({true, 30, 800, 1500});
    timing.resetForDevice();
    require(!timing.active() && !timing.hasClock(), "new audio device should await prebuffer");
    timing.noteQueuedFrame(10.0, 0.02);
    require(timing.hasClock() && !timing.startIfReady(20),
            "queued audio should establish a clock before reaching the startup target");
    require(std::abs(timing.clockSeconds(0.015) - 10.005) < 1e-9,
            "audio clock should subtract unplayed audio from the queued end timestamp");
    timing.noteQueuedFrame(10.02, 0.02);
    require(timing.startIfReady(40) && timing.active(), "sufficient audio should start playback");
    require(!timing.startIfReady(0) && timing.active(), "a drained queue should keep playback started");
    timing.noteQueuedFrame(9.0, 0.02);
    require(std::abs(timing.clockSeconds(0.01) - 10.03) < 1e-9,
            "older audio timestamps should not move the queued end backward");
    require(!timing.shouldResetQueue(801) && !timing.shouldResetQueue(1500) &&
                timing.shouldResetQueue(1501),
            "only exceeding the hard limit should request an audio queue reset");
    timing.resetQueue(20.0);
    timing.noteQueuedFrame(20.0, 0.02);
    require(timing.active() && std::abs(timing.clockSeconds(0.02) - 20.0) < 1e-9,
            "hard queue reset should rebase the clock and continue playback");
    timing.reset();
    require(!timing.active() && !timing.hasClock() && timing.clockSeconds(0.0) == 0.0,
            "disconnect should clear audio startup and clock state");

    rtsp::AudioPlaybackTiming immediate({true, 0, 800, 1500});
    immediate.resetForDevice();
    require(immediate.active() && !immediate.hasClock(),
            "zero target latency should start the device before the first audio frame");
    const auto normalized = rtsp::normalizeAudioPlaybackOptions({true, 6000, 1, 2});
    require(normalized.targetLatencyMs == 5000 && normalized.maxQueueMs == 5500 &&
                normalized.hardResetQueueMs == 6000,
            "audio queue limits should retain their startup and recovery margins");
}

void testAudioPlayerAndSingleStreamSyncIntegration() {
    require(SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) == 0,
            "dummy audio driver should be selected for the integration test");
    rtsp::AudioPlayer audioPlayer({true, 30, 800, 1500});
    auto audio = std::make_shared<rtsp::MediaFrame>();
    audio->type = rtsp::MediaFrame::Type::AUDIO;
    audio->sampleRate = 48000;
    audio->channels = 2;
    audio->bytesPerSample = 2;
    audio->ptsSeconds = 20.0;
    audio->data.resize(960 * 2 * 2); // 20 ms, also exercises duration calculation from PCM bytes.
    require(audioPlayer.pushFrame(audio), "dummy device should accept the first audio frame");
    require(audioPlayer.hasClock() && !audioPlayer.getStats().active &&
                std::abs(audioPlayer.clockSeconds() - 20.0) < 1e-9,
            "prebuffered audio should expose its clock while the device remains paused");

    rtsp::SingleStreamSyncController sync;
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::PlaybackStats stats;
    auto pending = makeVideoFrame(10.0);
    auto frame = pending;
    buffer.push(makeVideoFrame(10.02));
    auto decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Wait && decision.waitMs == 5 &&
                !pending && buffer.empty() && stats.syncDroppedFrames == 2,
            "video should discard ready stale frames while audio is still prebuffering");

    audio->ptsSeconds = 20.02;
    require(audioPlayer.pushFrame(audio) && audioPlayer.getStats().active,
            "reaching the prebuffer target should start the audio device");
    pending = makeVideoFrame(11.0);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render && stats.audioActive,
            "single-stream sync should establish relative bases after audio startup");
    pending = makeVideoFrame(12.0);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Wait && decision.waitMs == 16,
            "video ahead of the audio clock should wait in bounded steps");
    pending = makeVideoFrame(1.0);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::WaitingForNewerFrame && !pending &&
                stats.syncDroppedFrames == 3,
            "late video without a ready replacement should wait for a newer frame");

    audioPlayer.reset();
    sync.reset();
    require(!audioPlayer.hasClock() && !audioPlayer.getStats().active,
            "audio reset should clear the published clock and playback state");
    pending = makeVideoFrame(50.0);
    frame = pending;
    decision = sync.synchronize(frame, pending, buffer, audioPlayer, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render && stats.syncDroppedFrames == 0,
            "reconnect should establish a fresh video-only base and reset dropped-frame counts");
}

void testMultiStreamSyncWaitAndOffset() {
    rtsp::SyncOptions options;
    options.maxWaitMs = 12;
    options.audioOffsetMs = 2000;
    rtsp::MultiStreamSyncController sync(options, 100);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::PlaybackStats stats;
    auto pending = makeVideoFrame(10.0, false, 1000);
    const auto original = pending;
    const auto decision = sync.synchronize(0, pending, buffer, true, stats);
    require(decision.type == rtsp::SyncDecision::Type::Wait && decision.waitMs == 12 &&
                stats.avSyncDiffMs == 12 && pending == original && decision.catchUpFrames == 0,
            "primary video should honor positive offset and keep its pending frame while waiting");

    options.maxWaitMs = 0;
    rtsp::MultiStreamSyncController minimumWait(options, 100);
    require(minimumWait.synchronize(0, pending, buffer, true, stats).waitMs == 1,
            "multi-stream mode should retain its minimum one-millisecond wait");

    options.audioOffsetMs = -1500;
    rtsp::MultiStreamSyncController negativeOffset(options, 1000);
    pending = makeVideoFrame(10.0);
    require(negativeOffset.synchronize(0, pending, buffer, true, stats).type ==
                rtsp::SyncDecision::Type::Render && stats.syncDroppedFrames == 0,
            "negative offset should clamp total receive delay to zero instead of dropping a new frame");
}

void testMultiStreamSyncBypassAndOnTimeFrames() {
    rtsp::SyncOptions options;
    options.lateDropMs = 5000;
    rtsp::MultiStreamSyncController sync(options, 10000);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::PlaybackStats stats;
    auto pending = makeVideoFrame(1.0);
    const auto original = pending;
    const auto queued = makeVideoFrame(2.0);
    buffer.push(queued);
    require(sync.synchronize(1, pending, buffer, true, stats).type == rtsp::SyncDecision::Type::Render,
            "secondary video should bypass the primary audio delay");
    require(sync.synchronize(0, pending, buffer, false, stats).type == rtsp::SyncDecision::Type::Render,
            "primary video should bypass delay while the audio clock is unavailable");
    options.enabled = false;
    rtsp::MultiStreamSyncController disabled(options, 10000);
    require(disabled.synchronize(0, pending, buffer, true, stats).type == rtsp::SyncDecision::Type::Render &&
                pending == original && buffer.size() == 1 && stats.syncDroppedFrames == 0,
            "disabled synchronization should retain both pending and queued frames");

    options.enabled = true;
    rtsp::MultiStreamSyncController onTime(options, 30);
    pending = makeVideoFrame(1.0, false, 100);
    const auto onTimeFrame = pending;
    const auto decision = onTime.synchronize(0, pending, buffer, true, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render && pending == onTimeFrame &&
                decision.catchUpFrames == 0 && buffer.size() == 1,
            "an on-time pending frame should render without draining other ready frames");
}

void testMultiStreamSyncCatchesUpUntilDeadline() {
    rtsp::SyncOptions options;
    options.lateDropMs = 500;
    rtsp::MultiStreamSyncController sync(options, 50);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::PlaybackStats stats;
    stats.syncDroppedFrames = 7;
    auto pending = makeVideoFrame(1.0, false, 3000);
    buffer.push(makeVideoFrame(2.0, false, 2000));
    const auto catchUp = makeVideoFrame(3.0, false, 100);
    buffer.push(catchUp);
    buffer.push(makeVideoFrame(4.0, false, 100));
    const auto decision = sync.synchronize(0, pending, buffer, true, stats);
    require(decision.type == rtsp::SyncDecision::Type::Render && pending == catchUp &&
                decision.catchUpFrames == 2 && stats.syncDroppedFrames == 9 && buffer.size() == 1,
            "catch-up should stop at the first on-time frame and count every consumed replacement");
}

void testMultiStreamSyncWaitsForReadyReplacement() {
    rtsp::SyncOptions options;
    options.lateDropMs = 500;
    rtsp::MultiStreamSyncController sync(options, 50);
    rtsp::JitterBuffer buffer(4, 0);
    rtsp::PlaybackStats stats;
    auto pending = makeVideoFrame(1.0, false, 3000);
    buffer.push(makeVideoFrame(2.0, false, 2000));
    auto decision = sync.synchronize(0, pending, buffer, true, stats);
    require(decision.type == rtsp::SyncDecision::Type::WaitingForNewerFrame && !pending &&
                decision.catchUpFrames == 1 && stats.syncDroppedFrames == 2,
            "exhausting stale replacements should clear the pending frame and retain drop counts");

    rtsp::JitterBuffer delayedBuffer(4, 1000, false);
    delayedBuffer.push(makeVideoFrame(4.0));
    pending = makeVideoFrame(3.0, false, 3000);
    decision = sync.synchronize(0, pending, delayedBuffer, true, stats);
    require(decision.type == rtsp::SyncDecision::Type::WaitingForNewerFrame && !pending &&
                decision.catchUpFrames == 0 && delayedBuffer.size() == 1,
            "catch-up must respect the jitter buffer release deadline");

    options.lateDropMs = 0;
    rtsp::MultiStreamSyncController noDrop(options, 50);
    pending = makeVideoFrame(3.0, false, 3000);
    require(noDrop.synchronize(0, pending, delayedBuffer, true, stats).type ==
                rtsp::SyncDecision::Type::Render && pending && stats.syncDroppedFrames == 3,
            "disabling late drops should render even an overdue pending frame");
}

void runTest(const std::string& name, void (*test)()) {
    test();
    std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
    try {
        runTest("JitterBuffer guards zero capacity", testJitterBufferRejectsZeroCapacity);
        runTest("JitterBuffer waits for latency including key frames",
                testJitterBufferWaitsForLatencyIncludingKeyFrames);
        runTest("JitterBuffer measures arrival jitter and adapts",
                testJitterBufferMeasuresArrivalJitterAndAdapts);
        runTest("JitterBuffer reorders and resets per-connection statistics",
                testJitterBufferReordersAndDropsTooLateFrames);
        runTest("JitterBuffer drops oldest frame when full", testJitterBufferDropsOldestWhenFull);
        runTest("JitterBuffer holds non-key frame until latency", testJitterBufferHoldsUntilLatency);
        runTest("ConfigLoader parses YAML", testConfigLoaderParsesYaml);
        runTest("ConfigLoader applies single URL override", testCommandLineOverrideSingleUrl);
        runTest("ConfigLoader applies multiple URL override", testCommandLineOverrideMultipleUrls);
        runTest("ConfigLoader applies face detection override",
                testCommandLineOverrideFaceDetection);
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
        runTest("Video timestamps recover from invalid progress and reset", testVideoTimestampRecoveryAndReset);
        runTest("Audio timing controls prebuffer, clock and reset", testAudioTimingPrebufferClockAndReset);
        runTest("Audio player integrates with single-stream synchronization",
                testAudioPlayerAndSingleStreamSyncIntegration);
        runTest("Multi-stream sync waits with audio offset", testMultiStreamSyncWaitAndOffset);
        runTest("Multi-stream sync preserves bypassed and on-time frames", testMultiStreamSyncBypassAndOnTimeFrames);
        runTest("Multi-stream sync catches up until the deadline", testMultiStreamSyncCatchesUpUntilDeadline);
        runTest("Multi-stream sync waits for a ready replacement", testMultiStreamSyncWaitsForReadyReplacement);
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
