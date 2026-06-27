// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk
{
  struct RenderTarget final
  {
    std::int32_t width;
    std::int32_t height;
  };

  RenderTarget fitSourceIntoTarget(RenderTarget source, RenderTarget target);
  bool shouldRefresh(RenderTarget current, RenderTarget next);
} // namespace ao::gtk
