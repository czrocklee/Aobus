// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "Layout.h"
#include <ao/Error.h>
#include <ao/tag/detail/TagError.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace ao::tag::mpeg::id3v2
{
  namespace text
  {
    static constexpr std::uint8_t kUtf8TwoByteHeader = 0xC0;
    static constexpr std::uint8_t kUtf8ThreeByteHeader = 0xE0;
    static constexpr std::uint8_t kUtf8FourByteHeader = 0xF0;
    static constexpr std::uint8_t kUtf8ContinuationHeader = 0x80;
    static constexpr std::uint8_t kUtf8ContinuationMask = 0x3F;
    static constexpr std::size_t kUtf8Shift18 = 18;
    static constexpr std::size_t kUtf8Shift12 = 12;
    static constexpr std::size_t kUtf8Shift6 = 6;
    static constexpr std::uint32_t kUtf16HighSurrogateBegin = 0xD800;
    static constexpr std::uint32_t kUtf16HighSurrogateEnd = 0xDBFF;
    static constexpr std::uint32_t kUtf16LowSurrogateBegin = 0xDC00;
    static constexpr std::uint32_t kUtf16LowSurrogateEnd = 0xDFFF;
    static constexpr std::uint32_t kUtf16SupplementaryBase = 0x10000;
    static constexpr std::uint32_t kUtf8ThreeByteLimit = 0x10000;
    static constexpr std::uint32_t kUtf8MaxCodePoint = 0x10FFFF;
    static constexpr std::uint32_t kReplacementCodePoint = 0xFFFD;

    inline bool isHighSurrogate(std::uint32_t cp) noexcept
    {
      return cp >= kUtf16HighSurrogateBegin && cp <= kUtf16HighSurrogateEnd;
    }

    inline bool isLowSurrogate(std::uint32_t cp) noexcept
    {
      return cp >= kUtf16LowSurrogateBegin && cp <= kUtf16LowSurrogateEnd;
    }

    inline void appendUtf8(std::string& out, std::uint32_t cp)
    {
      static constexpr std::uint32_t kAsciiLimit = 0x80;
      static constexpr std::uint32_t kTwoByteLimit = 0x800;

      if (isHighSurrogate(cp) || isLowSurrogate(cp) || cp > kUtf8MaxCodePoint)
      {
        cp = kReplacementCodePoint;
      }

      if (cp < kAsciiLimit)
      {
        out.push_back(static_cast<char>(cp));
      }
      else if (cp < kTwoByteLimit)
      {
        out.push_back(static_cast<char>(kUtf8TwoByteHeader | (cp >> kUtf8Shift6)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | (cp & kUtf8ContinuationMask)));
      }
      else if (cp < kUtf8ThreeByteLimit)
      {
        out.push_back(static_cast<char>(kUtf8ThreeByteHeader | (cp >> kUtf8Shift12)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | ((cp >> kUtf8Shift6) & kUtf8ContinuationMask)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | (cp & kUtf8ContinuationMask)));
      }
      else
      {
        out.push_back(static_cast<char>(kUtf8FourByteHeader | (cp >> kUtf8Shift18)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | ((cp >> kUtf8Shift12) & kUtf8ContinuationMask)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | ((cp >> kUtf8Shift6) & kUtf8ContinuationMask)));
        out.push_back(static_cast<char>(kUtf8ContinuationHeader | (cp & kUtf8ContinuationMask)));
      }
    }

    inline std::string latin1ToUtf8(std::span<std::byte const> buf)
    {
      auto result = std::string{};
      result.reserve(buf.size());

      for (auto const byte : buf)
      {
        appendUtf8(result, std::to_integer<std::uint8_t>(byte));
      }

      return result;
    }

    // ID3v2.4 UTF-8 text is already in the target encoding; copy it through,
    // dropping an optional leading BOM (EF BB BF).
    inline std::string utf8PassThrough(std::span<std::byte const> buf)
    {
      static constexpr std::size_t kBomSize = 3;
      static constexpr std::uint8_t kBom0 = 0xEF;
      static constexpr std::uint8_t kBom1 = 0xBB;
      static constexpr std::uint8_t kBom2 = 0xBF;

      auto const hasBom = buf.size() >= kBomSize && std::to_integer<std::uint8_t>(buf[0]) == kBom0 &&
                          std::to_integer<std::uint8_t>(buf[1]) == kBom1 &&
                          std::to_integer<std::uint8_t>(buf[2]) == kBom2;

      auto const* const start = hasBom ? buf.data() + kBomSize : buf.data();
      auto const size = hasBom ? buf.size() - kBomSize : buf.size();

      auto result = std::string{};
      result.reserve(size);

      for (auto const byte : std::span<std::byte const>{start, size})
      {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
      }

      return result;
    }

    // UCS-2 (ID3v2.3) and UTF-16BE (ID3v2.4) share this decoder: it is BOM-aware
    // and defaults to big-endian when no BOM is present.
    inline std::string utf16ToUtf8(std::span<std::byte const> buf)
    {
      static constexpr std::size_t kMinSize = 2;

      if (buf.size() < kMinSize)
      {
        return {};
      }

      auto const u16Span = utility::layout::viewArray<std::uint8_t>(buf);
      auto const* u16Begin = u16Span.data();
      auto const* const u16End = u16Begin + u16Span.size();

      static constexpr std::uint8_t kBomByte1Le = 0xFF;
      static constexpr std::uint8_t kBomByte2Le = 0xFE;
      static constexpr std::uint8_t kBomByte1Be = 0xFE;
      static constexpr std::uint8_t kBomByte2Be = 0xFF;

      bool bigEndian = true;

      if (u16Begin[0] == kBomByte1Le && u16Begin[1] == kBomByte2Le)
      {
        bigEndian = false;
        u16Begin += 2;
      }
      else if (u16Begin[0] == kBomByte1Be && u16Begin[1] == kBomByte2Be)
      {
        u16Begin += 2;
      }

      auto result = std::string{};
      result.reserve(buf.size()); // Heuristic

      auto readCodeUnit = [bigEndian](std::uint8_t const* it) noexcept
      {
        return bigEndian ? static_cast<std::uint16_t>((static_cast<std::uint16_t>(it[0]) << 8) | it[1])
                         : static_cast<std::uint16_t>((static_cast<std::uint16_t>(it[1]) << 8) | it[0]);
      };

      for (auto const* it = u16Begin; it + 1 < u16End; it += 2)
      {
        auto const cp = readCodeUnit(it);

        if (isHighSurrogate(cp))
        {
          if (auto const* const next = it + 2; next + 1 < u16End)
          {
            if (auto const low = readCodeUnit(next); isLowSurrogate(low))
            {
              auto const codePoint =
                kUtf16SupplementaryBase + ((cp - kUtf16HighSurrogateBegin) << 10) + (low - kUtf16LowSurrogateBegin);
              appendUtf8(result, codePoint);
              it = next;
              continue;
            }
          }

          appendUtf8(result, kReplacementCodePoint);
          continue;
        }

        appendUtf8(result, cp);
      }

      return result;
    }
  } // namespace text

  inline std::string convertToUtf8(std::span<std::byte const> buf, Encoding encoding)
  {
    if (buf.empty())
    {
      return {};
    }

    switch (encoding)
    {
      case Encoding::Latin1: return text::latin1ToUtf8(buf);
      case Encoding::Utf8: return text::utf8PassThrough(buf);
      case Encoding::Ucs2:
      case Encoding::Utf16Be: return text::utf16ToUtf8(buf);
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
        detail::throwTagError(
          Error::Code::CorruptData,
          std::format("invalid id3v2 tag: frame size {} exceeds tag boundary {}", size(), availableSize));
      }
    }

    void const* data() const { return _data; }

    std::size_t size() const { return contentSize() + sizeof(CommonFrameLayout); }

    std::string_view id() const
    {
      auto const* const common = static_cast<CommonFrameLayout const*>(_data);
      auto const& id = common->id;
      return {id.data(), id.size()};
    }

    template<typename Layout>
    Layout const& layout() const
    {
      if (sizeof(Layout) > size())
      {
        detail::throwTagError(
          Error::Code::CorruptData,
          std::format("invalid id3v2 frame, expect layout size {} > frame size {}", sizeof(Layout), size()));
      }

      return *static_cast<Layout const*>(_data);
    }

  protected:
    std::size_t contentSize() const
    {
      auto const* const common = static_cast<CommonFrameLayout const*>(_data);
      return frameSize(*common);
    }

  private:
    void const* _data;
  };

  using V22FrameView = FrameView<V22CommonFrameLayout>;

  using V23FrameView = FrameView<V23CommonFrameLayout>;

  using V24FrameView = FrameView<V24CommonFrameLayout>;

  template<typename FrameViewLayout>
  class TextFrameView : public FrameView<typename FrameViewLayout::CommonLayout>
  {
  public:
    using Base = FrameView<typename FrameViewLayout::CommonLayout>;

    using Base::Base;

    std::string text() const
    {
      auto const* const begin = static_cast<std::byte const*>(Base::data()) + sizeof(FrameViewLayout);
      auto const* const end = begin + (Base::size() - sizeof(FrameViewLayout));
      std::string result =
        convertToUtf8(std::span<std::byte const>{begin, end}, Base::template layout<FrameViewLayout>().encoding);

      while (!result.empty() && result.back() == '\0')
      {
        result.pop_back();
      }

      return result;
    }
  };

  using V23TextFrameView = TextFrameView<V23TextFrameLayout>;

  using V24TextFrameView = TextFrameView<V24TextFrameLayout>;

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

    FrameViewIterator operator++(std::int32_t)
    {
      auto temp = *this;
      increment();
      return temp;
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
} // namespace ao::tag::mpeg::id3v2
