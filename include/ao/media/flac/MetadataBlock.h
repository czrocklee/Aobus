// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "MetadataBlockLayout.h"
#include <boost/endian/conversion.hpp>
#include <cstring>
#include <gsl-lite/gsl-lite.hpp>
#include <ao/Exception.h>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::flac
{
  namespace detail
  {
    template<typename LengthType, boost::endian::order Order = boost::endian::order::big>
    LengthType parseLength(char const*& ptr, char const* end)
    {
      if (ptr + sizeof(LengthType) > end)
      {
        AO_THROW_FORMAT(
          ao::Exception, "invalid flac block, expect length field size {} >= {}", end - ptr, sizeof(LengthType));
      }

      LengthType length;
      std::memcpy(&length, ptr, sizeof(LengthType));
      boost::endian::conditional_reverse_inplace<Order, boost::endian::order::native>(length);
      ptr += sizeof(LengthType);
      return length;
    }

    template<typename LengthType, boost::endian::order Order = boost::endian::order::big>
    std::string_view parseString(char const*& ptr, char const* end)
    {
      LengthType length = parseLength<LengthType, Order>(ptr, end);

      if (ptr + length > end)
      {
        AO_THROW_FORMAT(ao::Exception, "invalid flac block, expect available field length {} >= {}", end - ptr, length);
      }

      char const* start = ptr;
      ptr += length;
      return {start, length};
    }
  } // namespace detail

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
    MetadataBlockView(void const* data)
      : _data{data}
    {
    }

    void const* data() const { return _data; }

    std::uint32_t size() const override
    {
      return layout<MetadataBlockLayout>().size.value() + sizeof(MetadataBlockLayout);
    }

    MetadataBlockType type() const override { return layout<MetadataBlockLayout>().type; }

    template<typename Layout>
    Layout const& layout() const
    {
      if (auto size = static_cast<MetadataBlockLayout const*>(_data)->size.value(); typename Layout::FixedSize{})
      {
        gsl_Expects(size == sizeof(Layout));
      }
      else
      {
        gsl_Expects(size >= sizeof(Layout));
      }

      return *static_cast<Layout const*>(_data);
    }

  private:
    void const* _data;
    friend class MetadataBlockViewIterator;
  };

  class VorbisCommentBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    template<typename Visitor>
    void visitComments(Visitor&& visitor) const
    {
      auto&& handleComment = visitor;
      char const* ptr = static_cast<char const*>(data()) + sizeof(MetadataBlockLayout);
      char const* end = ptr + size() - sizeof(MetadataBlockLayout);
      detail::parseString<std::uint32_t, boost::endian::order::little>(ptr, end); // vendor string

      std::uint32_t const count = detail::parseLength<std::uint32_t, boost::endian::order::little>(ptr, end);

      for (std::uint32_t i = 0; i < count; ++i)
      {
        handleComment(detail::parseString<std::uint32_t, boost::endian::order::little>(ptr, end));
      }

      if (auto sizeLeft = static_cast<std::size_t>(end - ptr); sizeLeft > 0)
      {
        AO_THROW_FORMAT(ao::Exception,
                        "invalid flac vorbis_comment block, unexpected content \"{}\"",
                        std::string_view{ptr, sizeLeft});
      }
    }

    std::vector<std::string_view> comments() const;
  };

  class PictureBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    std::span<std::byte const> blob() const;
  };

  class StreamInfoBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    StreamInfoLayout const& layout() const
    {
      auto const* ptr = static_cast<std::uint8_t const*>(data()) + sizeof(MetadataBlockLayout);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      return *reinterpret_cast<StreamInfoLayout const*>(ptr);
    }

    std::uint32_t sampleRate() const { return (layout().packedFields.value() >> 44) & 0xFFFFF; }

    std::uint8_t channels() const { return ((layout().packedFields.value() >> 41) & 0x07) + 1; }

    std::uint8_t bitDepth() const { return ((layout().packedFields.value() >> 36) & 0x1F) + 1; }

    std::uint64_t totalSamples() const { return layout().packedFields.value() & 0xFFFFFFFFF; }
  };

  class MetadataBlockViewIterator
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;
    using value_type = MetadataBlockView const;
    using pointer = MetadataBlockView const*;
    using reference = MetadataBlockView const&;
    using iterator_category = std::forward_iterator_tag;

    static constexpr std::size_t StreamInfoBlockSize = 38;

    MetadataBlockViewIterator()
      : _view{nullptr}, _sizeLeft{0}
    {
    }

    MetadataBlockViewIterator(void const* data, std::size_t size);

    // Forward iterator operations
    reference operator*() const { return _view; }

    pointer operator->() const { return &_view; }

    MetadataBlockViewIterator& operator++()
    {
      increment();
      return *this;
    }

    MetadataBlockViewIterator operator++(int)
    {
      auto tmp = *this;
      increment();
      return tmp;
    }

    bool operator==(MetadataBlockViewIterator const& other) const { return _view._data == other._view._data; }

    bool operator!=(MetadataBlockViewIterator const& other) const { return !(*this == other); }

  private:
    void increment();

    MetadataBlockView _view;
    std::size_t _sizeLeft;
  };
} // namespace ao::media::flac
