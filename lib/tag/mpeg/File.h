// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/tag/TagFile.h>

namespace ao::tag::mpeg
{
  class FrameView;

  class File : public TagFile
  {
  public:
    using TagFile::TagFile;

    library::TrackBuilder loadTrack() const override;

  private:
    std::uint32_t calculateDuration(FrameView const& frame, bool hasId3v1) const;
  };
}
