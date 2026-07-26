// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

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
    CachedListSource(CachedListSourceDefinition definition, std::unique_ptr<TrackSource> implementationPtr);
    ~CachedListSource() override;

    CachedListSource(CachedListSource const&) = delete;
    CachedListSource& operator=(CachedListSource const&) = delete;
    CachedListSource(CachedListSource&&) = delete;
    CachedListSource& operator=(CachedListSource&&) = delete;

    CachedListSourceDefinition const& definition() const noexcept { return _definition; }

    void rebind(CachedListSourceDefinition definition, std::unique_ptr<TrackSource> implementationPtr);
    bool trySynchronizeManualDefinition(CachedListSourceDefinition const& definition);
    void semanticInvalidate();

    void applyManualEditScript(delta::RegularTrackEditScript const& script);

    std::size_t size() const override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    void discardSnapshot() noexcept override;
    ManualListSource& manualImplementation();
    void syncManualDefinition(ManualListSource const& source);
    void subscribeToImplementation();
    void handleImplementationBatch(TrackSourceDelta const& batch);

    CachedListSourceDefinition _definition{};
    std::unique_ptr<TrackSource> _implementationPtr;
    async::Subscription _implementationSubscription;
    std::size_t _lastPublishedSize = 0;
  };
} // namespace ao::rt
