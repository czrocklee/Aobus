// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MprisBridge.h"

#include "common/UStringConvert.h"
#include "platform/MprisArtUrlCache.h"
#include "platform/MprisPlaybackEndpoint.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <giomm/file.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::platform::test
{
  namespace
  {
    async::Task<Result<std::optional<std::vector<std::byte>>>> readEmptyMprisResourceAfterOneFailureAsync(
      std::shared_ptr<std::atomic_bool> failNextPtr,
      rt::test::AsyncTestState<std::size_t> readCount,
      ResourceId /*resourceId*/,
      std::stop_token /*stopToken*/)
    {
      readCount.increment();

      if (failNextPtr->exchange(false))
      {
        co_return makeError(Error::Code::IoError, "injected MPRIS resource read failure");
      }

      co_return std::optional<std::vector<std::byte>>{};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> cancelMprisResourceReadAsync(
      rt::test::AsyncTestState<std::size_t> readCount,
      ResourceId /*resourceId*/,
      std::stop_token /*stopToken*/)
    {
      readCount.increment();
      async::throwOperationCancelled();
      co_return std::optional<std::vector<std::byte>>{};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readMprisResourceAsync(std::vector<std::byte> bytes,
                                                                                      ResourceId /*resourceId*/,
                                                                                      std::stop_token /*stopToken*/)
    {
      co_return std::optional{std::move(bytes)};
    }

    ResourceId addResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
    {
      auto transaction = library::test::writeTransaction(library);
      auto writer = library::test::physicalWriter(library.resources(), transaction);
      auto resourceIdRes = writer.create(bytes);
      REQUIRE(resourceIdRes);
      REQUIRE(transaction.commit());
      return *resourceIdRes;
    }

    std::filesystem::path pathFromFileUrl(std::string const& url)
    {
      auto const filePtr = Gio::File::create_for_uri(url);
      REQUIRE(filePtr);
      auto path = filePtr->get_path();
      REQUIRE(!path.empty());
      return std::filesystem::path{path};
    }

    bool hasExpectedFileBytes(std::filesystem::path const& path, std::span<std::byte const> expected)
    {
      auto const actual = ao::test::readFile(path);

      if (actual.size() != expected.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < actual.size(); ++index)
      {
        if (std::byte{static_cast<unsigned char>(actual[index])} != expected[index])
        {
          return false;
        }
      }

      return true;
    }

    rt::ViewId prepareAllTracksView(rt::AppRuntime& runtime)
    {
      runtime.sources().reloadAllTracks();
      auto const* const listOrder = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
      REQUIRE(listOrder != nullptr);
      auto const res = runtime.workspace().navigate({
        .target = rt::GlobalViewKind::AllTracks,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = listOrder->spec,
          },
      });
      REQUIRE(res);
      return *res;
    }

    class FakePlaybackSource final
    {
    public:
      MprisBridge::PlaybackSource source()
      {
        return {
          .snapshot = [this] -> rt::PlaybackSnapshot const& { return _snapshot; },
          .onSnapshot =
            [this](rt::PlaybackSnapshotObserver observer)
          {
            _observer = std::move(observer);
            return async::Subscription{[this] { _observer = nullptr; }};
          },
          .elapsed = [this] { return _snapshot.transport.elapsed; },
        };
      }

      void publish(rt::PlaybackSnapshot snapshot)
      {
        _snapshot = std::move(snapshot);
        REQUIRE(_observer);
        _observer(_snapshot);
      }

    private:
      rt::PlaybackSnapshot _snapshot{};
      rt::PlaybackSnapshotObserver _observer{};
    };

    rt::PlaybackSnapshot playbackSnapshot(TrackId const trackId,
                                          ResourceId const resourceId,
                                          rt::PlaybackOccurrenceId const occurrenceId = {1})
    {
      auto snapshot = rt::PlaybackSnapshot{};
      snapshot.transport.occurrenceId = occurrenceId;
      snapshot.transport.nowPlaying = rt::NowPlayingInfo{
        .trackId = trackId,
        .coverArtId = resourceId,
        .title = "Bridge Track",
      };
      return snapshot;
    }
  } // namespace

  TEST_CASE("MprisBridge - playback status maps transport to MPRIS states", "[gtk][unit][mpris]")
  {
    CHECK(MprisBridge::playbackStatus(audio::Transport::Opening) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Buffering) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Playing) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Paused) == "Paused");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Idle) == "Stopped");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Seeking) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Stopping) == "Stopped");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Error) == "Stopped");
  }

  TEST_CASE("MprisBridge - metadata snapshot maps playback state to MPRIS fields", "[gtk][unit][mpris]")
  {
    auto const state = rt::PlaybackTransportSnapshot{
      .occurrenceId = rt::PlaybackOccurrenceId{7},
      .duration = std::chrono::seconds{125},
      .nowPlaying =
        rt::NowPlayingInfo{
          .trackId = TrackId{42},
          .coverArtId = ResourceId{77},
          .title = "Keyboard Partita",
          .artist = "Johann Sebastian Bach",
          .album = "Partitas",
        },
    };

    auto const metadata = MprisBridge::metadataForState(state, "file:///tmp/aobus-cover.png");

    CHECK(metadata.trackObjectPath == "/org/mpris/MediaPlayer2/Track/42_7");
    CHECK(metadata.title == "Keyboard Partita");
    CHECK(metadata.artist == "Johann Sebastian Bach");
    CHECK(metadata.album == "Partitas");
    CHECK(metadata.artUrl == "file:///tmp/aobus-cover.png");
    CHECK(metadata.lengthUs == 125'000'000);
    CHECK(MprisBridge::metadataForState(rt::PlaybackTransportSnapshot{}).trackObjectPath.empty());
  }

  TEST_CASE("MprisBridge - same-track replay changes metadata identity", "[gtk][regression][mpris]")
  {
    auto before = playbackSnapshot(TrackId{42}, ResourceId{77}, rt::PlaybackOccurrenceId{8});
    auto after = before;
    after.transport.occurrenceId = rt::PlaybackOccurrenceId{9};

    CHECK(MprisBridge::shouldEmitMetadataChanged(before.transport, after.transport));
    CHECK(MprisBridge::metadataForState(before.transport).trackObjectPath == "/org/mpris/MediaPlayer2/Track/42_8");
    CHECK(MprisBridge::metadataForState(after.transport).trackObjectPath == "/org/mpris/MediaPlayer2/Track/42_9");
  }

  TEST_CASE("MprisBridge - delayed art URL completion cannot publish for a replaced track",
            "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kFirstTrackId = TrackId{1};
    constexpr auto kSecondTrackId = TrackId{2};
    constexpr auto kFirstResourceId = ResourceId{11};
    constexpr auto kSecondResourceId = ResourceId{22};
    struct PendingArt final
    {
      ResourceId resourceId = kInvalidResourceId;
      MprisBridge::OnArtUrlReady complete;
    };

    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto playbackSource = FakePlaybackSource{};
    auto pending = std::vector<PendingArt>{};
    std::int32_t cancellationCount = 0;
    auto bridge = MprisBridge{
      playback,
      actions,
      MprisBridge::Callbacks{
        .requestArtUrl =
          [&](ResourceId const resourceId, MprisBridge::OnArtUrlReady complete)
        {
          pending.push_back({.resourceId = resourceId, .complete = std::move(complete)});
          auto const pendingIndex = pending.size() - 1;
          return utility::ScopedRegistration{[&cancellationCount, &pending, pendingIndex]
                                             {
                                               ++cancellationCount;
                                               pending[pendingIndex].complete("file:///tmp/unregister-stale.png");
                                             }};
        },
      },
      playbackSource.source()};
    bridge.start();

    playbackSource.publish(playbackSnapshot(kFirstTrackId, kFirstResourceId));
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].resourceId == kFirstResourceId);
    CHECK(bridge.metadataSnapshot().trackObjectPath ==
          MprisBridge::trackObjectPath(kFirstTrackId, rt::PlaybackOccurrenceId{1}));
    CHECK(bridge.metadataSnapshot().artUrl.empty());

    playbackSource.publish(playbackSnapshot(kSecondTrackId, kSecondResourceId));
    REQUIRE(pending.size() == 2);
    CHECK(pending[1].resourceId == kSecondResourceId);
    CHECK(cancellationCount == 1);
    CHECK(bridge.metadataSnapshot().trackObjectPath ==
          MprisBridge::trackObjectPath(kSecondTrackId, rt::PlaybackOccurrenceId{1}));

    pending[0].complete("file:///tmp/stale.png");
    CHECK(bridge.metadataSnapshot().artUrl.empty());

    pending[1].complete("file:///tmp/current.png");
    CHECK(bridge.metadataSnapshot().artUrl == "file:///tmp/current.png");
    CHECK(cancellationCount == 2);
  }

  TEST_CASE("MprisBridge - art requester exceptions reach the snapshot owner", "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kTrackId = TrackId{3};
    constexpr auto kResourceId = ResourceId{33};
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto playbackSource = FakePlaybackSource{};
    auto capturedCompletion = MprisBridge::OnArtUrlReady{};
    auto bridge = MprisBridge{playback,
                              actions,
                              MprisBridge::Callbacks{
                                .requestArtUrl = [&](ResourceId const resourceId,
                                                     MprisBridge::OnArtUrlReady complete) -> utility::ScopedRegistration
                                {
                                  CHECK(resourceId == kResourceId);
                                  capturedCompletion = std::move(complete);
                                  throw std::runtime_error{"request failed"};
                                },
                              },
                              playbackSource.source()};
    bridge.start();

    CHECK_THROWS_AS(playbackSource.publish(playbackSnapshot(kTrackId, kResourceId)), std::runtime_error);
    REQUIRE(capturedCompletion);
  }

  TEST_CASE("toUString - UTF-8 conversion preserves multibyte metadata", "[gtk][regression][mpris]")
  {
    constexpr auto kTitle = std::string_view{"龙卷风"};

    auto const converted = toUString(kTitle);

    CHECK(converted.raw() == kTitle);
  }

  TEST_CASE("MprisArtUrlCache - exports library cover art resources as file URLs", "[gtk][unit][mpris]")
  {
    constexpr auto kPngBytes = std::array{std::byte{0x89},
                                          std::byte{0x50},
                                          std::byte{0x4E},
                                          std::byte{0x47},
                                          std::byte{0x0D},
                                          std::byte{0x0A},
                                          std::byte{0x1A},
                                          std::byte{0x0A},
                                          std::byte{0x00},
                                          std::byte{0x01}};
    constexpr auto kUnknownBytes = std::array{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};

    CHECK(MprisArtUrlCache::extensionForBytes(kUnknownBytes) == ".img");

    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto resourceId = kInvalidResourceId;
    auto trackId = kInvalidTrackId;
    auto fixture = ao::gtk::test::GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      {
        auto const fixtureUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "cover-track.flac");
        resourceId = addResource(musicLibrary, kPngBytes);
        trackId = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Cover Track", .uri = fixtureUri, .coverArtId = resourceId});
      }};
    ao::gtk::test::installCoverCacheEntry(fixture.cacheDirectory(), kPngBytes);
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto& playback = runtime.playback();
    auto const cacheDir = fixture.tempDir().path() / "mpris-art";
    std::filesystem::create_directories(cacheDir);
    auto const stalePath = cacheDir / (std::to_string(resourceId.raw()) + ".img");

    {
      auto output = std::ofstream{stalePath, std::ios::binary | std::ios::trunc};
      REQUIRE(output);
      output.put('\0');
    }

    auto cache = MprisArtUrlCache{runtime.resourceBytes(), runtime.async(), cacheDir};
    bool callbackOnExecutor = false;
    auto const requestUrl = [&](ResourceId const requestedResourceId)
    {
      auto url = std::string{};
      bool completed = false;
      auto request = cache.requestUrl(requestedResourceId,
                                      [&](std::string resolvedUrl)
                                      {
                                        callbackOnExecutor = runtime.async().callbackExecutor().isCurrent();
                                        url = std::move(resolvedUrl);
                                        completed = true;
                                      });
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return completed; }));
      return url;
    };

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, trackId));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, trackId));
    CHECK(playback.snapshot().transport.nowPlaying.coverArtId == resourceId);

    auto const url = requestUrl(playback.snapshot().transport.nowPlaying.coverArtId);
    REQUIRE(url.starts_with("file://"));
    CHECK(callbackOnExecutor);

    auto const exportedPath = pathFromFileUrl(url);
    CHECK(exportedPath.extension() == ".png");
    CHECK(std::filesystem::is_regular_file(exportedPath));
    auto const permissions = std::filesystem::status(exportedPath).permissions();
    CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
    CHECK_FALSE(std::filesystem::exists(stalePath));
    CHECK(hasExpectedFileBytes(exportedPath, kPngBytes));
    auto cachedWriteTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24};
    std::filesystem::last_write_time(exportedPath, cachedWriteTime);
    cachedWriteTime = std::filesystem::last_write_time(exportedPath);
    CHECK(requestUrl(resourceId) == url);
    CHECK(std::filesystem::last_write_time(exportedPath) == cachedWriteTime);

    REQUIRE(std::filesystem::remove(exportedPath));
    CHECK(requestUrl(resourceId) == url);
    CHECK(std::filesystem::is_regular_file(exportedPath));
    CHECK(hasExpectedFileBytes(exportedPath, kPngBytes));

    {
      auto output = std::ofstream{exportedPath, std::ios::binary | std::ios::trunc};
      REQUIRE(output);
      output.put('\0');
    }

    CHECK(requestUrl(resourceId) == url);
    CHECK(hasExpectedFileBytes(exportedPath, kPngBytes));

    auto const metadata = MprisBridge::metadataForState(playback.snapshot().transport, url);
    CHECK(metadata.artUrl == url);
    CHECK(requestUrl(kInvalidResourceId).empty());
    CHECK(requestUrl(ResourceId{999999}).empty());

    std::int32_t cancelledCallbackCount = 0;
    bool activeWaiterCompleted = false;
    auto cancelledRequest = cache.requestUrl(ResourceId{999998}, [&](std::string) { ++cancelledCallbackCount; });
    [[maybe_unused]] auto activeRequest =
      cache.requestUrl(ResourceId{999998}, [&](std::string) { activeWaiterCompleted = true; });
    cancelledRequest.reset();
    REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return activeWaiterCompleted; }));
    CHECK(cancelledCallbackCount == 0);
  }

  TEST_CASE("MprisArtUrlCache - failed replacement preserves the previously published URI",
            "[gtk][unit][mpris][concurrency]")
  {
    constexpr auto kResourceId = ResourceId{51};
    constexpr auto kPngBytes = std::array{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}};
    auto const jpegBytes = std::vector{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xDB}};
    auto tempDir = ao::test::TempDir{};
    auto const cacheDir = tempDir.path() / "mpris-preserved-art";
    auto const oldPath = cacheDir / (std::to_string(kResourceId.raw()) + ".png");
    auto const replacementPath = cacheDir / (std::to_string(kResourceId.raw()) + ".jpg");
    std::filesystem::create_directories(cacheDir);

    {
      auto output = std::ofstream{oldPath, std::ios::binary};
      auto const oldBytes = std::span<std::byte const>{kPngBytes};
      auto const byteView = utility::bytes::stringView(oldBytes);
      output.write(byteView.data(), static_cast<std::streamsize>(byteView.size()));
      REQUIRE(output);
    }

    // A directory at the replacement path makes the platform rename fail after
    // the new payload is complete. The old extension must remain published.
    std::filesystem::create_directory(replacementPath);

    auto executor = rt::test::QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto byteCache = rt::ResourceByteMemoryCache{runtime, std::bind_front(readMprisResourceAsync, jpegBytes)};
    auto cache = MprisArtUrlCache{byteCache, runtime, cacheDir};
    auto url = std::string{};
    bool completed = false;
    auto request = cache.requestUrl(kResourceId,
                                    [&](std::string resolvedUrl)
                                    {
                                      url = std::move(resolvedUrl);
                                      completed = true;
                                    });

    REQUIRE(request);
    REQUIRE(executor.tryDrainUntil([&] { return completed; }));
    CHECK(url.empty());
    CHECK(hasExpectedFileBytes(oldPath, kPngBytes));
    CHECK(std::filesystem::is_directory(replacementPath));

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("MprisArtUrlCache - failed resource reads terminate their request flight",
            "[gtk][unit][mpris][concurrency]")
  {
    auto executor = rt::test::QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto tempDir = ao::test::TempDir{};
    constexpr auto kMissingResourceId = ResourceId{999997};

    SECTION("a Result failure completes empty and permits retry")
    {
      auto callbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto readCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto receivedNonEmptyUrl = rt::test::AsyncTestState<bool>::create(true);
      auto failNextPtr = std::make_shared<std::atomic_bool>(true);
      auto byteCache = rt::ResourceByteMemoryCache{
        runtime, std::bind_front(readEmptyMprisResourceAfterOneFailureAsync, failNextPtr, readCount)};
      auto cache = MprisArtUrlCache{byteCache, runtime, tempDir.path() / "mpris-exceptional-load"};

      auto request = cache.requestUrl(kMissingResourceId,
                                      [callbackCount, receivedNonEmptyUrl](std::string url)
                                      {
                                        receivedNonEmptyUrl.set(!url.empty());
                                        callbackCount.increment();
                                      });
      REQUIRE(request);
      REQUIRE(executor.tryDrainUntil([&] { return callbackCount.load() == 1; }));
      CHECK_FALSE(receivedNonEmptyUrl.load());

      auto retryReceivedNonEmptyUrl = rt::test::AsyncTestState<bool>::create(true);
      auto retry = cache.requestUrl(kMissingResourceId,
                                    [callbackCount, retryReceivedNonEmptyUrl](std::string url)
                                    {
                                      retryReceivedNonEmptyUrl.set(!url.empty());
                                      callbackCount.increment();
                                    });
      REQUIRE(retry);
      REQUIRE(executor.tryDrainUntil([&] { return callbackCount.load() == 2; }));
      CHECK(readCount.load() == 2);
      CHECK_FALSE(retryReceivedNonEmptyUrl.load());

      runtime.requestStop();
      runtime.join();
    }

    SECTION("cancellation escapes without invoking the waiter")
    {
      auto callbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto readCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto byteCache = rt::ResourceByteMemoryCache{runtime, std::bind_front(cancelMprisResourceReadAsync, readCount)};
      auto cache = MprisArtUrlCache{byteCache, runtime, tempDir.path() / "mpris-cancelled-load"};

      auto request = cache.requestUrl(kMissingResourceId, [callbackCount](std::string) { callbackCount.increment(); });
      REQUIRE(request);
      REQUIRE(executor.tryDrainUntil([&] { return readCount.load() == 1; }));

      runtime.requestStop();
      runtime.join();
      CHECK(callbackCount.load() == 0);
    }
  }

  TEST_CASE("MprisBridge - elapsed helpers convert MPRIS time without overflow", "[gtk][unit][mpris]")
  {
    CHECK(MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds{1234}) == 1'234'000);
    CHECK(MprisBridge::fromMprisMicroseconds(1'234'567) == std::chrono::milliseconds{1234});
    CHECK(MprisBridge::fromMprisMicroseconds(-1'234'567) == std::chrono::milliseconds{-1234});
    CHECK(MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds::max()) ==
          std::numeric_limits<std::int64_t>::max());
    CHECK(MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds::min()) ==
          std::numeric_limits<std::int64_t>::min());
  }

  TEST_CASE("MprisBridge - Seeked follows final seek identity rather than elapsed drift", "[gtk][regression][mpris]")
  {
    auto before = rt::PlaybackTransportSnapshot{.elapsed = std::chrono::milliseconds{100}};
    auto after = before;
    after.elapsed = std::chrono::milliseconds{900};

    CHECK_FALSE(MprisBridge::shouldEmitSeeked(before, after));

    after.positionRevision = rt::PlaybackPositionRevision{.value = 1};
    CHECK_FALSE(MprisBridge::shouldEmitSeeked(before, after));

    after.finalSeekRevision = rt::PlaybackFinalSeekRevision{.value = 1};
    CHECK(MprisBridge::shouldEmitSeeked(before, after));
  }

  TEST_CASE("MprisBridge - loop status maps runtime repeat modes", "[gtk][unit][mpris]")
  {
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::Off) == "None");
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::One) == "Track");
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::All) == "Playlist");

    auto const optOff = MprisBridge::repeatModeForLoopStatus("None");
    REQUIRE(optOff);
    CHECK(*optOff == rt::RepeatMode::Off);

    auto const optOne = MprisBridge::repeatModeForLoopStatus("Track");
    REQUIRE(optOne);
    CHECK(*optOne == rt::RepeatMode::One);

    auto const optAll = MprisBridge::repeatModeForLoopStatus("Playlist");
    REQUIRE(optAll);
    CHECK(*optAll == rt::RepeatMode::All);
    CHECK_FALSE(MprisBridge::repeatModeForLoopStatus("Album").has_value());
  }

  TEST_CASE("MprisBridge - player methods execute shared and guarded playback commands", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    std::int32_t playSelectionCount = 0;
    auto actions = uimodel::PlaybackActions{playback, [&playSelectionCount] { ++playSelectionCount; }};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    CHECK(endpoint.tryDispatchPlayerMethod("PlayPause"));
    CHECK(endpoint.tryDispatchPlayerMethod("Play"));
    CHECK(playSelectionCount == 2);

    REQUIRE(playback.commands().startFromView(viewId, firstTrack));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, firstTrack));
    CHECK(endpoint.tryDispatchPlayerMethod("Next"));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    CHECK(endpoint.tryDispatchPlayerMethod("Previous"));
    CHECK(playback.snapshot().succession.currentTrackId == firstTrack);

    CHECK(endpoint.tryDispatchPlayerMethod("Pause"));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(endpoint.tryDispatchPlayerMethod("Stop"));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK_FALSE(endpoint.tryDispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - publication-issued past-end Seek advances after handoff",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, firstTrack));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, firstTrack));

    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};
    bool requested = false;
    bool handled = false;
    auto const subscription = playback.events().onSnapshot(
      [&](rt::PlaybackSnapshot const&) noexcept
      {
        if (!requested)
        {
          requested = true;
          endpoint.handleSeek(99'000'000);
          handled = true;
        }
      });

    playback.commands().setVolume(0.5F);
    REQUIRE(requested);
    CHECK(handled);
    auto const before = playback.snapshot().transport;
    CHECK(before.nowPlaying.trackId == firstTrack);
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, secondTrack));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == secondTrack);
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    CHECK(playback.snapshot().transport.occurrenceId != before.occurrenceId);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
  }

  TEST_CASE("MprisBridge - root methods dispatch to injected GTK lifecycle callbacks", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    std::int32_t raiseCount = 0;
    std::int32_t quitCount = 0;
    auto callbacks = MprisBridge::Callbacks{
      .raise =
        [&raiseCount]
      {
        ++raiseCount;
        return true;
      },
      .quit =
        [&quitCount]
      {
        ++quitCount;
        return true;
      },
    };
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    CHECK(endpoint.tryDispatchRootMethod("Raise"));
    CHECK(endpoint.tryDispatchRootMethod("Quit"));
    CHECK_FALSE(endpoint.tryDispatchRootMethod("Unsupported"));
    CHECK(raiseCount == 1);
    CHECK(quitCount == 1);
  }

  TEST_CASE("MprisBridge - unsupported player methods are rejected", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    CHECK_FALSE(endpoint.tryDispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - capability properties mirror playback command capability", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    auto const checkCapability = [&endpoint, &actions, &playback](
                                   std::string_view const propertyName, uimodel::PlaybackCommand const command)
    {
      auto const optCapability = endpoint.playerCapabilityProperty(propertyName, playback.snapshot().transport);
      REQUIRE(optCapability);
      CHECK(*optCapability == actions.isCapable(command));
    };

    REQUIRE(playback.commands().startFromView(viewId, firstTrack));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, firstTrack));

    checkCapability("CanPlay", uimodel::PlaybackCommand::Play);
    checkCapability("CanPause", uimodel::PlaybackCommand::Pause);
    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);

    REQUIRE(endpoint.tryDispatchPlayerMethod("Next"));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, secondTrack));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    REQUIRE(endpoint.tryDispatchPlayerMethod("Next"));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == secondTrack);

    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);
    CHECK(endpoint.playerCapabilityProperty("CanControl", playback.snapshot().transport).value_or(false));
    CHECK_FALSE(endpoint.playerCapabilityProperty("Volume", playback.snapshot().transport).has_value());
  }

  TEST_CASE("MprisBridge - seek capability requires a current subject with known positive duration",
            "[gtk][regression][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};
    auto state = rt::PlaybackTransportSnapshot{};
    state.nowPlaying.trackId = TrackId{1};
    state.occurrenceId = rt::PlaybackOccurrenceId{.value = 1};

    CHECK(endpoint.playerCapabilityProperty("CanSeek", state) == std::optional{false});
    state.duration = std::chrono::milliseconds{-1};
    CHECK(endpoint.playerCapabilityProperty("CanSeek", state) == std::optional{false});
    state.duration = std::chrono::seconds{1};
    CHECK(endpoint.playerCapabilityProperty("CanSeek", state) == std::optional{true});
    state.occurrenceId = {};
    CHECK(endpoint.playerCapabilityProperty("CanSeek", state) == std::optional{false});
    state.occurrenceId = rt::PlaybackOccurrenceId{.value = 1};
    state.nowPlaying.trackId = kInvalidTrackId;
    CHECK(endpoint.playerCapabilityProperty("CanSeek", state) == std::optional{false});
  }

  TEST_CASE("MprisBridge - volume setter delegates to playback service normalization", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    endpoint.dispatchSetVolume(0.42);
    CHECK(playback.snapshot().transport.volume.level == Catch::Approx{0.42F});

    endpoint.dispatchSetVolume(5.0);
    CHECK(playback.snapshot().transport.volume.level == 1.0F);

    endpoint.dispatchSetVolume(-1.0);
    CHECK(playback.snapshot().transport.volume.level == 0.0F);
  }

  TEST_CASE("MprisBridge - rate setter keeps fixed rate and pauses on zero", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Rate Track", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    REQUIRE(playback.commands().startFromView(viewId, trackId));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, trackId));
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.tryDispatchSetRate(2.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.tryDispatchSetRate(-1.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.tryDispatchSetRate(0.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

    CHECK(endpoint.tryDispatchSetRate(1.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

    CHECK_FALSE(endpoint.tryDispatchSetRate(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(endpoint.tryDispatchSetRate(std::numeric_limits<double>::quiet_NaN()));
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters delegate to playback sequence", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    endpoint.dispatchSetShuffle(true);
    CHECK(playback.snapshot().succession.shuffle == rt::ShuffleMode::On);

    CHECK(endpoint.tryDispatchSetLoopStatus("Track"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::One);

    CHECK(endpoint.tryDispatchSetLoopStatus("Playlist"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::All);

    CHECK(endpoint.tryDispatchSetLoopStatus("None"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::Off);

    CHECK_FALSE(endpoint.tryDispatchSetLoopStatus("Album"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::Off);
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters update an active sequence", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    [[maybe_unused]] auto const track1 =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Queue 1", .uri = fixturePath});
    auto const track2 =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Queue 2", .uri = fixturePath});

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, track2));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, track2));
    REQUIRE_FALSE(playback.snapshot().succession.hasNext);

    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    CHECK(endpoint.tryDispatchSetLoopStatus("Playlist"));
    CHECK(playback.snapshot().succession.hasNext);

    CHECK(endpoint.tryDispatchSetLoopStatus("None"));
    REQUIRE_FALSE(playback.snapshot().succession.hasNext);

    endpoint.dispatchSetShuffle(true);
    CHECK(playback.snapshot().succession.hasNext);
  }

  TEST_CASE("MprisBridge - seek methods update playback service position", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = ao::gtk::test::addRuntimeTrack(runtime,
                                                        library::test::TrackSpec{.title = "MPRIS Seek",
                                                                                 .artist = "Desktop Artist",
                                                                                 .album = "Desktop Album",
                                                                                 .uri = fixturePath,
                                                                                 .duration = std::chrono::seconds{10}});
    auto const nextTrackId =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "MPRIS Next", .uri = fixturePath});

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, trackId));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, trackId));
    playback.commands().seek(std::chrono::milliseconds{500});

    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};

    auto const firstOccurrenceId = playback.snapshot().transport.occurrenceId;
    auto const firstTrackPath = MprisBridge::trackObjectPath(trackId, firstOccurrenceId);
    endpoint.handleSeek(200'000);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{700});

    endpoint.handleSetPosition(firstTrackPath, 150'000);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    endpoint.handleSetPosition("/org/mpris/MediaPlayer2/Track/999_1", 4'000'000);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    endpoint.handleSetPosition(firstTrackPath, -1);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    endpoint.handleSetPosition(firstTrackPath, 99'000'000);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    REQUIRE(playback.commands().startFromView(viewId, trackId));
    REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil(
      [&]
      {
        auto const& state = playback.snapshot().transport;
        return state.nowPlaying.trackId == trackId && state.occurrenceId != firstOccurrenceId;
      }));
    auto const replayed = playback.snapshot().transport;
    REQUIRE(replayed.occurrenceId != firstOccurrenceId);
    endpoint.handleSetPosition(firstTrackPath, 250'000);
    CHECK(playback.snapshot().transport.elapsed == replayed.elapsed);

    auto const replayedTrackPath = MprisBridge::trackObjectPath(trackId, replayed.occurrenceId);
    endpoint.handleSetPosition(replayedTrackPath, 250'000);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{250});

    endpoint.handleSeek(99'000'000);
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, nextTrackId));
    CHECK(playback.snapshot().succession.currentTrackId == nextTrackId);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == nextTrackId);

    auto const lastTrack = playback.snapshot().transport;
    REQUIRE_FALSE(actions.isEnabled(uimodel::PlaybackCommand::Next));
    endpoint.handleSeek(99'000'000);
    CHECK(playback.snapshot().transport.occurrenceId == lastTrack.occurrenceId);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == nextTrackId);
    CHECK(playback.snapshot().transport.elapsed == lastTrack.elapsed);

    playback.commands().stop();
    auto const stopped = playback.snapshot().transport;
    endpoint.handleSeek(1'000'000);
    endpoint.handleSetPosition(firstTrackPath, 1'000'000);
    CHECK(playback.snapshot().transport.occurrenceId == stopped.occurrenceId);
    CHECK(playback.snapshot().transport.elapsed == stopped.elapsed);
    CHECK(playback.snapshot().transport.finalSeekRevision == stopped.finalSeekRevision);
  }

  TEST_CASE("MprisBridge - queued Next makes observer SetPosition a successful stale no-op",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrackId = ao::gtk::test::addRuntimeTrack(
      runtime,
      library::test::TrackSpec{.title = "Observer First", .uri = fixturePath, .duration = std::chrono::seconds{10}});
    auto const secondTrackId = ao::gtk::test::addRuntimeTrack(
      runtime,
      library::test::TrackSpec{.title = "Observer Second", .uri = fixturePath, .duration = std::chrono::seconds{10}});
    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, firstTrackId));
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, firstTrackId));

    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, actions, callbacks};
    auto const before = playback.snapshot().transport;
    auto const firstPath = MprisBridge::trackObjectPath(firstTrackId, before.occurrenceId);
    bool requested = false;
    bool setPositionHandled = false;
    auto const subscription = playback.events().onSnapshot(
      [&](rt::PlaybackSnapshot const&) noexcept
      {
        if (!requested)
        {
          requested = true;
          playback.commands().next();
          endpoint.handleSetPosition(firstPath, 1'000'000);
          setPositionHandled = true;
        }
      });

    playback.commands().setVolume(0.5F);
    REQUIRE(requested);
    CHECK(setPositionHandled);
    REQUIRE(ao::gtk::test::tryWaitForPlaybackSettlement(runtime, secondTrackId));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == secondTrackId);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
  }
} // namespace ao::gtk::platform::test
