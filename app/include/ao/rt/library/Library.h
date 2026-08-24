// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstdint>
#include <filesystem>
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
  class LibraryReader;
  class LibraryTaskService;
  class LibraryWriter;

  /**
   * @brief How much the open library may hold, and how far it has grown.
   *
   * `highWaterBytes` is the peak the storage has needed, not a measure of live
   * data: deleting records returns their pages for reuse without lowering it.
   * A decision about capacity reads the peak, because that is what a mutation
   * runs out of.
   */
  struct LibraryStorageCapacity final
  {
    std::uint64_t mapBytes = 0;
    std::uint64_t highWaterBytes = 0;

    friend bool operator==(LibraryStorageCapacity const&, LibraryStorageCapacity const&) = default;
  };

  // CQRS façade over the music library, exposing four cooperating roles:
  // reader (consistent point-in-time reads), writer (synchronous mutations),
  // task service (long-running async operations) and changes (the mutation event bus).
  // Library owns none of its collaborators: the MusicLibrary storage, async
  // Runtime and LibraryChanges bus are injected by reference and outlive it.
  // It merely wires them together and hands out the role objects.
  class Library final
  {
  public:
    /// @param cacheDirectory Where the derived cover cache lives. A composition
    ///        root resolves it, because the runtime owns paths derived from a
    ///        supplied root and does not discover platform application
    ///        directories. Empty is supported: a cover read then re-extracts
    ///        from a carrier file every time, which costs latency, and costs the
    ///        image itself for content whose carrier files are all gone.
    static Result<std::unique_ptr<Library>> create(async::Runtime& asyncRuntime,
                                                   library::MusicLibrary& storage,
                                                   LibraryChanges& changes,
                                                   std::filesystem::path cacheDirectory = {});
    ~Library();

    Library(Library const&) = delete;
    Library& operator=(Library const&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    LibraryReader reader() const;
    /// What the open storage may hold, for a caller deciding whether it is enough.
    LibraryStorageCapacity storageCapacity() const;
    LibraryChanges const& changes() const noexcept;
    LibraryWriter& writer() noexcept;
    LibraryTaskService& taskService() noexcept;

    // Frontend-facing list mutation. linux-gtk is barred from reaching
    // LibraryWriter directly (frontend-core guardrail), so these stay.
    Result<ListId> createList(LibraryListDraft const& draft);
    Result<UpdateListReply> updateList(LibraryListDraft const& draft);
    Result<DeleteListReply> deleteList(ListId listId, DeleteListOptions options = {});
    Result<DeleteListReply> previewDeleteList(ListId listId, DeleteListOptions options = {});
    Result<DeleteListSubtreeReply> deleteListAndDescendants(ListId listId, DeleteListOptions options = {});
    Result<DeleteListSubtreeReply> previewDeleteListAndDescendants(ListId listId, DeleteListOptions options = {});

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
    explicit Library(std::unique_ptr<Impl> implPtr);
    std::unique_ptr<Impl> _implPtr;

    friend class CoreRuntime;
  };
} // namespace ao::rt
