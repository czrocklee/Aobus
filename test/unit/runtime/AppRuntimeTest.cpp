// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    class DummyAudioProvider final : public audio::IBackendProvider
    {
    public:
      void shutdown() noexcept override {}
      audio::Subscription subscribeDevices(OnDevicesChangedCallback /*callback*/) override { return {}; }
      Status status() const override { return Status{.metadata = {.id = audio::BackendId{"dummy"}}}; }
      std::unique_ptr<audio::IBackend> createBackend(audio::Device const& /*device*/,
                                                     audio::ProfileId const& /*profile*/) override
      {
        return nullptr;
      }
      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }
    };
  }

  TEST_CASE("AppRuntime - Coverage", "[runtime][unit]")
  {
    auto tempDir = TempDir{};

    auto appPtr = std::make_unique<AppRuntime>(AppRuntimeDependencies{
      .executorPtr = std::make_unique<MockExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    });

    CHECK(appPtr->musicRoot() == std::filesystem::path{tempDir.path()});
    CHECK(appPtr->databasePath() == std::filesystem::path{tempDir.path()} / ".aobus" / "library");

    // Verify accessors
    [[maybe_unused]] auto& commands = appPtr->library().writer();
    [[maybe_unused]] auto& notifications = appPtr->notifications();

    // addAudioProvider
    appPtr->addAudioProvider(std::make_unique<DummyAudioProvider>());

    // reloadAllTracks
    REQUIRE_NOTHROW(appPtr->reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    CHECK(appPtr->playSelectionInFocusedView() == kInvalidTrackId);

    // Add a view and focus it
    appPtr->workspace().navigateTo(GlobalViewKind::AllTracks);

    // playSelectionInFocusedView (with focused view but no selection)
    CHECK(appPtr->playSelectionInFocusedView() == kInvalidTrackId);

    // Cover polymorphic destruction of CoreRuntime
    auto const corePtr = std::unique_ptr<CoreRuntime>{std::move(appPtr)};
  }
} // namespace ao::rt::test
