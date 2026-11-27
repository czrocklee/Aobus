// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PictureBlock.h"

#include <ao/PictureType.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::media::file::detail
{
  namespace
  {
    std::optional<std::uint32_t> takeU32(std::span<std::byte const>& bytes) noexcept
    {
      auto const* const field = utility::bytes::tryLayout<boost::endian::big_uint32_buf_t>(bytes);

      if (field == nullptr)
      {
        return std::nullopt;
      }

      bytes = bytes.subspan(sizeof(*field));
      return field->value();
    }

    std::optional<std::span<std::byte const>> takeSized(std::span<std::byte const>& bytes) noexcept
    {
      auto const optSize = takeU32(bytes);

      if (!optSize || *optSize > bytes.size())
      {
        return std::nullopt;
      }

      auto const value = bytes.first(*optSize);
      bytes = bytes.subspan(*optSize);
      return value;
    }
  } // namespace

  std::optional<PictureBlock> parsePictureBlock(std::span<std::byte const> payload) noexcept
  {
    // Width, height, colour depth, and indexed-colour count, none of which the
    // library records.
    constexpr std::size_t kImageScalarCount = 4;

    auto const optRawType = takeU32(payload);

    if (!optRawType || !takeSized(payload) || !takeSized(payload))
    {
      return std::nullopt;
    }

    for (std::size_t index = 0; index < kImageScalarCount; ++index)
    {
      if (!takeU32(payload))
      {
        return std::nullopt;
      }
    }

    auto const optBytes = takeSized(payload);

    if (!optBytes || !payload.empty())
    {
      return std::nullopt;
    }

    auto const type = *optRawType <= static_cast<std::uint32_t>(PictureType::PublisherLogo)
                        ? static_cast<PictureType>(*optRawType)
                        : PictureType::Other;
    return PictureBlock{.type = type, .bytes = *optBytes};
  }
} // namespace ao::media::file::detail
