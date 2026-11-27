// PNRT shim for core/frontend/framebuffer_layout.h (runtime 25).
//
// video_core/dsmod/aux_routing.h includes this header for one function, InlineGamePictureRect,
// which fits the game's picture inside its part of a window that shows the game and the companion
// side by side (the emulator's one-screen "Simulated" mode). This host has no such mode: the game
// and the companion are always on their own screens, and no lifted file calls
// InlineGamePictureRect. The stand-in answers "the picture fills the rectangle" (no letterbox),
// which is all that function needs to compile.
#pragma once

#include "common/common_types.h"

namespace Layout {

struct FramebufferLayout {
    struct Screen {
        u32 left{};
        u32 top{};
        u32 right{};
        u32 bottom{};
        u32 GetWidth() const {
            return right - left;
        }
        u32 GetHeight() const {
            return bottom - top;
        }
    };
    u32 width{};
    u32 height{};
    Screen screen{};
};

inline FramebufferLayout DefaultFrameLayout(u32 width, u32 height) noexcept {
    return FramebufferLayout{width, height, {0, 0, width, height}};
}

} // namespace Layout
