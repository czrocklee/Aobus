// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "Layout.h"
#include <ao/Exception.h>
#include <cstring>
#include <string>
#include <string_view>

namespace ao::tag::mpeg::id3v2
{
  inline std::string convertToUtf8(char const* begin, char const* end, Encoding encoding)
  {
    if (begin >= end)
    {
      return {};
    }

    auto const size = static_cast<std::size_t>(end - begin);

    if (encoding == Encoding::Latin1)
    {
      static constexpr std::uint8_t kAsciiLimit = 0x80;
      static constexpr std::uint8_t kUtf8TwoByteHeader = 0xC0;
      static constexpr std::uint8_t kUtf8ContinuationHeader = 0x80;
      static constexpr std::uint8_t kUtf8ContinuationMask = 0x3F;
      static constexpr std::size_t kUtf8Shift6 = 6;

      auto result = std::string{};
      result.reserve(size);

      for (auto const* it = begin; it != end; ++it)
      {
        auto const ch = static_cast<unsigned char>(*it);

        if (ch < kAsciiLimit)
        {
          result.push_back(static_cast<char>(ch));
        }
        else
        {
          result.push_back(static_cast<char>(kUtf8TwoByteHeader | (ch >> kUtf8Shift6)));
          result.push_back(static_cast<char>(kUtf8ContinuationHeader | (ch & kUtf8ContinuationMask)));
        }
      }

      return result;
    }

    if (encoding == Encoding::Ucs2)
    {
      static constexpr std::size_t kMinUcs2Size = 2;

      if (size < kMinUcs2Size)
      {
        return {};
      }

      auto const* u16_begin = reinterpret_cast<std::uint8_t const*>(begin); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      auto const* u16_end = reinterpret_cast<std::uint8_t const*>(end);     // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

      bool big_endian = true;
      static constexpr std::uint8_t kBomByte1Le = 0xFF;
      static constexpr std::uint8_t kBomByte2Le = 0xFE;
      static constexpr std::uint8_t kBomByte1Be = 0xFE;
      static constexpr std::uint8_t kBomByte2Be = 0xFF;

      if (u16_begin[0] == kBomByte1Le && u16_begin[1] == kBomByte2Le)
      {
        big_endian = false;
        u16_begin += 2;
      }
      else if (u16_begin[0] == kBomByte1Be && u16_begin[1] == kBomByte2Be)
      {
        big_endian = true;
        u16_begin += 2;
      }

      static constexpr std::uint16_t kUcs2AsciiLimit = 0x80;
      static constexpr std::uint16_t kUcs2TwoByteLimit = 0x800;
      static constexpr std::uint8_t kUtf8ThreeByteHeader = 0xE0;
      static constexpr std::uint8_t kUtf8TwoByteHeader = 0xC0;
      static constexpr std::uint8_t kUtf8ContinuationHeader = 0x80;
      static constexpr std::uint8_t kUtf8ContinuationMask = 0x3F;
      static constexpr std::size_t kUtf8Shift12 = 12;
      static constexpr std::size_t kUtf8Shift6 = 6;

      auto result = std::string{};
      result.reserve(size); // Heuristic

      for (auto const* it = u16_begin; it + 1 < u16_end; it += 2)
      {
        if (std::uint16_t const cp = big_endian ? (static_cast<std::uint16_t>(it[0]) << 8) | it[1]
                                                : (static_cast<std::uint16_t>(it[1]) << 8) | it[0];
            cp < kUcs2AsciiLimit)
        {
          result.push_back(static_cast<char>(cp));
        }
        else if (cp < kUcs2TwoByteLimit)
        {
          result.push_back(static_cast<char>(kUtf8TwoByteHeader | (cp >> kUtf8Shift6)));
          result.push_back(static_cast<char>(kUtf8ContinuationHeader | (cp & kUtf8ContinuationMask)));
        }
        else
        {
          result.push_back(static_cast<char>(kUtf8ThreeByteHeader | (cp >> kUtf8Shift12)));
          result.push_back(static_cast<char>(kUtf8ContinuationHeader | ((cp >> kUtf8Shift6) & kUtf8ContinuationMask)));
          result.push_back(static_cast<char>(kUtf8ContinuationHeader | (cp & kUtf8ContinuationMask)));
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
        ao::throwException<Exception>("invalid id3v2 tag: frame size {} exceeds tag boundary {}", size(), availableSize);
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
        ao::throwException<Exception>(
          "invalid id3v2 frame, expect layout size {} > frame size {}", sizeof(Layout), size());
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
      const auto* begin = static_cast<char const*>(Base::data()) + sizeof(FrameViewLayout);
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