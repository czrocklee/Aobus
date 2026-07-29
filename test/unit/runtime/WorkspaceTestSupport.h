// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/WorkspaceService.h>

#include <string>

namespace ao::rt::test
{
  struct WorkspaceRuntimeFixture final
  {
    WorkspaceRuntimeFixture();

    ListId createList(std::string name);

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    AppRuntime runtime;
    ListId firstListId;
    ListId secondListId;
    ListId thirdListId;
    ListId fourthListId;
  };

  ViewId requireNavigation(AppRuntime& runtime, NavigationRequest const& request);

  ViewId requireNavigation(AppRuntime& runtime, NavigationTarget const& target);

  ViewId requireBackNavigation(AppRuntime& runtime);

  ViewId requireForwardNavigation(AppRuntime& runtime);
} // namespace ao::rt::test
