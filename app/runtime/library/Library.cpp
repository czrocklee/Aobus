// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryWriter.h>

#include <memory>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  struct Library::Impl final
  {
    library::MusicLibrary& storage;
    LibraryChanges& changeBus;
    LibraryWriter writer;
    LibraryTasks tasks;

    Impl(async::Runtime& asyncRuntime, library::MusicLibrary& libraryStorage, LibraryChanges& changes)
      : storage{libraryStorage}
      , changeBus{changes}
      , writer{libraryStorage, changes}
      , tasks{asyncRuntime, libraryStorage, changes}
    {
    }
  };

  Library::Library(async::Runtime& asyncRuntime, library::MusicLibrary& storage, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, storage, changes)}
  {
  }

  Library::~Library() = default;

  LibraryReader Library::reader() const
  {
    return LibraryReader{_implPtr->storage};
  }

  LibraryChanges const& Library::changes() const noexcept
  {
    return _implPtr->changeBus;
  }

  LibraryChanges& Library::changes() noexcept
  {
    return _implPtr->changeBus;
  }

  LibraryWriter& Library::writer() noexcept
  {
    return _implPtr->writer;
  }

  LibraryTasks& Library::tasks() noexcept
  {
    return _implPtr->tasks;
  }
} // namespace ao::rt
