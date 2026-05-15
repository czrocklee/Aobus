// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "AtomLayout.h"
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::mp4
{
  class Atom
  {
  public:
    using Visitor = std::move_only_function<bool(Atom const&)>;

    virtual ~Atom() = default;

    Atom(Atom const&) = delete;
    Atom& operator=(Atom const&) = delete;

  protected:
    Atom() = default;
    Atom(Atom&&) = default;
    Atom& operator=(Atom&&) = default;

  public:
    virtual std::uint32_t length() const = 0;

    virtual std::string_view type() const = 0;

    virtual Atom const* parent() const = 0;

    virtual bool isLeaf() const = 0;

    virtual void visitChildren(Visitor visitor) const = 0;

    /**
     * @brief Find an atom by its path (e.g., {"root", "moov", "trak"}).
     * @return Pointer to the found atom, or nullptr if not found.
     */
    Atom const* find(std::span<std::string_view const> path) const;
  };

  class AtomView : public Atom
  {
  public:
    AtomView(std::span<std::byte const> data, Atom& parent)
      : _data{data}, _parent{parent}
    {
    }

    std::uint32_t length() const override { return layout<AtomLayout>().length.value(); }

    std::string_view type() const override
    {
      return utility::bytes::stringView(utility::bytes::view(layout<AtomLayout>().type));
    }

    Atom const* parent() const override { return &_parent; }

    std::span<std::byte const> bytes() const { return _data; }

    template<typename Layout>
    Layout const& layout() const
    {
      if constexpr (sizeof(typename Layout::FixedSize) != 0 && Layout::FixedSize::value)
      {
        gsl_Expects(utility::layout::view<AtomLayout>(_data)->length.value() == sizeof(Layout));
      }
      else
      {
        gsl_Expects(utility::layout::view<AtomLayout>(_data)->length.value() >= sizeof(Layout));
      }

      return *utility::layout::view<Layout>(_data);
    }

  private:
    std::span<std::byte const> _data;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    Atom& _parent;
  };

  class LeafAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return true; };

    void visitChildren(Visitor /*visitor*/) const override {}
  };

  class ContainerAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return false; };

    void visitChildren(Visitor visitor) const override
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
      return std::ranges::fold_left(_children, 0U, [](auto size, auto const& ptr) { return size + ptr->length(); });
    }

    std::string_view type() const override { return "root"; }

    Atom const* parent() const override { return nullptr; }

    bool isLeaf() const override { return false; };

    void visitChildren(Visitor visitor) const override
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

  RootAtom fromBuffer(std::span<std::byte const> data);
} // namespace ao::media::mp4
