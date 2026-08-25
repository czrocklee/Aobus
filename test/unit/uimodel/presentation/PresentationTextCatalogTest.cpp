// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  using i18n::MessageId;

  TEST_CASE("PresentationTextCatalog - resolves one-to-one frontend messages through typed ids",
            "[uimodel][unit][localization]")
  {
    auto const& english = ao::test::englishPresentationTextCatalog();
    CHECK(english.text(MessageId::GtkPreferencesTitle) == "Preferences");
    CHECK(english.text(MessageId::LibraryBusyTryAgain) == "Library is busy. Try again.");
    CHECK(english.format(MessageId::GtkShortcutConflictMessage, {{"chord", "Ctrl+P"}, {"owner", "Play/Pause"}}) ==
          "Ctrl+P is already assigned to \"Play/Pause\". Reassign it to this action?");
    CHECK(english.playbackActionLabel(PlaybackCommand::PlayPause) == "Play/Pause");
    CHECK(english.playbackActionLabel(PlaybackCommand::Next) == "Next");
    CHECK(english.playbackActionLabel(PlaybackCommand::Previous) == "Previous");
    CHECK(english.playbackActionLabel(PlaybackCommand::ToggleShuffle) == "Toggle Shuffle");
    CHECK(english.playbackActionLabel(PlaybackCommand::CycleRepeat) == "Cycle Repeat");

    auto const german = ao::test::presentationTextCatalog("de-DE");
    CHECK(german.format(MessageId::GtkPresentationCopyLabel, {{"label", "Alben"}}) == "Kopie von Alben");
    CHECK(german.format(MessageId::TuiPresentationBadge, {{"id", "Standard"}}) == "Ansicht:Standard");
    CHECK(german.playbackActionLabel(PlaybackCommand::PlayPause) == "Wiedergabe/Pause");

    auto const chinese = ao::test::presentationTextCatalog("zh-CN");
    CHECK(chinese.format(MessageId::GtkPresentationCopyLabel, {{"label", "专辑"}}) == "专辑 副本");
    CHECK(chinese.format(MessageId::TuiPresentationBadge, {{"id", "默认"}}) == "视图:默认");
    CHECK(chinese.playbackActionLabel(PlaybackCommand::PlayPause) == "播放/暂停");
    CHECK(chinese.trackFieldLabel(rt::TrackField::SampleRate) == "采样率");

    auto const traditionalChinese = ao::test::presentationTextCatalog("zh-TW");
    CHECK(traditionalChinese.format(MessageId::GtkPresentationCopyLabel, {{"label", "專輯"}}) == "專輯 複本");
    CHECK(traditionalChinese.format(MessageId::TuiPresentationBadge, {{"id", "預設"}}) == "檢視:預設");
    CHECK(traditionalChinese.playbackActionLabel(PlaybackCommand::PlayPause) == "播放/暫停");
    CHECK(traditionalChinese.trackFieldLabel(rt::TrackField::SampleRate) == "取樣率");

    auto const japanese = ao::test::presentationTextCatalog("ja-JP");
    CHECK(japanese.format(MessageId::GtkPresentationCopyLabel, {{"label", "アルバム"}}) == "アルバム のコピー");
    CHECK(japanese.format(MessageId::TuiPresentationBadge, {{"id", "デフォルト"}}) == "表示:デフォルト");
    CHECK(japanese.playbackActionLabel(PlaybackCommand::PlayPause) == "再生/一時停止");
    CHECK(japanese.trackFieldLabel(rt::TrackField::SampleRate) == "サンプリングレート");

    auto const spanish = ao::test::presentationTextCatalog("es-ES");
    CHECK(spanish.format(MessageId::GtkPresentationCopyLabel, {{"label", "Álbumes"}}) == "Copia de Álbumes");
    CHECK(spanish.format(MessageId::TuiPresentationBadge, {{"id", "predeterminado"}}) == "vista:predeterminado");
    CHECK(spanish.playbackActionLabel(PlaybackCommand::PlayPause) == "Reproducir/Pausar");
    CHECK(spanish.trackFieldLabel(rt::TrackField::SampleRate) == "Frecuencia de muestreo");

    auto const french = ao::test::presentationTextCatalog("fr-FR");
    CHECK(french.format(MessageId::GtkPresentationCopyLabel, {{"label", "Albums"}}) == "Copie de « Albums »");
    CHECK(french.format(MessageId::TuiPresentationBadge, {{"id", "par défaut"}}) == "vue:par défaut");
    CHECK(french.playbackActionLabel(PlaybackCommand::PlayPause) == "Lecture/Pause");
    CHECK(french.trackFieldLabel(rt::TrackField::SampleRate) == "Fréquence d'échantillonnage");
  }

  TEST_CASE("PresentationTextCatalog - exhaustively labels track presentation semantics",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishPresentationTextCatalog();

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
    CHECK(catalog.text(MessageId::CreateCustomTrackPresentation) == "Create Custom View...");
  }

  TEST_CASE("PresentationTextCatalog - renders structured group headings only at the UIModel boundary",
            "[uimodel][unit][presentation]")
  {
    auto const heading = rt::TrackGroupHeading{
      .primary = std::string{"Greatest Hits"},
      .secondary = rt::MissingTrackValueKind::Artist,
      .tertiary = std::uint16_t{2020},
    };

    CHECK(formatTrackGroupHeading(ao::test::englishPresentationTextCatalog(), heading) ==
          TrackGroupHeadingPresentation{
            .primaryText = "Greatest Hits",
            .secondaryText = "Unknown Artist",
            .tertiaryText = "2020",
          });
  }

  TEST_CASE("PresentationTextCatalog - owns backend profile and semantic icon presentation",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishPresentationTextCatalog();

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
    CHECK(catalog.text(MessageId::SystemDefaultOutputDevice) == "System Default");
  }

  TEST_CASE("PresentationTextCatalog - resolves typed completion details", "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishPresentationTextCatalog();

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
    auto const& catalog = ao::test::englishPresentationTextCatalog();

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
    auto const& catalog = ao::test::englishPresentationTextCatalog();
    using Kind = rt::LibraryTaskProgressKind;

    CHECK(catalog.libraryTaskProgressDetail(Kind::Scanning, "Scanning: literal.flac") ==
          "Scanning: Scanning: literal.flac");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Scanning, "literal.flac") == "Scanning library");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Updating, "literal.flac") == "Updating library");
    CHECK(catalog.libraryTaskProgressCompact(Kind::Fingerprinting, "literal.flac") == "Fingerprinting: literal.flac");
    CHECK(catalog.libraryTaskProgressCompact(Kind::IndexingAudioIdentity, "literal.flac") ==
          "Indexing audio identity: literal.flac");
  }

  TEST_CASE("PresentationTextCatalog - track filter errors carry the parser diagnostic verbatim",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishPresentationTextCatalog();

    CHECK(catalog.format(MessageId::TrackFilterError, {{"diagnostic", "unexpected token ')'"}}) ==
          "Filter error: unexpected token ')'");
    CHECK(catalog.format(MessageId::TrackFilterError, {{"diagnostic", ""}}) == "Filter error: ");
  }

  TEST_CASE("PresentationTextCatalog - formats shared library and smart-list copy",
            "[uimodel][unit][presentation][localization]")
  {
    auto const& catalog = ao::test::englishPresentationTextCatalog();

    CHECK(catalog.text(MessageId::LibraryAllTracks) == "All Tracks");
    CHECK(catalog.text(MessageId::LibraryUnnamedList) == "<Unnamed List>");
    CHECK(catalog.format(MessageId::TrackCount, {{"count", 1}}) == "1 track");
    CHECK(catalog.format(MessageId::TrackCount, {{"count", 4}}) == "4 tracks");
    CHECK(catalog.trackSelectionSummary(0).empty());
    CHECK(catalog.trackSelectionSummary(2, "5:00") == "2 items selected (5:00)");
    CHECK(catalog.smartListMembershipEditingText(true, R"(#"road-trip")") ==
          R"(Direct membership editing via #"road-trip")");
    CHECK(catalog.smartListMembershipEditingText(false) == "Computed membership — edit tags or the expression");
    CHECK(catalog.text(MessageId::SmartListExpressionNone) == "(none)");
    CHECK(catalog.smartListPreviewStatus(true, 4, false, true) == "Showing all 4 tracks from source");
    CHECK(catalog.smartListPreviewStatus(true, 14, true, false) == "Showing 10 of 14 matches");
    CHECK(catalog.smartListPreviewStatus(false, 0, false, false) == "Invalid filter");
    CHECK(catalog.text(MessageId::SmartListUntitledTrack) == "(untitled)");
    CHECK(catalog.text(MessageId::ListOrderSavedListsOnly) == "Manual ordering is available for saved Lists only.");
    CHECK(catalog.format(MessageId::ListOrderMoved, {{"count", 1}}) == "Moved 1 track in Manual Order.");
    CHECK(catalog.format(MessageId::ListOrderMoved, {{"count", 3}}) == "Moved 3 tracks in Manual Order.");
    CHECK(catalog.format(MessageId::ListOrderReset, {{"count", 2}}) ==
          "Reset Manual Order and forgot 2 saved positions.");
    CHECK(catalog.format(MessageId::ListOrderForgotHidden, {{"count", 1}}) == "Forgot 1 hidden saved position.");
    CHECK(catalog.listMembershipNotification(
            rt::AuthoringStatus::Applied, ListMembershipOperation::Add, "Road", "#road", 2, 0) ==
          "Added #road to 2 tracks in Road.");
    CHECK(catalog.listMembershipNotification(
            rt::AuthoringStatus::Busy, ListMembershipOperation::Add, "Road", "#road", 0, 0) ==
          "Library is busy. Try again.");
    CHECK(catalog.listMembershipNotification(
            rt::AuthoringStatus::NoOp, ListMembershipOperation::Remove, "Road", "#road", 0, 0) ==
          "No #road membership or saved position remained in Road.");
    CHECK(catalog.listMembershipNotification(
            rt::AuthoringStatus::Applied, ListMembershipOperation::Remove, "Road", "#road", 2, 1) ==
          "Removed #road from 2 tracks and forgot 1 saved position in Road.");

    CHECK(catalog.text(MessageId::LibraryAudioIdentityIndexingComplete) == "Audio identity indexing complete");
    CHECK(catalog.text(MessageId::LibraryReadyIndexingAudioIdentity) ==
          "Library ready; indexing audio identity in background");
    CHECK(catalog.format(MessageId::LibraryExportFailed, {{"error", "disk full"}}) == "Export failed: disk full");
    CHECK(catalog.text(MessageId::LibraryExported) == "Library exported successfully");
    CHECK(catalog.format(MessageId::LibraryImportFailed, {{"error", "bad YAML"}}) == "Import failed: bad YAML");
    CHECK(catalog.text(MessageId::LibraryImportConfirmationUnavailable) ==
          "Import failed: Confirmation is unavailable");
    CHECK(catalog.text(MessageId::LibraryImported) == "Library imported successfully");
  }

  TEST_CASE("PresentationTextCatalog - neutral German supplies shared semantic copy and plural policy",
            "[uimodel][unit][presentation][localization]")
  {
    auto const catalog = ao::test::presentationTextCatalog("de-DE");

    CHECK(catalog.trackFieldLabel(rt::TrackField::Title) == "Titel");
    CHECK(catalog.trackGroupKeyLabel(rt::TrackGroupKey::None) == "Keine");
    CHECK(catalog.missingTrackValueLabel(rt::MissingTrackValueKind::Artist) == "Unbekannter Interpret");
    REQUIRE(catalog.builtinTrackPresentation("library"));
    CHECK(catalog.builtinTrackPresentation("library")->label == "Bibliothek");
    CHECK(catalog.text(MessageId::CreateCustomTrackPresentation) == "Benutzerdefinierte Ansicht erstellen...");
    CHECK(catalog.audioProfile(audio::kProfileShared).label == "Gemeinsam genutzter Modus");
    CHECK(catalog.text(MessageId::SystemDefaultOutputDevice) == "Systemstandard");
    CHECK(catalog.completionDetail({.kind = rt::CompletionDetailKind::Field}) == "Feld");
    CHECK(catalog.trackChannelText(1) == "Mono");
    CHECK(catalog.trackChannelText(3) == "3 Kanäle");
    CHECK(catalog.text(MessageId::TrackTechnicalUnknown) == "Unbekannt");
    CHECK(catalog.text(MessageId::PlaybackNotPlaying) == "Keine Wiedergabe");
    CHECK(catalog.text(MessageId::PlaybackConnectingAudioEngine) == "Verbindung zur Audio-Engine wird hergestellt...");
    CHECK(catalog.text(MessageId::PlaybackUnknownArtist) == "Unbekannter Interpret");
    CHECK(catalog.text(MessageId::PlaybackAudioPipeline) == "Audiokette");
    CHECK(catalog.transportControlLabel(PlaybackCommand::Play) == "Wiedergeben");
    CHECK(catalog.transportControlLabel(PlaybackCommand::Pause) == "Pausieren");
    CHECK(catalog.transportControlLabel(PlaybackCommand::Next) == "Nächster Titel");
    CHECK(catalog.volumeTooltip(42, false, false) == "Lautstärke: 42%");
    CHECK(catalog.volumeTooltip(42, true, true) == "Lautstärke: 42% (Stumm)");
    CHECK(catalog.volumeTooltip(42, false, true) == "Lautstärke: 42% (Hardware)");

    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
            .count = 1,
          }) == "1 nicht abspielbarer Titel übersprungen");
    CHECK(catalog.notificationMessage(rt::NotificationReport{
            .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
            .count = 4,
          }) == "4 nicht abspielbare Titel übersprungen");
    CHECK(catalog.libraryTaskProgressDetail(rt::LibraryTaskProgressKind::Scanning, "Straße.flac") ==
          "Scannen: Straße.flac");
    CHECK(catalog.format(MessageId::TrackFilterError, {{"diagnostic", "unerwartetes Token"}}) ==
          "Filterfehler: unerwartetes Token");
    CHECK(catalog.text(MessageId::LibraryAllTracks) == "Alle Titel");
    CHECK(catalog.text(MessageId::LibraryUnnamedList) == "<Unbenannte Liste>");
    CHECK(catalog.format(MessageId::TrackCount, {{"count", 3}}) == "3 Titel");
    CHECK(catalog.trackSelectionSummary(2, "5:00") == "2 Elemente ausgewählt (5:00)");
    CHECK(catalog.text(MessageId::SmartListExpressionNone) == "(keiner)");
    CHECK(catalog.smartListPreviewStatus(true, 4, false, true) == "Alle 4 Titel aus der Quelle werden angezeigt");
    CHECK(catalog.format(MessageId::ListOrderMoved, {{"count", 2}}) ==
          "2 Titel wurden in der manuellen Sortierung verschoben.");
    CHECK(catalog.listMembershipNotification(
            rt::AuthoringStatus::Applied, ListMembershipOperation::Add, "Straße", "#straße", 2, 0) ==
          "#straße wurde für 2 Titel in Straße hinzugefügt.");
    CHECK(catalog.format(MessageId::LibraryExportFailed, {{"error", "Datenträger voll"}}) ==
          "Export fehlgeschlagen: Datenträger voll");
  }

  TEST_CASE("PresentationTextCatalog - unsupported locale falls back to the complete English surface",
            "[uimodel][unit][presentation][localization]")
  {
    auto const catalog = ao::test::presentationTextCatalog("sv-SE");

    for (std::size_t index = 0; index < rt::kTrackFieldCount; ++index)
    {
      INFO("Track field index " << index);
      CHECK_FALSE(catalog.trackFieldLabel(static_cast<rt::TrackField>(index)).empty());
    }

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      INFO("Built-in presentation " << preset.spec.id);
      auto const optText = catalog.builtinTrackPresentation(preset.spec.id);
      REQUIRE(optText);
      CHECK_FALSE(optText->label.empty());
      CHECK_FALSE(optText->description.empty());
    }

    CHECK(catalog.trackFieldLabel(rt::TrackField::Title) == "Title");
    CHECK(catalog.trackChannelText(6) == "6 channels");
    CHECK(catalog.text(MessageId::TrackTechnicalUnknown) == "Unknown");
    CHECK(catalog.transportControlLabel(PlaybackCommand::Previous) == "Previous Track");
    CHECK(catalog.volumeTooltip(42, false, false) == "Volume: 42%");
  }

  TEST_CASE("PresentationTextCatalog - pseudo copy owns borrowed text and preserves runtime arguments",
            "[uimodel][unit][presentation][localization]")
  {
    auto catalog = ao::test::presentationTextCatalog("qps-ploc");
    auto const titleView = catalog.trackFieldLabel(rt::TrackField::Title);
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) -- tests that copies share identical buffer pointer
    auto const catalogCopy = catalog;

    CHECK(titleView.starts_with("[!! "));
    CHECK(titleView.ends_with(" !!]"));
    CHECK(catalogCopy.trackFieldLabel(rt::TrackField::Title).data() == titleView.data());

    auto const message = catalog.notificationMessage(rt::NotificationReport{
      .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
      .subject = "Straße",
      .detail = "E42",
    });
    CHECK(message.contains("Straße"));
    CHECK(message.contains("E42"));
    CHECK(message.size() > std::string_view{"Playback stopped for Straße: E42"}.size());

    auto const volume = catalog.volumeTooltip(42, true, false);
    CHECK(volume.contains("42"));
    CHECK(volume.starts_with("[!! "));
    CHECK(volume.ends_with(" !!]"));
    CHECK(catalog.text(MessageId::PlaybackNotPlaying).starts_with("[!! "));
  }
} // namespace ao::uimodel::test
