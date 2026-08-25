// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  enum class MissingTrackValueKind : std::uint8_t;
  enum class AuthoringStatus : std::uint8_t;
}

namespace ao::uimodel
{
  class AudioQualityFormatter;

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

  /**
   * Immutable locale-selected catalog for shared interactive presentation.
   *
   * Borrowed views remain valid for the lifetime of this value. Messages with
   * runtime arguments are formatted on demand by the underlying catalog.
   */
  class PresentationTextCatalog final
  {
  public:
    explicit PresentationTextCatalog(i18n::MessageCatalog const& catalog);
    PresentationTextCatalog(PresentationTextCatalog const&) = default;
    PresentationTextCatalog(PresentationTextCatalog&&) noexcept = default;
    PresentationTextCatalog& operator=(PresentationTextCatalog const&) = default;
    PresentationTextCatalog& operator=(PresentationTextCatalog&&) noexcept = default;
    ~PresentationTextCatalog();

    std::string_view text(i18n::MessageId id) const noexcept;
    std::string format(i18n::MessageId id, std::initializer_list<i18n::MessageArgument> arguments) const;

    std::string_view trackFieldLabel(rt::TrackField field) const noexcept;
    std::string_view trackGroupKeyLabel(rt::TrackGroupKey key) const noexcept;
    std::string_view missingTrackValueLabel(rt::MissingTrackValueKind kind) const noexcept;
    std::optional<TrackPresentationText> builtinTrackPresentation(std::string_view id) const noexcept;
    AudioBackendPresentation audioBackend(audio::BackendId const& id) const;
    AudioProfilePresentation audioProfile(audio::ProfileId const& id) const;
    std::string completionDetail(rt::CompletionDetail const& detail) const;
    std::string notificationMessage(rt::NotificationMessage const& message) const;
    std::string notificationGroupMessage(rt::NotificationSeverity severity, std::size_t count) const;
    std::string libraryTaskProgressDetail(rt::LibraryTaskProgressKind kind, std::string_view subject) const;
    std::string libraryTaskProgressCompact(rt::LibraryTaskProgressKind kind, std::string_view subject) const;

    // What a finished scan is reported as. Every shell that can scan says this
    // sentence, so it is written once rather than once per window.
    std::string libraryScanMessage(LibraryScanOutcome const& outcome) const;

    std::string trackSelectionSummary(std::size_t count, std::string_view duration = {}) const;
    std::string smartListMembershipEditingText(bool direct, std::string_view expression = {}) const;
    std::string smartListPreviewStatus(bool expressionValid,
                                       std::size_t count,
                                       bool isAllTracks,
                                       bool localEmpty) const;
    std::string listMembershipNotification(rt::AuthoringStatus status,
                                           ListMembershipOperation operation,
                                           std::string_view listName,
                                           std::string_view tagExpression,
                                           std::size_t changedTrackCount,
                                           std::size_t forgottenPositionCount) const;

    std::string trackChannelText(std::uint8_t channels) const;
    std::string_view transportControlLabel(PlaybackCommand command) const noexcept;
    std::string_view playbackActionLabel(PlaybackCommand command) const noexcept;
    std::string volumeTooltip(std::int32_t percent, bool muted, bool hardwareAssisted) const;
    AudioQualityFormatter const& audioQualityFormatter() const noexcept;

  private:
    struct Impl;

    std::shared_ptr<Impl const> _implPtr;
  };
} // namespace ao::uimodel
