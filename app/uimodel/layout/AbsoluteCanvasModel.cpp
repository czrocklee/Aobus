// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/AbsoluteCanvasModel.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::uimodel::layout
{
  namespace
  {
    constexpr int kCornerHitRadius = 10;

    bool hitCorner(std::int32_t cornerX, std::int32_t cornerY, double mouseX, double mouseY)
    {
      return std::abs(mouseX - static_cast<double>(cornerX)) <= kCornerHitRadius &&
             std::abs(mouseY - static_cast<double>(cornerY)) <= kCornerHitRadius;
    }
  } // namespace

  bool absoluteCanvasZOrderLess(std::int32_t zIndexA,
                                std::int32_t insertOrderA,
                                std::int32_t zIndexB,
                                std::int32_t insertOrderB)
  {
    if (zIndexA != zIndexB)
    {
      return zIndexA < zIndexB;
    }

    return insertOrderA < insertOrderB;
  }

  bool absoluteCanvasZOrderLess(AbsoluteCanvasItem const& itemA, AbsoluteCanvasItem const& itemB)
  {
    return absoluteCanvasZOrderLess(itemA.zIndex, itemA.insertOrder, itemB.zIndex, itemB.insertOrder);
  }

  std::optional<std::size_t> hitTestAbsoluteCanvas(std::span<AbsoluteCanvasItem const> items,
                                                   std::int32_t posX,
                                                   std::int32_t posY)
  {
    auto optHitIndex = std::optional<std::size_t>{};

    for (std::size_t index = 0; index < items.size(); ++index)
    {
      auto const& item = items[index];

      if (auto const& rect = item.rect;
          posX < rect.x || posX > (rect.x + rect.width) || posY < rect.y || posY > (rect.y + rect.height))
      {
        continue;
      }

      if (!optHitIndex || absoluteCanvasZOrderLess(items[*optHitIndex], item))
      {
        optHitIndex = index;
      }
    }

    return optHitIndex;
  }

  AbsoluteCanvasResizeCorner detectAbsoluteCanvasResizeCorner(std::int32_t width,
                                                              std::int32_t height,
                                                              double relX,
                                                              double relY)
  {
    if (hitCorner(0, 0, relX, relY))
    {
      return AbsoluteCanvasResizeCorner::TopLeft;
    }

    if (hitCorner(width, 0, relX, relY))
    {
      return AbsoluteCanvasResizeCorner::TopRight;
    }

    if (hitCorner(0, height, relX, relY))
    {
      return AbsoluteCanvasResizeCorner::BottomLeft;
    }

    if (hitCorner(width, height, relX, relY))
    {
      return AbsoluteCanvasResizeCorner::BottomRight;
    }

    return AbsoluteCanvasResizeCorner::None;
  }

  std::int32_t snapAbsoluteCanvasValue(std::int32_t value, bool snapToGrid, std::int32_t gridSize)
  {
    if (!snapToGrid || gridSize <= 0)
    {
      return value;
    }

    return ((value + gridSize / 2) / gridSize) * gridSize;
  }

  AbsoluteCanvasRect updateAbsoluteCanvasMoveDrag(AbsoluteCanvasRect startRect,
                                                  std::int32_t offsetX,
                                                  std::int32_t offsetY)
  {
    startRect.x += offsetX;
    startRect.y += offsetY;
    return startRect;
  }

  AbsoluteCanvasRect commitAbsoluteCanvasMoveDrag(AbsoluteCanvasRect startRect,
                                                  std::int32_t offsetX,
                                                  std::int32_t offsetY,
                                                  bool snapToGrid,
                                                  std::int32_t gridSize)
  {
    auto rect = updateAbsoluteCanvasMoveDrag(startRect, offsetX, offsetY);
    rect.x = snapAbsoluteCanvasValue(rect.x, snapToGrid, gridSize);
    rect.y = snapAbsoluteCanvasValue(rect.y, snapToGrid, gridSize);
    return rect;
  }

  AbsoluteCanvasRect updateAbsoluteCanvasResizeDrag(AbsoluteCanvasRect startRect,
                                                    AbsoluteCanvasResizeCorner corner,
                                                    std::int32_t offsetX,
                                                    std::int32_t offsetY,
                                                    std::int32_t minWidth,
                                                    std::int32_t minHeight,
                                                    bool snapToGrid,
                                                    std::int32_t gridSize)
  {
    auto rect = startRect;

    switch (corner)
    {
      case AbsoluteCanvasResizeCorner::TopLeft:
        rect.x = snapAbsoluteCanvasValue(startRect.x + offsetX, snapToGrid, gridSize);
        rect.y = snapAbsoluteCanvasValue(startRect.y + offsetY, snapToGrid, gridSize);
        rect.width = std::max(minWidth, startRect.width - offsetX);
        rect.height = std::max(minHeight, startRect.height - offsetY);
        break;

      case AbsoluteCanvasResizeCorner::TopRight:
        rect.y = snapAbsoluteCanvasValue(startRect.y + offsetY, snapToGrid, gridSize);
        rect.width = std::max(minWidth, startRect.width + offsetX);
        rect.height = std::max(minHeight, startRect.height - offsetY);
        break;

      case AbsoluteCanvasResizeCorner::BottomLeft:
        rect.x = snapAbsoluteCanvasValue(startRect.x + offsetX, snapToGrid, gridSize);
        rect.width = std::max(minWidth, startRect.width - offsetX);
        rect.height = std::max(minHeight, startRect.height + offsetY);
        break;

      case AbsoluteCanvasResizeCorner::BottomRight:
        rect.width = std::max(minWidth, startRect.width + offsetX);
        rect.height = std::max(minHeight, startRect.height + offsetY);
        break;

      case AbsoluteCanvasResizeCorner::None:
      default: break;
    }

    return rect;
  }

  AbsoluteCanvasRect commitAbsoluteCanvasResizeDrag(AbsoluteCanvasRect rect, bool snapToGrid, std::int32_t gridSize)
  {
    rect.x = snapAbsoluteCanvasValue(rect.x, snapToGrid, gridSize);
    rect.y = snapAbsoluteCanvasValue(rect.y, snapToGrid, gridSize);
    rect.width = snapAbsoluteCanvasValue(rect.width, snapToGrid, gridSize);
    rect.height = snapAbsoluteCanvasValue(rect.height, snapToGrid, gridSize);
    return rect;
  }

  AbsoluteCanvasRect nudgeAbsoluteCanvasRect(AbsoluteCanvasRect rect,
                                             AbsoluteCanvasNudgeDirection direction,
                                             bool snapToGrid,
                                             std::int32_t gridSize)
  {
    switch (auto const step = snapToGrid && gridSize > 0 ? gridSize : 1; direction)
    {
      case AbsoluteCanvasNudgeDirection::Up: rect.y -= step; break;
      case AbsoluteCanvasNudgeDirection::Down: rect.y += step; break;
      case AbsoluteCanvasNudgeDirection::Left: rect.x -= step; break;
      case AbsoluteCanvasNudgeDirection::Right: rect.x += step; break;
    }

    return rect;
  }
}
