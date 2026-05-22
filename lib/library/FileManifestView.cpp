// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/FileManifestView.h"

#include "ao/Exception.h"
#include "ao/Type.h"
#include "ao/library/FileManifestLayout.h"
#include "ao/utility/ByteView.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::library
{
  FileManifestView::FileManifestView(std::span<std::byte const> data)
    : _data{data}
  {
    if (_data.size() < sizeof(FileManifestHeader))
    {
      ao::throwException<Exception>("FileManifestView: Data too small for header (size: {})", _data.size());
    }
  }

  TrackId FileManifestView::trackId() const noexcept
  {
    return header().trackId;
  }

  std::uint64_t FileManifestView::fileSize() const noexcept
  {
    return header().fileSize();
  }

  std::uint64_t FileManifestView::mtime() const noexcept
  {
    return header().mtime();
  }

  FileStatus FileManifestView::status() const noexcept
  {
    return header().status;
  }

  FileManifestHeader const& FileManifestView::header() const noexcept
  {
    return *utility::layout::view<FileManifestHeader>(_data);
  }
} // namespace ao::library
