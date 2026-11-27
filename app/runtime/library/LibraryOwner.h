// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LibraryWriteLane.h"
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>

namespace ao::async
{
  class Runtime;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;

  /// Owns the write lane and the roles backed by it. Storage, async runtime and change bus are borrowed.
  class LibraryOwner final
  {
  public:
    LibraryOwner(async::Runtime& asyncRuntime, library::WritableMusicLibrary writable, LibraryChanges& changes);
    ~LibraryOwner();
    LibraryOwner(LibraryOwner const&) = delete;
    LibraryOwner& operator=(LibraryOwner const&) = delete;
    LibraryOwner(LibraryOwner&&) = delete;
    LibraryOwner& operator=(LibraryOwner&&) = delete;

    Library const& library() const noexcept { return _library; }
    LibraryCommands& libraryCommands() noexcept { return _commands; }
    LibraryJobs& libraryJobs() noexcept { return _jobs; }
    /// Terminally and idempotently close admission and settle admitted mutations while workers can release leases.
    /// Keep this owner alive through Runtime::requestStop()/join(); closing alone does not quiesce all jobs.
    void beginClosing() noexcept;

  private:
    LibraryOwner(async::Runtime& asyncRuntime,
                 library::MusicLibrary& storage,
                 library::WritableMusicLibrary&& writable,
                 LibraryChanges& changes);

    LibraryWriteLane _writeLane;
    Library _library;
    LibraryCommands _commands;
    LibraryJobs _jobs;
  };
} // namespace ao::rt
