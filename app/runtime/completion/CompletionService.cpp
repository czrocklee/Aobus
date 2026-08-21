// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionService.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <array>
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

    void sortVocabulary(std::vector<VocabularyEntry>& entries, TextOrderingPolicy const* const textOrderingPolicy)
    {
      if (textOrderingPolicy != nullptr)
      {
        struct OrderedEntry final
        {
          VocabularyEntry entry;
          std::string sortKey;
        };

        auto ordered = std::vector<OrderedEntry>{};
        ordered.reserve(entries.size());

        for (auto& entry : entries)
        {
          auto sortKey = std::string{};
          auto const keyRes = textOrderingPolicy->makeSortKeyInto(sortKey, entry.value);
          AO_INVARIANT(keyRes.has_value(),
                       "Admitted completion text failed locale sort-key derivation: {}",
                       keyRes.error().message);
          ordered.push_back(OrderedEntry{.entry = std::move(entry), .sortKey = std::move(sortKey)});
        }

        std::ranges::sort(ordered,
                          [](OrderedEntry const& lhs, OrderedEntry const& rhs)
                          {
                            if (lhs.entry.frequency != rhs.entry.frequency)
                            {
                              return lhs.entry.frequency > rhs.entry.frequency;
                            }

                            if (auto const sortKeyOrder = lhs.sortKey.compare(rhs.sortKey); sortKeyOrder != 0)
                            {
                              return sortKeyOrder < 0;
                            }

                            return lhs.entry.value < rhs.entry.value;
                          });

        entries.clear();

        for (auto& entry : ordered)
        {
          entries.push_back(std::move(entry.entry));
        }

        return;
      }

      std::ranges::sort(
        entries,
        [](VocabularyEntry const& lhs, VocabularyEntry const& rhs)
        { return lhs.frequency > rhs.frequency || (lhs.frequency == rhs.frequency && lhs.value < rhs.value); });
    }

    template<typename Frequencies, typename GetAliases>
    std::vector<VocabularyEntry> sortedDictionaryVocabulary(Frequencies const& frequencies,
                                                            library::DictionaryStore const& dictionary,
                                                            TextOrderingPolicy const* textOrderingPolicy,
                                                            GetAliases getAliases)
    {
      auto entries = std::vector<VocabularyEntry>{};
      entries.reserve(frequencies.size());

      for (auto const& entry : frequencies)
      {
        if (auto const value = dictionary.getOrDefault(entry.id); !value.empty())
        {
          entries.push_back(VocabularyEntry{
            .value = std::string{value},
            .frequency = entry.frequency,
            .aliases = std::invoke(getAliases, entry.id, value),
          });
        }
      }

      sortVocabulary(entries, textOrderingPolicy);
      return entries;
    }

    void validateAggregateFields(std::span<TrackField const> fields)
    {
      for (std::size_t index = 0; index < fields.size(); ++index)
      {
        auto const field = fields[index];
        auto const optQueryField = trackFieldQueryField(field);
        auto const precedingFields = fields.first(index);
        AO_INVARIANT(optQueryField && (field == TrackField::Title || query::isDictionaryField(*optQueryField)));
        AO_INVARIANT(std::ranges::find(precedingFields, field) == precedingFields.end());
      }
    }
  } // namespace

  CompletionService::CompletionService(library::MusicLibrary const& library,
                                       LibraryChanges const& changes,
                                       TextOrderingPolicy const* textOrderingPolicy,
                                       CompletionAliasPolicy const* completionAliasPolicy)
    : _library{library}
    , _textOrderingPolicy{textOrderingPolicy}
    , _completionAliasPolicy{completionAliasPolicy}
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
      AO_INVARIANT(!definition.valueCompletion || supportsTrackFieldValueCompletion(definition.field));
    }
  }

  CompletionService::~CompletionService() = default;

  void CompletionService::requireOwnerThread() const
  {
    AO_EXPECTS(std::this_thread::get_id() == _ownerThread,
               "CompletionService accessed off its owning thread; vocabulary caches are not synchronized");
  }

  std::span<VocabularyEntry const> CompletionService::tags()
  {
    requireOwnerThread();
    ensureSnapshot();

    if (!_tagsReady)
    {
      materializeTags();
    }

    return _tags;
  }

  std::span<VocabularyEntry const> CompletionService::customKeys()
  {
    requireOwnerThread();
    ensureSnapshot();

    if (!_customKeysReady)
    {
      materializeCustomKeys();
    }

    return _customKeys;
  }

  std::span<VocabularyEntry const> CompletionService::valuesFor(TrackField field)
  {
    requireOwnerThread();

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
    requireOwnerThread();

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
    requireOwnerThread();
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

    for (auto const& [_, view] : reader)
    {
      if (!view.isHotValid() || !view.isColdValid())
      {
        continue;
      }

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

    // Retire every span borrower before replacing the snapshot-owned alias records.
    _tags.clear();
    _customKeys.clear();
    _aggregateValues.clear();

    for (auto& values : _values)
    {
      values.clear();
    }

    _titleFrequencies = std::move(titleFrequencies);
    _tagFrequencies = compress(tagCounts);
    _customKeyFrequencies = compress(customKeyCounts);
    _valueFrequencies = std::move(valueFrequencies);

    if (_completionAliasPolicy != nullptr)
    {
      _dictionaryAliases = std::vector<AliasRecord>(dictionarySize + 1);
      _titleAliases = std::vector<AliasRecord>(_titleFrequencies.size());
    }
    else
    {
      _dictionaryAliases.clear();
      _titleAliases.clear();
    }

    _tagsReady = false;
    _customKeysReady = false;
    _aggregateValuesReady = false;
    _valuesReady.fill(false);
    _snapshotDirty = false;
  }

  void CompletionService::materializeTags()
  {
    _tags = sortedDictionaryVocabulary(_tagFrequencies,
                                       _library.dictionary(),
                                       _textOrderingPolicy,
                                       [this](DictionaryId const id, std::string_view const text)
                                       { return aliasesForDictionary(id, text); });
    _tagsReady = true;
  }

  void CompletionService::materializeCustomKeys()
  {
    _customKeys = sortedDictionaryVocabulary(_customKeyFrequencies,
                                             _library.dictionary(),
                                             _textOrderingPolicy,
                                             [this](DictionaryId const id, std::string_view const text)
                                             { return aliasesForDictionary(id, text); });
    _customKeysReady = true;
  }

  void CompletionService::materializeValues(TrackField field)
  {
    trackFieldArrayAt(_values, field) = sortedDictionaryVocabulary(
      trackFieldArrayAt(_valueFrequencies, field),
      _library.dictionary(),
      _textOrderingPolicy,
      [this](DictionaryId const id, std::string_view const text) { return aliasesForDictionary(id, text); });
    trackFieldArrayAt(_valuesReady, field) = true;
  }

  std::span<std::string const> CompletionService::aliasesForDictionary(DictionaryId const id,
                                                                       std::string_view const text)
  {
    if (_completionAliasPolicy == nullptr)
    {
      return {};
    }

    AO_INVARIANT(id != kInvalidDictionaryId && id.raw() < _dictionaryAliases.size());
    return resolveAliases(_dictionaryAliases[id.raw()], text);
  }

  std::span<std::string const> CompletionService::aliasesForTitle(std::size_t const titleIndex,
                                                                  std::string_view const text)
  {
    if (_completionAliasPolicy == nullptr)
    {
      return {};
    }

    AO_INVARIANT(titleIndex < _titleAliases.size());
    return resolveAliases(_titleAliases[titleIndex], text);
  }

  std::span<std::string const> CompletionService::aliasesFor(AliasHandle const handle, std::string_view const text)
  {
    switch (handle.source)
    {
      case AliasSource::Dictionary:
        return aliasesForDictionary(DictionaryId{static_cast<std::uint32_t>(handle.index)}, text);
      case AliasSource::Title: return aliasesForTitle(handle.index, text);
    }

    AO_FATAL("Unhandled completion alias source");
  }

  std::span<std::string const> CompletionService::resolveAliases(AliasRecord& record, std::string_view const text)
  {
    if (_completionAliasPolicy == nullptr)
    {
      return {};
    }

    if (!record.resolved)
    {
      auto const result = _completionAliasPolicy->makeAliasesInto(record.values, text);
      AO_INVARIANT(result.has_value(), "Admitted completion text failed alias derivation: {}", result.error().message);
      record.resolved = true;
    }

    return record.values;
  }

  void CompletionService::materializeAggregateValues()
  {
    struct AggregateValue final
    {
      std::uint32_t frequency = 0;
      AliasHandle aliasHandle;
    };

    using OwnedAggregateValues =
      boost::unordered_flat_map<std::string, AggregateValue, TransparentStringHash, std::equal_to<>>;

    auto counts = OwnedAggregateValues{};
    counts.reserve(_library.dictionary().size());
    auto dictionaryFrequencies = std::vector<std::uint32_t>(_library.dictionary().size() + 1);
    auto const addAggregateValue =
      [&](std::string_view const value, std::uint32_t const frequency, AliasHandle const aliasHandle)
    {
      if (value.empty())
      {
        return;
      }

      if (auto const iter = counts.find(value); iter != counts.end())
      {
        iter->second.frequency += frequency;

        if (aliasHandle.source == AliasSource::Dictionary && iter->second.aliasHandle.source != AliasSource::Dictionary)
        {
          iter->second.aliasHandle = aliasHandle;
        }
      }
      else
      {
        counts.emplace(std::string{value}, AggregateValue{.frequency = frequency, .aliasHandle = aliasHandle});
      }
    };

    for (auto const field : _aggregateFields)
    {
      if (field == TrackField::Title)
      {
        for (std::size_t index = 0; index < _titleFrequencies.size(); ++index)
        {
          auto const& entry = _titleFrequencies[index];
          addAggregateValue(entry.value, entry.frequency, AliasHandle{.source = AliasSource::Title, .index = index});
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
        addAggregateValue(dictionary.getOrDefault(DictionaryId{static_cast<std::uint32_t>(rawId)}),
                          frequency,
                          AliasHandle{.source = AliasSource::Dictionary, .index = rawId});
      }
    }

    auto values = std::vector<VocabularyEntry>{};
    values.reserve(counts.size());

    for (auto const& [value, aggregate] : counts)
    {
      values.push_back(VocabularyEntry{
        .value = value,
        .frequency = aggregate.frequency,
        .aliases = aliasesFor(aggregate.aliasHandle, value),
      });
    }

    _aggregateValues = std::move(values);
    _aggregateValuesReady = true;
  }
} // namespace ao::rt
