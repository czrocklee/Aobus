// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "media/mp4/AtomDispatch.h"
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/utility/ByteView.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

    // Parse one level of children out of a container's payload. Child containers
    // are created without recursing into them — each ContainerAtomView retains its
    // own payload and parses its children on first access (see Atom.h). This keeps
    // the walk lazy: only the subtrees a consumer actually visits get materialized.
    void parseChildren(Atom const& parent, std::span<std::byte const> data, std::vector<std::unique_ptr<Atom>>& out)
    {
      while (data.size() >= kAtomHeaderSize)
      {
        auto const* layout = utility::layout::view<AtomLayout>(data);
        auto const length = layout->length.value();

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

        if (auto const atomBytes = data.subspan(0, length); optSkip && length >= kAtomHeaderSize + *optSkip)
        {
          auto const payload = data.subspan(kAtomHeaderSize + *optSkip, length - kAtomHeaderSize - *optSkip);
          out.push_back(std::make_unique<ContainerAtomView>(atomBytes, payload, parent));
        }
        else
        {
          // Not a known container, or too short to hold the declared skip header:
          // treat it as an opaque leaf, matching the original eager behavior.
          out.push_back(std::make_unique<LeafAtomView>(atomBytes, parent));
        }

        data = data.subspan(length);
      }
    }
  } // namespace

  void ContainerAtomView::ensureParsed() const
  {
    if (_parsed)
    {
      return;
    }

    _parsed = true;
    parseChildren(*this, _payload, _children);
  }

  void RootAtom::ensureParsed() const
  {
    if (_parsed)
    {
      return;
    }

    _parsed = true;
    parseChildren(*this, _data, _children);
  }

  RootAtom fromBuffer(std::span<std::byte const> data)
  {
    return RootAtom{data};
  }
} // namespace ao::media::mp4
