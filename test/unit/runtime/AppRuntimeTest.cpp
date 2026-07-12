// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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

      Result<> open(audio::Format const& /*format*/, audio::RenderTarget* target) override
      {
        _statePtr->renderTarget = target;
        return {};
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
  } // namespace

  TEST_CASE("AppRuntime - dependencies expose services and empty selection is safe", "[runtime][unit][app-runtime]")
  {
    auto tempDir = ao::test::TempDir{};

    auto appPtr = std::make_unique<AppRuntime>(AppRuntimeDependencies{
      .executorPtr = std::make_unique<MockExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    });

    CHECK(appPtr->musicRoot() == std::filesystem::path{tempDir.path()});
    CHECK(appPtr->databasePath() == std::filesystem::path{tempDir.path()} / ".aobus" / "library");

    // Verify accessors
    [[maybe_unused]] auto& commands = appPtr->library().writer();
    [[maybe_unused]] auto& notifications = appPtr->notifications();

    appPtr->addAudioProvider(
      makeReadyAudioProvider(audio::BackendProvider::Status{.descriptor = {.id = audio::BackendId{"dummy"}}}));

    // reloadAllTracks
    CHECK_NOTHROW(appPtr->reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    auto const withoutFocus = appPtr->playSelectionInFocusedView();
    REQUIRE_FALSE(withoutFocus);
    CHECK(withoutFocus.error().code == Error::Code::InvalidState);

    // Add a view and focus it
    REQUIRE(appPtr->workspace().navigateTo(GlobalViewKind::AllTracks));

    // playSelectionInFocusedView (with focused view but no selection)
    auto const withoutSelection = appPtr->playSelectionInFocusedView();
    REQUIRE_FALSE(withoutSelection);
    CHECK(withoutSelection.error().code == Error::Code::NotFound);

    // Cover polymorphic destruction of CoreRuntime
    auto const corePtr = std::unique_ptr<CoreRuntime>{std::move(appPtr)};
  }

  TEST_CASE("AppRuntime - teardown is deferred until playback callbacks quiesce",
            "[runtime][regression][app-runtime][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto audioStatePtr = std::make_shared<AppRuntimeAudioState>();
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto appPtr = std::make_unique<AppRuntime>(AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    });

    appPtr->addAudioProvider(std::make_unique<AppRuntimeProvider>(audioStatePtr));
    REQUIRE(audioStatePtr->onDevicesChanged);
    audioStatePtr->onDevicesChanged(makeReadyAudioStatus().devices);
    executor->drain();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrackId = library::test::addTrack(
      appPtr->musicLibrary(),
      library::test::TrackSpec{.title = "First", .uri = fixturePath, .codec = AudioCodec::Flac});
    auto const secondTrackId = library::test::addTrack(
      appPtr->musicLibrary(),
      library::test::TrackSpec{.title = "Second", .uri = fixturePath, .codec = AudioCodec::Flac});
    appPtr->sources().reloadAllTracks();
    auto const listId = ao::test::requireValue(appPtr->library().writer().createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Teardown order",
      .trackIds = {firstTrackId, secondTrackId},
    }));
    auto const viewId = ao::test::requireValue(appPtr->views().createView({.listId = listId}, true)).viewId;
    REQUIRE(appPtr->playbackSequence().playFromView(viewId, firstTrackId));
    REQUIRE(audioStatePtr->renderTarget != nullptr);

    bool callbackEntered = false;
    bool callbackCompleted = false;
    auto const sequenceSubscription = appPtr->playbackSequence().onChanged(
      [&](PlaybackSequenceState const& state)
      {
        callbackEntered = state.currentTrackId == secondTrackId;
        callbackCompleted = true;
      });

    auto output = std::array<std::byte, 4096>{};
    bool crossedSpliceBoundary = false;

    for (std::int32_t i = 0; i < 100000 && !crossedSpliceBoundary; ++i)
    {
      crossedSpliceBoundary = audioStatePtr->renderTarget->renderPcm(output).positionFrameOffset > 0;
    }

    REQUIRE(crossedSpliceBoundary);
    REQUIRE(executor->drainUntil([&] { return callbackEntered; }));
    CHECK(callbackCompleted);
    REQUIRE(appPtr);

    appPtr.reset();
    CHECK_FALSE(appPtr);
  }
} // namespace ao::rt::test
