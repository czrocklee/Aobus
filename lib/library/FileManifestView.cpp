// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Fnv1a.h>

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

  std::uint64_t FileManifestView::audioPayloadLength() const noexcept
  {
    return header().audioPayloadLength();
  }

  utility::Hash128 FileManifestView::audioSignature() const noexcept
  {
    return header().audioSignature();
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
