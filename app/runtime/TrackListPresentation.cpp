// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackListPresentation.h"

namespace ao::app
{
  TrackListPresentationSnapshot presentationForGroup(TrackGroupKey groupBy)
  {
    TrackListPresentationSnapshot snapshot;
    snapshot.groupBy = groupBy;

    auto pushTerm = [&snapshot](TrackSortField field) {
      snapshot.effectiveSortBy.push_back(TrackSortTerm{field, true});
    };

    switch (groupBy)
    {
    case TrackGroupKey::None:
      break;

    case TrackGroupKey::Artist:
      pushTerm(TrackSortField::Artist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Artist);
      break;

    case TrackGroupKey::Album:
      pushTerm(TrackSortField::AlbumArtist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Artist);
      snapshot.redundantFields.push_back(TrackSortField::Album);
      snapshot.redundantFields.push_back(TrackSortField::AlbumArtist);
      break;

    case TrackGroupKey::AlbumArtist:
      pushTerm(TrackSortField::AlbumArtist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::AlbumArtist);
      break;

    case TrackGroupKey::Genre:
      pushTerm(TrackSortField::Genre);
      pushTerm(TrackSortField::Artist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Genre);
      break;

    case TrackGroupKey::Composer:
      pushTerm(TrackSortField::Composer);
      pushTerm(TrackSortField::Artist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Composer);
      break;

    case TrackGroupKey::Work:
      pushTerm(TrackSortField::Work);
      pushTerm(TrackSortField::Artist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Work);
      break;

    case TrackGroupKey::Year:
      pushTerm(TrackSortField::Year);
      pushTerm(TrackSortField::Artist);
      pushTerm(TrackSortField::Album);
      pushTerm(TrackSortField::DiscNumber);
      pushTerm(TrackSortField::TrackNumber);
      pushTerm(TrackSortField::Title);

      snapshot.redundantFields.push_back(TrackSortField::Year);
      break;
    }

    return snapshot;
  }
}
