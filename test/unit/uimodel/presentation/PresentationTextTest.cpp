// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/presentation/PresentationText.h>

#include "test/unit/MessageCatalogTestSupport.h"
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

  TEST_CASE("i18n::MessageCatalog - resolves one-to-one frontend messages through typed ids",
            "[uimodel][unit][localization]")
  {
    auto const& english = ao::test::englishMessageCatalog();
    CHECK(i18n::requiredText(english, MessageId::GtkPreferencesTitle) == "Preferences");
    CHECK(i18n::requiredText(english, MessageId::LibraryBusyTryAgain) == "Library is busy. Try again.");
    CHECK(i18n::requiredFormat(
            english, MessageId::GtkShortcutConflictMessage, {{"chord", "Ctrl+P"}, {"owner", "Play/Pause"}}) ==
          "Ctrl+P is already assigned to \"Play/Pause\". Reassign it to this action?");
    CHECK(playbackActionLabel(english, PlaybackCommand::PlayPause) == "Play/Pause");
    CHECK(playbackActionLabel(english, PlaybackCommand::Next) == "Next");
    CHECK(playbackActionLabel(english, PlaybackCommand::Previous) == "Previous");
    CHECK(playbackActionLabel(english, PlaybackCommand::ToggleShuffle) == "Toggle Shuffle");
    CHECK(playbackActionLabel(english, PlaybackCommand::CycleRepeat) == "Cycle Repeat");

    auto const german = ao::test::messageCatalog("de-DE");
    CHECK(i18n::requiredFormat(german, MessageId::GtkPresentationCopyLabel, {{"label", "Alben"}}) == "Kopie von Alben");
    CHECK(i18n::requiredFormat(german, MessageId::TuiPresentationBadge, {{"id", "Standard"}}) == "Ansicht:Standard");
    CHECK(playbackActionLabel(german, PlaybackCommand::PlayPause) == "Wiedergabe/Pause");

    auto const chinese = ao::test::messageCatalog("zh-CN");
    CHECK(i18n::requiredFormat(chinese, MessageId::GtkPresentationCopyLabel, {{"label", "专辑"}}) == "专辑 副本");
    CHECK(i18n::requiredFormat(chinese, MessageId::TuiPresentationBadge, {{"id", "默认"}}) == "视图:默认");
    CHECK(playbackActionLabel(chinese, PlaybackCommand::PlayPause) == "播放/暂停");
    CHECK(trackFieldLabel(chinese, rt::TrackField::SampleRate) == "采样率");

    auto const traditionalChinese = ao::test::messageCatalog("zh-TW");
    CHECK(i18n::requiredFormat(traditionalChinese, MessageId::GtkPresentationCopyLabel, {{"label", "專輯"}}) ==
          "專輯 複本");
    CHECK(i18n::requiredFormat(traditionalChinese, MessageId::TuiPresentationBadge, {{"id", "預設"}}) == "檢視:預設");
    CHECK(playbackActionLabel(traditionalChinese, PlaybackCommand::PlayPause) == "播放/暫停");
    CHECK(trackFieldLabel(traditionalChinese, rt::TrackField::SampleRate) == "取樣率");

    auto const japanese = ao::test::messageCatalog("ja-JP");
    CHECK(i18n::requiredFormat(japanese, MessageId::GtkPresentationCopyLabel, {{"label", "アルバム"}}) ==
          "アルバム のコピー");
    CHECK(i18n::requiredFormat(japanese, MessageId::TuiPresentationBadge, {{"id", "デフォルト"}}) == "表示:デフォルト");
    CHECK(playbackActionLabel(japanese, PlaybackCommand::PlayPause) == "再生/一時停止");
    CHECK(trackFieldLabel(japanese, rt::TrackField::SampleRate) == "サンプリングレート");

    auto const spanish = ao::test::messageCatalog("es-ES");
    CHECK(i18n::requiredFormat(spanish, MessageId::GtkPresentationCopyLabel, {{"label", "Álbumes"}}) ==
          "Copia de Álbumes");
    CHECK(i18n::requiredFormat(spanish, MessageId::TuiPresentationBadge, {{"id", "predeterminado"}}) ==
          "vista:predeterminado");
    CHECK(playbackActionLabel(spanish, PlaybackCommand::PlayPause) == "Reproducir/Pausar");
    CHECK(trackFieldLabel(spanish, rt::TrackField::SampleRate) == "Frecuencia de muestreo");

    auto const french = ao::test::messageCatalog("fr-FR");
    CHECK(i18n::requiredFormat(french, MessageId::GtkPresentationCopyLabel, {{"label", "Albums"}}) ==
          "Copie de « Albums »");
    CHECK(i18n::requiredFormat(french, MessageId::TuiPresentationBadge, {{"id", "par défaut"}}) == "vue:par défaut");
    CHECK(playbackActionLabel(french, PlaybackCommand::PlayPause) == "Lecture/Pause");
    CHECK(trackFieldLabel(french, rt::TrackField::SampleRate) == "Fréquence d'échantillonnage");
  }

  TEST_CASE("i18n::MessageCatalog - exhaustively labels track presentation semantics", "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    for (std::size_t index = 0; index < rt::kTrackFieldCount; ++index)
    {
      auto const field = static_cast<rt::TrackField>(index);
      INFO("Track field index " << index);
      CHECK_FALSE(trackFieldLabel(catalog, field).empty());
    }

    for (std::size_t index = 0; index < rt::kTrackGroupKeyCount; ++index)
    {
      auto const key = static_cast<rt::TrackGroupKey>(index);
      INFO("Track group key index " << index);
      CHECK_FALSE(trackGroupKeyLabel(catalog, key).empty());
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
      CHECK_FALSE(missingTrackValueLabel(catalog, kind).empty());
    }

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      INFO("Built-in presentation " << preset.spec.id);
      auto const optText = builtinTrackPresentation(catalog, preset.spec.id);
      REQUIRE(optText);
      CHECK_FALSE(optText->label.empty());
      CHECK_FALSE(optText->description.empty());
    }

    CHECK_FALSE(builtinTrackPresentation(catalog, "extension-view"));
    CHECK(trackFieldLabel(catalog, rt::TrackField::SampleRate) == "Sample Rate");
    CHECK(i18n::requiredText(catalog, MessageId::CreateCustomTrackPresentation) == "Create Custom View...");
  }

  TEST_CASE("i18n::MessageCatalog - renders structured group headings only at the UIModel boundary",
            "[uimodel][unit][presentation]")
  {
    auto const heading = rt::TrackGroupHeading{
      .primary = std::string{"Greatest Hits"},
      .secondary = rt::MissingTrackValueKind::Artist,
      .tertiary = std::uint16_t{2020},
    };

    CHECK(formatTrackGroupHeading(ao::test::englishMessageCatalog(), heading) == TrackGroupHeadingPresentation{
                                                                                   .primaryText = "Greatest Hits",
                                                                                   .secondaryText = "Unknown Artist",
                                                                                   .tertiaryText = "2020",
                                                                                 });
  }

  TEST_CASE("i18n::MessageCatalog - owns backend profile and semantic icon presentation",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    auto const pipeWire = audioBackendPresentation(catalog, audio::kBackendPipeWire);
    CHECK(pipeWire.label == "PipeWire");
    CHECK(pipeWire.shortLabel == "PW");
    CHECK(pipeWire.iconKind == AudioIconKind::AudioServer);
    CHECK_FALSE(pipeWire.description.empty());

    auto const alsa = audioBackendPresentation(catalog, audio::kBackendAlsa);
    CHECK(alsa.label == "ALSA");
    CHECK(alsa.iconKind == AudioIconKind::OutputDevice);

    auto const wasapi = audioBackendPresentation(catalog, audio::kBackendWasapi);
    CHECK(wasapi.outputDeviceDescriptionFallback == "WASAPI render endpoint");

    auto const coreAudio = audioBackendPresentation(catalog, audio::kBackendCoreAudio);
    CHECK(coreAudio.label == "Core Audio");
    CHECK(coreAudio.description == "macOS shared audio output through Core Audio");
    CHECK(coreAudio.shortLabel == "Core Audio");
    CHECK(coreAudio.outputDeviceDescriptionFallback == "Core Audio output device");
    CHECK(coreAudio.iconKind == AudioIconKind::OutputDevice);

    auto const unknown = audioBackendPresentation(catalog, audio::BackendId{"extension-backend"});
    CHECK(unknown.label == "extension-backend");
    CHECK(unknown.shortLabel == "extension-backend");
    CHECK(unknown.description.empty());

    CHECK(audioProfilePresentation(catalog, audio::kProfileShared).label == "Shared Mode");
    CHECK(audioProfilePresentation(catalog, audio::kProfileExclusive).label == "Exclusive Mode");
    CHECK(audioProfilePresentation(catalog, audio::ProfileId{"extension-profile"}).label == "extension-profile");
    CHECK(i18n::requiredText(catalog, MessageId::SystemDefaultOutputDevice) == "System Default");
  }

  TEST_CASE("i18n::MessageCatalog - resolves typed completion details", "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(completionDetail(catalog, {.kind = rt::CompletionDetailKind::Field}) == "field");
    CHECK(completionDetail(catalog, {.kind = rt::CompletionDetailKind::Alias}) == "alias");
    CHECK(completionDetail(catalog, {.kind = rt::CompletionDetailKind::Operator}) == "operator");
    CHECK(completionDetail(catalog, {.kind = rt::CompletionDetailKind::LogicalOperator}) == "logical operator");
    CHECK(completionDetail(catalog, rt::CompletionDetail::makeUsageFrequency(42)) == "42");
    CHECK(completionDetail(catalog, rt::CompletionDetail::makeResolvedText("frontend detail")) == "frontend detail");
    CHECK(completionDetail(catalog, {}).empty());
  }

  TEST_CASE("i18n::MessageCatalog - expands structured playback reports", "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTrackOpenFailed,
                                .trackId = TrackId{7},
                                .detail = "file missing",
                              }) == "Could not play track 7: file missing");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackDecodeFailed,
                                .subject = "Song",
                                .detail = "bad frame",
                              }) == "Playback failed for Song: bad frame");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackRouteActivationFailed,
                                .detail = "route unavailable",
                              }) == "Could not start playback: route unavailable");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackDeviceLost,
                                .detail = "device removed",
                              }) == "Playback device failed: device removed");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackSequenceFinished,
                              }) == "Playback sequence finished");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
                                .count = 1,
                              }) == "Skipped 1 unplayable track");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
                                .count = 4,
                              }) == "Skipped 4 unplayable tracks");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackStoppedAfterFailures,
                                .count = 3,
                              }) == "Playback stopped after 3 unplayable tracks");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
                                .subject = "Song",
                                .detail = "decode failed",
                              }) == "Playback stopped for Song: decode failed");

    // Fallbacks: an empty detail resolves to a generic failure reason.
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTrackOpenFailed,
                                .trackId = TrackId{7},
                              }) == "Could not play track 7: unknown error");
    // Fallbacks: an empty subject with no track id resolves to a generic subject.
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackDecodeFailed,
                                .detail = "bad frame",
                              }) == "Playback failed for playback: bad frame");
    // Fallbacks: PlaybackStoppedForTrack without a subject, with and without a detail.
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
                                .detail = "decode failed",
                              }) == "Playback stopped: decode failed");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
                              }) == "Playback stopped");

    CHECK(notificationMessage(catalog, rt::NotificationMessage{std::string{"Frontend message"}}) == "Frontend message");
  }

  TEST_CASE("i18n::MessageCatalog - library progress is selected by kind rather than text prefixes",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    using Kind = rt::LibraryTaskProgressKind;

    CHECK(libraryTaskProgressDetail(catalog, Kind::Scanning, "Scanning: literal.flac") ==
          "Scanning: Scanning: literal.flac");
    CHECK(libraryTaskProgressCompact(catalog, Kind::Scanning, "literal.flac") == "Scanning library");
    CHECK(libraryTaskProgressCompact(catalog, Kind::Updating, "literal.flac") == "Updating library");
    CHECK(libraryTaskProgressCompact(catalog, Kind::Fingerprinting, "literal.flac") == "Fingerprinting: literal.flac");
    CHECK(libraryTaskProgressCompact(catalog, Kind::IndexingAudioIdentity, "literal.flac") ==
          "Indexing audio identity: literal.flac");
    CHECK(libraryTaskProgressCompact(catalog, Kind::PreparingImport, "backup.yaml") == "Preparing import: backup.yaml");
    CHECK(libraryTaskProgressCompact(catalog, Kind::Importing, "backup.yaml") == "Importing: backup.yaml");
    CHECK(libraryTaskProgressCompact(catalog, Kind::Exporting, "backup.yaml") == "Exporting: backup.yaml");
  }

  TEST_CASE("i18n::MessageCatalog - track filter errors carry the parser diagnostic verbatim",
            "[uimodel][unit][presentation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(i18n::requiredFormat(catalog, MessageId::TrackFilterError, {{"diagnostic", "unexpected token ')'"}}) ==
          "Filter error: unexpected token ')'");
    CHECK(i18n::requiredFormat(catalog, MessageId::TrackFilterError, {{"diagnostic", ""}}) == "Filter error: ");
  }

  TEST_CASE("i18n::MessageCatalog - formats shared library and smart-list copy",
            "[uimodel][unit][presentation][localization]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(i18n::requiredText(catalog, MessageId::LibraryAllTracks) == "All Tracks");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryUnnamedList) == "<Unnamed List>");
    CHECK(i18n::requiredFormat(catalog, MessageId::TrackCount, {{"count", 1}}) == "1 track");
    CHECK(i18n::requiredFormat(catalog, MessageId::TrackCount, {{"count", 4}}) == "4 tracks");
    CHECK(trackSelectionSummary(catalog, 0).empty());
    CHECK(trackSelectionSummary(catalog, 2, "5:00") == "2 items selected (5:00)");
    CHECK(smartListMembershipEditingText(catalog, true, R"(#"road-trip")") ==
          R"(Direct membership editing via #"road-trip")");
    CHECK(smartListMembershipEditingText(catalog, false) == "Computed membership — edit tags or the expression");
    CHECK(i18n::requiredText(catalog, MessageId::SmartListExpressionNone) == "(none)");
    CHECK(smartListPreviewStatus(catalog, true, 4, false, true) == "Showing all 4 tracks from source");
    CHECK(smartListPreviewStatus(catalog, true, 14, true, false) == "Showing 10 of 14 matches");
    CHECK(smartListPreviewStatus(catalog, false, 0, false, false) == "Invalid filter");
    CHECK(i18n::requiredText(catalog, MessageId::SmartListUntitledTrack) == "(untitled)");
    CHECK(i18n::requiredText(catalog, MessageId::ListOrderSavedListsOnly) ==
          "Manual ordering is available for saved Lists only.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderMoved, {{"count", 1}}) == "Moved 1 track in Manual Order.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderMoved, {{"count", 3}}) ==
          "Moved 3 tracks in Manual Order.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderReset, {{"count", 2}}) ==
          "Reset Manual Order and forgot 2 saved positions.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderForgotHidden, {{"count", 1}}) ==
          "Forgot 1 hidden saved position.");
    CHECK(formatListMembershipMessage(
            catalog, rt::AuthoringStatus::Applied, ListMembershipOperation::Add, "Road", "#road", 2, 0) ==
          "Added #road to 2 tracks in Road.");
    CHECK(formatListMembershipMessage(
            catalog, rt::AuthoringStatus::Busy, ListMembershipOperation::Add, "Road", "#road", 0, 0) ==
          "Library is busy. Try again.");
    CHECK(formatListMembershipMessage(
            catalog, rt::AuthoringStatus::NoOp, ListMembershipOperation::Remove, "Road", "#road", 0, 0) ==
          "No #road membership or saved position remained in Road.");
    CHECK(formatListMembershipMessage(
            catalog, rt::AuthoringStatus::Applied, ListMembershipOperation::Remove, "Road", "#road", 2, 1) ==
          "Removed #road from 2 tracks and forgot 1 saved position in Road.");

    CHECK(i18n::requiredText(catalog, MessageId::LibraryAudioIdentityIndexingComplete) ==
          "Audio identity indexing complete");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryReadyIndexingAudioIdentity) ==
          "Library ready; indexing audio identity in background");
    CHECK(i18n::requiredFormat(catalog, MessageId::LibraryExportFailed, {{"error", "disk full"}}) ==
          "Export failed: disk full");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryExported) == "Library exported successfully");
    CHECK(i18n::requiredFormat(catalog, MessageId::LibraryImportFailed, {{"error", "bad YAML"}}) ==
          "Import failed: bad YAML");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryImportConfirmationUnavailable) ==
          "Import failed: Confirmation is unavailable");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryImported) == "Library imported successfully");
  }

  TEST_CASE("i18n::MessageCatalog - neutral German supplies shared semantic copy and plural policy",
            "[uimodel][unit][presentation][localization]")
  {
    auto const catalog = ao::test::messageCatalog("de-DE");

    CHECK(trackFieldLabel(catalog, rt::TrackField::Title) == "Titel");
    CHECK(trackGroupKeyLabel(catalog, rt::TrackGroupKey::None) == "Keine");
    CHECK(missingTrackValueLabel(catalog, rt::MissingTrackValueKind::Artist) == "Unbekannter Interpret");
    REQUIRE(builtinTrackPresentation(catalog, "library"));
    CHECK(builtinTrackPresentation(catalog, "library")->label == "Bibliothek");
    CHECK(i18n::requiredText(catalog, MessageId::CreateCustomTrackPresentation) ==
          "Benutzerdefinierte Ansicht erstellen...");
    CHECK(audioProfilePresentation(catalog, audio::kProfileShared).label == "Gemeinsam genutzter Modus");
    CHECK(i18n::requiredText(catalog, MessageId::SystemDefaultOutputDevice) == "Systemstandard");
    CHECK(completionDetail(catalog, {.kind = rt::CompletionDetailKind::Field}) == "Feld");
    CHECK(trackChannelText(catalog, 1) == "Mono");
    CHECK(trackChannelText(catalog, 3) == "3 Kanäle");
    CHECK(i18n::requiredText(catalog, MessageId::TrackTechnicalUnknown) == "Unbekannt");
    CHECK(i18n::requiredText(catalog, MessageId::PlaybackNotPlaying) == "Keine Wiedergabe");
    CHECK(i18n::requiredText(catalog, MessageId::PlaybackConnectingAudioEngine) ==
          "Verbindung zur Audio-Engine wird hergestellt...");
    CHECK(i18n::requiredText(catalog, MessageId::PlaybackUnknownArtist) == "Unbekannter Interpret");
    CHECK(i18n::requiredText(catalog, MessageId::PlaybackAudioPipeline) == "Audiokette");
    CHECK(transportControlLabel(catalog, PlaybackCommand::Play) == "Wiedergeben");
    CHECK(transportControlLabel(catalog, PlaybackCommand::Pause) == "Pausieren");
    CHECK(transportControlLabel(catalog, PlaybackCommand::Next) == "Nächster Titel");
    CHECK(volumeTooltip(catalog, 42, false, false) == "Lautstärke: 42%");
    CHECK(volumeTooltip(catalog, 42, true, true) == "Lautstärke: 42% (Stumm)");
    CHECK(volumeTooltip(catalog, 42, false, true) == "Lautstärke: 42% (Hardware)");

    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
                                .count = 1,
                              }) == "1 nicht abspielbarer Titel übersprungen");
    CHECK(notificationMessage(catalog,
                              rt::NotificationReport{
                                .templateId = rt::NotificationReportTemplate::PlaybackTracksSkipped,
                                .count = 4,
                              }) == "4 nicht abspielbare Titel übersprungen");
    CHECK(libraryTaskProgressDetail(catalog, rt::LibraryTaskProgressKind::Scanning, "Straße.flac") ==
          "Scannen: Straße.flac");
    CHECK(i18n::requiredFormat(catalog, MessageId::TrackFilterError, {{"diagnostic", "unerwartetes Token"}}) ==
          "Filterfehler: unerwartetes Token");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryAllTracks) == "Alle Titel");
    CHECK(i18n::requiredText(catalog, MessageId::LibraryUnnamedList) == "<Unbenannte Liste>");
    CHECK(i18n::requiredFormat(catalog, MessageId::TrackCount, {{"count", 3}}) == "3 Titel");
    CHECK(trackSelectionSummary(catalog, 2, "5:00") == "2 Elemente ausgewählt (5:00)");
    CHECK(i18n::requiredText(catalog, MessageId::SmartListExpressionNone) == "(keiner)");
    CHECK(smartListPreviewStatus(catalog, true, 4, false, true) == "Alle 4 Titel aus der Quelle werden angezeigt");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderMoved, {{"count", 2}}) ==
          "2 Titel wurden in der manuellen Sortierung verschoben.");
    CHECK(formatListMembershipMessage(
            catalog, rt::AuthoringStatus::Applied, ListMembershipOperation::Add, "Straße", "#straße", 2, 0) ==
          "#straße wurde für 2 Titel in Straße hinzugefügt.");
    CHECK(i18n::requiredFormat(catalog, MessageId::LibraryExportFailed, {{"error", "Datenträger voll"}}) ==
          "Export fehlgeschlagen: Datenträger voll");
  }

  TEST_CASE("i18n::MessageCatalog - Spanish manual-order verbs agree with singular and plural counts",
            "[uimodel][unit][localization][regression]")
  {
    auto const catalog = ao::test::messageCatalog("es-ES");

    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderMoved, {{"count", 1}}) ==
          "Se movió 1 pista en Orden manual.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderMoved, {{"count", 2}}) ==
          "Se movieron 2 pistas en Orden manual.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderReset, {{"count", 1}}) ==
          "Se restableció Orden manual y se descartó 1 posición guardada.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderForgotHidden, {{"count", 1}}) ==
          "Se descartó 1 posición guardada oculta.");
    CHECK(i18n::requiredFormat(catalog, MessageId::ListOrderForgotHidden, {{"count", 2}}) ==
          "Se descartaron 2 posiciones guardadas ocultas.");
  }

  TEST_CASE("i18n::MessageCatalog - unsupported locale falls back to the complete English surface",
            "[uimodel][unit][presentation][localization]")
  {
    auto const catalog = ao::test::messageCatalog("sv-SE");

    for (std::size_t index = 0; index < rt::kTrackFieldCount; ++index)
    {
      INFO("Track field index " << index);
      CHECK_FALSE(trackFieldLabel(catalog, static_cast<rt::TrackField>(index)).empty());
    }

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      INFO("Built-in presentation " << preset.spec.id);
      auto const optText = builtinTrackPresentation(catalog, preset.spec.id);
      REQUIRE(optText);
      CHECK_FALSE(optText->label.empty());
      CHECK_FALSE(optText->description.empty());
    }

    CHECK(trackFieldLabel(catalog, rt::TrackField::Title) == "Title");
    CHECK(trackChannelText(catalog, 6) == "6 channels");
    CHECK(i18n::requiredText(catalog, MessageId::TrackTechnicalUnknown) == "Unknown");
    CHECK(transportControlLabel(catalog, PlaybackCommand::Previous) == "Previous Track");
    CHECK(volumeTooltip(catalog, 42, false, false) == "Volume: 42%");
  }

  TEST_CASE("i18n::MessageCatalog - pseudo copy owns borrowed text and preserves runtime arguments",
            "[uimodel][unit][presentation][localization]")
  {
    auto catalog = ao::test::messageCatalog("qps-ploc");
    auto const titleView = trackFieldLabel(catalog, rt::TrackField::Title);
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) -- tests that copies share identical buffer pointer
    auto const catalogCopy = catalog;

    CHECK(titleView.starts_with("[!! "));
    CHECK(titleView.ends_with(" !!]"));
    CHECK(trackFieldLabel(catalogCopy, rt::TrackField::Title).data() == titleView.data());

    auto const message = notificationMessage(catalog,
                                             rt::NotificationReport{
                                               .templateId = rt::NotificationReportTemplate::PlaybackStoppedForTrack,
                                               .subject = "Straße",
                                               .detail = "E42",
                                             });
    CHECK(message.contains("Straße"));
    CHECK(message.contains("E42"));
    CHECK(message.size() > std::string_view{"Playback stopped for Straße: E42"}.size());

    auto const volume = volumeTooltip(catalog, 42, true, false);
    CHECK(volume.contains("42"));
    CHECK(volume.starts_with("[!! "));
    CHECK(volume.ends_with(" !!]"));
    CHECK(i18n::requiredText(catalog, MessageId::PlaybackNotPlaying).starts_with("[!! "));
  }
} // namespace ao::uimodel::test
