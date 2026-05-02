// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/tag/File.h>

namespace ao::tag::mpeg
{
  class FrameView;

  class File : public ao::tag::File
  {
  public:
    using ao::tag::File::File;

    ao::library::TrackBuilder loadTrack() const override;

  private:
    std::uint32_t calculateDuration(FrameView const& frame, bool hasId3v1) const;
  };
}
