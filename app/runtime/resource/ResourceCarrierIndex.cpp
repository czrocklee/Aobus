// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ResourceCarrierIndex.h"

#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace ao::rt
{
  ResourceCarrierIndex::ResourceCarrierIndex(std::uint64_t const libraryRevision, CarrierMap carriers)
    : _libraryRevision{libraryRevision}, _carriers{std::move(carriers)}
  {
  }

  std::span<std::string const> ResourceCarrierIndex::carrierUris(ResourceId const resourceId) const
  {
    auto const found = _carriers.find(resourceId);
    return found == _carriers.end() ? std::span<std::string const>{} : std::span<std::string const>{found->second};
  }

  ResourceCarrierIndex buildResourceCarrierIndex(library::MusicLibrary const& library,
                                                 library::ReadTransaction const& transaction)
  {
    auto carriers = ResourceCarrierIndex::CarrierMap{};

    // Cover references and the URI both live on the cold side, so half the track
    // record is enough.
    for (auto const& [trackId, view] : library.tracks().reader(transaction).cold())
    {
      auto const uri = view.property().uri();

      for (auto const cover : view.coverArt())
      {
        if (cover.resourceId == kInvalidResourceId)
        {
          continue;
        }

        auto& uris = carriers[cover.resourceId];

        // One track carrying the same image twice is a single candidate. Real
        // collections do repeat a picture type within a track, so this is not
        // hypothetical.
        if (!uris.empty() && uris.back() == uri)
        {
          continue;
        }

        uris.emplace_back(uri);
      }
    }

    return ResourceCarrierIndex{library.libraryRevision(transaction), std::move(carriers)};
  }
} // namespace ao::rt
