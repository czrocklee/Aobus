// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <functional>
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

  // CQRS façade over the music library, exposing four cooperating roles:
  // reader (consistent point-in-time reads), writer (synchronous mutations),
  // task service (long-running async operations) and changes (the mutation event bus).
  // Library owns none of its collaborators: the MusicLibrary storage, async
  // Runtime and LibraryChanges bus are injected by reference and outlive it.
  // It merely wires them together and hands out the role objects.
  class Library final
  {
  public:
    static Result<std::unique_ptr<Library>> create(async::Runtime& asyncRuntime,
                                                   library::MusicLibrary& storage,
                                                   LibraryChanges& changes);
    ~Library();

    Library(Library const&) = delete;
    Library& operator=(Library const&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    LibraryReader reader() const;
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
      std::move_only_function<void(LibraryAuthoringAvailability const&)> handler) const;
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
