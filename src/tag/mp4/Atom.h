// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "AtomLayout.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace rs::tag::mp4
{
  class Atom
  {
  public:
    using Visitor = std::function<bool(Atom const&)>;

    virtual ~Atom() = default;

    virtual std::uint32_t length() const = 0;

    virtual std::string_view type() const = 0;

    virtual Atom const* parent() const = 0;

    virtual bool isLeaf() const = 0;

    virtual void visitChildren(Visitor const& visitor) const = 0;
  };

  class AtomView : public Atom
  {
  public:
    AtomView(void const* data, std::size_t /*size*/, Atom& parent)
      : _data{data}, _parent{parent}
    {
    }

    std::uint32_t length() const override { return layout<AtomLayout>().length.value(); }

    std::string_view type() const override { return std::string_view{layout<AtomLayout>().type.data(), 4}; }

    Atom const* parent() const override { return &_parent; }

    template<typename Layout>
    Layout const& layout() const
    {
      if (auto length = static_cast<AtomLayout const*>(_data)->length.value(); typename Layout::FixedSize{})
      {
        assert(length == sizeof(Layout));
      }
      else
      {
        assert(length >= sizeof(Layout));
      }

      return *static_cast<Layout const*>(_data);
    }

  private:
    void const* _data;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    Atom& _parent;
  };

  class LeafAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return true; };

    void visitChildren(Visitor const&) const override {}
  };

  class ContainerAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return false; };

    void visitChildren(Visitor const& visitor) const override
    {
      for (auto const& child : _children)
      {
        if (!std::invoke(visitor, *child))
        {
          break;
        }
      }
    }

    void add(std::unique_ptr<Atom> child) { _children.push_back(std::move(child)); }

  private:
    std::vector<std::unique_ptr<Atom>> _children;
  };

  class RootAtom : public Atom
  {
  public:
    std::uint32_t length() const override
    {
      return std::ranges::fold_left(_children, 0u, [](auto size, auto const& ptr) { return size + ptr->length(); });
    }

    std::string_view type() const override { return "root"; }

    Atom const* parent() const override { return nullptr; }

    bool isLeaf() const override { return false; };

    void visitChildren(Visitor const& visitor) const override
    {
      for (auto const& child : _children)
      {
        if (!std::invoke(visitor, *child))
        {
          break;
        }
      }
    }

    void add(std::unique_ptr<Atom> atom) { _children.push_back(std::move(atom)); }

  private:
    std::vector<std::unique_ptr<Atom>> _children;
  };

  RootAtom fromBuffer(void const* data, std::size_t);
}
