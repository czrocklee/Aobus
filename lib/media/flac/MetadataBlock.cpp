// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/media/detail/MediaError.h>
#include <ao/media/flac/MetadataBlock.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::flac
{
  std::vector<std::string_view> VorbisCommentBlockView::comments() const
  {
    auto comments = std::vector<std::string_view>{};
    visitComments([&comments](std::string_view comment) { comments.push_back(comment); });
    return comments;
  }

  namespace
  {
    char const* picturePayloadStart(void const* blockData)
    {
      return static_cast<char const*>(blockData) + sizeof(MetadataBlockLayout);
    }
  }

  std::uint32_t PictureBlockView::pictureType() const
  {
    char const* ptr = picturePayloadStart(data());
    char const* const end = ptr + size() - sizeof(MetadataBlockLayout);
    return detail::parseLength<std::uint32_t>(ptr, end);
  }

  std::span<std::byte const> PictureBlockView::blob() const
  {
    // Number of 32-bit fields for picture metadata (width, height, depth, colors)
    constexpr std::size_t kPictureMetaFieldCount = 4;

    char const* ptr = picturePayloadStart(data());
    char const* end = ptr + size() - sizeof(MetadataBlockLayout);
    detail::parseLength<std::uint32_t>(ptr, end); // picture type
    detail::parseString<std::uint32_t>(ptr, end); // MIME type
    detail::parseString<std::uint32_t>(ptr, end); // description

    for (std::size_t i = 0; i < kPictureMetaFieldCount; ++i)
    {
      detail::parseLength<std::uint32_t>(ptr, end); // width/height/color depth/color count
    }

    std::string_view const blob = detail::parseString<std::uint32_t>(ptr, end);
    return utility::bytes::view(blob);
  }

  MetadataBlockViewIterator::MetadataBlockViewIterator(void const* data, std::size_t size)
    : _view{data}, _sizeLeft{size}
  {
    if (size < kStreamInfoBlockSize || _view.type() != MetadataBlockType::StreamInfo)
    {
      ao::media::detail::throwMediaError(
        Error::Code::CorruptData, "invalid flac metadata blocks, first block must be StreamInfo");
    }
  }

  void MetadataBlockViewIterator::increment()
  {
    auto const currentSize = _view.size();

    // The current block must fit within the remaining bytes; otherwise a
    // truncated last block would be accepted before advancing.
    if (currentSize > _sizeLeft)
    {
      ao::media::detail::throwMediaError(
        Error::Code::CorruptData, "invalid flac metadata blocks size, exceeding the file boundary");
    }

    if (_view.layout<MetadataBlockLayout>().isLastBlock)
    {
      _view = MetadataBlockView{nullptr};
      return;
    }

    _sizeLeft -= currentSize;

    // The next block header must be fully readable before we interpret it.
    if (_sizeLeft < sizeof(MetadataBlockLayout))
    {
      ao::media::detail::throwMediaError(
        Error::Code::CorruptData, "invalid flac metadata blocks size, exceeding the file boundary");
    }

    _view = MetadataBlockView{static_cast<char const*>(_view.data()) + currentSize};

    if (_view.size() > _sizeLeft)
    {
      ao::media::detail::throwMediaError(
        Error::Code::CorruptData, "invalid flac metadata blocks size, exceeding the file boundary");
    }
  }
} // namespace ao::media::flac
