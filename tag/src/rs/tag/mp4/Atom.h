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

#include "AtomLayout.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <numeric>

namespace rs::tag::mp4
{

  class Atom
  {
  public:
    using Visitor = std::function<bool(const Atom&)>;

    virtual ~Atom() = default;

    virtual std::uint32_t length() const = 0;

    virtual std::string_view type() const = 0;

    virtual const Atom* parent() const = 0;

    virtual bool isLeaf() const = 0;

    virtual void visitChildren(const Visitor& visitor) const = 0;
  };

  class AtomView : public Atom
  {
  public:
    AtomView(const void* data, std::size_t size, Atom& parent) : _data{data}, _size{size}, _parent{parent} {}

    std::uint32_t length() const override { return layout<AtomLayout>().length.value(); }

    std::string_view type() const override { return std::string_view{layout<AtomLayout>().type.data(), 4}; }

    const Atom* parent() const override { return &_parent; }

    template<typename Layout>
    const Layout& layout() const
    {
      if (auto length = static_cast<const AtomLayout*>(_data)->length.value(); typename Layout::FixedSize{})
      {
        assert(length == sizeof(Layout));
      }
      else
      {
        assert(length >= sizeof(Layout));
      }

      return *static_cast<const Layout*>(_data);
    }

  private:
    const void* _data;
    std::size_t _size;
    Atom& _parent;
  };

  class LeafAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return true; };

    void visitChildren(const Visitor& visitor) const override { return; }
  };

  class ContainerAtomView : public AtomView
  {
  public:
    using AtomView::AtomView;
    using Visitor = Atom::Visitor;

    bool isLeaf() const override { return false; };

    void visitChildren(const Visitor& visitor) const override
    {
      for (const auto& child : _children)
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
      return std::accumulate(_children.begin(), _children.end(), 0, [](auto size, auto& ptr) { return size + ptr->length(); });
    }

    std::string_view type() const override { return "root"; }

    const Atom* parent() const override { return nullptr; }

    bool isLeaf() const override { return false; };

    void visitChildren(const Visitor& visitor) const override
    {
      for (const auto& child : _children)
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

  RootAtom fromBuffer(const void* data, std::size_t);
}