// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppRuntime.h>

#include "lib/audio/NullBackend.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTestSupport.h"
#include "test/runtime/PlaybackUiTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  namespace
  {
    // An individual owner operation on either borrow would breach the boundary.
    template<typename Runtime>
    concept HasInteractiveOwnerAccess =
      requires(Runtime& runtime) { runtime.core(); } || requires(Runtime& runtime) { runtime.owner(); } ||
      requires(Runtime& runtime) { runtime.databasePath(); } ||
      requires(Runtime& runtime) { runtime.musicLibrary(); } ||
      requires(Runtime& runtime) { runtime.libraryCommands(); } ||
      requires(Runtime& runtime) { runtime.libraryJobs(); } || requires(Runtime& runtime) { runtime.shutdown(); } ||
      requires(AppRuntimeDependencies dependencies) { Runtime::create(std::move(dependencies)); };

    template<typename Reads>
    concept HasLibraryOwnerAccess =
      requires(Reads& reads) { reads.owner(); } || requires(Reads& reads) { reads.commands(); } ||
      requires(Reads& reads) { reads.jobs(); } || requires(Reads& reads) { reads.libraryCommands(); } ||
      requires(Reads& reads) { reads.libraryJobs(); } || requires(Reads& reads) { reads.beginClosing(); } ||
      requires(library::MusicLibrary& storage) { Reads::prepare(storage); };

    template<typename Jobs>
    concept CanSubmitLibraryJob =
      requires(Jobs& jobs) { jobs.prepareLibraryImportAsync({}, ImportMode::Restore); } ||
      requires(Jobs& jobs, LibraryImportPlan plan) { jobs.applyLibraryImportPlanAsync(std::move(plan)); } ||
      requires(Jobs& jobs, ExportMode mode) { jobs.exportLibraryAsync({}, mode); } ||
      requires(Jobs& jobs) { jobs.buildScanPlanAsync(); } ||
      requires(Jobs& jobs, ScanPlan plan) { jobs.applyScanPlanAsync(std::move(plan)); } ||
      requires(Jobs& jobs) { jobs.verifyAudioFingerprintAsync("track.flac"); } ||
      requires(Jobs& jobs) { jobs.backfillAudioIdentityAsync(); };

    template<typename OwnerRef>
    concept GrantsAnyRuntimeBorrow = requires { std::declval<OwnerRef>().runtime(); } || requires {
      std::declval<OwnerRef>().libraryCommands();
    } || requires { std::declval<OwnerRef>().libraryJobs(); };

    // Test move construction independently of the borrow's inaccessible destructor.
    template<typename Runtime>
    concept CanMoveConstructBorrow = requires(Runtime& runtime) { ::new Runtime(std::move(runtime)); };

    static_assert(!std::is_base_of_v<CoreRuntime, AppRuntime>);
    static_assert(!CanMoveConstructBorrow<AppRuntime>);
    static_assert(!std::is_move_assignable_v<AppRuntime>);
    static_assert(!std::is_destructible_v<AppRuntime>);
    static_assert(!std::is_destructible_v<Library>);
    static_assert(!std::is_destructible_v<LibraryCommands>);
    static_assert(!std::is_destructible_v<LibraryJobs>);
    static_assert(!GrantsAnyRuntimeBorrow<AppRuntimeOwner const&>);
    static_assert(!GrantsAnyRuntimeBorrow<AppRuntimeOwner&&>);
    static_assert(!GrantsAnyRuntimeBorrow<AppRuntimeOwner const&&>);
    static_assert(!HasInteractiveOwnerAccess<AppRuntime>);
    static_assert(!HasLibraryOwnerAccess<Library>);
    static_assert(!CanSubmitLibraryJob<LibraryJobs const>);

    static_assert(std::is_nothrow_move_constructible_v<AppRuntimeOwner> && !std::is_move_assignable_v<AppRuntimeOwner>);
    static_assert(std::is_final_v<CoreRuntime>);
    static_assert(std::is_nothrow_move_constructible_v<CoreRuntime> && !std::is_move_assignable_v<CoreRuntime>);

    static_assert(requires(AppRuntime& runtime, CoreRuntime& core, LibraryCommands& commands) {
      { runtime.library() } -> std::same_as<Library const&>;
      { runtime.jobProgress() } -> std::same_as<LibraryJobs const&>;
      { core.library() } -> std::same_as<Library const&>;
      { core.musicLibrary() } -> std::same_as<library::MusicLibrary const&>;
      { commands.library() } -> std::same_as<Library const&>;
    });
    static_assert(requires(AppRuntimeDependencies dependencies, AppRuntimeOwner& owner, CoreRuntime& core) {
      { AppRuntimeOwner::open(std::move(dependencies)) } -> std::same_as<Result<AppRuntimeOwner>>;
      { owner.runtime() } -> std::same_as<AppRuntime&>;
      { owner.libraryCommands() } -> std::same_as<LibraryCommands&>;
      { owner.libraryJobs() } -> std::same_as<LibraryJobs&>;
      { core.libraryCommands() } -> std::same_as<LibraryCommands&>;
      { core.libraryJobs() } -> std::same_as<LibraryJobs&>;
      { owner.shutdown() } noexcept -> std::same_as<void>;
      { core.shutdown() } noexcept -> std::same_as<void>;
    });

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

      audio::BackendOpenResult open(audio::SignalFormat const& sourceFormat, audio::RenderTarget& target) override
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

      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
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

      utility::ScopedRegistration subscribeGraph(std::string_view /*routeAnchor*/,
                                                 OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      std::shared_ptr<AppRuntimeAudioState> _statePtr;
      Status _status;
    };
  } // namespace

  TEST_CASE("AppRuntime - missing workspace config store is rejected")
  {
    auto tempDir = ao::test::TempDir{};

    auto const runtimeRes = AppRuntimeOwner::open(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
    });

    REQUIRE_FALSE(runtimeRes);
    CHECK(runtimeRes.error().code == Error::Code::InvalidInput);
    CHECK(runtimeRes.error().message == "AppRuntime requires a workspace config store");
  }

  TEST_CASE("runtime factories preserve storage construction errors")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const databaseFile = ao::test::TempFile{".mdb"};

    SECTION("CoreRuntime")
    {
      auto const res = CoreRuntime::create(std::make_unique<InlineExecutor>(),
                                           tempDir.path(),
                                           databaseFile.path,
                                           tempDir.path() / "cache",
                                           library::test::kTestMusicLibraryMapBytes);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
    }

    SECTION("AppRuntime")
    {
      auto const res = AppRuntimeOwner::open(AppRuntimeDependencies{
        .executorPtr = std::make_unique<InlineExecutor>(),
        .musicRoot = tempDir.path(),
        .databasePath = databaseFile.path,
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
      });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
    }
  }

  TEST_CASE("CoreRuntime - move construction preserves service identity after the source retires")
  {
    auto tempDir = ao::test::TempDir{};
    auto optRuntime = std::optional<CoreRuntime>{};
    auto const* libraryAddress = static_cast<void const*>(nullptr);
    auto const* asyncAddress = static_cast<void const*>(nullptr);
    auto const* sourcesAddress = static_cast<void const*>(nullptr);
    auto const musicRoot = tempDir.path() / ".";
    auto const databasePath = LibraryPaths{musicRoot}.databasePath();
    std::filesystem::path const* musicRootAddress = nullptr;
    std::filesystem::path const* databasePathAddress = nullptr;

    {
      auto source = ao::test::requireValue(CoreRuntime::create(std::make_unique<InlineExecutor>(),
                                                               musicRoot,
                                                               databasePath,
                                                               tempDir.path() / "cache",
                                                               library::test::kTestMusicLibraryMapBytes));
      libraryAddress = &source.library();
      asyncAddress = &source.async();
      sourcesAddress = &source.sources();
      musicRootAddress = &source.musicRoot();
      databasePathAddress = &source.databasePath();
      optRuntime.emplace(std::move(source));
    }

    REQUIRE(optRuntime);
    CHECK(static_cast<void const*>(&optRuntime->library()) == libraryAddress);
    CHECK(static_cast<void const*>(&optRuntime->async()) == asyncAddress);
    CHECK(static_cast<void const*>(&optRuntime->sources()) == sourcesAddress);
    CHECK_NOTHROW(optRuntime->sources().reloadAllTracks());
    CHECK(optRuntime->musicRoot() == musicRoot);
    CHECK(optRuntime->databasePath() == databasePath);
    CHECK(&optRuntime->musicRoot() == musicRootAddress);
    CHECK(&optRuntime->databasePath() == databasePathAddress);

    optRuntime->shutdown();
    CHECK(*musicRootAddress == musicRoot);
    CHECK(*databasePathAddress == databasePath);
    CHECK(&optRuntime->musicRoot() == musicRootAddress);
    CHECK(&optRuntime->databasePath() == databasePathAddress);
  }

  TEST_CASE("AppRuntimeOwner - move construction preserves interactive service identity after the source retires")
  {
    auto tempDir = ao::test::TempDir{};
    auto optOwner = std::optional<AppRuntimeOwner>{};
    AppRuntime* runtimeAddress = nullptr;
    Library const* libraryAddress = nullptr;
    LibraryCommands* commandsAddress = nullptr;
    LibraryJobs* jobsAddress = nullptr;
    auto const* playbackAddress = static_cast<void const*>(nullptr);
    auto const* workspaceAddress = static_cast<void const*>(nullptr);
    auto const* resourcesAddress = static_cast<void const*>(nullptr);

    {
      auto source = ao::test::requireValue(AppRuntimeOwner::open(AppRuntimeDependencies{
        .executorPtr = std::make_unique<InlineExecutor>(),
        .musicRoot = tempDir.path(),
        .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
      }));
      auto& runtime = source.runtime();
      runtimeAddress = &runtime;
      libraryAddress = &runtime.library();
      commandsAddress = &source.libraryCommands();
      jobsAddress = &source.libraryJobs();
      playbackAddress = &runtime.playback();
      workspaceAddress = &runtime.workspace();
      resourcesAddress = &runtime.resourceBytes();
      optOwner.emplace(std::move(source));
    }

    REQUIRE(optOwner);
    auto& runtime = optOwner->runtime();
    CHECK(&runtime == runtimeAddress);
    CHECK(&runtime.library() == libraryAddress);
    CHECK(&optOwner->libraryCommands() == commandsAddress);
    CHECK(&optOwner->libraryCommands().library() == libraryAddress);
    CHECK(&optOwner->libraryJobs() == jobsAddress);
    CHECK(&runtime.jobProgress() == jobsAddress);
    CHECK(static_cast<void const*>(&runtime.playback()) == playbackAddress);
    CHECK(static_cast<void const*>(&runtime.workspace()) == workspaceAddress);
    CHECK(static_cast<void const*>(&runtime.resourceBytes()) == resourcesAddress);
    auto const res = runtime.playSelectionInFocusedView();
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("AppRuntime - factory materializes All Tracks before exposure")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const databasePath = LibraryPaths{tempDir.path()}.databasePath();
    {
      auto library = library::test::makeTestMusicLibrary(tempDir.path(), databasePath);
      [[maybe_unused]] auto const trackId = library::test::addTrackWithUniqueFixtureUri(
        library, library::test::TrackSpec{.title = "Already persisted", .uri = "persisted.flac"});
    }

    auto owner = ao::test::requireValue(AppRuntimeOwner::open(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = databasePath,
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
    }));
    auto allTracks = ao::test::requireValue(owner.runtime().sources().acquire(kAllTracksListId));

    CHECK(allTracks->size() == 1);
  }

  TEST_CASE("AppRuntime - playback discard mutates only the selected store")
  {
    auto tempDir = ao::test::TempDir{};
    auto overrideStore = ConfigStore{tempDir.path() / "playback.yaml"};
    auto const sentinel = PlaybackSessionState{.sourceListId = kAllTracksListId,
                                               .currentTrackId = TrackId{7},
                                               .currentTrackUri = "sentinel.flac",
                                               .volume = 0.25F};
    REQUIRE(overrideStore.save(kPlaybackSessionConfigGroup, sentinel, PlaybackSessionYamlSchema{}));

    SECTION("null override uses the workspace store")
    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      REQUIRE(runtime.workspaceConfigStore().save(kPlaybackSessionConfigGroup, sentinel, PlaybackSessionYamlSchema{}));
      REQUIRE(runtime.discardRestorablePlaybackSession());
      CHECK_FALSE(ao::test::requireValue(runtime.workspaceConfigStore().contains(kPlaybackSessionConfigGroup)));
      auto retained = PlaybackSessionState{};
      REQUIRE(
        ao::test::requireValue(overrideStore.load(kPlaybackSessionConfigGroup, retained, PlaybackSessionYamlSchema{})));
      CHECK(retained == sentinel);
    }

    SECTION("explicit override leaves the workspace store untouched")
    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir, &overrideStore);
      auto& runtime = ownerPtr->runtime();
      REQUIRE(runtime.workspaceConfigStore().save(kPlaybackSessionConfigGroup, sentinel, PlaybackSessionYamlSchema{}));
      REQUIRE(runtime.discardRestorablePlaybackSession());
      CHECK_FALSE(ao::test::requireValue(overrideStore.contains(kPlaybackSessionConfigGroup)));
      auto retained = PlaybackSessionState{};
      REQUIRE(ao::test::requireValue(
        runtime.workspaceConfigStore().load(kPlaybackSessionConfigGroup, retained, PlaybackSessionYamlSchema{})));
      CHECK(retained == sentinel);
    }
  }

  TEST_CASE("AppRuntime - dependencies expose services and empty selection is safe")
  {
    auto tempDir = ao::test::TempDir{};
    auto const databasePath = LibraryPaths{tempDir.path()}.databasePath();

    auto owner = ao::test::requireValue(AppRuntimeOwner::open(AppRuntimeDependencies{
      .executorPtr = std::make_unique<InlineExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = databasePath,
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    }));
    auto& runtime = owner.runtime();

    CHECK(runtime.musicRoot() == std::filesystem::path{tempDir.path()});

    runtime.addAudioProvider(
      makeReadyAudioProvider(audio::BackendProvider::Status{.descriptor = {.id = audio::BackendId{"dummy"}}}));

    // reloadAllTracks
    CHECK_NOTHROW(runtime.sources().reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    auto const withoutFocusRes = runtime.playSelectionInFocusedView();
    REQUIRE_FALSE(withoutFocusRes);
    CHECK(withoutFocusRes.error().code == Error::Code::InvalidState);

    // Add a view and focus it
    REQUIRE(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));

    // playSelectionInFocusedView (with focused view but no selection)
    auto const withoutSelectionRes = runtime.playSelectionInFocusedView();
    REQUIRE_FALSE(withoutSelectionRes);
    CHECK(withoutSelectionRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("AppRuntime - focused playback skips a hidden first selection and starts the next selected track")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const hiddenId = fixture.addPlayableTrack("Hidden A");
    auto const shownId = fixture.addPlayableTrack("Visible B");
    auto& runtime = fixture.runtime();
    auto const selection = std::vector{hiddenId, shownId};
    REQUIRE(runtime.views().setSelection(fixture.viewId(), selection));
    REQUIRE(runtime.views().setFilter(fixture.viewId(), R"($title = "Visible B")"));
    fixture.executor().drain();

    auto const previousRevision = runtime.playback().snapshot().transport.positionRevision;
    CHECK(requireValue(runtime.playSelectionInFocusedView()) == shownId);
    REQUIRE(fixture.tryWaitForPlayback(shownId, previousRevision));
    CHECK(runtime.playback().snapshot().succession.currentTrackId == shownId);
    CHECK(runtime.views().trackListState(fixture.viewId()).selection == selection);
  }

  TEST_CASE("AppRuntime - playSelectionInFocusedView rejects hidden selection before playback admission")
  {
    auto tempDir = TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto ownerPtr = makeRuntime(tempDir, std::move(executorPtr));
    auto& runtime = ownerPtr->runtime();
    auto const hiddenTrackId = addRuntimeTrack(*ownerPtr, {.title = "Selected"});
    auto const shownTrackId = addRuntimeTrack(*ownerPtr, {.title = "Unselected"});
    auto const viewId = requireValue(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));
    auto& views = runtime.views();
    REQUIRE(views.setSelection(viewId, {hiddenTrackId}));
    REQUIRE(views.setFilter(viewId, R"($title = "Unselected")"));
    executor->drain();
    auto const projectionPtr = requireValue(views.findTrackListProjection(viewId));
    REQUIRE(projectionPtr->size() == 1);
    REQUIRE(projectionPtr->trackIdAt(0) == shownTrackId);
    REQUIRE(views.trackListState(viewId).selection == std::vector{hiddenTrackId});

    auto& playback = runtime.playback();
    auto const before = playback.snapshot();
    auto optPlayRes = std::optional<Result<TrackId>>{};
    auto const sub = playback.events().onRevealTrackRequested(
      [&](PlaybackRevealTrackRequest const&) noexcept { optPlayRes.emplace(runtime.playSelectionInFocusedView()); });

    // An observer-issued start would queue successfully, even if its track was later rejected by the playback layer.
    playback.commands().revealTrack(shownTrackId);

    auto const hasPlayRes = optPlayRes.has_value();
    REQUIRE(hasPlayRes);
    REQUIRE_FALSE(*optPlayRes);
    CHECK(optPlayRes->error().code == Error::Code::NotFound);
    CHECK(executor->queuedCount() == 0);
    CHECK(playback.snapshot() == before);
    CHECK(views.trackListState(viewId).selection == std::vector{hiddenTrackId});
  }

  TEST_CASE("CoreRuntime - shutdown retires a queued library publication", "[concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    std::size_t publications = 0;
    auto runtime = ao::test::requireValue(CoreRuntime::create(std::move(executorPtr),
                                                              tempDir.path(),
                                                              LibraryPaths{tempDir.path()}.databasePath(),
                                                              tempDir.path() / "cache",
                                                              library::test::kTestMusicLibraryMapBytes));

    auto subscription =
      runtime.library().changes().onChanged([&publications](LibraryChangeSet const&) { ++publications; });
    REQUIRE(runQueuedTask(
      runtime.async(), *executor, runtime.libraryCommands().createListAsync(ListDraft{.name = "Live publication"})));
    REQUIRE(publications == 1);
    publications = 0;
    auto const revisionBefore = runtime.library().snapshot().revision();

    [[maybe_unused]] auto future =
      runtime.async().spawn(runtime.libraryCommands().createListAsync(ListDraft{.name = "Committed before close"}));
    REQUIRE(executor->tryWaitUntilQueued());
    REQUIRE(executor->queuedCount() == 1);
    {
      auto snapshot = runtime.library().snapshot();
      REQUIRE(snapshot.revision() == revisionBefore + 1);
      REQUIRE(std::ranges::any_of(
        snapshot.lists(), [](ListNode const& node) { return node.name == "Committed before close"; }));
    }

    CHECK(publications == 0);
    runtime.shutdown();
    CHECK_NOTHROW(executor->drain());
    CHECK(publications == 0);
  }

  TEST_CASE("AppRuntime - teardown after playback callback settlement shuts down the provider", "[concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto audioStatePtr = std::make_shared<AppRuntimeAudioState>();
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto ownerPtr =
      std::make_unique<AppRuntimeOwner>(ao::test::requireValue(AppRuntimeOwner::open(AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = tempDir.path(),
        .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .workspaceConfigStorePtr =
          std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
      })));
    auto& runtime = ownerPtr->runtime();

    runtime.addAudioProvider(std::make_unique<AppRuntimeProvider>(audioStatePtr));
    REQUIRE(audioStatePtr->onDevicesChanged);
    audioStatePtr->onDevicesChanged(makeReadyAudioStatus().devices);
    executor->drain();

    auto const fixtureUri =
      audio::test::installAudioFixture(runtime.musicRoot(), "basic_metadata.flac", "playable.flac");
    auto const firstTrackId =
      addRuntimeTrack(*ownerPtr,
                      library::test::TrackSpec{.title = "First", .uri = fixtureUri, .codec = AudioCodec::Flac},
                      [executor] { executor->drain(); });
    auto const secondTrackId =
      addRuntimeTrack(*ownerPtr,
                      library::test::TrackSpec{.title = "Second", .uri = fixtureUri, .codec = AudioCodec::Flac},
                      [executor] { executor->drain(); });
    runtime.sources().reloadAllTracks();
    auto const listId = ao::test::requireValue(
      runRuntimeTask(runtime, ownerPtr->libraryCommands().createListAsync(ListDraft{.name = "Teardown order"})));
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
    auto const previousPositionRevision = runtime.playback().snapshot().transport.positionRevision;
    REQUIRE(runtime.playback().commands().startFromView(viewId, firstTrackId));
    REQUIRE(tryWaitForPlaybackSettlement(
      *executor, previousPositionRevision, [&] { return runtime.playback().snapshot().transport.positionRevision; }));
    REQUIRE(audioStatePtr->renderTarget != nullptr);

    bool callbackEntered = false;
    auto const sequenceSubscription = runtime.playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) noexcept
      { callbackEntered = snapshot.succession.currentTrackId == secondTrackId; });

    executor->drain();
    auto output = std::array<std::byte, 4096>{};
    REQUIRE(tryDriveRenderUntil(*audioStatePtr->renderTarget, *executor, output, [&] { return callbackEntered; }));
    CHECK_FALSE(audioStatePtr->providerShutdownStarted.load());
    ownerPtr.reset();
    CHECK(audioStatePtr->providerShutdownStarted.load());
  }
} // namespace ao::rt::test
