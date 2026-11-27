// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/ShuffleHistory.h"
#include "test/audio/EngineTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;

  namespace
  {
    void useShuffleChooser(PlaybackSuccessionTransportFixture& fixture, auto chooser)
    {
      fixture.successionPtr.reset();
      fixture.successionPtr =
        std::make_unique<PlaybackSuccession>(fixture.transport.executor,
                                             fixture.views,
                                             fixture.sources,
                                             fixture.transport.libraryFixture.library(),
                                             fixture.transport.playbackTransport,
                                             fixture.transport.notificationService,
                                             fixture.asyncRuntime,
                                             [chooser] -> ShuffleHistory::CandidateChooser { return chooser; });
    }

    void relinkToHealthyInput(PlaybackSuccessionTransportFixture& fixture, TrackId const trackId)
    {
      auto const uri = fixture.transport.installAudioFixture("basic_metadata.flac", "relinked.flac");
      auto const path = fixture.transport.libraryFixture.root() / uri;
      fixture.transport.libraryFixture.updateTrack(trackId, [&](library::test::TrackSpec& spec) { spec.uri = uri; });
      fixture.trackPaths.at(trackId) = path;
      fixture.decoderProbePtr->registerPath(path);
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - a failed prepared track can retry a relink but not a new title", "[concurrency]")
  {
    auto const relink = GENERATE(false, true);
    CAPTURE(relink);
    auto failure = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-1.flac"},
      .stagedFailureGateSequence = {&failure},
    }};
    auto release = audio::test::StagedFailureReleaseGuard{failure};
    fixture.buildTwoTrackManualView();
    fixture.commandsFixture.releaseLibrary();
    auto const candidate = fixture.secondTrackId;
    auto const oldPath = fixture.trackPaths.at(candidate);
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(failure.tryWaitForRead());

    // The installed decoder still owns the old input. Make the library edit visible before its error arrives.
    if (relink)
    {
      relinkToHealthyInput(fixture, candidate);
    }
    else
    {
      fixture.transport.libraryFixture.updateTrack(
        candidate, [](library::test::TrackSpec& spec) { spec.title = "Retitled"; });
    }

    release.release();
    fixture.settlePreparedFailure();

    if (relink)
    {
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(candidate, 0));
      CHECK(fixture.transport.playbackTransport.clearPreparedNext());
    }

    CHECK(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(oldPath) == 1);
    CHECK(fixture.successionPtr->state().optResolvedSuccessor == candidate);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
  }

  TEST_CASE("PlaybackSuccession - a retired prepared failure leaves a pending replacement intact", "[concurrency]")
  {
    auto failure = audio::test::StagedFailureGate{};
    auto replacementPtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = replacementPtr,
      .blockedFileName = "relinked.flac",
      .stagedFailureFileNames = {"transport-playable-1.flac"},
      .stagedFailureGateSequence = {&failure},
    }};
    auto releaseFailure = audio::test::StagedFailureReleaseGuard{failure};
    auto releaseReplacement = playback_succession::PreparationReleaseGuard{replacementPtr};
    fixture.buildTwoTrackManualView();
    fixture.commandsFixture.releaseLibrary();
    auto const candidate = fixture.secondTrackId;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(failure.tryWaitForRead());
    relinkToHealthyInput(fixture, candidate);
    auto& executor = fixture.transport.executor;
    fixture.settleLookaheadPreparation();
    executor.drain();

    // Engine drops the armed source before queueing its failure. Do not pump the owner before replacement.
    releaseFailure.release();
    REQUIRE(executor.tryWaitUntilQueued());
    REQUIRE(fixture.retiredStagedDecoders() == 1);
    fixture.successionPtr->setRepeatMode(RepeatMode::All);
    REQUIRE(replacementPtr->tryWaitForEntry());
    releaseReplacement.release();
    REQUIRE(fixture.tryWaitForDecoderActivationAfter(candidate, 0));

    // A cancelled inspection followed by another prepare would open a third decoder for the replacement.
    CHECK(replacementPtr->createdPtr->load() == 2);
    CHECK(fixture.transport.playbackTransport.clearPreparedNext());
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE(
    "PlaybackSuccession - shuffle exhaustion keeps the reservation only for the chain with an installed failure",
    "[concurrency]")
  {
    auto const separateChain = GENERATE(false, true);
    CAPTURE(separateChain);
    std::size_t draws = 0;
    auto preferred = kInvalidTrackId;
    auto failure = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-2.flac"},
      .stagedFailureFileNames = {"transport-playable-1.flac"},
      .stagedFailureGateSequence = {&failure},
    }};
    auto release = audio::test::StagedFailureReleaseGuard{failure};
    fixture.buildThreeTrackManualView();
    auto const a = fixture.firstTrackId;
    auto const b = fixture.secondTrackId;
    auto const c = fixture.thirdTrackId;
    auto const d = fixture.addPlayableTrack("Not in the list yet");
    fixture.commandsFixture.releaseLibrary();
    useShuffleChooser(fixture,
                      [&](std::span<TrackId const> candidates)
                      {
                        ++draws;
                        return std::ranges::contains(candidates, preferred) ? preferred : candidates.front();
                      });
    auto& succession = *fixture.successionPtr;
    preferred = b;

    if (!separateChain)
    {
      succession.setShuffleMode(ShuffleMode::On);
    }

    REQUIRE(fixture.playAndWait(a));
    REQUIRE(failure.tryWaitForRead());
    REQUIRE(succession.state().optResolvedSuccessor == b);
    auto const occurrence = fixture.transport.playbackTransport.state().occurrenceId;
    release.release();
    fixture.settlePreparedFailure();

    if (separateChain)
    {
      // The sequential B failure has already settled. Start an independent shuffle chain at C on the same A.
      REQUIRE(draws == 0);
      preferred = c;
      succession.setShuffleMode(ShuffleMode::On);
      fixture.settleLookaheadPreparation();
    }

    // B's installed input stays excluded in either chain. C failed during staging, below the three-attempt cap.
    auto const opens = fixture.decoderProbePtr->snapshot().outputDecoderConstructions;
    REQUIRE(opens.at(fixture.trackPaths.at(b)) == 1);
    REQUIRE(opens.at(fixture.trackPaths.at(c)) == 1);
    REQUIRE(fixture.decoderActivationCount(c) == 0);
    REQUIRE(succession.state().optResolvedSuccessor == c);
    REQUIRE(draws == (separateChain ? 1U : 2U));

    // A real projection change recomputes foreground Next. Inspect that decision before pumping any new staging.
    preferred = d;
    auto const drawsBeforeRefresh = draws;
    fixture.transport.libraryFixture.updateTrack(
      d, [](library::test::TrackSpec& spec) { spec.tags.emplace_back("transportorder"); });
    fixture.sources.reloadAllTracks();
    CHECK(draws == drawsBeforeRefresh + (separateChain ? 1U : 0U));
    CHECK(succession.state().optResolvedSuccessor == (separateChain ? d : c));
    CHECK(fixture.transport.playbackTransport.state().occurrenceId == occurrence);
  }

  TEST_CASE("PlaybackSuccession - shuffle stops after three installed failures with a healthy alternative remaining",
            "[concurrency]")
  {
    auto domains = std::vector<std::vector<TrackId>>{};
    auto failure = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-1.flac", "transport-playable-2.flac", "transport-playable-3.flac"},
      // Each matched path gates only its first output open. An erroneous fourth attempt remains observable and finite.
      .stagedFailureGateSequence = {&failure},
    }};
    auto releaseOnExit = audio::test::StagedFailureReleaseGuard{failure};
    auto const a = fixture.addPlayableTrack("A");
    auto const b = fixture.addPlayableTrack("B");
    auto const c = fixture.addPlayableTrack("C");
    auto const d = fixture.addPlayableTrack("D");
    auto const e = fixture.addPlayableTrack("E");
    fixture.openManualView(std::array{a, b, c, d, e});
    useShuffleChooser(fixture,
                      [&domains, priority = std::array{b, c, d, e}](std::span<TrackId const> candidates)
                      {
                        domains.emplace_back(candidates.begin(), candidates.end());

                        for (auto const id : priority)
                        {
                          if (std::ranges::contains(candidates, id))
                          {
                            return id;
                          }
                        }

                        return candidates.front();
                      });
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    succession.setShuffleMode(ShuffleMode::On);
    REQUIRE(fixture.playAndWait(a));
    auto const occurrence = transport.state().occurrenceId;
    auto const failing = std::array{b, c, d};

    for (auto const trackId : failing)
    {
      CAPTURE(trackId);
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(trackId, 0));
      REQUIRE(succession.state().optResolvedSuccessor == trackId);
      REQUIRE(failure.tryWaitForRead());
      failure.release();
    }

    fixture.settlePreparedFailure(3);
    auto const opens = fixture.decoderProbePtr->snapshot().outputDecoderConstructions;
    CHECK(opens.at(fixture.trackPaths.at(e)) == 0);
    CHECK(domains.size() == 3);
    CHECK(std::ranges::all_of(domains, [e](auto const& domain) { return std::ranges::contains(domain, e); }));

    for (auto const trackId : failing)
    {
      CHECK(opens.at(fixture.trackPaths.at(trackId)) == 1);
    }

    CHECK_FALSE(transport.clearPreparedNext());
    CHECK(succession.state().hasNext);
    CHECK(succession.state().optResolvedSuccessor == d);
    CHECK(succession.state().currentTrackId == a);
    CHECK(transport.state().occurrenceId == occurrence);
    CHECK(transport.state().nowPlaying.trackId == a);
    CHECK(transport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - shuffle exhausts only unchanged failed prepared alternatives", "[concurrency]")
  {
    auto failure = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-1.flac", "transport-playable-2.flac"},
      // Only the first open of each alternative fails. A mistaken retry becomes observable and cannot loop.
      .stagedFailureGateSequence = {&failure},
    }};
    auto releaseFirst = audio::test::StagedFailureReleaseGuard{failure};
    auto releaseSecond = audio::test::StagedFailureReleaseGuard{failure};
    fixture.buildThreeTrackManualView();
    fixture.commandsFixture.releaseLibrary();
    auto& succession = *fixture.successionPtr;
    succession.setShuffleMode(ShuffleMode::On);
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(failure.tryWaitForRead());
    auto const optFirst = succession.state().optResolvedSuccessor;
    REQUIRE((optFirst == fixture.secondTrackId || optFirst == fixture.thirdTrackId));
    auto const other = optFirst == fixture.secondTrackId ? fixture.thirdTrackId : fixture.secondTrackId;
    REQUIRE(fixture.decoderActivationCount(*optFirst) == 1);
    REQUIRE(fixture.decoderActivationCount(other) == 0);

    releaseFirst.release();
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&] { return fixture.decoderActivationCount(other) != 0 || fixture.decoderActivationCount(*optFirst) > 1; }));
    REQUIRE(fixture.decoderActivationCount(other) == 1);
    REQUIRE(failure.tryWaitForRead());
    auto const firstPath = fixture.trackPaths.at(*optFirst);

    SECTION("unchanged alternatives are exhausted")
    {
      releaseSecond.release();
      fixture.settlePreparedFailure(2);
      CHECK(succession.state().optResolvedSuccessor == other);
      CHECK_FALSE(fixture.transport.playbackTransport.clearPreparedNext());
    }

    SECTION("the earlier failed alternative has been relinked while the second decoder was armed")
    {
      relinkToHealthyInput(fixture, *optFirst);
      releaseSecond.release();
      fixture.settlePreparedFailure(2);
      auto const& relinkedPath = fixture.trackPaths.at(*optFirst);
      REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(relinkedPath) == 1);
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(*optFirst, 0));
      CHECK(succession.state().optResolvedSuccessor == optFirst);
      CHECK(fixture.transport.playbackTransport.clearPreparedNext());
    }

    auto const opens = fixture.decoderProbePtr->snapshot().outputDecoderConstructions;
    CHECK(opens.at(firstPath) == 1);
    CHECK(opens.at(fixture.trackPaths.at(other)) == 1);
    CHECK(succession.state().hasNext);
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }
} // namespace ao::rt::test
