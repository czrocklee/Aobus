// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Atom.h"
#include <cassert>
#include <iostream>
#include <map>

namespace rs::tag::mp4
{
  namespace
  {
    std::map<std::string, std::size_t, std::less<>> ContainerAtomIterested = {{"moov", 0},
                                                                              {"udta", 0},
                                                                              {"meta", 4},
                                                                              {"ilst", 0}};

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, char const* data, std::size_t size)
    {
      for (auto end = data + size; data != end;)
      {
        auto const* layout = reinterpret_cast<AtomLayout const*>(data);
        auto length = layout->length.value();
        auto type = std::string_view{layout->type.data(), 4};

        if (auto iter = ContainerAtomIterested.find(type); iter != ContainerAtomIterested.end())
        {
          auto child = std::make_unique<ContainerAtomView>(data, length, parent);
          parseAtoms(*child, data + 8 + iter->second, length - 8 - iter->second);
          parent.add(std::move(child));
        }
        else
        {
          parent.add(std::make_unique<LeafAtomView>(data, length, parent));
        }

        data += length;
      }
    }
  }

  RootAtom fromBuffer(void const* data, std::size_t size)
  {
    RootAtom root;
    parseAtoms(root, static_cast<char const*>(data), size);
    return root;
  }
}
