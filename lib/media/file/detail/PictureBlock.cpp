// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PictureBlock.h"

#include "Decoder.h"
#include <ao/PictureType.h>

#include <boost/endian/detail/order.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::media::file::detail
{
  std::optional<PictureBlock> parsePictureBlock(std::span<std::byte const> payload) noexcept
  {
    // Width, height, colour depth, and indexed-colour count, none of which the
    // library records.
    constexpr std::size_t kImageScalarCount = 4;

    std::size_t offset = 0;
    auto const optRawType = readU32(payload, offset, boost::endian::order::big);

    if (!optRawType || !readSized(payload, offset, boost::endian::order::big) ||
        !readSized(payload, offset, boost::endian::order::big))
    {
      return std::nullopt;
    }

    for (std::size_t index = 0; index < kImageScalarCount; ++index)
    {
      if (!readU32(payload, offset, boost::endian::order::big))
      {
        return std::nullopt;
      }
    }

    auto const optBytes = readSized(payload, offset, boost::endian::order::big);

    if (!optBytes || offset != payload.size())
    {
      return std::nullopt;
    }

    auto const type = *optRawType <= static_cast<std::uint32_t>(PictureType::PublisherLogo)
                        ? static_cast<PictureType>(*optRawType)
                        : PictureType::Other;
    return PictureBlock{.type = type, .bytes = *optBytes};
  }
} // namespace ao::media::file::detail
