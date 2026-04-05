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
    std::map<std::string, std::size_t, std::less<>> const ContainerAtomIterested = {
      {"moov", 0}, {"udta", 0}, {"meta", 4}, {"ilst", 0}, {"trak", 0}, {"mdia", 0}, {"minf", 0}, {"stbl", 0}};

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, std::span<char const> data)
    {
      while (!data.empty())
      {
        auto const* layout = reinterpret_cast<AtomLayout const*>(data.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        auto length = layout->length.value();
        auto type = std::string_view{layout->type.data(), 4};

        if (auto iter = ContainerAtomIterested.find(type); iter != ContainerAtomIterested.end())
        {
          // Atom header is 8 bytes (4 for size + 4 for type)
          constexpr std::size_t kAtomHeaderSize = 8;

          auto child = std::make_unique<ContainerAtomView>(data.data(), length, parent);
          parseAtoms(*child, data.subspan(kAtomHeaderSize + iter->second, length - kAtomHeaderSize - iter->second));
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
