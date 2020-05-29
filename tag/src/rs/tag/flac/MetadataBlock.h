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

#pragma once

#include "MetadataBlockLayout.h"
#include <rs/common/Exception.h>

#include <boost/endian/conversion.hpp>
#include <boost/iterator/iterator_facade.hpp>

namespace rs::tag::flac
{
  class MetadataBlock
  {
  public:
    virtual ~MetadataBlock() = default;

    virtual MetadataBlockType type() const = 0;

    virtual std::uint32_t size() const = 0;
  };

  class MetadataBlockView : public MetadataBlock
  {
  public:
    MetadataBlockView(const void* data) : _data{data} {}

    const void* data() const { return _data; }

    std::uint32_t size() const override { return layout<MetadataBlockLayout>().size.value() + sizeof(MetadataBlockLayout); }

    MetadataBlockType type() const override { return layout<MetadataBlockLayout>().type; }

    template<typename Layout>
    const Layout& layout() const
    {
      if (auto size = static_cast<const MetadataBlockLayout*>(_data)->size.value(); typename Layout::FixedSize{})
      {
        assert(size == sizeof(Layout));
      }
      else
      {
        assert(size >= sizeof(Layout));
      }

      return *static_cast<const Layout*>(_data);
    }

  private:
    const void* _data;
    friend class MetadataBlockViewIterator;
  };

  class VorbisCommentBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    std::vector<std::string_view> comments() const
    {
      auto* ptr = static_cast<const char*>(data()) + sizeof(MetadataBlockLayout);
      auto* end = ptr + size() - sizeof(MetadataBlockLayout);
      parseString(ptr, end); // vendor string

      std::uint32_t count = parseLength(ptr, end);
      std::vector<std::string_view> comments;
      comments.reserve(count);

      for (auto i = 0u; i < count; ++i)
      {
        comments.push_back(parseString(ptr, end));
      }

      if (auto sizeLeft = static_cast<std::size_t>(end - ptr); sizeLeft > 0)
      {
        RS_THROW_FORMAT(
          common::Exception, "invalid flac vorbis_comment block, unexpected content \"{}\"", std::string_view{ptr, sizeLeft});
      }

      return comments;
    }

  private:
    static std::uint32_t parseLength(const char*& ptr, const char* end)
    {
      if (ptr + sizeof(std::uint32_t) > end)
      {
        RS_THROW(common::Exception, "invalid flac vorbis_comment block, expect length field size > 4");
      }

      std::uint32_t length = boost::endian::load_little_u32(reinterpret_cast<const unsigned char*>(ptr));
      ptr += sizeof(std::uint32_t);
      return length;
    }

    static std::string_view parseString(const char*& ptr, const char* end)
    {
      std::uint32_t length = parseLength(ptr, end);

      if (ptr + length > end)
      {
        RS_THROW_FORMAT(common::Exception, "invalid flac vorbis_comment block, expect available string length >= {}", length);
      }

      const char* start = ptr;
      ptr += length;
      return {start, length};
    }
  };

  class MetadataBlockViewIterator
    : public boost::iterator_facade<MetadataBlockViewIterator, const MetadataBlockView, boost::forward_traversal_tag>
  {
  public:
    static constexpr std::size_t StreamInfoBlockSize = 38;

    MetadataBlockViewIterator() : _view{nullptr}, _sizeLeft{0} {}

    MetadataBlockViewIterator(const void* data, std::size_t size) : _view{data}, _sizeLeft{size}
    {
      if (size < StreamInfoBlockSize || _view.type() != MetadataBlockType::StreamInfo)
      {
        RS_THROW(common::Exception, "invalid flac metadata blocks, first block must be StreamInfo");
      }
    }

  private:
    friend class boost::iterator_core_access;

    void increment()
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

    bool equal(const MetadataBlockViewIterator& other) const { return _view._data == other._view._data; }

    const MetadataBlockView& dereference() const { return _view; }

    MetadataBlockView _view;
    std::size_t _sizeLeft;
  };
}