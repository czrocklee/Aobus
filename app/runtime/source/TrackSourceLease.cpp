// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/source/TrackSourceLease.h>

#include <ao/Exception.h>
#include <ao/rt/source/TrackSource.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  TrackSourceLease::TrackSourceLease(std::shared_ptr<TrackSource> sourcePtr)
    : _sourcePtr{std::move(sourcePtr)}
  {
    if (_sourcePtr == nullptr)
    {
      throwException<Exception>("Track source lease requires a source");
    }
  }
} // namespace ao::rt
