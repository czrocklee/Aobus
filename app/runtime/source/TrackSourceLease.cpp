// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/source/TrackSourceLease.h>

#include <ao/rt/source/TrackSource.h>

#include <gsl-lite/gsl-lite.hpp>

#include <memory>
#include <utility>

namespace ao::rt
{
  TrackSourceLease::TrackSourceLease(std::shared_ptr<TrackSource> sourcePtr)
    : _sourcePtr{std::move(sourcePtr)}
  {
    gsl_Expects(_sourcePtr != nullptr && "Track source lease requires a source");
  }
} // namespace ao::rt
