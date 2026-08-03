#pragma once

#include <cstdint>
#include <vector>

void HudSetPixel(std::vector<uint8_t>& pixels, int texW, int texH, int x, int y,
                 uint8_t b, uint8_t g, uint8_t r, uint8_t a);
void HudFillRect(std::vector<uint8_t>& pixels, int texW, int texH, int x, int y, int w, int h,
                 uint8_t b, uint8_t g, uint8_t r, uint8_t a);
const uint8_t* FindHudGlyph(char c);
int HudDrawText(std::vector<uint8_t>& pixels, int texW, int texH, int x, int y,
                const char* text, int scale,
                uint8_t b, uint8_t g, uint8_t r, uint8_t a);
