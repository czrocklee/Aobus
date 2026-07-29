// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/ListOrderSourceTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>

#include <cstddef>
#include <vector>

namespace ao::rt::test
{
  std::vector<std::byte> ListViewOwner::buildPayload(std::vector<TrackId> const& ids)
  {
    auto builder = library::ListBuilder::makeEmpty();

    for (auto id : ids)
    {
      builder.orderTrackIds().add(id);
    }

    return ao::test::requireValue(builder.serialize());
  }
} // namespace ao::rt::test
