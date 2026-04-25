// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "MetadataBlock.h"
#include <rs/utility/ByteView.h>

namespace rs::tag::flac
{
  std::vector<std::string_view> VorbisCommentBlockView::comments() const
  {
    std::vector<std::string_view> comments;
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
    std::string_view blob = detail::parseString<std::uint32_t>(ptr, end);
    return utility::bytes::view(blob);
  }

  MetadataBlockViewIterator::MetadataBlockViewIterator(void const* data, std::size_t size)
    : _view{data}, _sizeLeft{size}
  {
    if (size < StreamInfoBlockSize || _view.type() != MetadataBlockType::StreamInfo)
    {
      RS_THROW(rs::Exception, "invalid flac metadata blocks, first block must be StreamInfo");
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
      RS_THROW(rs::Exception, "invalid flac metadata blocks size, exceeding the file boundary");
    }
  }
}
