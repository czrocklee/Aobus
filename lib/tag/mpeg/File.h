// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>

#include <chrono>

namespace ao::tag::mpeg
{
  class FrameView;

  class File : public TagFile
  {
  public:
    using TagFile::TagFile;

  private:
    Result<library::TrackBuilder> loadTrackImpl() const override;

    std::chrono::milliseconds calculateDuration(FrameView const& frame, bool hasId3v1) const;
  };
} // namespace ao::tag::mpeg
