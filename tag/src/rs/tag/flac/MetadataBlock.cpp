/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MetadataBlock.h"
#include <rs/common/Exception.h>
#include <boost/endian/conversion.hpp>
#include <string_view>

namespace rs::tag::flac
{
  namespace
  {
    template<typename LengthType, boost::endian::order Order = boost::endian::order::big>
    static LengthType parseLength(const char*& ptr, const char* end)
    {
      if (ptr + sizeof(LengthType) > end)
      {
        RS_THROW_FORMAT(
          common::Exception, "invalid flac block, expect length field size {} >= {}", end - ptr, sizeof(LengthType));
      }

      LengthType length;
      std::memcpy(&length, ptr, sizeof(LengthType));
      boost::endian::conditional_reverse_inplace<Order, boost::endian::order::native>(length);
      ptr += sizeof(LengthType);
      return length;
    }

    template<typename LengthType, boost::endian::order Order = boost::endian::order::big>
    static std::string_view parseString(const char*& ptr, const char* end)
    {
      LengthType length = parseLength<LengthType, Order>(ptr, end);

      if (ptr + length > end)
      {
        RS_THROW_FORMAT(common::Exception, "invalid flac block, expect available field length {} >= {}", end - ptr, length);
      }

      const char* start = ptr;
      ptr += length;
      return {start, length};
    }
  }

  std::vector<std::string_view> VorbisCommentBlockView::comments() const
  {
    auto ptr = static_cast<const char*>(data()) + sizeof(MetadataBlockLayout);
    auto end = ptr + size() - sizeof(MetadataBlockLayout);
    parseString<std::uint32_t, boost::endian::order::little>(ptr, end); // vendor string

    std::uint32_t count = parseLength<std::uint32_t, boost::endian::order::little>(ptr, end);
    std::vector<std::string_view> comments;
    comments.reserve(count);

    for (auto i = 0u; i < count; ++i)
    {
      comments.push_back(parseString<std::uint32_t, boost::endian::order::little>(ptr, end));
    }

    if (auto sizeLeft = static_cast<std::size_t>(end - ptr); sizeLeft > 0)
    {
      RS_THROW_FORMAT(
        common::Exception, "invalid flac vorbis_comment block, unexpected content \"{}\"", std::string_view{ptr, sizeLeft});
    }

    return comments;
  }

  boost::asio::const_buffer PictureBlockView::blob() const
  {
    auto ptr = static_cast<const char*>(data()) + sizeof(MetadataBlockLayout);
    auto end = ptr + size() - sizeof(MetadataBlockLayout);
    ptr += 4;                             // picture type
    parseString<std::uint32_t>(ptr, end); // MIME type
    parseString<std::uint32_t>(ptr, end); // description
    ptr += 16;                            // width/height/color depth/color count
    std::string_view blob = parseString<std::uint32_t>(ptr, end);
    return boost::asio::buffer(blob.data(), blob.size());
  }

  MetadataBlockViewIterator::MetadataBlockViewIterator(const void* data, std::size_t size) : _view{data}, _sizeLeft{size}
  {
    if (size < StreamInfoBlockSize || _view.type() != MetadataBlockType::StreamInfo)
    {
      RS_THROW(common::Exception, "invalid flac metadata blocks, first block must be StreamInfo");
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
    _view = MetadataBlockView{static_cast<const char*>(_view.data()) + _view.size()};

    if (_view.size() > _sizeLeft)
    {
      RS_THROW(common::Exception, "invalid flac metadata blocks size, exceeding the file boundary");
    }
  }
}