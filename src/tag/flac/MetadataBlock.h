// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "MetadataBlockLayout.h"
#include <boost/iterator/iterator_facade.hpp>
#include <cassert>
#include <span>
#include <string_view>
#include <vector>

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
    MetadataBlockView(void const* data) : _data{data} {}

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
        assert(size == sizeof(Layout));
      }
      else
      {
        assert(size >= sizeof(Layout));
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

    std::vector<std::string_view> comments() const;
  };

  class PictureBlockView : public MetadataBlockView
  {
  public:
    using MetadataBlockView::MetadataBlockView;

    std::span<std::byte const> blob() const;
  };

  class MetadataBlockViewIterator
    : public boost::iterator_facade<MetadataBlockViewIterator, MetadataBlockView const, boost::forward_traversal_tag>
  {
  public:
    static constexpr std::size_t StreamInfoBlockSize = 38;

    MetadataBlockViewIterator() : _view{nullptr}, _sizeLeft{0} {}

    MetadataBlockViewIterator(void const* data, std::size_t size);

  private:
    friend class boost::iterator_core_access;

    void increment();

    bool equal(MetadataBlockViewIterator const& other) const { return _view._data == other._view._data; }

    MetadataBlockView const& dereference() const { return _view; }

    MetadataBlockView _view;
    std::size_t _sizeLeft;
  };
}
