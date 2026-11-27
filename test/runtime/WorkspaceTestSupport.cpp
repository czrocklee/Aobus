// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/runtime/WorkspaceTestSupport.h"

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace ao::rt::test
{
  WorkspaceRuntimeFixture::WorkspaceRuntimeFixture()
    : _ownerPtr{makeStateOnlyRuntime(_tempDir)}
    , _listIds{
        .first = createList("First"),
        .second = createList("Second"),
        .third = createList("Third"),
        .fourth = createList("Fourth"),
      }
  {
  }

  AppRuntime& WorkspaceRuntimeFixture::runtime() const noexcept
  {
    return _ownerPtr->runtime();
  }

  LibraryCommands& WorkspaceRuntimeFixture::commands() const noexcept
  {
    return _ownerPtr->libraryCommands();
  }

  LibraryJobs& WorkspaceRuntimeFixture::jobs() const noexcept
  {
    return _ownerPtr->libraryJobs();
  }

  WorkspaceListIds WorkspaceRuntimeFixture::listIds() const noexcept
  {
    return _listIds;
  }

  std::filesystem::path const& WorkspaceRuntimeFixture::tempPath() const
  {
    return _tempDir.path();
  }

  ListId WorkspaceRuntimeFixture::createList(std::string name) const
  {
    return ao::test::requireValue(
      runRuntimeTask(runtime(), commands().createListAsync(ListDraft{.name = std::move(name)})));
  }

  ViewId requireNavigation(AppRuntime& runtime, NavigationRequest const& request)
  {
    auto res = runtime.workspace().navigate(request);
    REQUIRE(res);
    settleRuntimeCallbacks(runtime);
    return *res;
  }

  ViewId requireNavigation(AppRuntime& runtime, NavigationTarget const& target)
  {
    return requireNavigation(runtime, NavigationRequest{.target = target});
  }

  ViewId requireBackNavigation(AppRuntime& runtime)
  {
    auto res = runtime.workspace().goBack();
    REQUIRE(res);
    settleRuntimeCallbacks(runtime);
    return runtime.workspace().snapshot().activeViewId;
  }

  ViewId requireForwardNavigation(AppRuntime& runtime)
  {
    auto res = runtime.workspace().goForward();
    REQUIRE(res);
    settleRuntimeCallbacks(runtime);
    return runtime.workspace().snapshot().activeViewId;
  }
} // namespace ao::rt::test
