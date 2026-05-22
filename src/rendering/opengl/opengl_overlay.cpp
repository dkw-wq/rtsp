#include "opengl_video_renderer.hpp"
#include "../common/overlay_font.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace rtsp::rendering::opengl {

std::array<std::string, 12> OpenGlVideoRenderer::makeOverlayLines() const {
    std::ostringstream fps;
    fps << "FPS: " << std::fixed << std::setprecision(1) << playbackStats_.fps;

    return {
        fps.str(),
        "DECODER: " + playbackStats_.decoderBackend,
        "HW: " + playbackStats_.hardwareDecodeStatus,
        "DECODED: " + std::to_string(playbackStats_.decodedFrames),
        "DROPPED: " + std::to_string(playbackStats_.droppedFrames),
        "SYNC DROP: " + std::to_string(playbackStats_.syncDroppedFrames),
        "BUFFER: " + std::to_string(playbackStats_.jitterBufferSize),
        "LATENCY: " + std::to_string(playbackStats_.latencyMs) + "MS",
        "AUDIO: " + std::string(playbackStats_.audioActive ? "ON " : "OFF ") +
            std::to_string(playbackStats_.audioQueueMs) + "MS",
        "AV DIFF: " + std::to_string(playbackStats_.avSyncDiffMs) + "MS",
        "FILTER: " + filterPipeline_.describe(),
        recorder_.isRecording() ? "REC: ON" : "REC: OFF"
    };
}

void OpenGlVideoRenderer::drawOverlay() {
    if (!window_) {
        return;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    if (drawableWidth <= 0 || drawableHeight <= 0) {
        return;
    }

    const auto lines = makeOverlayLines();
    const float scale = 2.0F;
    const float lineHeight = 8.0F * scale;
    const float left = 10.0F;
    const float top = 10.0F;
    size_t maxLineLength = 0;
    for (const std::string& line : lines) {
        maxLineLength = std::max(maxLineLength, line.size());
    }
    const float width = static_cast<float>(maxLineLength) * 6.0F * scale + 12.0F;
    const float height = 12.0F + lineHeight * static_cast<float>(lines.size());

    glViewport(0, 0, drawableWidth, drawableHeight);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl_.useProgram(overlayProgram_);
    if (overlayScreenSizeLocation_ >= 0) {
        gl_.uniform2f(overlayScreenSizeLocation_,
                      static_cast<GLfloat>(drawableWidth),
                      static_cast<GLfloat>(drawableHeight));
    }

    overlayVertices_.clear();
    drawFaceOverlays(drawableWidth, drawableHeight);
    flushOverlay(0.12F, 0.94F, 0.55F, 1.0F);

    overlayVertices_.clear();
    drawRect(left - 6.0F, top - 6.0F, width, height);
    flushOverlay(0.0F, 0.0F, 0.0F, 0.62F);

    overlayVertices_.clear();
    float y = top;
    for (const std::string& line : lines) {
        drawText(left, y, line, scale);
        y += lineHeight;
    }
    flushOverlay(0.72F, 1.0F, 0.86F, 1.0F);

    glDisable(GL_BLEND);
}

void OpenGlVideoRenderer::drawFaceOverlays(int drawableWidth, int drawableHeight) {
    if (faceOverlays_.empty()) {
        return;
    }

    const float thickness = std::max(2.0F, static_cast<float>(drawableWidth) / 640.0F);

    for (const FaceDetectionResult& result : faceOverlays_) {
        if (result.streamIndex >= activeVideoSlots_ ||
            result.frameWidth <= 0 ||
            result.frameHeight <= 0) {
            continue;
        }

        const auto viewport =
            calculateAspectFitViewport(drawableWidth,
                                       drawableHeight,
                                       result.frameWidth,
                                       result.frameHeight,
                                       static_cast<uint32_t>(result.streamIndex),
                                       activeVideoSlots_);
        if (viewport.width <= 0 || viewport.height <= 0) {
            continue;
        }

        const float scaleX = static_cast<float>(viewport.width) /
                             static_cast<float>(result.frameWidth);
        const float scaleY = static_cast<float>(viewport.height) /
                             static_cast<float>(result.frameHeight);
        const float viewportX = static_cast<float>(viewport.x);
        const float viewportY = static_cast<float>(viewport.y);
        for (const FaceBox& face : result.faces) {
            drawRectOutline(viewportX + face.x * scaleX,
                            viewportY + face.y * scaleY,
                            face.width * scaleX,
                            face.height * scaleY,
                            thickness);
        }
    }
}

void OpenGlVideoRenderer::drawRectOutline(float x,
                                          float y,
                                          float width,
                                          float height,
                                          float thickness) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }

    drawRect(x, y, width, thickness);
    drawRect(x, y + height - thickness, width, thickness);
    drawRect(x, y, thickness, height);
    drawRect(x + width - thickness, y, thickness, height);
}

void OpenGlVideoRenderer::drawText(float x, float y, const std::string& text, float scale) {
    float cursorX = x;
    for (char rawCh : text) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(rawCh)));
        if (ch == ' ') {
            cursorX += 4.0F * scale;
            continue;
        }

        const Glyph glyph = glyphFor(ch);
        for (size_t row = 0; row < glyph.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                const uint8_t mask = static_cast<uint8_t>(1U << (4 - col));
                if ((glyph[row] & mask) == 0) {
                    continue;
                }

                drawRect(cursorX + static_cast<float>(col) * scale,
                         y + static_cast<float>(row) * scale,
                         scale,
                         scale);
            }
        }

        cursorX += 6.0F * scale;
    }
}

void OpenGlVideoRenderer::drawRect(float x, float y, float width, float height) {
    const float right = x + width;
    const float bottom = y + height;
    const std::array<float, 12> vertices = {
        x, y,
        right, y,
        right, bottom,
        x, y,
        right, bottom,
        x, bottom
    };
    overlayVertices_.insert(overlayVertices_.end(), vertices.begin(), vertices.end());
}

void OpenGlVideoRenderer::flushOverlay(float red, float green, float blue, float alpha) {
    if (overlayVertices_.empty()) {
        return;
    }

    if (overlayColorLocation_ >= 0) {
        gl_.uniform4f(overlayColorLocation_, red, green, blue, alpha);
    }

    gl_.bindVertexArray(overlayVao_);
    gl_.bindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    gl_.bufferData(GL_ARRAY_BUFFER,
                   static_cast<std::ptrdiff_t>(overlayVertices_.size() * sizeof(float)),
                   overlayVertices_.data(),
                   GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(overlayVertices_.size() / 2));
    gl_.bindBuffer(GL_ARRAY_BUFFER, 0);
    gl_.bindVertexArray(0);
}

} // namespace rtsp::rendering::opengl
