#include "config_loader.hpp"
#include "jitter_buffer.hpp"
#include "sync_controller.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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
        runTest("SyncController waits for future video frame", testSyncWaitsForFutureVideoFrame);
        runTest("SyncController drops late frame and uses catch-up frame",
                testSyncDropsLateFrameAndUsesCatchUpFrame);
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
