// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>

#include <cstddef>
#include <vector>

namespace ao::rt::test
{
  // Owns the serialized payload so the ListView span stays valid.
  class ListViewOwner final
  {
  public:
    explicit ListViewOwner(std::vector<TrackId> const& ids)
      : _payload{buildPayload(ids)}, _view{_payload}
    {
    }

    library::ListView const& view() const { return _view; }

  private:
    static std::vector<std::byte> buildPayload(std::vector<TrackId> const& ids)
    {
      auto builder = library::ListBuilder::createNew();

      for (auto id : ids)
      {
        builder.tracks().add(id);
      }

      return builder.serialize();
    }

    std::vector<std::byte> _payload;
    library::ListView _view;
  };
} // namespace ao::rt::test
