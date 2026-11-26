// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using playback_succession::PlaybackSuccessionTransportFixture;
    using playback_succession::PlaybackSuccessionTransportFixtureConfig;

    PlaybackSuccessionTransportFixtureConfig emptyFirstTracks(std::int32_t count)
    {
      auto config = PlaybackSuccessionTransportFixtureConfig{};

      for (std::int32_t i = 0; i < count; ++i)
      {
        config.immediateEofFileNames.emplace_back(std::format("transport-playable-{}.flac", i));
      }

      return config;
    }

    struct EmptyWalk final
    {
      std::vector<TrackId> subjects;
      PlaybackOccurrenceId firstOccurrence{};
      std::vector<NotificationMessage> messages;
    };

    EmptyWalk playThroughEmptyTracks(PlaybackSuccessionTransportFixture& fixture, TrackId first, std::size_t maxVisits)
    {
      auto& succession = *fixture.successionPtr;
      auto& transport = fixture.transport.playbackTransport;
      auto& executor = fixture.transport.executor;
      auto walk = EmptyWalk{};
      auto lastOccurrence = PlaybackOccurrenceId{};
      auto const changed = succession.onChanged(
        [&](PlaybackSuccessionState const& state)
        {
          if (auto const& playback = transport.state(); state.sourceState == PlaybackSuccessionSourceState::Live &&
                                                        playback.occurrenceId != PlaybackOccurrenceId{} &&
                                                        playback.occurrenceId != lastOccurrence)
          {
            if (walk.subjects.empty())
            {
              walk.firstOccurrence = playback.occurrenceId;
            }

            lastOccurrence = playback.occurrenceId;
            walk.subjects.push_back(playback.nowPlaying.trackId);
          }
        });
      auto const feed = fixture.transport.notificationService.onFeedUpdated(
        [&](NotificationFeedUpdate const& update)
        {
          auto const& entries = update.feedPtr->entries;

          if (auto const it = std::ranges::find(entries, update.id, &NotificationEntry::id); it != entries.end())
          {
            walk.messages.push_back(it->message);
          }
        });
      std::size_t ends = 0;
      auto const idle = transport.onIdle([&] { ++ends; });
      REQUIRE(succession.playFromView(fixture.viewId, first));
      auto const settled = [&]
      {
        return !walk.subjects.empty() && (succession.state().sourceState == PlaybackSuccessionSourceState::Inactive ||
                                          transport.state().transport == audio::Transport::Playing);
      };
      // Stop driving a regression even when every executor turn queues another EOF.
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      std::ignore = executor.tryDrainUntil(
        [&] { return settled() || ends > maxVisits + 1 || std::chrono::steady_clock::now() >= deadline; });

      if (!settled())
      {
        succession.clear();
        std::ignore = transport.stop();
        FAIL("Empty traversal did not finish or reach a playable track within " << maxVisits << " candidates");
      }

      return walk;
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - empty tracks finish normally when no eligible successor can start", "[concurrency]")
  {
    auto const [repeat, shuffle, count, playableNeighbor] =
      GENERATE(Catch::Generators::table<RepeatMode, ShuffleMode, std::int32_t, bool>(
        {{RepeatMode::One, ShuffleMode::Off, 1, false},
         {RepeatMode::All, ShuffleMode::Off, 1, false},
         {RepeatMode::One, ShuffleMode::On, 1, true},
         {RepeatMode::All, ShuffleMode::Off, 3, false},
         {RepeatMode::All, ShuffleMode::On, 3, false},
         {RepeatMode::Off, ShuffleMode::Off, 3, false},
         {RepeatMode::Off, ShuffleMode::On, 3, false}}));
    CAPTURE(repeat, shuffle, count, playableNeighbor);
    auto fixture = PlaybackSuccessionTransportFixture{emptyFirstTracks(count)};
    auto empty = std::vector<TrackId>{};

    for (std::int32_t i = 0; i < count; ++i)
    {
      empty.push_back(fixture.addPlayableTrack(std::format("Empty {}", i)));
    }

    auto projection = empty;

    if (playableNeighbor)
    {
      projection.push_back(fixture.addPlayableTrack("Playable neighbor"));
    }

    fixture.openManualView(projection);
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
    succession.setPlaybackMode(shuffle, repeat);

    auto const walk = playThroughEmptyTracks(fixture, empty.front(), empty.size());
    CHECK(std::ranges::is_permutation(walk.subjects, empty));
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    CHECK(transport.state().occurrenceId == PlaybackOccurrenceId{});
    REQUIRE(walk.messages.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(walk.messages.front()));
    CHECK(std::get<NotificationReport>(walk.messages.front()).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    transport.resume();
    fakeit::Verify(Method(backend.mock(), open)).Exactly(count);
    fakeit::Verify(Method(backend.mock(), start)).Never();

    if (repeat == RepeatMode::One && !playableNeighbor)
    {
      auto const retry = playThroughEmptyTracks(fixture, empty.front(), empty.size());
      CHECK(retry.subjects == empty);
      CHECK(retry.firstOccurrence != walk.firstOccurrence);
      CHECK(retry.messages == walk.messages);
      fakeit::Verify(Method(backend.mock(), open)).Exactly(2);
      fakeit::Verify(Method(backend.mock(), start)).Never();
    }
  }

  TEST_CASE("PlaybackSuccession - clean empty tracks yield to a playable successor", "[concurrency]")
  {
    auto const [repeat, shuffle] =
      GENERATE(Catch::Generators::table<RepeatMode, ShuffleMode>({{RepeatMode::Off, ShuffleMode::Off},
                                                                  {RepeatMode::Off, ShuffleMode::On},
                                                                  {RepeatMode::All, ShuffleMode::Off},
                                                                  {RepeatMode::All, ShuffleMode::On}}));
    CAPTURE(repeat, shuffle);
    auto fixture = PlaybackSuccessionTransportFixture{emptyFirstTracks(3)};
    fixture.buildFourTrackManualView();
    fixture.successionPtr->setPlaybackMode(shuffle, repeat);
    auto const walk = playThroughEmptyTracks(fixture, fixture.firstTrackId, 4);
    auto const& playback = fixture.transport.playbackTransport.state();
    REQUIRE(playback.transport == audio::Transport::Playing);
    CHECK(playback.nowPlaying.trackId == fixture.fourthTrackId);
    CHECK(playback.occurrenceId != PlaybackOccurrenceId{});
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(walk.messages.empty());

    if (shuffle == ShuffleMode::Off)
    {
      CHECK(walk.subjects ==
            std::vector{fixture.firstTrackId, fixture.secondTrackId, fixture.thirdTrackId, fixture.fourthTrackId});
    }

    auto subjects = walk.subjects;
    std::ranges::sort(subjects);
    CHECK(std::ranges::adjacent_find(subjects) == subjects.end());
    audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
    REQUIRE(std::in_range<std::int32_t>(subjects.size()));
    fakeit::Verify(Method(backend.mock(), open)).Exactly(static_cast<std::int32_t>(subjects.size()));
    fakeit::Verify(Method(backend.mock(), start)).Once();
  }
} // namespace ao::rt::test
