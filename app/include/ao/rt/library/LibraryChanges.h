// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/TrackEditScript.h>

#include <cstdint>
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
  namespace detail
  {
    enum class LibraryPublicationTerminal : std::uint8_t
    {
      Published,
      RetiredByClosing,
    };
  } // namespace detail

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

  class LibraryWriteLane;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    // libraryIdentity is an immutable diagnostic label for post-commit fatal
    // paths. CoreRuntime supplies the physical database path.
    LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision, std::string libraryIdentity);
    ~LibraryChanges();

    LibraryChanges(LibraryChanges const&) = delete;
    LibraryChanges& operator=(LibraryChanges const&) = delete;
    LibraryChanges(LibraryChanges&&) = delete;
    LibraryChanges& operator=(LibraryChanges&&) = delete;

    // Publication runs in two phases (doc/spec/library/runtime/change-publication.md).
    // Both phases run synchronously on the callback executor without
    // coordinator mutex ownership. Callables must defer runtime shutdown or
    // owner destruction to a later executor turn.
    //
    // Phase one delivers the revision to the single bound replica -- the one
    // consumer that keeps derived state the rest of the runtime reads from.
    // An escaping apply exception is diagnosed and aborted by the publication
    // boundary; it is not a recoverable publication result. At most one replica may be bound;
    // the returned handle unbinds. A new binding is rejected while publication
    // is active. Unbinding does not interrupt a replica already pinned for the
    // current delivery; it only prevents later deliveries.
    async::Subscription bindReplica(std::string replicaName,
                                    compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply) const;

    // Phase two announces an applied revision. Reaching an observer means
    // the replica applied the revision and the library is readable at it.
    // Escaping observer exceptions are diagnosed by this publication boundary
    // with the pinned replica and revision context.
    async::Subscription onChanged(compat::MoveOnlyFunction<void(LibraryChangeSet const&)> handler) const;

  private:
    friend class LibraryWriteLane;

    void publishFromCoordinator(LibraryChangeSet changeSet,
                                compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal terminal,
                                                              std::string libraryIdentity,
                                                              std::string replicaName)> completion) noexcept;
    bool publicationDeliveryInProgressFromCoordinator() const noexcept;
    void sealAndRetireFromCoordinator() noexcept;

    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
