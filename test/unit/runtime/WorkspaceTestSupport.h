// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <string>
#include <utility>

namespace ao::rt::test
{
  struct WorkspaceRuntimeFixture final
  {
    WorkspaceRuntimeFixture()
      : runtime{makeRuntime(tempDir)}
      , firstListId{createList("First")}
      , secondListId{createList("Second")}
      , thirdListId{createList("Third")}
      , fourthListId{createList("Fourth")}
    {
    }

    ListId createList(std::string name)
    {
      return ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = std::move(name)}));
    }

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    AppRuntime runtime;
    ListId firstListId;
    ListId secondListId;
    ListId thirdListId;
    ListId fourthListId;
  };

  inline ViewId requireNavigation(AppRuntime& runtime,
                                  NavigationTarget const& target,
                                  NavigationOptions const options = {})
  {
    auto result = runtime.workspace().navigateTo(target, options);
    REQUIRE(result);
    return result->activeViewId;
  }

  inline ViewId requireBackNavigation(AppRuntime& runtime)
  {
    auto result = runtime.workspace().goBack();
    REQUIRE(result);
    return result->activeViewId;
  }

  inline ViewId requireForwardNavigation(AppRuntime& runtime)
  {
    auto result = runtime.workspace().goForward();
    REQUIRE(result);
    return result->activeViewId;
  }
} // namespace ao::rt::test
