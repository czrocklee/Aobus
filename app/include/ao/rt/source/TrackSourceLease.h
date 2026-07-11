// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackSource.h"
#include <ao/Exception.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  class TrackSourceLease final
  {
  public:
    explicit TrackSourceLease(std::shared_ptr<TrackSource> sourcePtr)
      : _sourcePtr{std::move(sourcePtr)}
    {
      if (_sourcePtr == nullptr)
      {
        throwException<Exception>("Track source lease requires a source");
      }
    }

    TrackSourceLease(TrackSourceLease const&) = default;
    TrackSourceLease& operator=(TrackSourceLease const&) = default;
    TrackSourceLease(TrackSourceLease&&) noexcept = default;
    TrackSourceLease& operator=(TrackSourceLease&&) noexcept = default;
    ~TrackSourceLease() = default;

    TrackSource& source() const noexcept { return *_sourcePtr; }
    TrackSource& operator*() const noexcept { return *_sourcePtr; }
    TrackSource* operator->() const noexcept { return _sourcePtr.get(); }

  private:
    std::shared_ptr<TrackSource> _sourcePtr;
  };
} // namespace ao::rt
