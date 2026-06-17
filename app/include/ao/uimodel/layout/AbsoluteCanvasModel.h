// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ao::uimodel::layout
{
  struct AbsoluteCanvasRect final
  {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
  };

  struct AbsoluteCanvasItem final
  {
    std::string id;
    AbsoluteCanvasRect rect{};
    std::int32_t zIndex = 0;
    std::int32_t insertOrder = 0;
  };

  enum class AbsoluteCanvasResizeCorner : std::uint8_t
  {
    None = 0,
    TopLeft = 1,
    TopRight = 2,
    BottomLeft = 3,
    BottomRight = 4
  };

  enum class AbsoluteCanvasNudgeDirection : std::uint8_t
  {
    Up = 0,
    Down = 1,
    Left = 2,
    Right = 3
  };

  bool absoluteCanvasZOrderLess(std::int32_t zIndexA,
                                std::int32_t insertOrderA,
                                std::int32_t zIndexB,
                                std::int32_t insertOrderB);

  bool absoluteCanvasZOrderLess(AbsoluteCanvasItem const& itemA, AbsoluteCanvasItem const& itemB);

  std::optional<std::size_t> hitTestAbsoluteCanvas(std::span<AbsoluteCanvasItem const> items,
                                                   std::int32_t posX,
                                                   std::int32_t posY);

  AbsoluteCanvasResizeCorner detectAbsoluteCanvasResizeCorner(std::int32_t width,
                                                              std::int32_t height,
                                                              double relX,
                                                              double relY);

  std::int32_t snapAbsoluteCanvasValue(std::int32_t value, bool snapToGrid, std::int32_t gridSize);

  AbsoluteCanvasRect updateAbsoluteCanvasMoveDrag(AbsoluteCanvasRect startRect,
                                                  std::int32_t offsetX,
                                                  std::int32_t offsetY);

  AbsoluteCanvasRect commitAbsoluteCanvasMoveDrag(AbsoluteCanvasRect startRect,
                                                  std::int32_t offsetX,
                                                  std::int32_t offsetY,
                                                  bool snapToGrid,
                                                  std::int32_t gridSize);

  AbsoluteCanvasRect updateAbsoluteCanvasResizeDrag(AbsoluteCanvasRect startRect,
                                                    AbsoluteCanvasResizeCorner corner,
                                                    std::int32_t offsetX,
                                                    std::int32_t offsetY,
                                                    std::int32_t minWidth,
                                                    std::int32_t minHeight,
                                                    bool snapToGrid,
                                                    std::int32_t gridSize);

  AbsoluteCanvasRect commitAbsoluteCanvasResizeDrag(AbsoluteCanvasRect rect, bool snapToGrid, std::int32_t gridSize);

  AbsoluteCanvasRect nudgeAbsoluteCanvasRect(AbsoluteCanvasRect rect,
                                             AbsoluteCanvasNudgeDirection direction,
                                             bool snapToGrid,
                                             std::int32_t gridSize);
}
