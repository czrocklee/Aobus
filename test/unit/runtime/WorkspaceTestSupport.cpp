// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/WorkspaceTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::rt::test
{
  WorkspaceRuntimeFixture::WorkspaceRuntimeFixture()
    : runtimePtr{makeStateOnlyRuntime(tempDir)}
    , firstListId{createList("First")}
    , secondListId{createList("Second")}
    , thirdListId{createList("Third")}
    , fourthListId{createList("Fourth")}
  {
  }

  AppRuntime& WorkspaceRuntimeFixture::runtime() const noexcept
  {
    return *runtimePtr;
  }

  ListId WorkspaceRuntimeFixture::createList(std::string name) const
  {
    return ao::test::requireValue(
      runRuntimeTask(runtime(), runtime().library().commands().createListAsync(ListDraft{.name = std::move(name)})));
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
