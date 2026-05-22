#pragma once

extern "C" {
#include <SDL2/SDL_scancode.h>
}

namespace rtsp::rendering {

bool isKeyDown(SDL_Scancode scancode, int windowsVirtualKey);
bool keyJustPressed(bool currentDown, bool& previousDown);

} // namespace rtsp::rendering
