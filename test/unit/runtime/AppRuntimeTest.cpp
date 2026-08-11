// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppRuntime.h>

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::test;

  namespace
  {
    struct AppRuntimeAudioState final
    {
      audio::BackendProvider::OnDevicesChangedCallback onDevicesChanged;
      audio::RenderTarget* renderTarget = nullptr;
      AsyncTestState<bool> providerShutdownStarted = AsyncTestState<bool>::create(false);
    };

    class AppRuntimeBackend final : public audio::NullBackend
    {
    public:
      explicit AppRuntimeBackend(std::shared_ptr<AppRuntimeAudioState> statePtr)
        : _statePtr{std::move(statePtr)}
      {
      }

      Result<audio::OpenedPcmMode> open(audio::SignalFormat const& sourceFormat, audio::RenderTarget& target) override
      {
        _statePtr->renderTarget = &target;
        return audio::NullBackend::open(sourceFormat, target);
      }

      audio::BackendId backendId() const override { return audio::BackendId{"test_backend"}; }
      audio::ProfileId profileId() const override { return audio::ProfileId{audio::kProfileShared}; }

    private:
      std::shared_ptr<AppRuntimeAudioState> _statePtr;
    };

    class AppRuntimeProvider final : public audio::BackendProvider
    {
    public:
      explicit AppRuntimeProvider(std::shared_ptr<AppRuntimeAudioState> statePtr)
        : _statePtr{std::move(statePtr)}, _status{makeReadyAudioStatus()}
      {
      }

      void shutdown() noexcept override { _statePtr->providerShutdownStarted.set(true); }

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        _statePtr->onDevicesChanged = std::move(callback);
        return {};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<AppRuntimeBackend>(_statePtr);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      std::shared_ptr<AppRuntimeAudioState> _statePtr;
      Status _status;
    };

    async::Task<void> attemptWriterDuringQueuedPublication(CoreRuntime* runtime,
                                                           AsyncTestState<bool> started,
                                                           AsyncTestState<bool> rejectedByClosing)
    {
      started.set(true);
      auto const result = runtime->library().createList(LibraryWriter::ListDraft{.name = "Closing writer"});
      rejectedByClosing.set(!result && result.error().code == Error::Code::InvalidState);
      co_return;
    }
  } // namespace

  TEST_CASE("AppRuntime - missing workspace config store is rejected", "[runtime][unit][app-runtime]")
  {
    auto tempDir = ao::test::TempDir{};

    auto const runtimeRes = AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
    });

    REQUIRE_FALSE(runtimeRes);
    CHECK(runtimeRes.error().code == Error::Code::InvalidInput);
    CHECK(runtimeRes.error().message == "AppRuntime requires a workspace config store");
  }

  TEST_CASE("runtime factories preserve storage construction errors", "[runtime][unit][app-runtime][factory]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const databaseFile = ao::test::TempFile{".mdb"};

    SECTION("CoreRuntime")
    {
      auto const result = CoreRuntime::create(
        std::make_unique<InlineExecutor>(), tempDir.path(), databaseFile.path, library::test::kTestMusicLibraryMapSize);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("AppRuntime")
    {
      auto const result = AppRuntime::create(AppRuntimeDependencies{
        .executorPtr = std::make_unique<InlineExecutor>(),
        .musicRoot = tempDir.path(),
        .databasePath = databaseFile.path,
        .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
        .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
      });
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }
  }

  TEST_CASE("AppRuntime - factory materializes All Tracks before exposure", "[runtime][unit][app-runtime][factory]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const databasePath = LibraryPaths{tempDir.path()}.databasePath();
    {
      auto library = library::test::makeTestMusicLibrary(tempDir.path(), databasePath);
      [[maybe_unused]] auto const trackId = library::test::addTrackWithUniqueFixtureUri(
        library, library::test::TrackSpec{.title = "Already persisted", .uri = "persisted.flac"});
    }

    auto runtimePtr = ao::test::requireValue(AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = databasePath,
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
    }));
    auto allTracks = ao::test::requireValue(runtimePtr->sources().acquire(kAllTracksListId));

    CHECK(allTracks->size() == 1);
  }

  TEST_CASE("AppRuntime - playback session store uses fallback and explicit override", "[runtime][unit][app-runtime]")
  {
    auto tempDir = ao::test::TempDir{};
    auto overrideStore = ConfigStore{tempDir.path() / "playback.yaml"};

    SECTION("null override uses the workspace store")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      CHECK(&runtimePtr->playbackSessionConfigStore() == &runtimePtr->workspaceConfigStore());
    }

    SECTION("explicit override is preserved")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir, &overrideStore);
      CHECK(&runtimePtr->playbackSessionConfigStore() == &overrideStore);
      CHECK(&runtimePtr->playbackSessionConfigStore() != &runtimePtr->workspaceConfigStore());
    }
  }

  TEST_CASE("AppRuntime - dependencies expose services and empty selection is safe", "[runtime][unit][app-runtime]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const databasePath = LibraryPaths{tempDir.path()}.databasePath();

    auto appPtr = ao::test::requireValue(AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = databasePath,
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    }));

    CHECK(appPtr->musicRoot() == std::filesystem::path{tempDir.path()});
    CHECK(appPtr->databasePath() == databasePath);

    // Verify accessors
    [[maybe_unused]] auto& commands = appPtr->library().writer();
    [[maybe_unused]] auto& notifications = appPtr->notifications();

    appPtr->addAudioProvider(
      makeReadyAudioProvider(audio::BackendProvider::Status{.descriptor = {.id = audio::BackendId{"dummy"}}}));

    // reloadAllTracks
    CHECK_NOTHROW(appPtr->reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    auto const withoutFocusRes = appPtr->playSelectionInFocusedView();
    REQUIRE_FALSE(withoutFocusRes);
    CHECK(withoutFocusRes.error().code == Error::Code::InvalidState);

    // Add a view and focus it
    REQUIRE(appPtr->workspace().navigate({.target = GlobalViewKind::AllTracks}));

    // playSelectionInFocusedView (with focused view but no selection)
    auto const withoutSelectionRes = appPtr->playSelectionInFocusedView();
    REQUIRE_FALSE(withoutSelectionRes);
    CHECK(withoutSelectionRes.error().code == Error::Code::NotFound);

    // Cover polymorphic destruction of CoreRuntime
    auto const corePtr = std::unique_ptr<CoreRuntime>{std::move(appPtr)};
  }

  TEST_CASE("CoreRuntime - shutdown wakes a writer behind queued publication",
            "[runtime][regression][core-runtime][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = ao::test::requireValue(CoreRuntime::create(std::move(executorPtr),
                                                                 tempDir.path(),
                                                                 LibraryPaths{tempDir.path()}.databasePath(),
                                                                 library::test::kTestMusicLibraryMapSize));

    REQUIRE(runtimePtr->library().createList(LibraryWriter::ListDraft{.name = "Committed before close"}));
    REQUIRE(executor->queuedCount() == 1);

    auto started = AsyncTestState<bool>::create(false);
    auto rejectedByClosing = AsyncTestState<bool>::create(false);
    auto future =
      runtimePtr->async().spawn(attemptWriterDuringQueuedPublication(runtimePtr.get(), started, rejectedByClosing));
    REQUIRE(started.waitUntil(true));
    CHECK_FALSE(rejectedByClosing.load());

    runtimePtr->shutdown();

    CHECK(rejectedByClosing.load());
    CHECK_NOTHROW(future.get());
    CHECK_NOTHROW(executor->drain());
  }

  TEST_CASE("AppRuntime - teardown is deferred until playback callbacks quiesce",
            "[runtime][regression][app-runtime][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto audioStatePtr = std::make_shared<AppRuntimeAudioState>();
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto appPtr = ao::test::requireValue(AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    }));

    appPtr->addAudioProvider(std::make_unique<AppRuntimeProvider>(audioStatePtr));
    REQUIRE(audioStatePtr->onDevicesChanged);
    audioStatePtr->onDevicesChanged(makeReadyAudioStatus().devices);
    executor->drain();

    auto const fixtureUri =
      audio::test::installAudioFixture(appPtr->musicLibrary().rootPath(), "basic_metadata.flac", "playable.flac");
    auto const firstTrackId =
      addRuntimeTrack(*appPtr,
                      library::test::TrackSpec{.title = "First", .uri = fixtureUri, .codec = AudioCodec::Flac},
                      [executor] { executor->drain(); });
    auto const secondTrackId =
      addRuntimeTrack(*appPtr,
                      library::test::TrackSpec{.title = "Second", .uri = fixtureUri, .codec = AudioCodec::Flac},
                      [executor] { executor->drain(); });
    appPtr->sources().reloadAllTracks();
    auto const listId = ao::test::requireValue(appPtr->library().writer().createList(LibraryWriter::ListDraft{
      .name = "Teardown order",
    }));
    auto const viewId = ao::test::requireValue(appPtr->workspace().navigate({.target = listId}));
    auto const previousPositionRevision = appPtr->playback().snapshot().transport.positionRevision;
    REQUIRE(appPtr->playback().commands().startFromView(viewId, firstTrackId));
    REQUIRE(waitForPlaybackSettlement(
      *executor, previousPositionRevision, [&] { return appPtr->playback().snapshot().transport.positionRevision; }));
    REQUIRE(audioStatePtr->renderTarget != nullptr);

    bool callbackEntered = false;
    bool callbackCompleted = false;
    auto const sequenceSubscription = appPtr->playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) noexcept
      {
        callbackEntered = snapshot.succession.currentTrackId == secondTrackId;
        callbackCompleted = true;
      });

    executor->drain();
    auto output = std::array<std::byte, 4096>{};
    REQUIRE(driveRenderUntil(*audioStatePtr->renderTarget, *executor, output, [&] { return callbackEntered; }));
    CHECK(callbackCompleted);
    REQUIRE(appPtr);

    appPtr.reset();
    CHECK_FALSE(appPtr);
  }
} // namespace ao::rt::test
