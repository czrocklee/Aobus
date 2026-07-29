// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/PlaybackSuccessionSeekTestSupport.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/Player.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test::playback_succession
{
  PlaybackSuccessionSeekFixture::PlaybackSuccessionSeekFixture(audio::test::StagedFailureGate* const failureGate)
    : asyncRuntime{executor}
    , changes{libraryChangesExecutor, 0}
    , writerFixture{libraryFixture.library(), changes}
    , sources{libraryFixture.library(), changes}
    , views{executor, libraryFixture.library(), sources}
    , workspace{executor, views, changes}
  {
    // A 48 kHz clock represents every whole millisecond exactly, including
    // the 3001 ms edge immediately above the strict restart threshold.
    auto const format = audio::Format{.sampleRate = 48000, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto decoderFactory = audio::DecoderFactoryFn{};

    if (failureGate != nullptr)
    {
      decoderFactory = [failureGate](std::filesystem::path const&, audio::Format const&)
      { return std::make_unique<audio::test::StagedFailureDecoderSession>(failureGate); };
    }
    else
    {
      decoderFactory = [format](std::filesystem::path const&, audio::Format const&)
      {
        auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
          .sourceFormat = format,
          .outputFormat = format,
          .duration = std::chrono::seconds{10},
          .isLossy = false,
          .codec = AudioCodec::Flac,
        });
        auto const data = std::vector<std::byte>(100000, std::byte{0});
        decoderPtr->setReadScript(
          {{.data = data, .endOfStream = false}, {.data = data, .endOfStream = false}, {.endOfStream = true}});
        return decoderPtr;
      };
    }

    auto playerPtr = std::make_unique<audio::Player>(asyncRuntime, std::move(decoderFactory));
    transportPtr =
      std::make_unique<PlaybackTransport>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
    PlaybackBootstrap{*transportPtr}.addProvider(makeReadyAudioProvider());
    executor.drain();
  }

  PlaybackSuccessionSeekFixture::~PlaybackSuccessionSeekFixture() = default;

  LibraryWriter& PlaybackSuccessionSeekFixture::writer()
  {
    return writerFixture.writer();
  }

  TrackId PlaybackSuccessionSeekFixture::addPlayableTrack(std::string title)
  {
    auto const playableUri = std::format("seek-playable-{}.flac", nextPlayableFile++);
    audio::test::installAudioFixture(libraryFixture.root(), "basic_metadata.flac", playableUri);
    auto const created = ao::test::requireValue(writer().createTrackFromFile(libraryFixture.root() / playableUri));
    executor.drain();
    REQUIRE(writerFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title}));
    executor.drain();
    return created.trackId;
  }

  void PlaybackSuccessionSeekFixture::buildThreeTrackManualView()
  {
    firstTrackId = addPlayableTrack("First");
    secondTrackId = addPlayableTrack("Second");
    thirdTrackId = addPlayableTrack("Third");
    sources.reloadAllTracks();
    listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
      .name = "Long playback order",
    }));
    viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
    successionPtr = std::make_unique<PlaybackSuccession>(
      executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
  }

  void PlaybackSuccessionSeekFixture::buildSingleTrackManualView()
  {
    firstTrackId = addPlayableTrack("Failing current");
    sources.reloadAllTracks();
    listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
      .name = "Failing playback order",
    }));
    viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
    successionPtr = std::make_unique<PlaybackSuccession>(
      executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
  }

  Result<> PlaybackSuccessionSeekFixture::playAndWait(TrackId const trackId)
  {
    return playFromViewAndWait(*successionPtr, executor, viewId, trackId);
  }
} // namespace ao::rt::test::playback_succession
