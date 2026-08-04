// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <type_traits>

namespace ao::uimodel
{
  /**
   * @brief Surface a component tree is being built for.
   *
   * A main surface hosts interactive components; a tooltip surface hosts a
   * presentation-only subtree that neither binds actions nor persists runtime
   * state. Frontends select the surface when they build a node and propagate it
   * through the build recursion.
   */
  enum class LayoutSurface : std::uint8_t
  {
    Main,
    Tooltip,
  };

  /**
   * @brief Surfaces a registered component type declares support for.
   */
  enum class LayoutSurfaceCapability : std::uint8_t
  {
    Main = 1,
    Tooltip = 2,
  };

  using LayoutSurfaceCapabilityMask = std::underlying_type_t<LayoutSurfaceCapability>;

  constexpr LayoutSurfaceCapability surfaceCapability(LayoutSurface const surface) noexcept
  {
    return surface == LayoutSurface::Tooltip ? LayoutSurfaceCapability::Tooltip : LayoutSurfaceCapability::Main;
  }

  constexpr bool supportsSurface(LayoutSurfaceCapabilityMask const mask, LayoutSurface const surface) noexcept
  {
    return (mask & static_cast<LayoutSurfaceCapabilityMask>(surfaceCapability(surface))) != 0;
  }
} // namespace ao::uimodel
