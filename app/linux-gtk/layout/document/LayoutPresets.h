// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <cstdint>
#include <string_view>

namespace ao::gtk::layout
{
  enum class LayoutPresetId : std::uint8_t
  {
    Classic,
    Modern
  };

  LayoutPresetId presetIdFromString(std::string_view presetId);
  std::string_view presetIdToString(LayoutPresetId presetId) noexcept;

  uimodel::LayoutDocument makeDefaultLayout();
  uimodel::LayoutDocument makeBuiltInLayout(LayoutPresetId presetId);
} // namespace ao::gtk::layout
