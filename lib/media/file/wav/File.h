// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../detail/Content.h"
#include "../detail/Reader.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/wav/Riff.h>

#include <optional>

namespace ao::media::file::wav
{
  class File final : public detail::Reader
  {
  public:
    using detail::Reader::Reader;

    Result<detail::Content> readContent() const override;
    Result<PayloadView> audioPayload() const override;

  private:
    Result<media::wav::ParsedWave> const& parsed() const;

    mutable std::optional<Result<media::wav::ParsedWave>> _optParsedResult;
  };
} // namespace ao::media::file::wav
