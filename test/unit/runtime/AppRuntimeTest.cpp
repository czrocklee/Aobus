// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

using namespace ao;

  namespace ao::rt::test
{

  using namespace ao::rt;
  using namespace ao::lmdb::test;

  namespace
  {
    class MockExecutor final : public IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    class DummyAudioProvider final : public audio::IBackendProvider
    {
    public:
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
    auto workspaceConfigStore = std::make_shared<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml");

    auto app = std::make_unique<AppRuntime>(AppRuntimeDependencies{
      .executor = std::make_unique<MockExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .workspaceConfigStore = std::move(workspaceConfigStore),
    });

    CHECK(app->musicRoot() == std::filesystem::path{tempDir.path()});
    CHECK(app->databasePath() == std::filesystem::path{tempDir.path()} / ".aobus" / "library");

    // Verify accessors
    [[maybe_unused]] auto& commands = app->trackCommands();
    [[maybe_unused]] auto& notifications = app->notifications();

    // addAudioProvider
    app->addAudioProvider(std::make_unique<DummyAudioProvider>());

    // reloadAllTracks
    REQUIRE_NOTHROW(app->reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    CHECK(app->playSelectionInFocusedView() == kInvalidTrackId);

    // Add a view and focus it
    app->workspace().navigateTo(GlobalViewKind::AllTracks);

    // playSelectionInFocusedView (with focused view but no selection)
    CHECK(app->playSelectionInFocusedView() == kInvalidTrackId);

    // Cover polymorphic destruction of CoreRuntime
    auto const core = std::unique_ptr<CoreRuntime>{std::move(app)};
  }

} // namespace ao::rt::test