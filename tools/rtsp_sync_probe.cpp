#include "audio_player.hpp"
#include "jitter_buffer.hpp"
#include "rtsp_client.hpp"
#include "sync_controller.hpp"
#include "video_renderer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include <SDL2/SDL.h>

namespace {

struct VideoState {
    rtsp::JitterBuffer buffer{24, 30, false};
    std::shared_ptr<rtsp::MediaFrame> pending;
    rtsp::PlaybackStats stats;
    std::atomic<uint64_t> mappedFrames{0};
    uint64_t synchronizedFrames = 0;
    uint64_t waits = 0;
};

} // namespace

// Headless end-to-end probe: real demux/decode and audio queue, no camera capture or window.
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: rtsp_sync_probe VIDEO_URL USB_URL AUDIO_URL\n";
        return 2;
    }
    if (SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) != 0) {
        std::cerr << "Could not select the dummy audio device\n";
        return 2;
    }
    rtsp::AudioPlayer player({true, 50, 1000, 1500});
    std::array<VideoState, 2> videos;
    std::atomic<uint64_t> mappedAudioFrames{0};
    std::array<std::unique_ptr<rtsp::RtspClient>, 3> clients;
    for (size_t index = 0; index < clients.size(); ++index) {
        clients[index] = std::make_unique<rtsp::RtspClient>();
        clients[index]->setHardwareDecode("none");
        clients[index]->setAudioEnabled(index == 2);
        clients[index]->setVideoEnabled(index != 2);
        rtsp::RtspConnectionOptions connection;
        connection.timeoutMs = 3000;
        clients[index]->setConnectionOptions(connection);
        clients[index]->setFrameCallback([&, index](const std::shared_ptr<rtsp::MediaFrame>& frame) {
            if (index == 2) {
                if (frame->referenceTimeSeconds) {
                    ++mappedAudioFrames;
                }
                player.pushFrame(frame);
            } else {
                if (frame->referenceTimeSeconds) {
                    ++videos[index].mappedFrames;
                }
                videos[index].buffer.push(frame);
            }
        });
        if (!clients[index]->connect(argv[index + 1])) {
            std::cerr << "Could not connect probe stream " << index + 1 << '\n';
            return 1;
        }
        clients[index]->start();
    }

    rtsp::SyncOptions options;
    options.audioOffsetMs = 700; // Must be ignored in timestamp mode.
    rtsp::MultiStreamSyncController sync(options, 50);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
    bool hadAudioClock = false;
    bool reconnected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto clock = player.clockSnapshot();
        hadAudioClock = hadAudioClock || clock.referenceTimeSeconds.has_value();
        for (size_t index = 0; index < videos.size(); ++index) {
            auto& video = videos[index];
            if (!video.pending) {
                video.buffer.pop(video.pending, 0);
            }
            if (!video.pending) {
                continue;
            }
            const auto decision = sync.synchronize(index, video.pending, video.buffer, clock, video.stats);
            if (decision.type == rtsp::SyncDecision::Type::Wait) {
                ++video.waits;
            } else if (decision.type == rtsp::SyncDecision::Type::Render) {
                if (video.stats.timestampSyncActive) {
                    ++video.synchronizedFrames;
                }
                video.pending.reset();
            }
        }
        if (mappedAudioFrames >= 20 && videos[0].synchronizedFrames >= 20 &&
            videos[1].synchronizedFrames >= 20) {
            if (reconnected) {
                break;
            }
            // Recreate the second reader: FFmpeg's relative PTS origin changes on reconnect.
            clients[1]->stop();
            clients[1]->disconnect();
            videos[1].buffer.clear();
            videos[1].pending.reset();
            videos[1].stats = {};
            videos[1].mappedFrames = 0;
            videos[1].synchronizedFrames = 0;
            if (!clients[1]->connect(argv[2])) {
                std::cerr << "Second video reconnect failed\n";
                break;
            }
            clients[1]->start();
            reconnected = true;
            std::cout << "Second video reconnected; checking its new sender mapping\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Join callbacks before reporting counters or destroying the audio player/buffers.
    for (auto& client : clients) {
        client->stop();
        client->disconnect();
    }
    bool passed = hadAudioClock && mappedAudioFrames >= 20 && reconnected;
    std::cout << "Mapped audio frames: " << mappedAudioFrames << '\n';
    for (size_t index = 0; index < videos.size(); ++index) {
        const auto& video = videos[index];
        std::cout << "Video " << index + 1 << ": mapped=" << video.mappedFrames
                  << ", synchronized=" << video.synchronizedFrames << ", waits=" << video.waits
                  << ", drops=" << video.stats.syncDroppedFrames
                  << ", last AV diff=" << video.stats.avSyncDiffMs << " ms\n";
        passed = passed && video.synchronizedFrames >= 20;
    }
    std::cout << (passed ? "[PASS] Independent RTSP streams synchronized by sender timestamps\n" :
                          "[FAIL] Missing mappings or no synchronized playback\n");
    return passed ? 0 : 1;
}
