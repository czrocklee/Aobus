// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <memory>
#include <span>
#include <vector>

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
  class LibrarySnapshot;
  class LibraryJobs;
  class LibraryCommands;

  // CQRS façade over the music library, exposing four cooperating roles:
  // snapshots (consistent point-in-time reads), commands (sequenced
  // asynchronous mutations), jobs (long-running async operations), and changes
  // (the mutation event bus).
  // Library owns its writable capability and private role graph. The underlying
  // MusicLibrary storage, async Runtime and LibraryChanges bus are borrowed and
  // must outlive it.
  class Library final
  {
  public:
    /// Short-lived construction token carrying exclusive write authority for
    /// one final-position MusicLibrary. It is consumed by Library construction.
    class Prepared final
    {
    public:
      ~Prepared();

      Prepared(Prepared const&) = delete;
      Prepared& operator=(Prepared const&) = delete;
      Prepared(Prepared&&) noexcept;
      Prepared& operator=(Prepared&&) noexcept;

    private:
      struct Impl;
      explicit Prepared(std::unique_ptr<Impl> implPtr);
      std::unique_ptr<Impl> _implPtr;

      friend class Library;
    };

    /// Acquires the write authority needed to construct a Library against final storage.
    static Result<Prepared> prepare(library::MusicLibrary& storage);

    /// Consumes prepared write authority. The borrowed MusicLibrary, Runtime,
    /// and LibraryChanges must outlive this facade and pending tasks.
    Library(async::Runtime& asyncRuntime, Prepared prepared, LibraryChanges& changes);
    ~Library();

    Library(Library const&) = delete;
    Library& operator=(Library const&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    LibrarySnapshot snapshot() const;
    LibraryChanges const& changes() const noexcept;
    LibraryCommands& commands() noexcept;
    LibraryJobs& jobs() noexcept;

    LibraryAuthoringAvailability authoringAvailability() const;
    // Synchronous callback-executor notification. A handler must defer
    // Library destruction or CoreRuntime shutdown to a later executor turn.
    async::Subscription onAuthoringAvailabilityChanged(
      compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::span<TrackId const> effectiveTrackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::vector<TrackId>&& effectiveTrackIds) const;

  private:
    void beginClosing() noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class CoreRuntime;
  };
} // namespace ao::rt
