// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  struct ListOrderReset final
  {
    bool operator==(ListOrderReset const&) const = default;
  };

  struct ListOrderChange final
  {
    ListId listId = kInvalidListId;
    std::variant<delta::RegularTrackEditScript, ListOrderReset> operation{};

    bool operator==(ListOrderChange const&) const = default;
  };

  struct LibraryChangeSet final
  {
    std::uint64_t libraryRevision = 0;
    bool libraryReset = false;
    std::vector<TrackId> tracksInserted{};
    std::vector<TrackId> tracksDeleted{};
    std::vector<TrackId> tracksMutated{};
    std::vector<ListId> listsUpserted{};
    std::vector<ListId> listsDeleted{};
    std::vector<ListOrderChange> listOrderChanges{};

    bool operator==(LibraryChangeSet const&) const = default;
  };

  class LibraryMutationService;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision);
    ~LibraryChanges();

    LibraryChanges(LibraryChanges const&) = delete;
    LibraryChanges& operator=(LibraryChanges const&) = delete;
    LibraryChanges(LibraryChanges&&) = delete;
    LibraryChanges& operator=(LibraryChanges&&) = delete;

    // Publication runs in two phases (doc/spec/library/runtime/change-publication.md).
    //
    // Phase one delivers the revision to the single bound replica -- the one
    // consumer that keeps derived state the rest of the runtime reads from.
    // Applying a committed revision is a noexcept contract: failure is fatal,
    // not a recoverable publication result. At most one replica may be bound;
    // the returned handle unbinds. A new binding is rejected while publication
    // is active. Unbinding does not interrupt a replica already pinned for the
    // current delivery; it only prevents later deliveries.
    async::Subscription bindReplica(std::string replicaName,
                                    std::move_only_function<void(LibraryChangeSet const&) noexcept> apply) const;

    // Phase two announces an applied revision. Reaching an observer means
    // the replica applied the revision and the library is readable at it.
    // Observers are noexcept notifications.
    async::Subscription onChanged(std::move_only_function<void(LibraryChangeSet const&) noexcept> handler) const;

  private:
    friend class LibraryMutationService;

    void publishFromCoordinator(LibraryChangeSet changeSet, std::move_only_function<void() noexcept> completion);

    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
