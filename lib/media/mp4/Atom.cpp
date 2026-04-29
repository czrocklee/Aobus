// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/media/mp4/Atom.h>

#include <cassert>
#include <iostream>
#include <map>
#include <span>

namespace rs::media::mp4
{
  Atom const* Atom::find(std::span<std::string_view const> path) const
  {
    if (path.empty() || path[0] != type())
    {
      return nullptr;
    }

    if (path.size() == 1)
    {
      return this;
    }

    Atom const* found = nullptr;
    visitChildren(
      [&](Atom const& child)
      {
        found = child.find(path.subspan(1));
        return found == nullptr;
      });

    return found;
  }

  namespace
  {
    std::map<std::string, std::size_t, std::less<>> const ContainerAtomInterested = {{"moov", 0},
                                                                                     {"udta", 0},
                                                                                     {"meta", 4},
                                                                                     {"ilst", 0},
                                                                                     {"trak", 0},
                                                                                     {"mdia", 0},
                                                                                     {"minf", 0},
                                                                                     {"stbl", 0},
                                                                                     {"stsd", 8}};

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, std::span<std::byte const> data)
    {
      while (data.size() >= 8)
      {
        auto const* layout = utility::layout::view<AtomLayout>(data);
        auto length = layout->length.value();
        if (length < 8 || length > data.size())
        {
          break;
        }

        auto type = utility::bytes::stringView(utility::bytes::view(layout->type));
        auto skip = std::optional<std::size_t>{};

        if (auto iter = ContainerAtomInterested.find(type); iter != ContainerAtomInterested.end())
        {
          skip = iter->second;
        }
        else if (parent.type() == "stsd" && (type == "alac" || type == "mp4a"))
        {
          // Audio sample entries (alac, mp4a) in stsd have 28 bytes of fixed data
          skip = 28;
        }

        if (skip)
        {
          // Atom header is 8 bytes (4 for size + 4 for type)
          constexpr std::size_t kAtomHeaderSize = 8;
          auto child = std::make_unique<ContainerAtomView>(data.subspan(0, length), parent);
          parseAtoms(*child, data.subspan(kAtomHeaderSize + *skip, length - kAtomHeaderSize - *skip));
          parent.add(std::move(child));
        }
        else
        {
          parent.add(std::make_unique<LeafAtomView>(data.subspan(0, length), parent));
        }

        data = data.subspan(length);
      }
    }
  }

  RootAtom fromBuffer(std::span<std::byte const> data)
  {
    auto root = RootAtom{};
    parseAtoms(root, data);
    return root;
  }
} // namespace rs::media::mp4
