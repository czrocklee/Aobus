// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/SampleDescription.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ao::media::mp4
{
  std::string audioSampleEntryType(std::span<std::byte const> fileData)
  {
    auto const root = fromBuffer(fileData);

    static constexpr std::array kStsdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
      std::string_view{"stsd"},
    };

    auto const* stsdNode = root.find(kStsdPath);

    if (stsdNode == nullptr)
    {
      return {};
    }

    auto const& view = utility::unsafeDowncast<AtomView const>(*stsdNode);
    auto const bytes = view.bytes();

    if (bytes.size() < sizeof(StsdAtomLayout))
    {
      return {};
    }

    if (auto const& stsdLayout = view.layout<StsdAtomLayout>(); stsdLayout.entryCount.value() == 0)
    {
      return {};
    }

    auto const entryBytes = bytes.subspan(sizeof(StsdAtomLayout));

    if (entryBytes.size() < sizeof(AtomLayout))
    {
      return {};
    }

    auto const* const entryLayout = utility::layout::view<AtomLayout>(entryBytes);

    if (entryLayout->length.value() < sizeof(AtomLayout) || entryLayout->length.value() > entryBytes.size())
    {
      return {};
    }

    return std::string{entryLayout->type.data(), entryLayout->type.size()};
  }
} // namespace ao::media::mp4
