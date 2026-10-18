// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// XCloud Stream's cloud/play mark is drawn with Canvas for crisp headers and
// generated artwork. See assets/branding/ for the matching package icon.
#pragma once

#include "ui/canvas.h"
#include "ui/font.h"

namespace xc::ui {

constexpr Color kBrandGreen = rgba(72, 206, 123);

// Draw the two-link mark centred at (cx, cy), scaled by r. The color argument
// remains for callers of the original Canvas API; the mark uses brand colours.
void drawBrandMark(Canvas& c, float cx, float cy, float r, Color color = rgba(255, 255, 255));

// Draw the supplied XCloud Stream artwork (720 x 720) when packaged. Returns
// false when the app asset is unavailable so callers can keep a vector fallback.
bool drawBrandArtwork(Canvas& c, int x, int y);

// Square app icon and wide home-screen artwork.
void drawAppIcon(Canvas& c, const Fonts& fonts);
void drawHomeArt(Canvas& c, const Fonts& fonts, bool launch);

}  // namespace xc::ui
