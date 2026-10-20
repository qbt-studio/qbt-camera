#pragma once
#include "Pch.h"

// "No signal" picture shown to apps while QBT Camera on the PC or the phone isn't streaming.
class Placeholder {
public:
    /// `paused`: the phone app is in the background, rather than nothing connected at all.
    void Draw(BYTE* destination, LONG pitch, UINT32 width, UINT32 height, bool paused);

private:
    void Render(UINT32 width, UINT32 height, bool paused);

    bool paused_ = false;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    std::vector<BYTE> nv12_;
};
