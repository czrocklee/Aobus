// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/resource/ResourceDiskCache.h>

#include <ao/Contract.h>
#include <ao/utility/AtomicFile.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    /// One shard level keeps a user with many libraries from accumulating one
    /// flat directory of tens of thousands of files.
    constexpr std::size_t kShardNameLength = 2;

    constexpr auto kCoverDirectoryName = std::string_view{"cover"};

    /// One entry as eviction needs it.
    struct CachedEntry final
    {
      std::filesystem::path path{};
      std::size_t byteLength = 0;
      std::filesystem::file_time_type writeTime{};
    };

    std::optional<std::vector<std::byte>> readFileBytes(std::filesystem::path const& path)
    {
      auto errorCode = std::error_code{};
      auto const byteLength = std::filesystem::file_size(path, errorCode);

      if (errorCode)
      {
        return std::nullopt;
      }

      auto stream = std::ifstream{path, std::ios::binary};

      if (!stream)
      {
        return std::nullopt;
      }

      auto bytes = std::vector<std::byte>(static_cast<std::size_t>(byteLength));

      if (byteLength != 0)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte buffer fill at the iostream boundary.
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(byteLength));

        if (!stream || stream.gcount() != static_cast<std::streamsize>(byteLength))
        {
          return std::nullopt;
        }
      }

      return bytes;
    }

    void removeQuietly(std::filesystem::path const& path)
    {
      auto errorCode = std::error_code{};
      std::filesystem::remove(path, errorCode);
    }

    /// One entry's eviction inputs, or nothing when the entry cannot be measured.
    /// Each query carries its own error code: sharing one would let a later
    /// success clear an earlier failure and rank an entry on an unset length.
    std::optional<CachedEntry> measureEntry(std::filesystem::directory_entry const& entry)
    {
      auto errorCode = std::error_code{};

      if (!entry.is_regular_file(errorCode))
      {
        return std::nullopt;
      }

      auto const byteLength = entry.file_size(errorCode);

      if (errorCode)
      {
        return std::nullopt;
      }

      auto const writeTime = entry.last_write_time(errorCode);

      if (errorCode)
      {
        return std::nullopt;
      }

      return CachedEntry{
        .path = entry.path(), .byteLength = static_cast<std::size_t>(byteLength), .writeTime = writeTime};
    }
  } // namespace

  std::filesystem::path coverCacheDirectory(std::filesystem::path const& cacheDirectory)
  {
    return cacheDirectory.empty() ? cacheDirectory : cacheDirectory / kCoverDirectoryName;
  }

  ResourceDiskCache::ResourceDiskCache(Config config)
    : _config{std::move(config)}, _unconvergedBytes{_config.byteBudget / kConvergeWriteShare}
  {
    AO_EXPECTS(
      _config.directory.empty() || _config.maximumEntryBytes > 0, "A cache holding entries needs a maximum entry size");
  }

  std::filesystem::path ResourceDiskCache::entryPath(utility::Sha256Digest const& digest) const
  {
    AO_EXPECTS(isEnabled(), "A disabled cache has no entry paths");
    auto const hex = utility::sha256Hex(digest);
    return _config.directory / hex.substr(0, kShardNameLength) / hex;
  }

  std::optional<std::vector<std::byte>> ResourceDiskCache::read(utility::Sha256Digest const& digest) const
  {
    if (!isEnabled())
    {
      return std::nullopt;
    }

    auto const path = entryPath(digest);
    // Deliberately not bounded by `maximumEntryBytes`: that limit governs what
    // this cache installs, and a read that applied it would put a ceiling on the
    // administrative delivery the specification exempts, which is served from
    // this same tier. An entry longer than any caller may serve is refused by
    // that caller, and one whose content does not match its name is discarded
    // below.
    auto optBytes = readFileBytes(path);

    if (!optBytes)
    {
      return std::nullopt;
    }

    if (utility::computeSha256(*optBytes) != digest)
    {
      // The entry does not hold what its name claims, so it is evidence of
      // nothing. Removing it is best-effort; the walk continues either way.
      removeQuietly(path);
      return std::nullopt;
    }

    touch(path);
    return optBytes;
  }

  void ResourceDiskCache::store(utility::Sha256Digest const& digest, std::span<std::byte const> const bytes) const
  {
    if (!isEnabled() || bytes.size() > _config.maximumEntryBytes)
    {
      return;
    }

    auto const path = entryPath(digest);
    auto errorCode = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), errorCode);

    if (errorCode)
    {
      return;
    }

    // Two processes may write one entry at once, which content addressing makes
    // harmless: both write identical bytes under identical names, so whichever
    // replacement lands last is the same file either way.
    if (!utility::writeAtomically(path, utility::bytes::stringView(bytes)))
    {
      return;
    }

    if (accumulateWrite(bytes.size()))
    {
      converge();
    }
  }

  bool ResourceDiskCache::accumulateWrite(std::size_t const byteLength) const
  {
    auto const share = _config.byteBudget / kConvergeWriteShare;
    auto const written = _unconvergedBytes.fetch_add(byteLength, std::memory_order_relaxed) + byteLength;

    if (written < share)
    {
      return false;
    }

    // Concurrent writers may each cross the threshold and each walk, and one
    // that adds between another's read and this reset loses its bytes from the
    // count. Both outcomes only change when the next walk happens, so the
    // approximation costs nothing worth an exchange loop.
    _unconvergedBytes.store(0, std::memory_order_relaxed);
    return true;
  }

  void ResourceDiskCache::converge() const
  {
    auto errorCode = std::error_code{};
    auto entries = std::vector<CachedEntry>{};
    std::size_t totalBytes = 0;

    auto it = std::filesystem::recursive_directory_iterator{_config.directory, errorCode};

    if (errorCode)
    {
      return;
    }

    // The walk advances through `increment` rather than a range-for, because the
    // `operator++` a range-for calls reports failure by throwing however the
    // iterator was constructed. Convergence may be deferred; it may not fail the
    // request whose already-verified bytes this pass was called to retain.
    while (it != std::filesystem::recursive_directory_iterator{})
    {
      if (auto const optEntry = measureEntry(*it); optEntry)
      {
        // A row that cannot be measured cannot be ranked; skipping it leaves the
        // budget approached from below rather than abandoning the pass.
        totalBytes += optEntry->byteLength;
        entries.push_back(*optEntry);
      }

      it.increment(errorCode);

      if (errorCode)
      {
        // A census that cannot advance ends here. Counting fewer entries only
        // understates the total, so the budget pass below can evict less than a
        // complete census would and never more.
        break;
      }
    }

    if (totalBytes <= _config.byteBudget)
    {
      return;
    }

    // Least-recently-used over every entry, and it cannot be anything cleverer:
    // sparing the entries no carrier can rebuild would mean consulting the
    // database and opening candidate files at eviction time, to decide which
    // one-file-read rebuild to repeat.
    std::ranges::sort(entries, {}, &CachedEntry::writeTime);

    for (auto const& entry : entries)
    {
      if (totalBytes <= _config.byteBudget)
      {
        return;
      }

      std::filesystem::remove(entry.path, errorCode);

      if (errorCode)
      {
        errorCode.clear();
        continue;
      }

      totalBytes -= entry.byteLength;
    }
  }

  void ResourceDiskCache::touch(std::filesystem::path const& path) const
  {
    // Recency is the entry's own modification time, deliberately rather than a
    // sidecar index: an index would be more accurate and would make the cache a
    // small database with its own consistency, durability, and contention
    // problems, all to choose which one-file-read rebuild to repeat. Access time
    // is not used because a `noatime` or `relatime` mount makes it meaningless
    // and the cache cannot see how it was mounted.
    auto errorCode = std::error_code{};
    auto const writeTime = std::filesystem::last_write_time(path, errorCode);

    if (errorCode)
    {
      return;
    }

    auto const now = std::filesystem::file_time_type::clock::now();

    if (now - writeTime < kTouchInterval)
    {
      return;
    }

    // Where the touch fails, eviction degrades to oldest-materialized order,
    // which is the right answer for entries that all cost one file read.
    std::filesystem::last_write_time(path, now, errorCode);
  }
} // namespace ao::rt
