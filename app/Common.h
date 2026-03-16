// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/Track_generated.h>
#include <rs/reactive/AbstractItemList.h>

using MusicLibrary = rs::core::MusicLibrary;
using TrackId = MusicLibrary::TrackId;
using AbstractTrackList = rs::reactive::AbstractItemList<TrackId, rs::fbs::TrackT>;
using TrackObserver = AbstractTrackList::Observer;