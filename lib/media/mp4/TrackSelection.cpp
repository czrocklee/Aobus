// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/TrackSelection.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::media::mp4
{
  namespace
  {
    constexpr std::size_t kHandlerTypeOffset = sizeof(AtomLayout) + 8;
    constexpr std::size_t kHandlerTypeSize = 4;

    constexpr std::array kTrackHdlrPath = {
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"hdlr"},
    };

    constexpr std::array kTrackStsdPath = {
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
      std::string_view{"stsd"},
    };

    std::optional<std::string_view> trackHandlerType(Atom const& track)
    {
      auto const* const node = track.find(kTrackHdlrPath);

      if (node == nullptr)
      {
        return std::nullopt;
      }

      auto const& view = utility::unsafeDowncast<AtomView const>(*node);
      auto const bytes = view.bytes();

      if (bytes.size() < kHandlerTypeOffset + kHandlerTypeSize)
      {
        return std::nullopt;
      }

      return utility::bytes::stringView(bytes.subspan(kHandlerTypeOffset, kHandlerTypeSize));
    }

    bool isSupportedAudioSampleEntry(std::string_view sampleEntryType) noexcept
    {
      return sampleEntryType == "alac" || sampleEntryType == "mp4a";
    }

    void visitMovieTracks(Atom const& root, Atom::Visitor visitor)
    {
      auto keepGoing = true;

      root.visitChildren(
        [&](Atom const& movie)
        {
          if (!keepGoing)
          {
            return false;
          }

          if (movie.type() != "moov")
          {
            return true;
          }

          movie.visitChildren(
            [&](Atom const& track)
            {
              if (track.type() != "trak")
              {
                return true;
              }

              keepGoing = std::invoke(visitor, track);
              return keepGoing;
            });

          return keepGoing;
        });
    }
  } // namespace

  std::optional<std::string> firstSampleEntryType(AtomView const& stsdView)
  {
    auto const bytes = stsdView.bytes();

    if (bytes.size() < sizeof(StsdAtomLayout))
    {
      return std::nullopt;
    }

    if (auto const& stsdLayout = stsdView.layout<StsdAtomLayout>(); stsdLayout.entryCount.value() != 1)
    {
      return std::nullopt;
    }

    auto const entryBytes = bytes.subspan(sizeof(StsdAtomLayout));

    if (entryBytes.size() < sizeof(AtomLayout))
    {
      return std::nullopt;
    }

    auto const* const entryLayout = utility::layout::view<AtomLayout>(entryBytes);

    if (entryLayout->length.value() < sizeof(AtomLayout) || entryLayout->length.value() > entryBytes.size())
    {
      return std::nullopt;
    }

    return std::string{entryLayout->type.data(), entryLayout->type.size()};
  }

  std::optional<AudioTrackSelection> findAudioTrack(Atom const& root, std::string_view targetSampleEntryType)
  {
    auto optResult = std::optional<AudioTrackSelection>{};

    visitMovieTracks(root,
                     [&](Atom const& track)
                     {
                       auto const* const stsdNode = track.find(kTrackStsdPath);

                       if (stsdNode == nullptr)
                       {
                         return true;
                       }

                       auto const& stsdView = utility::unsafeDowncast<AtomView const>(*stsdNode);
                       auto optSampleEntryType = firstSampleEntryType(stsdView);

                       if (!optSampleEntryType)
                       {
                         return true;
                       }

                       if (!targetSampleEntryType.empty() && *optSampleEntryType != targetSampleEntryType)
                       {
                         return true;
                       }

                       auto const optHandlerType = trackHandlerType(track);

                       if (optHandlerType && *optHandlerType != "soun")
                       {
                         return true;
                       }

                       if (!optHandlerType && !isSupportedAudioSampleEntry(*optSampleEntryType))
                       {
                         return true;
                       }

                       optResult = AudioTrackSelection{
                         .track = &track,
                         .stsd = &stsdView,
                         .sampleEntryType = std::move(*optSampleEntryType),
                       };
                       return false;
                     });

    return optResult;
  }
} // namespace ao::media::mp4
