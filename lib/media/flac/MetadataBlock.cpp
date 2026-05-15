// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/media/flac/MetadataBlock.h>
#include <ao/Exception.h>
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

  std::span<std::byte const> PictureBlockView::blob() const
  {
    // Number of 32-bit fields for picture metadata (width, height, depth, colors)
    constexpr std::size_t kPictureMetaFieldCount = 4;

    char const* ptr = static_cast<char const*>(data()) + sizeof(MetadataBlockLayout);
    char const* end = ptr + size() - sizeof(MetadataBlockLayout);
    ptr += 4;                                              // picture type
    detail::parseString<std::uint32_t>(ptr, end);          // MIME type
    detail::parseString<std::uint32_t>(ptr, end);          // description
    ptr += kPictureMetaFieldCount * sizeof(std::uint32_t); // width/height/color depth/color count
    std::string_view const blob = detail::parseString<std::uint32_t>(ptr, end);
    return utility::bytes::view(blob);
  }

  MetadataBlockViewIterator::MetadataBlockViewIterator(void const* data, std::size_t size)
    : _view{data}, _sizeLeft{size}
  {
    if (size < kStreamInfoBlockSize || _view.type() != MetadataBlockType::StreamInfo)
    {
      ao::throwException<Exception>("invalid flac metadata blocks, first block must be StreamInfo");
    }
  }

  void MetadataBlockViewIterator::increment()
  {
    if (_view.layout<MetadataBlockLayout>().isLastBlock)
    {
      _view = MetadataBlockView{nullptr};
      return;
    }

    _sizeLeft -= _view.size();
    _view = MetadataBlockView{static_cast<char const*>(_view.data()) + _view.size()};

    if (_view.size() > _sizeLeft)
    {
      ao::throwException<Exception>("invalid flac metadata blocks size, exceeding the file boundary");
    }
  }
} // namespace ao::media::flac
