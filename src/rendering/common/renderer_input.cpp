#include "renderer_input.hpp"

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <SDL2/SDL.h>
}

namespace rtsp::rendering {

bool isKeyDown(SDL_Scancode scancode, int windowsVirtualKey) {
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    bool down = keys != nullptr && keys[scancode] != 0;
#ifdef _WIN32
    down = down || ((GetAsyncKeyState(windowsVirtualKey) & 0x8000) != 0);
#else
    (void)windowsVirtualKey;
#endif
    return down;
}

bool keyJustPressed(bool currentDown, bool& previousDown) {
    const bool pressed = currentDown && !previousDown;
    previousDown = currentDown;
    return pressed;
}

} // namespace rtsp::rendering
