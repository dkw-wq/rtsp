#include "opengl_video_renderer.hpp"

#include <algorithm>
#include <utility>

namespace rtsp::rendering::opengl {

namespace {

std::pair<uint32_t, uint32_t> gridForSlots(uint32_t slotCount) {
    const uint32_t clampedSlotCount = std::max<uint32_t>(slotCount, 1U);
    uint32_t columns = 1;
    while (columns * columns < clampedSlotCount) {
        ++columns;
    }
    const uint32_t rows = (clampedSlotCount + columns - 1U) / columns;
    return {columns, rows};
}

} // namespace

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
    const uint32_t clampedSlotCount = std::max<uint32_t>(slotCount, 1U);
    const auto [columns, rows] = gridForSlots(clampedSlotCount);
    const int panelWidth = std::max(1, drawableWidth / static_cast<int>(columns));
    const int panelHeight = std::max(1, drawableHeight / static_cast<int>(rows));
    const uint32_t safeSlot = std::min<uint32_t>(slotIndex, clampedSlotCount - 1U);
    const uint32_t column = safeSlot % columns;
    const uint32_t row = safeSlot / columns;
    const int panelX = panelWidth *
        static_cast<int>(column);
    const int panelY = drawableHeight - panelHeight * static_cast<int>(row + 1U);

    const ViewportRect local =
        calculateAspectFitViewport(panelWidth, panelHeight, videoWidth, videoHeight);
    return {
        static_cast<GLint>(panelX + local.x),
        static_cast<GLint>(panelY + local.y),
        local.width,
        local.height
    };
}

} // namespace rtsp::rendering::opengl
