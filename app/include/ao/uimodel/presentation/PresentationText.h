// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  enum class MissingTrackValueKind : std::uint8_t;
  enum class AuthoringStatus : std::uint8_t;
}

namespace ao::uimodel
{
  enum class ListMembershipOperation : std::uint8_t
  {
    Add,
    Remove,
  };

  struct TrackPresentationText final
  {
    std::string_view label{};
    std::string_view description{};

    bool operator==(TrackPresentationText const&) const = default;
  };

  enum class AudioIconKind : std::uint8_t
  {
    OutputDevice,
    AudioServer,
  };

  struct AudioBackendPresentation final
  {
    std::string label{};
    std::string description{};
    std::string shortLabel{};
    std::string outputDeviceDescriptionFallback{};
    AudioIconKind iconKind = AudioIconKind::OutputDevice;

    bool operator==(AudioBackendPresentation const&) const = default;
  };

  struct AudioProfilePresentation final
  {
    std::string label{};
    std::string description{};

    bool operator==(AudioProfilePresentation const&) const = default;
  };

  std::string_view trackFieldLabel(i18n::MessageCatalog const& catalog, rt::TrackField field) noexcept;
  std::string_view trackGroupKeyLabel(i18n::MessageCatalog const& catalog, rt::TrackGroupKey key) noexcept;
  std::string_view missingTrackValueLabel(i18n::MessageCatalog const& catalog, rt::MissingTrackValueKind kind) noexcept;
  std::optional<TrackPresentationText> builtinTrackPresentation(i18n::MessageCatalog const& catalog,
                                                                std::string_view id) noexcept;
  AudioBackendPresentation audioBackendPresentation(i18n::MessageCatalog const& catalog, audio::BackendId const& id);
  AudioProfilePresentation audioProfilePresentation(i18n::MessageCatalog const& catalog, audio::ProfileId const& id);
  std::string completionDetail(i18n::MessageCatalog const& catalog, rt::CompletionDetail const& detail);
  std::string notificationMessage(i18n::MessageCatalog const& catalog, rt::NotificationMessage const& message);
  std::string notificationGroupMessage(i18n::MessageCatalog const& catalog,
                                       rt::NotificationSeverity severity,
                                       std::size_t count);
  std::string libraryTaskProgressDetail(i18n::MessageCatalog const& catalog,
                                        rt::LibraryTaskProgressKind kind,
                                        std::string_view subject);
  std::string libraryTaskProgressCompact(i18n::MessageCatalog const& catalog,
                                         rt::LibraryTaskProgressKind kind,
                                         std::string_view subject);
  std::string formatLibraryScanMessage(i18n::MessageCatalog const& catalog, LibraryScanOutcome const& outcome);
  std::string trackSelectionSummary(i18n::MessageCatalog const& catalog,
                                    std::size_t count,
                                    std::string_view duration = {});
  std::string smartListMembershipEditingText(i18n::MessageCatalog const& catalog,
                                             bool direct,
                                             std::string_view expression = {});
  std::string smartListPreviewStatus(i18n::MessageCatalog const& catalog,
                                     bool expressionValid,
                                     std::size_t count,
                                     bool isAllTracks,
                                     bool localEmpty);
  std::string formatListMembershipMessage(i18n::MessageCatalog const& catalog,
                                          rt::AuthoringStatus status,
                                          ListMembershipOperation operation,
                                          std::string_view listName,
                                          std::string_view tagExpression,
                                          std::size_t changedTrackCount,
                                          std::size_t forgottenPositionCount);
  std::string trackChannelText(i18n::MessageCatalog const& catalog, std::uint8_t channels);
  std::string_view transportControlLabel(i18n::MessageCatalog const& catalog, PlaybackCommand command) noexcept;
  std::string_view playbackActionLabel(i18n::MessageCatalog const& catalog, PlaybackCommand command) noexcept;
  std::string volumeTooltip(i18n::MessageCatalog const& catalog,
                            std::int32_t percent,
                            bool muted,
                            bool hardwareAssisted);
} // namespace ao::uimodel
