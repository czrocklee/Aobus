// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  class TrackSourceCache;
  class ListOrderSource;

  std::optional<Error> trackSourceError(TrackSource const& source);

  struct CachedListSourceDefinition final
  {
    ListId listId = kInvalidListId;
    ListId parentId = kInvalidListId;
    std::string expression{};
    std::vector<TrackId> orderTrackIds{};

    friend bool operator==(CachedListSourceDefinition const&, CachedListSourceDefinition const&) = default;
  };

  class CachedListSource final : public TrackSource
  {
  public:
    CachedListSource(CachedListSourceDefinition definition, std::unique_ptr<ListOrderSource> implementationPtr);
    ~CachedListSource() override;

    CachedListSource(CachedListSource const&) = delete;
    CachedListSource& operator=(CachedListSource const&) = delete;
    CachedListSource(CachedListSource&&) = delete;
    CachedListSource& operator=(CachedListSource&&) = delete;

    CachedListSourceDefinition const& definition() const noexcept { return _definition; }
    std::optional<Error> sourceError() const;

    void rebind(CachedListSourceDefinition definition, std::unique_ptr<ListOrderSource> implementationPtr);
    bool trySynchronizeOrderDefinition(CachedListSourceDefinition const& definition);
    void semanticInvalidate();

    void applyOrderEditScript(delta::RegularTrackEditScript const& script);

    std::size_t size() const override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    void discardSnapshot() noexcept override;
    void syncOrderDefinition(ListOrderSource const& source);
    void subscribeToImplementation();
    void handleImplementationBatch(TrackSourceDelta const& batch);

    CachedListSourceDefinition _definition{};
    std::unique_ptr<ListOrderSource> _implementationPtr;
    async::Subscription _implementationSubscription;
    std::size_t _lastPublishedSize = 0;
  };
} // namespace ao::rt
