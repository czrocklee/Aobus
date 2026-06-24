// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/MappedFile.h>

#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tag
{
  class TagFile;
}

namespace ao::tag::detail
{
  std::string_view stashOwnedString(TagFile const& owner, std::string value);
}

namespace ao::tag
{
  class TagFile
  {
  public:
    TagFile(std::filesystem::path const& path);

    virtual ~TagFile() = default;

    // Abstract base class - disable copy/move
    TagFile(TagFile const&) = delete;
    TagFile& operator=(TagFile const&) = delete;
    TagFile(TagFile&&) = delete;
    TagFile& operator=(TagFile&&) = delete;

    /**
     * Parse and return track metadata as a TrackBuilder.
     *
     * The returned builder may borrow memory from two sources:
     * 1. The mmap'd file data (valid for the lifetime of this TagFile instance).
     * 2. The _ownedStrings deque (valid until the next loadTrack() call on this TagFile instance).
     *
     * IMPORTANT: The builder must have serialize() called before this TagFile is destroyed
     * or before loadTrack() is called again on this TagFile instance.
     *
     * Error model: external failures are returned as Result errors. Malformed
     * tag/container bytes return Error::Code::CorruptData; mapping failures return
     * Error::Code::IoError. See doc/design/error-model.md.
     */
    Result<library::TrackBuilder> loadTrack() const;

    /**
     * Open a tag file by path, auto-detecting format from extension.
     * Unsupported extensions return Error::Code::NotSupported.
     */
    static Result<std::unique_ptr<TagFile>> open(std::filesystem::path const& path);

  protected:
    void clearOwnedStrings() const { _ownedStrings.clear(); }

    void const* address() const noexcept { return _address; }
    std::size_t size() const noexcept { return _size; }

    virtual Result<library::TrackBuilder> loadTrackImpl() const = 0;

  private:
    friend std::string_view detail::stashOwnedString(TagFile const& owner, std::string value);

    Result<> mappedResult() const;

    std::string_view stashOwnedString(std::string value) const
    {
      _ownedStrings.push_back(std::move(value));
      return _ownedStrings.back();
    }

    mutable std::deque<std::string> _ownedStrings;

    // The file is memory-mapped through utility::MappedFile, which keeps
    // <boost/interprocess/*> out of this public header. The mapped address/size
    // are cached here (TagFile is non-movable, so they stay valid for this
    // instance's lifetime) to keep the accessors inline.
    utility::MappedFile _mappedFile;
    std::optional<Error> _optOpenError;
    void const* _address = nullptr;
    std::size_t _size = 0;
  };

  namespace detail
  {
    inline std::string_view stashOwnedString(TagFile const& owner, std::string value)
    {
      return owner.stashOwnedString(std::move(value));
    }
  } // namespace detail
} // namespace ao::tag
