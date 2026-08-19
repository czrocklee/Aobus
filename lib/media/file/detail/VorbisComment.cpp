// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "VorbisComment.h"

#include "Content.h"
#include "Decoder.h"
#include <ao/utility/ByteView.h>

#include <boost/endian/detail/order.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::file::detail
{
  namespace
  {
    using TextSetter = ContentBuilder::MetadataBuilder& (ContentBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter = ContentBuilder::MetadataBuilder& (ContentBuilder::MetadataBuilder::*)(std::uint16_t);

    template<TextSetter Setter>
    void handleText(ContentBuilder& builder, std::string_view value)
    {
      (builder.metadata().*Setter)(value);
    }

    [[maybe_unused]] void handleEnsembleFallback(ContentBuilder& builder, std::string_view value)
    {
      if (builder.metadata().ensemble().empty())
      {
        builder.metadata().ensemble(value);
      }
    }

    [[maybe_unused]] void handleSoloistFallback(ContentBuilder& builder, std::string_view value)
    {
      if (builder.metadata().soloist().empty())
      {
        builder.metadata().soloist(value);
      }
    }

    template<NumberSetter Setter>
    void handleNumber(ContentBuilder& builder, std::string_view value)
    {
      if (auto const optParsed = decodeUint16(value); optParsed)
      {
        (builder.metadata().*Setter)(*optParsed);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(ContentBuilder& builder, std::string_view value)
    {
      auto const pair = parseSlashPair(value);

      if (pair.optPrimary)
      {
        (builder.metadata().*PrimarySetter)(*pair.optPrimary);
      }

      if (pair.optSecondary)
      {
        (builder.metadata().*SecondarySetter)(*pair.optSecondary);
      }
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "media/file/detail/VorbisCommentDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
  } // namespace

  std::optional<std::vector<std::string_view>> parseVorbisComments(std::span<std::byte const> payload,
                                                                   VorbisCommentTrailing trailing)
  {
    std::size_t offset = 0;

    if (!readSized(payload, offset, boost::endian::order::little))
    {
      return std::nullopt;
    }

    auto const optCount = readU32(payload, offset, boost::endian::order::little);

    if (!optCount)
    {
      return std::nullopt;
    }

    auto comments = std::vector<std::string_view>{};

    for (std::uint32_t index = 0; index < *optCount; ++index)
    {
      auto const optComment = readSized(payload, offset, boost::endian::order::little);

      if (!optComment)
      {
        return std::nullopt;
      }

      comments.push_back(utility::bytes::stringView(*optComment));
    }

    if (trailing == VorbisCommentTrailing::Rejected && offset != payload.size())
    {
      return std::nullopt;
    }

    return comments;
  }

  std::optional<VorbisCommentField> splitVorbisComment(std::string_view comment)
  {
    auto const equalsOffset = comment.find('=');

    if (equalsOffset == std::string_view::npos)
    {
      return std::nullopt;
    }

    return VorbisCommentField{.key = comment.substr(0, equalsOffset), .value = comment.substr(equalsOffset + 1)};
  }

  bool applyVorbisComment(ContentBuilder& builder, VorbisCommentField field)
  {
    auto const* const entry = VorbisCommentDispatchTable::lookupVorbisField(field.key.data(), field.key.size());

    if (entry == nullptr)
    {
      return false;
    }

    entry->handler(builder, field.value);
    return true;
  }
} // namespace ao::media::file::detail
