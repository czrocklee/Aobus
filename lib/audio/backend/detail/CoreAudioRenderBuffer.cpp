// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioRenderBuffer.h"

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <cstddef>
#include <limits>
#include <span>

namespace ao::audio::backend::detail
{
  BoundCoreAudioRenderBuffer bindCoreAudioRenderBuffer(::AudioBufferList* const buffers,
                                                       std::span<std::byte> const stagingBuffer,
                                                       std::size_t const byteCount) noexcept
  {
    if (buffers == nullptr || buffers->mNumberBuffers != 1U || byteCount > stagingBuffer.size() ||
        byteCount > std::numeric_limits<::UInt32>::max())
    {
      return {};
    }

    auto& buffer = buffers->mBuffers[0];

    if (buffer.mData == nullptr)
    {
      buffer.mData = stagingBuffer.data();
    }
    else if (buffer.mDataByteSize < byteCount)
    {
      return {};
    }

    buffer.mDataByteSize = static_cast<::UInt32>(byteCount);
    return {.output = {static_cast<std::byte*>(buffer.mData), byteCount}, .valid = true};
  }
} // namespace ao::audio::backend::detail
