// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackSource.h"

#include <memory>

namespace ao::rt
{
  class TrackSourceLease final
  {
  public:
    explicit TrackSourceLease(std::shared_ptr<TrackSource const> sourcePtr);

    TrackSourceLease(TrackSourceLease const&) = default;
    TrackSourceLease& operator=(TrackSourceLease const&) = default;
    TrackSourceLease(TrackSourceLease&&) noexcept = default;
    TrackSourceLease& operator=(TrackSourceLease&&) noexcept = default;
    ~TrackSourceLease() = default;

    TrackSource const& source() const noexcept { return *_sourcePtr; }
    TrackSource const& operator*() const noexcept { return *_sourcePtr; }
    TrackSource const* operator->() const noexcept { return _sourcePtr.get(); }

  private:
    std::shared_ptr<TrackSource const> _sourcePtr;
  };
} // namespace ao::rt
