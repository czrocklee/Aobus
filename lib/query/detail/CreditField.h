// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/library/TrackLayout.h>
#include <ao/query/Field.h>

#include <span>

namespace ao::library
{
  class TrackView;
}

namespace ao::query::detail
{
  // Returns the selected physical names/roles without materializing logical credits.
  std::span<library::TrackCreditEntry const> creditFieldEntries(library::TrackView const& track, Field field);
} // namespace ao::query::detail
