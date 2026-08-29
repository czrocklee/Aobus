// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt::test
{
  struct TrackSourceAccess final
  {
    static void invalidate(TrackSource& source) noexcept { source.invalidate(); }
  };

  class MutableTrackSource final : public TrackSource
  {
  public:
    MutableTrackSource();
    ~MutableTrackSource() override;

    MutableTrackSource(MutableTrackSource const&) = delete;
    MutableTrackSource& operator=(MutableTrackSource const&) = delete;
    MutableTrackSource(MutableTrackSource&&) = delete;
    MutableTrackSource& operator=(MutableTrackSource&&) = delete;

    void addInitial(TrackId id);
    void setInitial(std::span<TrackId const> ids);
    void insert(TrackId id, std::size_t index);
    void append(TrackId id);
    void update(TrackId id);
    void remove(TrackId id);
    void reset(std::span<TrackId const> ids = {});
    void emitReset();
    void batchInsert(std::span<TrackId const> ids);
    void batchRemove(std::span<TrackId const> ids);
    void batchUpdate(std::span<TrackId const> ids);
    void updateByIdentity(TrackId id);
    void singleInsert(TrackId id);
    void singleRemove(TrackId id);
    void singleUpdate(TrackId id);
    void replaceWithBatch(std::span<TrackId const> ids, TrackSourceDelta batch);
    void publishBatch(TrackSourceDelta batch);

    std::size_t size() const override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    std::vector<TrackId> _ids;
  };

  std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::span<TrackId const> ids);
  std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::initializer_list<TrackId> ids);
  std::vector<TrackId> sourceTrackIds(TrackSource const& source);
  delta::RegularTrackEditScript const& sourceEditScript(TrackSourceDelta const& message);

  // Recorded batches are intentionally public as the spy's assertion surface.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  class TrackSourceBatchSpy final
  {
  public:
    explicit TrackSourceBatchSpy(TrackSource const& source);
    ~TrackSourceBatchSpy();

    TrackSourceBatchSpy(TrackSourceBatchSpy const&) = delete;
    TrackSourceBatchSpy& operator=(TrackSourceBatchSpy const&) = delete;
    TrackSourceBatchSpy(TrackSourceBatchSpy&&) = delete;
    TrackSourceBatchSpy& operator=(TrackSourceBatchSpy&&) = delete;

    void clear();

    // NOLINTNEXTLINE(aobus-readability-identifier-naming-extensions)
    std::vector<TrackSourceDelta> batches;

  private:
    async::Subscription _subscription;
  };
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
} // namespace ao::rt::test
