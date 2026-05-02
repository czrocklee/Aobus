// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <string_view>

namespace ao::gtk
{
  // Aobus Logo SVG Template: Precise 1:1 (80x80)
  // Gradient: Quality Indicator Color (COLOR_END) is dominant (last 2/3) for better visibility.
  constexpr std::string_view kLogoSvgTemplate = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 80" width="{{WIDTH_PX}}" height="{{HEIGHT_PX}}" preserveAspectRatio="xMidYMid meet">
  <defs>
    <linearGradient id="ao-gradient" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:{{COLOR_START}}" />
      <stop offset="33.3%" style="stop-color:{{COLOR_END}}" />
      <stop offset="100%" style="stop-color:{{COLOR_END}}" />
    </linearGradient>
  </defs>
  <style>
    .o-path { stroke: url(#ao-gradient); fill: none; }
  </style>
  <g>
    <path d="M 40 10 A 30 30 0 1 1 39.99 10"
          fill="none"
          stroke="url(#ao-gradient)"
          stroke-width="{{STROKE_WIDTH}}"
          stroke-linecap="round"
          transform="{{TRANSFORM}}" />
  </g>
</svg>
)svg";
} // namespace ao::gtk
