// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/TrackBuilder.h>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <deque>
#include <filesystem>
#include <memory>
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
  // NOLINTBEGIN(cppcoreguidelines-special-member-functions)
  class TagFile
  {
    friend std::string_view detail::stashOwnedString(TagFile const& owner, std::string value);

  public:
    enum class Mode : std::uint8_t
    {
      ReadOnly,
      ReadWrite
    };

    TagFile(std::filesystem::path const& path, Mode mode);

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
     */
    virtual library::TrackBuilder loadTrack() const = 0;

    /**
     * Open a tag file by path, auto-detecting format from extension.
     * Returns nullptr for unsupported extensions.
     */
    static std::unique_ptr<TagFile> open(std::filesystem::path const& path, Mode mode = Mode::ReadOnly);

  protected:
    void clearOwnedStrings() const { _ownedStrings.clear(); }

    boost::interprocess::mapped_region const& region() const noexcept { return _mappedRegion; }
    void* address() const noexcept { return _mappedRegion.get_address(); }
    std::size_t size() const noexcept { return _mappedRegion.get_size(); }

  private:
    std::string_view stashOwnedString(std::string value) const
    {
      _ownedStrings.push_back(std::move(value));
      return _ownedStrings.back();
    }

    mutable std::deque<std::string> _ownedStrings;

    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
  // NOLINTEND(cppcoreguidelines-special-member-functions)

  namespace detail
  {
    inline std::string_view stashOwnedString(TagFile const& owner, std::string value)
    {
      return owner.stashOwnedString(std::move(value));
    }
  } // namespace detail
}
