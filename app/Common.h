#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/reactive/AbstractItemList.h>
#include <rs/fbs/Track_generated.h>

using MusicLibrary = rs::core::MusicLibrary;
using TrackId = MusicLibrary::TrackId;
using AbstractTrackList = rs::reactive::AbstractItemList<TrackId, rs::fbs::TrackT>;
using TrackObserver = AbstractTrackList::Observer;