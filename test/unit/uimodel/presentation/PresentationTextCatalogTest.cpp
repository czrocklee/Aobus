// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("PresentationTextCatalog - exhaustively labels track presentation semantics",
            "[uimodel][unit][presentation]")
  {
    auto const catalog = PresentationTextCatalog{};

    for (std::size_t index = 0; index < rt::kTrackFieldCount; ++index)
    {
      auto const field = static_cast<rt::TrackField>(index);
      INFO("Track field index " << index);
      CHECK_FALSE(catalog.trackFieldLabel(field).empty());
    }

    for (std::size_t index = 0; index < rt::kTrackGroupKeyCount; ++index)
    {
      auto const key = static_cast<rt::TrackGroupKey>(index);
      INFO("Track group key index " << index);
      CHECK_FALSE(catalog.trackGroupKeyLabel(key).empty());
    }

    for (auto const kind : {rt::MissingTrackValueKind::Artist,
                            rt::MissingTrackValueKind::Album,
                            rt::MissingTrackValueKind::Year,
                            rt::MissingTrackValueKind::Genre,
                            rt::MissingTrackValueKind::Composer,
                            rt::MissingTrackValueKind::Conductor,
                            rt::MissingTrackValueKind::Ensemble,
                            rt::MissingTrackValueKind::Work})
    {
      CHECK_FALSE(catalog.missingTrackValueLabel(kind).empty());
    }

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      INFO("Built-in presentation " << preset.spec.id);
      auto const optText = catalog.builtinTrackPresentation(preset.spec.id);
      REQUIRE(optText);
      CHECK_FALSE(optText->label.empty());
      CHECK_FALSE(optText->description.empty());
    }

    CHECK_FALSE(catalog.builtinTrackPresentation("extension-view"));
    CHECK(catalog.trackFieldLabel(rt::TrackField::SampleRate) == "Sample Rate");
    CHECK(catalog.createCustomTrackPresentationLabel() == "Create Custom View...");
  }

  TEST_CASE("PresentationTextCatalog - renders structured group headings only at the UIModel boundary",
            "[uimodel][unit][presentation]")
  {
    auto const heading = rt::TrackGroupHeading{
      .primary = std::string{"Greatest Hits"},
      .secondary = rt::MissingTrackValueKind::Artist,
      .tertiary = std::uint16_t{2020},
    };

    CHECK(formatTrackGroupHeading(PresentationTextCatalog{}, heading) == TrackGroupHeadingPresentation{
                                                                           .primaryText = "Greatest Hits",
                                                                           .secondaryText = "Unknown Artist",
                                                                           .tertiaryText = "2020",
                                                                         });
  }

  TEST_CASE("PresentationTextCatalog - owns backend profile and semantic icon presentation",
            "[uimodel][unit][presentation]")
  {
    auto const catalog = PresentationTextCatalog{};

    auto const pipeWire = catalog.audioBackend(audio::kBackendPipeWire);
    CHECK(pipeWire.label == "PipeWire");
    CHECK(pipeWire.shortLabel == "PW");
    CHECK(pipeWire.iconKind == AudioIconKind::AudioServer);
    CHECK_FALSE(pipeWire.description.empty());

    auto const alsa = catalog.audioBackend(audio::kBackendAlsa);
    CHECK(alsa.label == "ALSA");
    CHECK(alsa.iconKind == AudioIconKind::OutputDevice);

    auto const wasapi = catalog.audioBackend(audio::kBackendWasapi);
    CHECK(wasapi.outputDeviceDescriptionFallback == "WASAPI render endpoint");

    auto const unknown = catalog.audioBackend(audio::BackendId{"extension-backend"});
    CHECK(unknown.label == "extension-backend");
    CHECK(unknown.shortLabel == "extension-backend");
    CHECK(unknown.description.empty());

    CHECK(catalog.audioProfile(audio::kProfileShared).label == "Shared Mode");
    CHECK(catalog.audioProfile(audio::kProfileExclusive).label == "Exclusive Mode");
    CHECK(catalog.audioProfile(audio::ProfileId{"extension-profile"}).label == "extension-profile");
    CHECK(catalog.systemDefaultOutputDeviceLabel() == "System Default");
  }

  TEST_CASE("PresentationTextCatalog - resolves typed completion details", "[uimodel][unit][presentation]")
  {
    auto const catalog = PresentationTextCatalog{};

    CHECK(catalog.completionDetail({.kind = rt::CompletionDetailKind::Field}) == "field");
    CHECK(catalog.completionDetail({.kind = rt::CompletionDetailKind::Alias}) == "alias");
    CHECK(catalog.completionDetail({.kind = rt::CompletionDetailKind::Operator}) == "operator");
    CHECK(catalog.completionDetail({.kind = rt::CompletionDetailKind::LogicalOperator}) == "logical operator");
    CHECK(catalog.completionDetail(rt::CompletionDetail::makeUsageFrequency(42)) == "42");
    CHECK(catalog.completionDetail(rt::CompletionDetail::makeResolvedText("frontend detail")) == "frontend detail");
    CHECK(catalog.completionDetail({}).empty());
  }

  TEST_CASE("PresentationTextCatalog - expands structured playback reports", "[uimodel][unit][presentation]")
  {
    auto const catalog = PresentationTextCatalog{};

    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTrackOpenFailed,
            .trackId = TrackId{7},
            .detail = "file missing",
          }) == "Could not play track 7: file missing");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackDecodeFailed,
            .subject = "Song",
            .detail = "bad frame",
          }) == "Playback failed for Song: bad frame");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackRouteActivationFailed,
            .detail = "route unavailable",
          }) == "Could not start playback: route unavailable");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackDeviceLost,
            .detail = "device removed",
          }) == "Playback device failed: device removed");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackSequenceFinished,
          }) == "Playback sequence finished");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
            .count = 1,
          }) == "Skipped 1 unplayable track");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
            .count = 4,
          }) == "Skipped 4 unplayable tracks");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackStoppedAfterFailures,
            .count = 3,
          }) == "Playback stopped after 3 unplayable tracks");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
            .subject = "Song",
            .detail = "decode failed",
          }) == "Playback stopped for Song: decode failed");

    // Fallbacks: an empty detail resolves to a generic failure reason.
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTrackOpenFailed,
            .trackId = TrackId{7},
          }) == "Could not play track 7: unknown error");
    // Fallbacks: an empty subject with no track id resolves to a generic subject.
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackDecodeFailed,
            .detail = "bad frame",
          }) == "Playback failed for playback: bad frame");
    // Fallbacks: PlaybackStoppedForTrack without a subject, with and without a detail.
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
            .detail = "decode failed",
          }) == "Playback stopped: decode failed");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
          }) == "Playback stopped");

    CHECK(catalog.notificationMessage(rt::NotificationMessage{std::string{"Frontend message"}}) == "Frontend message");
  }

  TEST_CASE("PresentationTextCatalog - library progress is selected by kind rather than text prefixes",
            "[uimodel][unit][presentation]")
  {
    auto const catalog = PresentationTextCatalog{};
    using Kind = rt::LibraryChanges::LibraryTaskProgressKind;

    CHECK(catalog.libraryTaskProgressDetail(Kind::Scanning, "Scanning: literal.flac") ==
          "Scanning: Scanning: literal.flac");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Scanning, "literal.flac") == "Scanning library");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Updating, "literal.flac") == "Updating library");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Fingerprinting, "literal.flac") == "Fingerprinting: literal.flac");
    CHECK(catalog.libraryTaskProgressCompact(Kind::IndexingAudioIdentity, "literal.flac") ==
          "Indexing audio identity: literal.flac");
  }
} // namespace ao::uimodel::test
