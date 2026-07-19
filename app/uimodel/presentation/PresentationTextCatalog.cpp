// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    using F = rt::TrackField;

    constexpr auto kTrackFieldLabels = std::to_array<std::string_view>({
      "Title",    "Artist",       "Album",        "Album Artist",    "Genre",    "Composer",  "Conductor",
      "Ensemble", "Work",         "Movement",     "Soloist",         "Year",     "Disc",      "Total Discs",
      "Track",    "Total Tracks", "Movement No.", "Total Movements", "Duration", "Tags",      "File Path",
      "Codec",    "Sample Rate",  "Channels",     "Bit Depth",       "Bitrate",  "File Size", "Modified",
      "Track #",  "Technical",    "Quality",
    });

    static_assert(kTrackFieldLabels.size() == rt::kTrackFieldCount);

    struct BuiltinTrackPresentationText final
    {
      std::string_view id;
      TrackPresentationText text;
    };

    constexpr auto kBuiltinTrackPresentationTexts = std::to_array<BuiltinTrackPresentationText>({
      {.id = "library", .text = {.label = "Library", .description = "All tracks in album-artist and album order."}},
      {.id = "list-order", .text = {.label = "List Order", .description = "Flat list preserving source order."}},
      {.id = "songs", .text = {.label = "Songs", .description = "Flat list of every track ordered by title."}},
      {.id = "albums", .text = {.label = "Albums", .description = "Grouped by album with track-oriented columns."}},
      {.id = "artists",
       .text = {.label = "Artists", .description = "Grouped by album artist with discography ordering."}},
      {.id = "performers",
       .text = {.label = "Performers", .description = "Grouped by track artist, including featured guests."}},
      {.id = "genres", .text = {.label = "Genres", .description = "Grouped by genre."}},
      {.id = "years", .text = {.label = "Years", .description = "Grouped by year."}},
      {.id = "classical-composers",
       .text = {.label = "Classical: Composers", .description = "Grouped by composer with work-oriented columns."}},
      {.id = "classical-conductors",
       .text = {.label = "Classical: Conductors",
                .description = "Grouped by conductor with work and ensemble columns."}},
      {.id = "classical-works",
       .text = {.label = "Classical: Works", .description = "Grouped by work with composer-oriented columns."}},
      {.id = "tagging",
       .text = {.label = "Tagging",
                .description = "Flat list with raw disc/track, genre, year, and tags for curation."}},
      {.id = "technical",
       .text = {.label = "Technical",
                .description = "Flat list of codec, bitrate, size, and path for file inspection."}},
    });
  } // namespace

  std::string_view PresentationTextCatalog::trackFieldLabel(rt::TrackField const field) const noexcept
  {
    auto const index = static_cast<std::size_t>(field);
    return index < kTrackFieldLabels.size() ? kTrackFieldLabels[index] : std::string_view{};
  }

  std::string_view PresentationTextCatalog::trackGroupKeyLabel(rt::TrackGroupKey const key) const noexcept
  {
    switch (key)
    {
      case rt::TrackGroupKey::None: return "None";
      case rt::TrackGroupKey::Artist: return "Artist";
      case rt::TrackGroupKey::Album: return "Album";
      case rt::TrackGroupKey::AlbumArtist: return "Album Artist";
      case rt::TrackGroupKey::Genre: return "Genre";
      case rt::TrackGroupKey::Composer: return "Composer";
      case rt::TrackGroupKey::Conductor: return "Conductor";
      case rt::TrackGroupKey::Ensemble: return "Ensemble";
      case rt::TrackGroupKey::Work: return "Work";
      case rt::TrackGroupKey::Year: return "Year";
    }

    return {};
  }

  std::string_view PresentationTextCatalog::missingTrackValueLabel(rt::MissingTrackValueKind const kind) const noexcept
  {
    switch (kind)
    {
      case rt::MissingTrackValueKind::Artist: return "Unknown Artist";
      case rt::MissingTrackValueKind::Album: return "Unknown Album";
      case rt::MissingTrackValueKind::Year: return "Unknown Year";
      case rt::MissingTrackValueKind::Genre: return "Unknown Genre";
      case rt::MissingTrackValueKind::Composer: return "Unknown Composer";
      case rt::MissingTrackValueKind::Conductor: return "Unknown Conductor";
      case rt::MissingTrackValueKind::Ensemble: return "Unknown Ensemble";
      case rt::MissingTrackValueKind::Work: return "Unknown Work";
    }

    return {};
  }

  std::optional<TrackPresentationText> PresentationTextCatalog::builtinTrackPresentation(
    std::string_view const id) const noexcept
  {
    for (auto const& entry : kBuiltinTrackPresentationTexts)
    {
      if (entry.id == id)
      {
        return entry.text;
      }
    }

    return std::nullopt;
  }

  std::string_view PresentationTextCatalog::createCustomTrackPresentationLabel() const noexcept
  {
    return "Create Custom View...";
  }

  AudioBackendPresentation PresentationTextCatalog::audioBackend(audio::BackendId const& id) const
  {
    if (id == audio::kBackendPipeWire)
    {
      return AudioBackendPresentation{
        .label = "PipeWire",
        .description = "Modern Linux audio server with low latency",
        .shortLabel = "PW",
        .outputDeviceDescriptionFallback = "PipeWire",
        .iconKind = AudioIconKind::AudioServer,
      };
    }

    if (id == audio::kBackendAlsa)
    {
      return AudioBackendPresentation{
        .label = "ALSA",
        .description = "Advanced Linux Sound Architecture (Direct Hardware Access)",
        .shortLabel = "ALSA",
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendWasapi)
    {
      return AudioBackendPresentation{
        .label = "WASAPI",
        .description = "Windows Audio Session API",
        .shortLabel = "WASAPI",
        .outputDeviceDescriptionFallback = "WASAPI render endpoint",
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    auto const& fallback = id.raw();
    return AudioBackendPresentation{
      .label = std::string{fallback},
      .shortLabel = std::string{fallback},
    };
  }

  AudioProfilePresentation PresentationTextCatalog::audioProfile(audio::ProfileId const& id) const
  {
    if (id == audio::kProfileShared)
    {
      return AudioProfilePresentation{
        .label = "Shared Mode",
        .description = "System-level mixing with other applications",
      };
    }

    if (id == audio::kProfileExclusive)
    {
      return AudioProfilePresentation{
        .label = "Exclusive Mode",
        .description = "Direct access to the hardware device",
      };
    }

    return AudioProfilePresentation{.label = std::string{id.raw()}};
  }

  std::string_view PresentationTextCatalog::systemDefaultOutputDeviceLabel() const noexcept
  {
    return "System Default";
  }

  std::string PresentationTextCatalog::completionDetail(rt::CompletionDetail const& detail) const
  {
    switch (detail.kind)
    {
      case rt::CompletionDetailKind::None: return {};
      case rt::CompletionDetailKind::ResolvedText: return detail.resolvedText;
      case rt::CompletionDetailKind::Field: return "field";
      case rt::CompletionDetailKind::Alias: return "alias";
      case rt::CompletionDetailKind::Operator: return "operator";
      case rt::CompletionDetailKind::LogicalOperator: return "logical operator";
      case rt::CompletionDetailKind::Frequency: return std::to_string(detail.frequency);
    }

    return {};
  }

  std::string PresentationTextCatalog::notificationMessage(rt::NotificationMessage const& message) const
  {
    auto const* report = std::get_if<rt::NotificationReport>(&message);

    if (report == nullptr)
    {
      return std::get<std::string>(message);
    }

    auto const failureReason = report->detail.empty() ? std::string{"unknown error"} : report->detail;
    auto const trackLabel = [&]
    {
      if (!report->subject.empty())
      {
        return report->subject;
      }

      if (report->trackId != kInvalidTrackId)
      {
        return "track " + std::to_string(report->trackId.raw());
      }

      return std::string{"playback"};
    };

    switch (report->templateId)
    {
      case rt::NotificationReportTemplate::PlaybackTrackOpenFailed:
        return "Could not play " + trackLabel() + ": " + failureReason;
      case rt::NotificationReportTemplate::PlaybackDecodeFailed:
        return "Playback failed for " + trackLabel() + ": " + failureReason;
      case rt::NotificationReportTemplate::PlaybackRouteActivationFailed:
        return "Could not start playback: " + failureReason;
      case rt::NotificationReportTemplate::PlaybackDeviceLost: return "Playback device failed: " + failureReason;
      case rt::NotificationReportTemplate::PlaybackSequenceFinished: return "Playback sequence finished";
      case rt::NotificationReportTemplate::PlaybackTracksSkipped:
        return report->count == 1 ? std::string{"Skipped 1 unplayable track"}
                                  : "Skipped " + std::to_string(report->count) + " unplayable tracks";
      case rt::NotificationReportTemplate::PlaybackStoppedAfterFailures:
        return "Playback stopped after " + std::to_string(report->count) + " unplayable tracks";
      case rt::NotificationReportTemplate::PlaybackStoppedForTrack:
      {
        auto text =
          report->subject.empty() ? std::string{"Playback stopped"} : "Playback stopped for " + report->subject;

        if (!report->detail.empty())
        {
          text += ": " + report->detail;
        }

        return text;
      }
    }

    return "Notification";
  }

  std::string PresentationTextCatalog::libraryTaskProgressDetail(rt::LibraryChanges::LibraryTaskProgressKind const kind,
                                                                 std::string_view const subject) const
  {
    auto prefix = std::string_view{};

    switch (kind)
    {
      case rt::LibraryChanges::LibraryTaskProgressKind::Scanning: prefix = "Scanning"; break;
      case rt::LibraryChanges::LibraryTaskProgressKind::Updating: prefix = "Updating"; break;
      case rt::LibraryChanges::LibraryTaskProgressKind::Fingerprinting: prefix = "Fingerprinting"; break;
      case rt::LibraryChanges::LibraryTaskProgressKind::IndexingAudioIdentity:
        prefix = "Indexing audio identity";
        break;
    }

    return subject.empty() ? std::string{prefix} : std::string{prefix} + ": " + std::string{subject};
  }

  std::string PresentationTextCatalog::libraryTaskProgressCompact(
    rt::LibraryChanges::LibraryTaskProgressKind const kind,
    std::string_view const subject) const
  {
    switch (kind)
    {
      case rt::LibraryChanges::LibraryTaskProgressKind::Scanning: return "Scanning library";
      case rt::LibraryChanges::LibraryTaskProgressKind::Updating: return "Updating library";
      case rt::LibraryChanges::LibraryTaskProgressKind::Fingerprinting:
      case rt::LibraryChanges::LibraryTaskProgressKind::IndexingAudioIdentity:
        return libraryTaskProgressDetail(kind, subject);
    }

    return {};
  }
} // namespace ao::uimodel
