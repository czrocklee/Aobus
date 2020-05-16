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

#include "Atom.h"
#include <map>
#include <iostream>

namespace rs::tag::mp4
{
  namespace
  {
    /* std::map<std::string, std::size_t, std::less<>> ContainerAtom = {{"moov", 0},
                                                                     {"trak", 0},
                                                                     {"mdia", 0},
                                                                     {"minf", 0},
                                                                     {"dinf", 0},
                                                                     {"stbl", 0},
                                                                     {"udta", 0},
                                                                     {"meta", 4},
                                                                     {"ilst", 0},
                                                                     {"name", 0},
                                                                     {"trkn", 0}}; */
    std::map<std::string, std::size_t, std::less<>> ContainerAtomIterested = {{"moov", 0},
                                                                              {"udta", 0},
                                                                              {"meta", 4},
                                                                              {"ilst", 0}};

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, const char* data, std::size_t size)
    {
      for (auto end = data + size; data != end;)
      {
        const auto* layout = reinterpret_cast<const AtomLayout*>(data);
        auto length = layout->length.value();
        std::string_view type{layout->type.data(), 4};

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

  RootAtom fromBuffer(const void* data, std::size_t size)
  {
    RootAtom root;
    parseAtoms(root, static_cast<const char*>(data), size);
    return root;
  }
}