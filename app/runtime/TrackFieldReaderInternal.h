// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/FileManifestStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>

namespace ao::library
{
  class DictionaryStore;
  class TrackView;
}

namespace ao::rt
{
  TrackFieldRawValue readTrackFieldRawValue(TrackField field,
                                            library::TrackView const& view,
                                            library::DictionaryStore const& dict,
                                            library::FileManifestStore::Reader const* manifestReader);
} // namespace ao::rt
