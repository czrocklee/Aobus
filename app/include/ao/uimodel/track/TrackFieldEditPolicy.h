// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

namespace ao::uimodel::track
{
  bool trackFieldCanWritePatch(rt::TrackField field) noexcept;
  bool writeTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value);
} // namespace ao::uimodel::track
