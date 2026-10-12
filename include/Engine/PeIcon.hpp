#pragma once
// Replaces the icon of a Windows executable (the icon Explorer shows) without Windows and without external tools.
//
// FlyPlayer.exe is built with placeholder icons (resources/player.rc): uncompressed 32-bit DIB images. This finds
// them in the PE resource section and overwrites each, in place, with the given picture resized to the slot's exact
// size, so the file keeps its length and nothing else in it moves. Slots that are not 32-bit DIBs (e.g. PNG-compressed
// ones) are left alone.
#include <cstdint>
#include <string>

namespace peicon {

// `rgba` is width*height*4 bytes (straight alpha). Returns the number of icon images replaced (0 = nothing done),
// and sets `error` when the file is not a usable PE or has no replaceable icon.
int ReplaceExeIcon(const std::string& exePath, const uint8_t* rgba, int width, int height, std::string& error);

// The same resize + DIB encoding, exposed for tests. Output is a complete RT_ICON payload (BITMAPINFOHEADER,
// bottom-up BGRA pixels, 1-bit AND mask).
void EncodeIconDib(const uint8_t* rgba, int srcW, int srcH, int size, std::string& out);

} // namespace peicon
