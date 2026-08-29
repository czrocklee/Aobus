// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackCommandText.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <string_view>

namespace ao::uimodel
{
  std::string_view transportControlLabel(i18n::MessageCatalog const& catalog, PlaybackCommand const command) noexcept
  {
    using i18n::MessageId;
    using i18n::requiredText;

    switch (command)
    {
      case PlaybackCommand::Play:
      case PlaybackCommand::PlayPause: return requiredText(catalog, MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return requiredText(catalog, MessageId::PlaybackControlPause);
      case PlaybackCommand::Stop: return requiredText(catalog, MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return requiredText(catalog, MessageId::PlaybackControlNextTrack);
      case PlaybackCommand::Previous: return requiredText(catalog, MessageId::PlaybackControlPreviousTrack);
      case PlaybackCommand::ToggleShuffle: return requiredText(catalog, MessageId::PlaybackControlShuffle);
      case PlaybackCommand::CycleRepeat: return requiredText(catalog, MessageId::PlaybackControlRepeat);
    }

    return {};
  }

  std::string_view playbackActionLabel(i18n::MessageCatalog const& catalog, PlaybackCommand const command) noexcept
  {
    using i18n::MessageId;
    using i18n::requiredText;

    switch (command)
    {
      case PlaybackCommand::Play: return requiredText(catalog, MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return requiredText(catalog, MessageId::PlaybackControlPause);
      case PlaybackCommand::PlayPause: return requiredText(catalog, MessageId::PlaybackActionPlayPause);
      case PlaybackCommand::Stop: return requiredText(catalog, MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return requiredText(catalog, MessageId::PlaybackActionNext);
      case PlaybackCommand::Previous: return requiredText(catalog, MessageId::PlaybackActionPrevious);
      case PlaybackCommand::ToggleShuffle: return requiredText(catalog, MessageId::PlaybackActionToggleShuffle);
      case PlaybackCommand::CycleRepeat: return requiredText(catalog, MessageId::PlaybackActionCycleRepeat);
    }

    return {};
  }
} // namespace ao::uimodel
