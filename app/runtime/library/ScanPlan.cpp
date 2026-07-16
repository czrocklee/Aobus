// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/library/ScanPlan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    ScanItem const* findItem(std::vector<ScanItem> const& items,
                             ScanClassification classification,
                             std::string_view uri)
    {
      auto const iter = std::ranges::find_if(items,
                                             [classification, uri](ScanItem const& item)
                                             { return item.classification == classification && item.uri == uri; });
      return iter == items.end() ? nullptr : &*iter;
    }
  } // namespace

  ScanPlan::ScanPlan(std::array<std::byte, 16> libraryId,
                     std::uint64_t const libraryRevision,
                     std::vector<ScanItem> items) noexcept
    : _libraryId{libraryId}, _libraryRevision{libraryRevision}, _items{std::move(items)}
  {
  }

  ScanPlan::ScanPlan(ScanPlan&& other) noexcept
    : _libraryId{other._libraryId}
    , _libraryRevision{other._libraryRevision}
    , _items{std::move(other._items)}
    , _executable{std::exchange(other._executable, false)}
  {
    other._libraryId = {};
    other._libraryRevision = 0;
  }

  ScanPlan& ScanPlan::operator=(ScanPlan&& other) noexcept
  {
    if (this != &other)
    {
      _libraryId = other._libraryId;
      _libraryRevision = other._libraryRevision;
      _items = std::move(other._items);
      _executable = std::exchange(other._executable, false);
      other._libraryId = {};
      other._libraryRevision = 0;
    }

    return *this;
  }

  Result<ScanPlan> ScanPlan::makeRelinkPlan(std::string_view oldUri, std::string_view newUri) &&
  {
    if (!_executable)
    {
      return makeError(Error::Code::InvalidState, "Scan plan has already been consumed");
    }

    auto const* const missingItem = findItem(_items, ScanClassification::Missing, oldUri);

    if (missingItem == nullptr || missingItem->trackId == kInvalidTrackId || missingItem->uri.empty())
    {
      return makeError(Error::Code::InvalidInput, "Relink source is not an unresolved missing item");
    }

    auto const* const newItem = findItem(_items, ScanClassification::New, newUri);

    if (newItem == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "Relink destination is not an unresolved new item");
    }

    if (!hasAudioIdentity(*missingItem) || !hasAudioIdentity(*newItem) ||
        missingItem->audioPayloadLength != newItem->audioPayloadLength ||
        missingItem->audioSignature != newItem->audioSignature)
    {
      return makeError(Error::Code::InvalidInput, "Relink source and destination audio identities do not match");
    }

    auto movedItem = *newItem;
    movedItem.classification = ScanClassification::Moved;
    movedItem.oldUri = missingItem->uri;
    movedItem.trackId = missingItem->trackId;

    auto items = std::vector<ScanItem>{};
    items.reserve(1);
    items.push_back(std::move(movedItem));
    auto const libraryId = _libraryId;
    auto const libraryRevision = _libraryRevision;
    _libraryId = {};
    _libraryRevision = 0;
    _items.clear();
    _executable = false;
    return ScanPlan{libraryId, libraryRevision, std::move(items)};
  }
} // namespace ao::rt
