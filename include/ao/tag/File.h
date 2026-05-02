// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <filesystem>
#include <ao/library/TrackBuilder.h>

#include <deque>
#include <string>
#include <string_view>

namespace ao::tag
{
  class File;
}

namespace ao::tag::detail
{
  std::string_view stashOwnedString(ao::tag::File const& owner, std::string value);
}

namespace ao::tag
{
  // NOLINTBEGIN(cppcoreguidelines-special-member-functions)
  class File
  {
    friend std::string_view detail::stashOwnedString(ao::tag::File const& owner, std::string value);

  public:
    enum class Mode
    {
      ReadOnly,
      ReadWrite
    };

    File(std::filesystem::path const& path, Mode mode);

    virtual ~File() = default;

    // Abstract base class - disable copy/move
    File(File const&) = delete;
    File& operator=(File const&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    /**
     * Parse and return track metadata as a TrackBuilder.
     *
     * The returned builder may borrow memory from two sources:
     * 1. The mmap'd file data (valid for the lifetime of this File instance).
     * 2. The _ownedStrings deque (valid until the next loadTrack() call on this File instance).
     *
     * IMPORTANT: The builder must have serialize() called before this File is destroyed
     * or before loadTrack() is called again on this File instance.
     */
    virtual ao::library::TrackBuilder loadTrack() const = 0;

    /**
     * Open a tag file by path, auto-detecting format from extension.
     * Returns nullptr for unsupported extensions.
     */
    static std::unique_ptr<File> open(std::filesystem::path const& path, Mode mode = Mode::ReadOnly);

  protected:
    void clearOwnedStrings() const { _ownedStrings.clear(); }

  private:
    std::string_view stashOwnedString(std::string value) const
    {
      _ownedStrings.push_back(std::move(value));
      return _ownedStrings.back();
    }

    mutable std::deque<std::string> _ownedStrings;

  protected:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
  // NOLINTEND(cppcoreguidelines-special-member-functions)

  namespace detail
  {
    inline std::string_view stashOwnedString(ao::tag::File const& owner, std::string value)
    {
      return owner.stashOwnedString(std::move(value));
    }
  } // namespace detail
}
