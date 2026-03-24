// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Atom.h"

#include <cassert>
#include <iostream>
#include <map>
#include <span>

namespace rs::tag::mp4
{
  namespace
  {
    std::map<std::string, std::size_t, std::less<>> ContainerAtomIterested = {{"moov", 0},
                                                                              {"udta", 0},
                                                                              {"meta", 4},
                                                                              {"ilst", 0}};

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, std::span<char const> data)
    {
      while (!data.empty())
      {
        auto const* layout = reinterpret_cast<AtomLayout const*>(data.data());
        auto length = layout->length.value();
        auto type = std::string_view{layout->type.data(), 4};

        if (auto iter = ContainerAtomIterested.find(type); iter != ContainerAtomIterested.end())
        {
          auto child = std::make_unique<ContainerAtomView>(data.data(), length, parent);
          parseAtoms(*child, data.subspan(8 + iter->second, length - 8 - iter->second));
          parent.add(std::move(child));
        }
        else
        {
          parent.add(std::make_unique<LeafAtomView>(data.data(), length, parent));
        }

        data = data.subspan(length);
      }
    }
  }

  RootAtom fromBuffer(void const* data, std::size_t size)
  {
    RootAtom root;
    parseAtoms(root, std::span<char const>{static_cast<char const*>(data), size});
    return root;
  }
}
