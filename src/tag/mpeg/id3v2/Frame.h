// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "Layout.h"
#include <boost/iterator/iterator_facade.hpp>
#include <boost/locale/encoding.hpp>
#include <rs/Exception.h>
#include <string_view>

namespace rs::tag::mpeg::id3v2
{

  template<typename CommonFrameLayout>
  class FrameView
  {
  public:
    FrameView(void const* data, std::size_t availableSize) : _data{data}
    {
      if (availableSize > 0 && (availableSize < sizeof(CommonFrameLayout) || availableSize < size()))
      {
        RS_THROW_FORMAT(
          rs::Exception, "invalid id3v2 tag: frame size {} exceeds tag boundary {}", size(), availableSize);
      }
    }

    void const* data() const { return _data; }

    std::size_t size() const { return contentSize() + sizeof(CommonFrameLayout); }

    std::string_view id() const
    {
      auto const& id = static_cast<CommonFrameLayout const*>(_data)->id;
      return {id.data(), id.size()};
    }

    template<typename Layout>
    Layout const& layout() const
    {
      if (sizeof(Layout) > size())
      {
        RS_THROW_FORMAT(
          rs::Exception, "invalid id3v2 frame, expect layout size {} > frame size {}", sizeof(Layout), size());
      }

      return *static_cast<Layout const*>(_data);
    }

  protected:
    std::size_t contentSize() const { return frameSize(*static_cast<CommonFrameLayout const*>(_data)); }

  private:
    void const* _data;
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
      auto begin = static_cast<char const*>(Base::data()) + sizeof(FrameViewLayout);
      auto end = static_cast<char const*>(Base::data()) + Base::size();
      auto encoding = Base::template layout<FrameViewLayout>().encoding;
      return boost::locale::conv::to_utf<char>(begin, end, encoding == Encoding::Latin_1 ? "Latin1" : "UCS-2");
    }
  };

  using V23TextFrameView = TextFrameView<V23TextFrameLayout>;

  template<typename ViewT>
  class FrameViewIterator
    : public boost::iterator_facade<FrameViewIterator<ViewT>, ViewT const, boost::forward_traversal_tag>
  {
  public:
    FrameViewIterator() : _view{nullptr, 0}, _sizeLeft{0} {}

    FrameViewIterator(void const* data, std::size_t size) : _view{size > 0 ? data : nullptr, size}, _sizeLeft{size} {}

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

      char const* nextFrame = static_cast<char const*>(_view.data()) + _view.size();
      bool isPadding = (*nextFrame == 0 && std::memcmp(nextFrame, nextFrame + 1, _sizeLeft - 1) == 0);
      _view = isPadding ? ViewT{nullptr, 0} : ViewT{nextFrame, _sizeLeft};
    }

    bool equal(FrameViewIterator const& other) const { return _view.data() == other._view.data(); }

    ViewT const& dereference() const { return _view; }

    ViewT _view;
    std::size_t _sizeLeft;
  };
}