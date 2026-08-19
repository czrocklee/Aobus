// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListRecordValidation.h"

#include "TextAdmission.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ao::library
{
  Result<> validateSerializedList(std::span<std::byte const> const bytes)
  {
    auto const view = ListView{bytes};

    if (!view.isValid())
    {
      return makeError(Error::Code::CorruptData, "List record has a non-canonical structural layout");
    }

    auto const* header = utility::layout::view<ListHeader>(bytes);
    constexpr auto kMaxTextLength = static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max());

    if (header->nameLength > kMaxTextLength || header->descLength > kMaxTextLength ||
        header->filterLength > kMaxTextLength)
    {
      return makeError(Error::Code::CorruptData, "List record contains a text field above the product limit");
    }

    for (auto const& [text, context] : {
           std::pair{view.name(), std::string_view{"List name"}},
           std::pair{view.description(), std::string_view{"List description"}},
         })
    {
      if (auto textRes = detail::validatePersistedLibraryText(text, context); !textRes)
      {
        return std::unexpected{textRes.error()};
      }
    }

    if (auto filterRes = detail::validatePersistedLibraryUtf8(view.filter(), "List filter"); !filterRes)
    {
      return std::unexpected{filterRes.error()};
    }

    auto seen = std::unordered_set<std::uint32_t>{};
    seen.reserve(view.orderTrackIds().size());

    for (auto const trackId : view.orderTrackIds())
    {
      if (trackId == kInvalidTrackId)
      {
        return makeError(Error::Code::CorruptData, "List record contains the reserved Track id zero");
      }

      if (!seen.insert(trackId.raw()).second)
      {
        return makeError(Error::Code::CorruptData, "List record contains a duplicate saved-order Track id");
      }
    }

    return {};
  }
} // namespace ao::library
