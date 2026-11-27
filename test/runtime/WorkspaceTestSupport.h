// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/WorkspaceService.h>

#include <filesystem>
#include <memory>
#include <string>

namespace ao::rt::test
{
  struct WorkspaceListIds final
  {
    ListId first;
    ListId second;
    ListId third;
    ListId fourth;
  };

  class WorkspaceRuntimeFixture final
  {
  public:
    WorkspaceRuntimeFixture();

    AppRuntime& runtime() const noexcept;
    LibraryCommands& commands() const noexcept;
    LibraryJobs& jobs() const noexcept;
    WorkspaceListIds listIds() const noexcept;
    std::filesystem::path const& tempPath() const;

    ListId createList(std::string name) const;

  private:
    ao::test::TempDir _tempDir;
    std::unique_ptr<AppRuntimeOwner> _ownerPtr;
    WorkspaceListIds _listIds;
  };

  ViewId requireNavigation(AppRuntime& runtime, NavigationRequest const& request);

  ViewId requireNavigation(AppRuntime& runtime, NavigationTarget const& target);

  ViewId requireBackNavigation(AppRuntime& runtime);

  ViewId requireForwardNavigation(AppRuntime& runtime);
} // namespace ao::rt::test
