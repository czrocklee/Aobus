// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  std::string_view transportControlLabel(i18n::MessageCatalog const& catalog, PlaybackCommand command) noexcept;
  std::string_view playbackActionLabel(i18n::MessageCatalog const& catalog, PlaybackCommand command) noexcept;
} // namespace ao::uimodel
