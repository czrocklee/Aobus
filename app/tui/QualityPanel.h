// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <cstdint>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct PlaybackTransportSnapshot;
} // namespace ao::rt

namespace ao::uimodel
{
  enum class AudioQualityCategory : std::uint8_t;
}

namespace ao::tui
{
  uimodel::AobusSoulRgb qualityIndicatorColor(uimodel::AudioQualityCategory category);

  std::int32_t qualityPanelColumns(i18n::MessageCatalog const& textCatalog,
                                   rt::PlaybackTransportSnapshot const& state,
                                   std::int32_t terminalColumns);
  ftxui::Element qualityPanel(i18n::MessageCatalog const& textCatalog,
                              rt::PlaybackTransportSnapshot const& state,
                              std::int32_t columns = 0);
} // namespace ao::tui
