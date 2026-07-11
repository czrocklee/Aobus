// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  class TrackSourceCache;
  class ManualListSource;
  struct ManualTracksInsert;
  struct ManualTracksMove;
  struct ManualTracksRemove;

  enum class CachedListSourceKind : std::uint8_t
  {
    Manual,
    Smart,
  };

  struct CachedListSourceDefinition final
  {
    ListId parentId = kInvalidListId;
    CachedListSourceKind kind = CachedListSourceKind::Manual;
    std::string smartExpression{};
    std::vector<TrackId> storedTrackIds{};

    friend bool operator==(CachedListSourceDefinition const&, CachedListSourceDefinition const&) = default;
  };

  class CachedListSource final : public TrackSource
  {
  public:
    CachedListSource(ListId listId,
                     CachedListSourceDefinition definition,
                     TrackSourceLease parentLease,
                     std::unique_ptr<TrackSource> implementationPtr);
    ~CachedListSource() override;

    CachedListSource(CachedListSource const&) = delete;
    CachedListSource& operator=(CachedListSource const&) = delete;
    CachedListSource(CachedListSource&&) = delete;
    CachedListSource& operator=(CachedListSource&&) = delete;

    ListId listId() const noexcept { return _listId; }
    CachedListSourceDefinition const& definition() const noexcept { return _definition; }

    void rebind(CachedListSourceDefinition definition,
                TrackSourceLease parentLease,
                std::unique_ptr<TrackSource> implementationPtr);
    bool trySynchronizeManualDefinition(CachedListSourceDefinition const& definition);
    void semanticInvalidate();

    // Internal detailed-event path used by TrackSourceCache.
    void applyManualTracksInsert(ManualTracksInsert const& operation);
    void applyManualTracksRemove(ManualTracksRemove const& operation);
    void applyManualTracksMove(ManualTracksMove const& operation);

    std::size_t size() const override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    ManualListSource& manualImplementation();
    void syncManualDefinition(ManualListSource const& source);
    void subscribeToImplementation();
    void handleImplementationBatch(TrackSourceDeltaBatch const& batch);

    ListId _listId = kInvalidListId;
    CachedListSourceDefinition _definition{};
    TrackSourceLease _parentLease;
    std::unique_ptr<TrackSource> _implementationPtr;
    Subscription _implementationSubscription;
    std::size_t _publishedSize = 0;
  };
} // namespace ao::rt
