// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackEditScript.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::delta
{
  namespace
  {
    enum class Stage : std::uint8_t
    {
      Remove,
      Insert,
    };

    bool checkedAdd(std::size_t& value, std::size_t amount) noexcept
    {
      if (amount > std::numeric_limits<std::size_t>::max() - value)
      {
        return false;
      }

      value += amount;
      return true;
    }

    template<typename Range>
    void appendCoalesced(std::vector<RegularTrackEdit>& edits, Range range)
    {
      if (range.trackIds.empty())
      {
        return;
      }

      if (auto* const previous = edits.empty() ? nullptr : std::get_if<Range>(&edits.back());
          previous != nullptr && previous->start + previous->trackIds.size() == range.start)
      {
        previous->trackIds.append_range(range.trackIds);
        return;
      }

      edits.push_back(std::move(range));
    }

    std::vector<TrackId> longestCommonSubsequence(std::span<TrackId const> from, std::span<TrackId const> to)
    {
      auto toIndex = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>{};
      toIndex.reserve(to.size());

      for (std::size_t index = 0; index < to.size(); ++index)
      {
        toIndex.emplace(to[index], index);
      }

      struct Candidate final
      {
        TrackId trackId{};
        std::size_t toIndex = 0;
      };
      auto candidates = std::vector<Candidate>{};
      candidates.reserve(std::min(from.size(), to.size()));

      for (auto const trackId : from)
      {
        if (auto const it = toIndex.find(trackId); it != toIndex.end())
        {
          candidates.push_back(Candidate{.trackId = trackId, .toIndex = it->second});
        }
      }

      auto tails = std::vector<std::size_t>{};
      auto predecessors = std::vector<std::size_t>(candidates.size(), candidates.size());
      tails.reserve(candidates.size());

      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        auto const position =
          static_cast<std::size_t>(std::ranges::lower_bound(tails,
                                                            candidates[index].toIndex,
                                                            {},
                                                            [&candidates](std::size_t candidateIndex)
                                                            { return candidates[candidateIndex].toIndex; }) -
                                   tails.begin());

        if (position != 0)
        {
          predecessors[index] = tails[position - 1];
        }

        if (position == tails.size())
        {
          tails.push_back(index);
        }
        else
        {
          tails[position] = index;
        }
      }

      auto result = std::vector<TrackId>{};
      result.reserve(tails.size());

      if (!tails.empty())
      {
        for (auto index = tails.back(); index != candidates.size(); index = predecessors[index])
        {
          result.push_back(candidates[index].trackId);
        }

        std::ranges::reverse(result);
      }

      return result;
    }
  } // namespace

  bool validate(RegularTrackEditScript const& script, std::size_t const initialSize) noexcept
  {
    auto stage = Stage::Remove;
    auto size = initialSize;
    auto previousRemoveStart = initialSize;
    std::size_t previousInsertEnd = 0;
    std::size_t previousUpdateEnd = 0;

    for (auto const& edit : script.edits)
    {
      if (auto const* removal = std::get_if<RemoveRange>(&edit); removal != nullptr)
      {
        if (stage != Stage::Remove || removal->trackIds.empty() || removal->start >= previousRemoveStart ||
            removal->start > size || removal->trackIds.size() > size - removal->start)
        {
          return false;
        }

        previousRemoveStart = removal->start;
        size -= removal->trackIds.size();
        previousUpdateEnd = 0;
        continue;
      }

      if (auto const* insertion = std::get_if<InsertRange>(&edit); insertion != nullptr)
      {
        if (insertion->trackIds.empty() || insertion->start < previousInsertEnd || insertion->start > size ||
            !checkedAdd(size, insertion->trackIds.size()))
        {
          return false;
        }

        stage = Stage::Insert;
        previousInsertEnd = insertion->start + insertion->trackIds.size();
        previousUpdateEnd = 0;
        continue;
      }

      auto const* update = std::get_if<UpdateRange>(&edit);

      if (update == nullptr || update->trackIds.empty() || update->start < previousUpdateEnd || update->start > size ||
          update->trackIds.size() > size - update->start)
      {
        return false;
      }

      previousUpdateEnd = update->start + update->trackIds.size();
    }

    return true;
  }

  Result<std::vector<TrackId>> apply(std::vector<TrackId> initial, RegularTrackEditScript const& script)
  {
    if (!validate(script, initial.size()))
    {
      return std::unexpected{makeError(Error::Code::InvalidInput, "Invalid regular track edit script")};
    }

    auto removed = std::vector<bool>(initial.size());

    for (auto const& edit : script.edits)
    {
      auto const* removal = std::get_if<RemoveRange>(&edit);

      if (removal == nullptr)
      {
        break;
      }

      for (std::size_t offset = 0; offset < removal->trackIds.size(); ++offset)
      {
        auto const index = removal->start + offset;

        if (initial[index] != removal->trackIds[offset] || removed[index])
        {
          return std::unexpected{makeError(Error::Code::InvalidInput, "Track removal identity mismatch")};
        }

        removed[index] = true;
      }
    }

    auto retained = std::vector<TrackId>{};
    retained.reserve(initial.size());

    for (std::size_t index = 0; index < initial.size(); ++index)
    {
      if (!removed[index])
      {
        retained.push_back(initial[index]);
      }
    }

    auto finalSize = retained.size();

    for (auto const& edit : script.edits)
    {
      if (auto const* insertion = std::get_if<InsertRange>(&edit); insertion != nullptr)
      {
        finalSize += insertion->trackIds.size();
      }
    }

    auto result = std::vector<TrackId>{};
    result.reserve(finalSize);
    std::size_t retainedIndex = 0;

    for (auto const& edit : script.edits)
    {
      auto const* insertion = std::get_if<InsertRange>(&edit);

      if (insertion == nullptr)
      {
        continue;
      }

      while (result.size() < insertion->start)
      {
        result.push_back(retained[retainedIndex++]);
      }

      result.append_range(insertion->trackIds);
    }

    result.insert(result.end(), retained.begin() + static_cast<std::ptrdiff_t>(retainedIndex), retained.end());

    for (auto const& edit : script.edits)
    {
      auto const* update = std::get_if<UpdateRange>(&edit);

      if (update == nullptr)
      {
        continue;
      }

      if (!std::ranges::equal(update->trackIds, std::span{result}.subspan(update->start, update->trackIds.size())))
      {
        return std::unexpected{makeError(Error::Code::InvalidInput, "Track update identity mismatch")};
      }
    }

    return result;
  }

  RegularTrackEditScript diff(std::span<TrackId const> from,
                              std::span<TrackId const> to,
                              std::span<TrackId const> updatedIds,
                              std::span<TrackId const> preferredMovedIds)
  {
    auto preferredMoved =
      boost::unordered_flat_set<TrackId, std::hash<TrackId>>{preferredMovedIds.begin(), preferredMovedIds.end()};
    auto eligibleFrom = std::vector<TrackId>{};
    auto eligibleTo = std::vector<TrackId>{};
    std::ranges::copy_if(from,
                         std::back_inserter(eligibleFrom),
                         [&preferredMoved](TrackId trackId) { return !preferredMoved.contains(trackId); });
    std::ranges::copy_if(to,
                         std::back_inserter(eligibleTo),
                         [&preferredMoved](TrackId trackId) { return !preferredMoved.contains(trackId); });
    auto const retainedIds = longestCommonSubsequence(eligibleFrom, eligibleTo);
    auto retained = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{retainedIds.begin(), retainedIds.end()};
    auto coalescer = Coalescer{};

    auto removalStart = from.size();

    while (removalStart != 0)
    {
      --removalStart;

      if (retained.contains(from[removalStart]))
      {
        continue;
      }

      auto start = removalStart;

      while (start != 0 && !retained.contains(from[start - 1]))
      {
        --start;
      }

      coalescer.appendRemove(start, from.subspan(start, removalStart - start + 1));
      removalStart = start;
    }

    for (std::size_t index = 0; index < to.size();)
    {
      if (retained.contains(to[index]))
      {
        ++index;
        continue;
      }

      auto const start = index;

      while (index < to.size() && !retained.contains(to[index]))
      {
        ++index;
      }

      coalescer.appendInsert(start, to.subspan(start, index - start));
    }

    auto updated = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{updatedIds.begin(), updatedIds.end()};

    for (std::size_t index = 0; index < to.size();)
    {
      if (!retained.contains(to[index]) || !updated.contains(to[index]))
      {
        ++index;
        continue;
      }

      auto const start = index;

      while (index < to.size() && retained.contains(to[index]) && updated.contains(to[index]))
      {
        ++index;
      }

      coalescer.appendUpdate(start, to.subspan(start, index - start));
    }

    return coalescer.take();
  }

  void Coalescer::appendRemove(std::size_t start, std::span<TrackId const> trackIds)
  {
    auto range = RemoveRange{.start = start, .trackIds = {trackIds.begin(), trackIds.end()}};

    if (auto* const previous = _script.edits.empty() ? nullptr : std::get_if<RemoveRange>(&_script.edits.back());
        previous != nullptr && start + range.trackIds.size() == previous->start)
    {
      range.trackIds.append_range(previous->trackIds);
      _script.edits.back() = std::move(range);
      return;
    }

    _script.edits.emplace_back(std::move(range));
  }

  void Coalescer::appendInsert(std::size_t start, std::span<TrackId const> trackIds)
  {
    appendCoalesced(_script.edits, InsertRange{.start = start, .trackIds = {trackIds.begin(), trackIds.end()}});
  }

  void Coalescer::appendUpdate(std::size_t start, std::span<TrackId const> trackIds)
  {
    appendCoalesced(_script.edits, UpdateRange{.start = start, .trackIds = {trackIds.begin(), trackIds.end()}});
  }

  RegularTrackEditScript Coalescer::take()
  {
    return std::exchange(_script, {});
  }
} // namespace ao::rt::delta
