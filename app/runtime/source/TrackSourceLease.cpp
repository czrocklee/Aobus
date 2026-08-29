// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/source/TrackSourceLease.h>

#include <ao/Contract.h>
#include <ao/rt/source/TrackSource.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  TrackSourceLease::TrackSourceLease(std::shared_ptr<TrackSource const> sourcePtr)
    : _sourcePtr{std::move(sourcePtr)}
  {
    AO_EXPECTS(_sourcePtr != nullptr, "Track source lease requires a source");
  }
} // namespace ao::rt
