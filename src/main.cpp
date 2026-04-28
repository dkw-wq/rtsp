#include "rtsp_client.hpp"
#include "decoder.hpp"
#include "video_renderer.hpp"
#include "jitter_buffer.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

// 全局变量用于控制主循环
static bool g_running = true;

int main(int argc, char* argv[]) {
    // 设置日志
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    SPDLOG_INFO("RTSP Player starting...");

    // 加载配置
    std::string rtspUrl = "rtsp://127.0.0.1:8554/webcam";
    int width = 1920;
    int height = 1080;

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
    } catch (const std::exception& e) {
        SPDLOG_WARN("Config file not found, using default: {}", e.what());
    }

    // 如果有命令行参数，使用命令行参数
    if (argc > 1) {
        rtspUrl = argv[1];
    }

    SPDLOG_INFO("RTSP URL: {}", rtspUrl);

    // 创建组件
    auto rtspClient = std::make_unique<rtsp::RtspClient>();
    auto jitterBuffer = std::make_unique<rtsp::JitterBuffer>(30, 100);
    auto renderer = std::make_unique<rtsp::VideoRenderer>();

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
    while (g_running && renderer->handleEvents()) {
        // 从jitter buffer获取帧
        if (jitterBuffer->pop(frame, 10)) {
            renderer->render(frame);
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