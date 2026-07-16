// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct TransparentStringHash final
    {
      using is_transparent = void;

      std::size_t operator()(std::string_view value) const { return std::hash<std::string_view>{}(value); }
      std::size_t operator()(std::string const& value) const { return (*this)(std::string_view{value}); }
    };

    using OwnedValueFrequencies =
      boost::unordered_flat_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>>;

    void addValue(OwnedValueFrequencies& counts, std::string_view value, std::uint32_t frequency = 1)
    {
      if (value.empty())
      {
        return;
      }

      if (auto const iter = counts.find(value); iter != counts.end())
      {
        iter->second += frequency;
      }
      else
      {
        counts.emplace(std::string{value}, frequency);
      }
    }

    void countDictionaryId(std::span<std::uint32_t> frequencies, DictionaryId id, std::uint32_t frequency = 1)
    {
      if (id != kInvalidDictionaryId && id.raw() < frequencies.size())
      {
        frequencies[id.raw()] += frequency;
      }
    }

    void sortVocabulary(std::vector<VocabularyEntry>& entries)
    {
      std::ranges::sort(
        entries,
        [](VocabularyEntry const& lhs, VocabularyEntry const& rhs)
        { return lhs.frequency > rhs.frequency || (lhs.frequency == rhs.frequency && lhs.value < rhs.value); });
    }

    template<typename Frequencies>
    std::vector<VocabularyEntry> sortedDictionaryVocabulary(Frequencies const& frequencies,
                                                            library::DictionaryStore const& dictionary)
    {
      auto entries = std::vector<VocabularyEntry>{};
      entries.reserve(frequencies.size());

      for (auto const& entry : frequencies)
      {
        if (auto const value = dictionary.getOrDefault(entry.id); !value.empty())
        {
          entries.push_back(VocabularyEntry{.value = std::string{value}, .frequency = entry.frequency});
        }
      }

      sortVocabulary(entries);
      return entries;
    }

    void validateAggregateFields(std::span<TrackField const> fields)
    {
      for (std::size_t index = 0; index < fields.size(); ++index)
      {
        auto const field = fields[index];
        auto const optQueryField = trackFieldQueryField(field);
        auto const precedingFields = fields.first(index);
        gsl_Expects(optQueryField && (field == TrackField::Title || query::isDictionaryField(*optQueryField)));
        gsl_Expects(std::ranges::find(precedingFields, field) == precedingFields.end());
      }
    }
  } // namespace

  CompletionService::CompletionService(library::MusicLibrary const& library, LibraryChanges const& changes)
    : _library{library}
    , _ownerThread{std::this_thread::get_id()}
    , _libraryChangeSubscription{changes.onChanged(
        [this](LibraryChangeSet const& changeSet)
        {
          if (changeSet.libraryReset || !changeSet.tracksInserted.empty() || !changeSet.tracksDeleted.empty() ||
              !changeSet.tracksMutated.empty())
          {
            invalidate();
          }
        })}
  {
    for (auto const& definition : trackFieldDefinitions())
    {
      gsl_Assert(!definition.valueCompletion || supportsTrackFieldValueCompletion(definition.field));
    }
  }

  CompletionService::~CompletionService() = default;

  void CompletionService::assertOwnerThread() const
  {
    assert(std::this_thread::get_id() == _ownerThread &&
           "CompletionService accessed off its owning thread; vocabulary caches are not synchronized");
  }

  std::span<VocabularyEntry const> CompletionService::tags()
  {
    assertOwnerThread();
    ensureSnapshot();

    if (!_tagsReady)
    {
      materializeTags();
    }

    return _tags;
  }

  std::span<VocabularyEntry const> CompletionService::customKeys()
  {
    assertOwnerThread();
    ensureSnapshot();

    if (!_customKeysReady)
    {
      materializeCustomKeys();
    }

    return _customKeys;
  }

  std::span<VocabularyEntry const> CompletionService::valuesFor(TrackField field)
  {
    assertOwnerThread();

    auto const* const definition = trackFieldDefinition(field);

    if (definition == nullptr || !supportsTrackFieldValueCompletion(field))
    {
      static auto const kEmpty = std::vector<VocabularyEntry>{};
      return kEmpty;
    }

    ensureSnapshot();

    if (!trackFieldArrayAt(_valuesReady, field))
    {
      materializeValues(definition->field);
    }

    return trackFieldArrayAt(_values, field);
  }

  std::span<VocabularyEntry const> CompletionService::aggregateValues(TrackValueVocabularySpec spec)
  {
    assertOwnerThread();

    if (!std::ranges::equal(spec.fields, _aggregateFields) || spec.includeTags != _aggregateIncludesTags)
    {
      validateAggregateFields(spec.fields);
      _aggregateFields.assign(spec.fields.begin(), spec.fields.end());
      _aggregateIncludesTags = spec.includeTags;
      _aggregateValuesReady = false;
    }

    if (_aggregateFields.empty() && !_aggregateIncludesTags)
    {
      _aggregateValues.clear();
      _aggregateValuesReady = true;
      return _aggregateValues;
    }

    ensureSnapshot();

    if (!_aggregateValuesReady)
    {
      materializeAggregateValues();
    }

    return _aggregateValues;
  }

  void CompletionService::invalidate()
  {
    assertOwnerThread();
    _snapshotDirty = true;
  }

  void CompletionService::ensureSnapshot()
  {
    if (_snapshotDirty)
    {
      rebuildSnapshot();
    }
  }

  void CompletionService::rebuildSnapshot()
  {
    struct FieldSource final
    {
      TrackField field;
      query::Field queryField;
    };

    auto const transaction = _library.readTransaction();
    auto const dictionarySize = _library.dictionary().size();
    auto titleCounts = OwnedValueFrequencies{};
    titleCounts.reserve(dictionarySize);
    auto tagCounts = std::vector<std::uint32_t>(dictionarySize + 1);
    auto customKeyCounts = std::vector<std::uint32_t>(dictionarySize + 1);
    auto valueCounts = std::array<std::vector<std::uint32_t>, kTrackFieldCount>{};
    auto fieldSources = std::vector<FieldSource>{};

    for (auto const& definition : trackFieldDefinitions())
    {
      if (definition.optQueryField && query::isDictionaryField(*definition.optQueryField))
      {
        fieldSources.push_back(FieldSource{.field = definition.field, .queryField = *definition.optQueryField});
        trackFieldArrayAt(valueCounts, definition.field).resize(dictionarySize + 1);
      }
    }

    auto const reader = _library.tracks().reader(transaction);

    for (auto const& [_, view] : reader.both())
    {
      addValue(titleCounts, view.metadata().title());

      for (auto const source : fieldSources)
      {
        countDictionaryId(
          trackFieldArrayAt(valueCounts, source.field), query::dictionaryFieldId(view, source.queryField));
      }

      for (auto const tagId : view.tags())
      {
        countDictionaryId(tagCounts, tagId);
      }

      for (auto const dictionaryId : view.customMetadata() | std::views::keys)
      {
        countDictionaryId(customKeyCounts, dictionaryId);
      }
    }

    auto compress = [](std::span<std::uint32_t const> counts)
    {
      auto frequencies = std::vector<DictionaryFrequency>{};

      for (std::size_t rawId = 1; rawId < counts.size(); ++rawId)
      {
        if (auto const frequency = counts[rawId]; frequency != 0)
        {
          frequencies.push_back(DictionaryFrequency{
            .id = DictionaryId{static_cast<std::uint32_t>(rawId)},
            .frequency = frequency,
          });
        }
      }

      return frequencies;
    };

    auto titleFrequencies = std::vector<VocabularyEntry>{};
    titleFrequencies.reserve(titleCounts.size());

    for (auto const& [value, frequency] : titleCounts)
    {
      titleFrequencies.push_back(VocabularyEntry{.value = value, .frequency = frequency});
    }

    auto valueFrequencies = std::array<std::vector<DictionaryFrequency>, kTrackFieldCount>{};

    for (auto const source : fieldSources)
    {
      trackFieldArrayAt(valueFrequencies, source.field) = compress(trackFieldArrayAt(valueCounts, source.field));
    }

    _titleFrequencies = std::move(titleFrequencies);
    _tagFrequencies = compress(tagCounts);
    _customKeyFrequencies = compress(customKeyCounts);
    _valueFrequencies = std::move(valueFrequencies);

    _tags.clear();
    _customKeys.clear();
    _aggregateValues.clear();

    for (auto& values : _values)
    {
      values.clear();
    }

    _tagsReady = false;
    _customKeysReady = false;
    _aggregateValuesReady = false;
    _valuesReady.fill(false);
    _snapshotDirty = false;
  }

  void CompletionService::materializeTags()
  {
    _tags = sortedDictionaryVocabulary(_tagFrequencies, _library.dictionary());
    _tagsReady = true;
  }

  void CompletionService::materializeCustomKeys()
  {
    _customKeys = sortedDictionaryVocabulary(_customKeyFrequencies, _library.dictionary());
    _customKeysReady = true;
  }

  void CompletionService::materializeValues(TrackField field)
  {
    trackFieldArrayAt(_values, field) =
      sortedDictionaryVocabulary(trackFieldArrayAt(_valueFrequencies, field), _library.dictionary());
    trackFieldArrayAt(_valuesReady, field) = true;
  }

  void CompletionService::materializeAggregateValues()
  {
    auto counts = OwnedValueFrequencies{};
    counts.reserve(_library.dictionary().size());
    auto dictionaryFrequencies = std::vector<std::uint32_t>(_library.dictionary().size() + 1);

    for (auto const field : _aggregateFields)
    {
      if (field == TrackField::Title)
      {
        for (auto const& entry : _titleFrequencies)
        {
          addValue(counts, entry.value, entry.frequency);
        }

        continue;
      }

      for (auto const& entry : trackFieldArrayAt(_valueFrequencies, field))
      {
        countDictionaryId(dictionaryFrequencies, entry.id, entry.frequency);
      }
    }

    if (_aggregateIncludesTags)
    {
      for (auto const& entry : _tagFrequencies)
      {
        countDictionaryId(dictionaryFrequencies, entry.id, entry.frequency);
      }
    }

    auto const& dictionary = _library.dictionary();

    for (std::size_t rawId = 1; rawId < dictionaryFrequencies.size(); ++rawId)
    {
      if (auto const frequency = dictionaryFrequencies[rawId]; frequency != 0)
      {
        addValue(counts, dictionary.getOrDefault(DictionaryId{static_cast<std::uint32_t>(rawId)}), frequency);
      }
    }

    auto values = std::vector<VocabularyEntry>{};
    values.reserve(counts.size());

    for (auto const& [value, frequency] : counts)
    {
      values.push_back(VocabularyEntry{.value = value, .frequency = frequency});
    }

    _aggregateValues = std::move(values);
    _aggregateValuesReady = true;
  }
} // namespace ao::rt
