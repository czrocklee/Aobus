// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class LibraryChanges;
  class LibraryReader;
  class LibraryTaskService;
  class LibraryWriter;

  // CQRS façade over the music library, exposing four cooperating roles:
  // reader (consistent point-in-time reads), writer (synchronous mutations),
  // task service (long-running async operations) and changes (the mutation event bus).
  // Library owns none of its collaborators: the MusicLibrary storage, async
  // Runtime and LibraryChanges bus are injected by reference and outlive it.
  // It merely wires them together and hands out the role objects.
  class Library final
  {
  public:
    Library(async::Runtime& asyncRuntime, library::MusicLibrary& storage, LibraryChanges& changes);
    ~Library();

    Library(Library const&) = delete;
    Library& operator=(Library const&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    LibraryReader reader() const;
    LibraryChanges const& changes() const noexcept;
    LibraryChanges& changes() noexcept;
    LibraryWriter& writer() noexcept;
    LibraryTaskService& taskService() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
