// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../detail/Content.h"
#include "../detail/Reader.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>

namespace ao::media::file::mpeg
{
  class FrameView;

  class File final : public detail::Reader
  {
  public:
    explicit File(std::span<std::byte const> bytes);

    ~File() override;

    File(File const&) = delete;
    File& operator=(File const&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    Result<detail::Content> readContent() const override;
    Result<PayloadView> audioPayload() const override;

  private:
    struct Index;
    struct CachedIndex;

    Result<Index> const& index() const;
    static std::chrono::milliseconds calculateDuration(FrameView const& frame, std::size_t payloadSize);

    mutable std::unique_ptr<CachedIndex> _cachedIndexPtr;
  };
} // namespace ao::media::file::mpeg
