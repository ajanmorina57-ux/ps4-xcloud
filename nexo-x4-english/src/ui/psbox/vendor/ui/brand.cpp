// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "ui/brand.h"
#include "ui/image_cache.h"

#include <cstdio>
#include <string>

namespace xc::ui {

namespace {

constexpr Color kGreen = rgba(72, 206, 123);
constexpr Color kDeepGreen = rgba(10, 14, 15);
constexpr Color kDeepBlue = rgba(7, 19, 37);

}  // namespace

void drawBrandMark(Canvas& c, float cx, float cy, float r, Color /*color*/) {
    // A compact cloud silhouette with a play mark, matching the XCloud Stream
    // package icon while staying legible at small in-app sizes.
    c.fillCircle(cx - r * 0.37f, cy + r * 0.02f, r * 0.34f, kGreen);
    c.fillCircle(cx - r * 0.04f, cy - r * 0.20f, r * 0.45f, kGreen);
    c.fillCircle(cx + r * 0.34f, cy - r * 0.01f, r * 0.34f, kGreen);
    c.fillRect({static_cast<int>(cx - r * 0.63f), static_cast<int>(cy - r * 0.02f),
                static_cast<int>(r * 1.25f), static_cast<int>(r * 0.37f)}, kGreen,
               static_cast<int>(r * 0.16f));
    c.line(cx - r * 0.12f, cy - r * 0.22f, cx + r * 0.26f, cy, r * 0.075f, kDeepGreen);
    c.line(cx + r * 0.26f, cy, cx - r * 0.12f, cy + r * 0.22f, r * 0.075f, kDeepGreen);
    c.line(cx - r * 0.12f, cy + r * 0.22f, cx - r * 0.12f, cy - r * 0.22f, r * 0.075f, kDeepGreen);
}

bool drawBrandArtwork(Canvas& c, int x, int y) {
    static Image artwork;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        FILE* file = std::fopen("/app0/assets/branding/xcloud-stream-logo.png", "rb");
        if (file) {
            if (std::fseek(file, 0, SEEK_END) == 0) {
                long length = std::ftell(file);
                if (length > 0 && length <= 2 * 1024 * 1024 && std::fseek(file, 0, SEEK_SET) == 0) {
                    std::string bytes(static_cast<size_t>(length), '\0');
                    if (std::fread(&bytes[0], 1, bytes.size(), file) == bytes.size())
                        (void)decodeImage(bytes, artwork);
                }
            }
            std::fclose(file);
        }
    }
    if (artwork.w != 720 || artwork.h != 720) return false;
    c.drawImage(artwork, x, y);
    return true;
}

void drawAppIcon(Canvas& c, const Fonts& fonts) {
    const int s = c.width();
    c.gradientV({0, 0, s, s}, rgba(13, 34, 59), kDeepBlue);
    drawBrandMark(c, s * 0.5f, s * 0.43f, s * 0.34f);
    const char* word = "XCLOUD STREAM";
    int px = s / 15;
    int w = fonts.bold.measure(word, px);
    fonts.bold.draw(c, word, (s - w) / 2, static_cast<int>(s * 0.79f), px, rgba(245, 250, 255));
}

void drawHomeArt(Canvas& c, const Fonts& fonts, bool launch) {
    const float w = static_cast<float>(c.width()), h = static_cast<float>(c.height());
    const float cx = launch ? w * 0.5f : w * 0.70f;
    const float cy = launch ? h * 0.42f : h * 0.47f;
    const float r = h * (launch ? 0.17f : 0.22f);

    c.gradientH({0, 0, c.width(), c.height()}, rgba(5, 13, 26), rgba(9, 35, 39));
    for (int i = 32; i > 0; --i) c.fillCircle(cx, cy, r * (1.0f + i * 0.09f), rgba(38, 139, 229, 3));
    for (int i = 12; i > 0; --i) c.fillCircle(cx, cy, r * (1.0f + i * 0.03f), rgba(69, 213, 139, 4));
    c.gradientV({0, 0, c.width(), static_cast<int>(h * 0.25f)}, rgba(0, 0, 0, 120), rgba(0, 0, 0, 0));
    c.gradientV({0, static_cast<int>(h * 0.7f), c.width(), static_cast<int>(h * 0.3f) + 1}, rgba(0, 0, 0, 0),
                rgba(0, 0, 0, 150));
    if (!launch) c.gradientH({0, 0, static_cast<int>(w * 0.45f), c.height()}, rgba(0, 0, 0, 140), rgba(0, 0, 0, 0));

    drawBrandMark(c, cx, cy, r);
    if (launch) {
        const char* name = "XCLOUD STREAM";
        int px = static_cast<int>(h * 0.065f);
        int tw = fonts.bold.measure(name, px);
        fonts.bold.draw(c, name, (c.width() - tw) / 2, static_cast<int>(cy + r * 1.45f), px, rgba(245, 250, 255));
    }
}

}  // namespace xc::ui
