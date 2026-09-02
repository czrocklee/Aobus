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
  namespace test
  {
    struct LibraryChangesAccess;
  }

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
  class TrackSourceCache;

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

    // Phase two announces an applied revision. Reaching an observer means
    // the replica applied the revision and the library is readable at it.
    // Escaping observer exceptions are diagnosed by this publication boundary
    // with the pinned replica and revision context.
    async::Subscription onChanged(compat::MoveOnlyFunction<void(LibraryChangeSet const&)> handler) const;

  private:
    friend class LibraryWriteLane;
    friend class TrackSourceCache;
    friend struct test::LibraryChangesAccess;

    // Publication phase one belongs to the runtime's single derived-state
    // replica. Ordinary library observers only see phase two through onChanged().
    async::Subscription bindReplica(std::string replicaName,
                                    compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply) const;

    void publishFromCoordinator(LibraryChangeSet changeSet,
                                compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal terminal,
                                                              std::string libraryIdentity,
                                                              std::string replicaName)> completion) noexcept;
    bool publicationDeliveryInProgressFromCoordinator() const noexcept;
    void sealAndRetireFromCoordinator() noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
