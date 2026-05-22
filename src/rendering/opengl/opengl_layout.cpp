#include "opengl_video_renderer.hpp"

#include <algorithm>

namespace rtsp::rendering::opengl {

ViewportRect calculateAspectFitViewport(int drawableWidth,
                                        int drawableHeight,
                                        int videoWidth,
                                        int videoHeight) {
    if (drawableWidth <= 0 || drawableHeight <= 0 ||
        videoWidth <= 0 || videoHeight <= 0) {
        return {};
    }

    const double drawableAspect =
        static_cast<double>(drawableWidth) / static_cast<double>(drawableHeight);
    const double videoAspect =
        static_cast<double>(videoWidth) / static_cast<double>(videoHeight);

    int fittedWidth = drawableWidth;
    int fittedHeight = drawableHeight;

    if (drawableAspect > videoAspect) {
        fittedWidth = static_cast<int>(static_cast<double>(drawableHeight) * videoAspect);
    } else {
        fittedHeight = static_cast<int>(static_cast<double>(drawableWidth) / videoAspect);
    }

    return {
        static_cast<GLint>((drawableWidth - fittedWidth) / 2),
        static_cast<GLint>((drawableHeight - fittedHeight) / 2),
        static_cast<GLsizei>(std::max(fittedWidth, 1)),
        static_cast<GLsizei>(std::max(fittedHeight, 1))
    };
}

ViewportRect calculateAspectFitViewport(int drawableWidth,
                                        int drawableHeight,
                                        int videoWidth,
                                        int videoHeight,
                                        uint32_t slotIndex,
                                        uint32_t slotCount) {
    const uint32_t columns = std::max<uint32_t>(slotCount, 1U);
    const int panelWidth = std::max(1, drawableWidth / static_cast<int>(columns));
    const int panelHeight = drawableHeight;
    const int panelX = panelWidth *
        static_cast<int>(std::min<uint32_t>(slotIndex, columns - 1U));

    const ViewportRect local =
        calculateAspectFitViewport(panelWidth, panelHeight, videoWidth, videoHeight);
    return {
        static_cast<GLint>(panelX + local.x),
        local.y,
        local.width,
        local.height
    };
}

} // namespace rtsp::rendering::opengl
