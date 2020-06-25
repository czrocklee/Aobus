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
#include <boost/asio/buffer.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <vector>
#include <cassert>
#include <string_view>

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

    std::vector<std::string_view> comments() const;
  };

  class PictureBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    boost::asio::const_buffer blob() const;
  };

  class MetadataBlockViewIterator
    : public boost::iterator_facade<MetadataBlockViewIterator, const MetadataBlockView, boost::forward_traversal_tag>
  {
  public:
    static constexpr std::size_t StreamInfoBlockSize = 38;

    MetadataBlockViewIterator() : _view{nullptr}, _sizeLeft{0} {}

    MetadataBlockViewIterator(const void* data, std::size_t size);

  private:
    friend class boost::iterator_core_access;

    void increment();

    bool equal(const MetadataBlockViewIterator& other) const { return _view._data == other._view._data; }

    const MetadataBlockView& dereference() const { return _view; }

    MetadataBlockView _view;
    std::size_t _sizeLeft;
  };
}
