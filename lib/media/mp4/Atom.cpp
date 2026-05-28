// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/media/mp4/Atom.h"

#include "ao/media/mp4/AtomLayout.h"
#include "ao/utility/ByteView.h"
#include "media/mp4/AtomDispatch.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace ao::media::mp4
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
    constexpr std::size_t kAudioSampleEntryHeaderSkip = 28;
    constexpr std::size_t kAtomHeaderSize = 8;

    template<typename ContainerAtom>
    void parseAtoms(ContainerAtom& parent, std::span<std::byte const> data)
    {
      while (data.size() >= kAtomHeaderSize)
      {
        auto const* layout = utility::layout::view<AtomLayout>(data);
        auto length = layout->length.value();

        if (length < kAtomHeaderSize || length > data.size())
        {
          break;
        }

        auto const type = utility::bytes::stringView(utility::bytes::view(layout->type));
        auto optSkip = std::optional<std::size_t>{};

        if (auto const* entry = ContainerAtomDispatchTable::lookupContainerAtom(type.data(), type.size());
            entry != nullptr)
        {
          optSkip = entry->skipSize;
        }
        else if (parent.type() == "stsd" && (type == "alac" || type == "mp4a"))
        {
          optSkip = kAudioSampleEntryHeaderSkip;
        }

        if (optSkip)
        {
          // Atom header is already defined as kAtomHeaderSize
          auto child = std::make_unique<ContainerAtomView>(data.subspan(0, length), parent);
          parseAtoms(*child, data.subspan(kAtomHeaderSize + *optSkip, length - kAtomHeaderSize - *optSkip));
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
} // namespace ao::media::mp4
