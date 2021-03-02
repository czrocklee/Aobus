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

#include "Layout.h"
#include <rs/common/Exception.h>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/locale/encoding.hpp>
#include <string_view>

namespace rs::tag::mpeg::id3v2
{

  template<typename CommonFrameLayout>
  class FrameView
  {
  public:
    FrameView(const void* data, std::size_t availableSize) : _data{data}
    {
      if (availableSize > 0 && (availableSize < sizeof(CommonFrameLayout) || availableSize < size()))
      {
        RS_THROW_FORMAT(common::Exception, "invalid id3v2 tag: frame size {} exceeds tag boundary {}", size(), availableSize);
      }
    }

    const void* data() const { return _data; }

    std::size_t size() const { return contentSize() + sizeof(CommonFrameLayout); }

    std::string_view id() const
    {
      const auto& id = static_cast<const CommonFrameLayout*>(_data)->id;
      return {id.data(), id.size()};
    }

    template<typename Layout>
    const Layout& layout() const
    {
      if (sizeof(Layout) > size())
      {
        RS_THROW_FORMAT(common::Exception, "invalid id3v2 frame, expect layout size {} > frame size {}", sizeof(Layout), size());
      }

      return *static_cast<const Layout*>(_data);
    }
  protected:
    std::size_t contentSize() const { return frameSize(*static_cast<const CommonFrameLayout*>(_data)); }

  private:
    const void* _data;
  };

  using V22FrameView = FrameView<V22CommonFrameLayout>;

  using V23FrameView = FrameView<V23CommonFrameLayout>;

  using V24FrameView = FrameView<V24CommonFrameLayout>;

  template<typename FrameViewLayout>
  class TextFrameView : public FrameView<typename FrameViewLayout::CommonLayout>
  {
    using Base = FrameView<typename FrameViewLayout::CommonLayout>;

  public:
    using Base::Base;

    std::string text() const
    {
      auto begin = static_cast<const char*>(Base::data()) + sizeof(FrameViewLayout);
      auto end = static_cast<const char*>(Base::data()) + Base::size();
      auto encoding = Base::template layout<FrameViewLayout>().encoding;
      return boost::locale::conv::to_utf<char>(begin, end, encoding == Encoding::Latin_1 ? "Latin1" : "UCS-2");
    }
  };

  using V23TextFrameView = TextFrameView<V23TextFrameLayout>;

  template<typename ViewT>
  class FrameViewIterator : public boost::iterator_facade<FrameViewIterator<ViewT>, const ViewT, boost::forward_traversal_tag>
  {
  public:
    FrameViewIterator() : _view{nullptr, 0}, _sizeLeft{0} {}

    FrameViewIterator(const void* data, std::size_t size) : _view{size > 0 ? data : nullptr, size}, _sizeLeft{size} {}

  private:
    friend class boost::iterator_core_access;

    void increment()
    {
      if (_sizeLeft == 0)
      {
        return;
      }

      _sizeLeft -= _view.size();

      if (_sizeLeft == 0)
      {
        _view = ViewT{nullptr, 0};
      }
      
      const char* nextFrame = static_cast<const char*>(_view.data()) + _view.size();
      bool isPadding = (*nextFrame == 0 && std::memcmp(nextFrame, nextFrame + 1, _sizeLeft - 1) == 0);
      _view = isPadding ? ViewT{nullptr, 0} : ViewT{nextFrame, _sizeLeft};
    }

    bool equal(const FrameViewIterator& other) const { return _view.data() == other._view.data(); }

    const ViewT& dereference() const { return _view; }

    ViewT _view;
    std::size_t _sizeLeft;
  };
}