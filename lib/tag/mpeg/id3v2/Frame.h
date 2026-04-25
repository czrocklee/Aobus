// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "Layout.h"
#include <cstring>
#include <rs/Exception.h>
#include <string>
#include <string_view>

namespace rs::tag::mpeg::id3v2
{
  inline std::string convertToUtf8(char const* begin, char const* end, Encoding encoding)
  {
    if (begin >= end)
    {
      return {};
    }

    auto const size = static_cast<std::size_t>(end - begin);

    if (encoding == Encoding::Latin_1)
    {
      std::string result;
      result.reserve(size);
      
      for (auto const* it = begin; it != end; ++it)
      {
        auto const c = static_cast<unsigned char>(*it);
        
        if (c < 0x80)
        {
          result.push_back(static_cast<char>(c));
        }
        else
        {
          result.push_back(static_cast<char>(0xC0 | (c >> 6)));
          result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
      }
      
      return result;
    }

    if (encoding == Encoding::UCS_2)
    {
      if (size < 2)
      {
        return {};
      }

      auto const* u16_begin = reinterpret_cast<std::uint8_t const*>(begin);
      auto const* u16_end = reinterpret_cast<std::uint8_t const*>(end);

      bool big_endian = true;
      
      if (u16_begin[0] == 0xFF && u16_begin[1] == 0xFE)
      {
        big_endian = false;
        u16_begin += 2;
      }
      else if (u16_begin[0] == 0xFE && u16_begin[1] == 0xFF)
      {
        big_endian = true;
        u16_begin += 2;
      }

      std::string result;
      result.reserve(size); // Heuristic

      for (auto const* it = u16_begin; it + 1 < u16_end; it += 2)
      {
        std::uint16_t cp = big_endian ? (static_cast<std::uint16_t>(it[0]) << 8) | it[1]
                                      : (static_cast<std::uint16_t>(it[1]) << 8) | it[0];

        if (cp < 0x80)
        {
          result.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
          result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
          result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
      }
      
      return result;
    }

    return {};
  }

  template<typename CommonFrameLayout>
  class FrameView
  {
  public:
    FrameView(void const* data, std::size_t availableSize)
      : _data{data}
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
      std::string result = convertToUtf8(begin, end, encoding);
      
      while (!result.empty() && result.back() == '\0')
      {
        result.pop_back();
      }
      
      return result;
    }
  };

  using V23TextFrameView = TextFrameView<V23TextFrameLayout>;

  template<typename ViewT>
  class FrameViewIterator
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;
    using value_type = ViewT const;
    using pointer = ViewT const*;
    using reference = ViewT const&;
    using iterator_category = std::forward_iterator_tag;

    FrameViewIterator()
      : _view{nullptr, 0}, _sizeLeft{0}
    {
    }

    FrameViewIterator(void const* data, std::size_t size)
      : _view{size > 0 ? data : nullptr, size}, _sizeLeft{size}
    {
    }

    // Forward iterator operations
    reference operator*() const { return _view; }

    pointer operator->() const { return &_view; }

    FrameViewIterator& operator++()
    {
      increment();
      return *this;
    }

    FrameViewIterator operator++(int)
    {
      auto tmp = *this;
      increment();
      return tmp;
    }

    bool operator==(FrameViewIterator const& other) const { return _view.data() == other._view.data(); }

    bool operator!=(FrameViewIterator const& other) const { return !(*this == other); }

  private:
    void increment()
    {
      if (_sizeLeft == 0)
      {
        return;
      }

      auto const lastSize = _view.size();
      
      if (_sizeLeft <= lastSize)
      {
        _sizeLeft = 0;
        _view = ViewT{nullptr, 0};
        return;
      }

      _sizeLeft -= lastSize;
      char const* nextFrame = static_cast<char const*>(_view.data()) + lastSize;

      // Check for padding (zeros at the end of the tag)
      bool isPadding = (*nextFrame == 0);
      
      if (isPadding && _sizeLeft > 1)
      {
        isPadding = (std::memcmp(nextFrame, nextFrame + 1, _sizeLeft - 1) == 0);
      }

      _view = isPadding ? ViewT{nullptr, 0} : ViewT{nextFrame, _sizeLeft};
      
      if (isPadding)
      {
        _sizeLeft = 0;
      }
    }

    ViewT _view;
    std::size_t _sizeLeft;
  };
}