// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Content.h"

#include <ao/AudioCodec.h>
#include <ao/media/file/Visitor.h>

#include <cstddef>

namespace ao::media::file::detail
{
  void Content::visit(Visitor& visitor) const
  {
    for (std::size_t index = 0; index < texts.size(); ++index)
    {
      if (auto const value = texts[index]; !value.empty())
      {
        visitor.text(static_cast<TextField>(index), value);
      }
    }

    for (std::size_t index = 0; index < numbers.size(); ++index)
    {
      if (auto const value = numbers[index]; value != 0)
      {
        visitor.number(static_cast<NumberField>(index), value);
      }
    }

    if (codec != AudioCodec::Unknown)
    {
      visitor.codec(codec);
    }

    if (duration > std::chrono::milliseconds{0})
    {
      visitor.duration(duration);
    }

    if (bitrate.raw() != 0)
    {
      visitor.bitrate(bitrate);
    }

    if (sampleRate.raw() != 0)
    {
      visitor.sampleRate(sampleRate);
    }

    if (channels.raw() != 0)
    {
      visitor.channels(channels);
    }

    if (bitDepth.raw() != 0)
    {
      visitor.bitDepth(bitDepth);
    }

    for (auto const& picture : pictures)
    {
      visitor.picture(picture.type, picture.bytes);
    }
  }
} // namespace ao::media::file::detail
