// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryChanges.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  enum class MissingTrackValueKind : std::uint8_t;
}

namespace ao::uimodel
{
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
   * Immutable authored-copy catalog for shared interactive presentation.
   *
   * The initial catalog is stateless because Aobus currently ships one English
   * vocabulary. Keeping it as a value type establishes the ownership boundary
   * without committing the application to a localization storage mechanism.
   */
  class PresentationTextCatalog final
  {
  public:
    std::string_view trackFieldLabel(rt::TrackField field) const noexcept;
    std::string_view trackGroupKeyLabel(rt::TrackGroupKey key) const noexcept;
    std::string_view missingTrackValueLabel(rt::MissingTrackValueKind kind) const noexcept;
    std::optional<TrackPresentationText> builtinTrackPresentation(std::string_view id) const noexcept;
    std::string_view createCustomTrackPresentationLabel() const noexcept;
    AudioBackendPresentation audioBackend(audio::BackendId const& id) const;
    AudioProfilePresentation audioProfile(audio::ProfileId const& id) const;
    std::string_view systemDefaultOutputDeviceLabel() const noexcept;
    std::string completionDetail(rt::CompletionDetail const& detail) const;
    std::string notificationMessage(rt::NotificationMessage const& message) const;
    std::string libraryTaskProgressDetail(rt::LibraryChanges::LibraryTaskProgressKind kind,
                                          std::string_view subject) const;
    std::string libraryTaskProgressCompact(rt::LibraryChanges::LibraryTaskProgressKind kind,
                                           std::string_view subject) const;
  };
} // namespace ao::uimodel
