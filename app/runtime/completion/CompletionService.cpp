// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionService.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
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
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

    using DictionaryCounts = boost::unordered_flat_map<std::uint32_t, std::uint32_t>;

    void countDictionaryId(DictionaryCounts& frequencies, DictionaryId id)
    {
      if (id != kInvalidDictionaryId)
      {
        ++frequencies[id.raw()];
      }
    }

    struct CreditDictionaryIds final
    {
      boost::unordered_flat_set<std::uint32_t> names;
      boost::unordered_flat_set<std::uint32_t> roles;
      boost::unordered_flat_set<std::uint32_t> categoryNames;
    };

    template<typename CountsForKind>
    void countTrackCredits(library::PerformanceView const& performance,
                           DictionaryCounts& nameCounts,
                           DictionaryCounts& roleCounts,
                           CreditDictionaryIds& ids,
                           CountsForKind countsForKind)
    {
      ids.names.clear();
      ids.roles.clear();

      for (auto const kind : {library::CreditKind::Conductor,
                              library::CreditKind::Ensemble,
                              library::CreditKind::Soloist,
                              library::CreditKind::Performer})
      {
        ids.categoryNames.clear();

        for (auto const& entry : performance.credits(kind))
        {
          ids.names.insert(entry.nameId.raw());
          ids.roles.insert(entry.roleId.raw());
          ids.categoryNames.insert(entry.nameId.raw());
        }

        for (auto const rawId : ids.categoryNames)
        {
          countDictionaryId(std::invoke(countsForKind, kind), DictionaryId{rawId});
        }
      }

      for (auto const rawId : ids.names)
      {
        countDictionaryId(nameCounts, DictionaryId{rawId});
      }

      for (auto const rawId : ids.roles)
      {
        countDictionaryId(roleCounts, DictionaryId{rawId});
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
            .aliases = std::invoke(getAliases, entry.aliasIndex, value),
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
        AO_INVARIANT(optQueryField && (field == TrackField::Title || query::isDictionaryField(*optQueryField) ||
                                       query::isCreditField(*optQueryField)));
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
    return vocabularyEntries(Vocabulary::Tags);
  }

  std::span<VocabularyEntry const> CompletionService::customKeys()
  {
    return vocabularyEntries(Vocabulary::CustomKeys);
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

    if (auto const optVocabulary = categoryVocabulary(field); optVocabulary)
    {
      return vocabularyEntries(*optVocabulary);
    }

    ensureSnapshot();

    if (!trackFieldArrayAt(_valuesReady, field))
    {
      materializeValues(definition->field);
    }

    return trackFieldArrayAt(_values, field);
  }

  std::span<VocabularyEntry const> CompletionService::creditNames()
  {
    return vocabularyEntries(Vocabulary::CreditNames);
  }

  std::span<VocabularyEntry const> CompletionService::creditNames(library::CreditKind kind)
  {
    return vocabularyEntries(creditVocabulary(kind));
  }

  std::span<VocabularyEntry const> CompletionService::creditRoles()
  {
    return vocabularyEntries(Vocabulary::CreditRoles);
  }

  CompletionService::Vocabulary CompletionService::creditVocabulary(library::CreditKind kind)
  {
    switch (kind)
    {
      case library::CreditKind::Conductor: return Vocabulary::ConductorNames;
      case library::CreditKind::Ensemble: return Vocabulary::EnsembleNames;
      case library::CreditKind::Soloist: return Vocabulary::SoloistNames;
      case library::CreditKind::Performer: return Vocabulary::PerformerNames;
    }

    AO_FATAL("Invalid credit completion kind");
  }

  std::optional<CompletionService::Vocabulary> CompletionService::categoryVocabulary(TrackField field)
  {
    if (auto const optKind = creditKindForTrackField(field); optKind)
    {
      return creditVocabulary(*optKind);
    }

    return std::nullopt;
  }

  CompletionService::VocabularyCache& CompletionService::vocabularyCache(Vocabulary vocabulary)
  {
    return _vocabularies[static_cast<std::size_t>(vocabulary)];
  }

  std::span<VocabularyEntry const> CompletionService::vocabularyEntries(Vocabulary vocabulary)
  {
    requireOwnerThread();
    ensureSnapshot();
    auto& cache = vocabularyCache(vocabulary);

    if (!cache.ready)
    {
      cache.entries = sortedDictionaryVocabulary(cache.frequencies,
                                                 _library.dictionary(),
                                                 _textOrderingPolicy,
                                                 [this](std::size_t aliasIndex, std::string_view text)
                                                 { return aliasesForDictionary(aliasIndex, text); });
      cache.ready = true;
    }

    return cache.entries;
  }

  std::span<CompletionService::DictionaryFrequency const> CompletionService::fieldFrequencies(TrackField field)
  {
    if (auto const optVocabulary = categoryVocabulary(field); optVocabulary)
    {
      return vocabularyCache(*optVocabulary).frequencies;
    }

    return trackFieldArrayAt(_valueFrequencies, field);
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
    auto const reader = _library.tracks().reader(transaction);
    auto titleCounts = OwnedValueFrequencies{};
    // The validated store guarantees matching hot/cold counts; reserve for the first build too.
    titleCounts.reserve(reader.entryCount());
    auto vocabularyCounts = std::array<DictionaryCounts, static_cast<std::size_t>(Vocabulary::Count)>{};

    for (std::size_t index = 0; index < vocabularyCounts.size(); ++index)
    {
      vocabularyCounts[index].reserve(_vocabularies[index].frequencies.size());
    }

    auto const countsFor = [&](Vocabulary vocabulary) -> DictionaryCounts&
    { return vocabularyCounts[static_cast<std::size_t>(vocabulary)]; };
    auto creditIds = CreditDictionaryIds{};
    auto valueCounts = std::array<DictionaryCounts, kTrackFieldCount>{};
    auto fieldSources = std::vector<FieldSource>{};

    for (auto const& definition : trackFieldDefinitions())
    {
      if (definition.optQueryField && query::isDictionaryField(*definition.optQueryField))
      {
        fieldSources.push_back(FieldSource{.field = definition.field, .queryField = *definition.optQueryField});
        trackFieldArrayAt(valueCounts, definition.field)
          .reserve(trackFieldArrayAt(_valueFrequencies, definition.field).size());
      }
    }

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
        countDictionaryId(countsFor(Vocabulary::Tags), tagId);
      }

      for (auto const dictionaryId : view.customMetadata() | std::views::keys)
      {
        countDictionaryId(countsFor(Vocabulary::CustomKeys), dictionaryId);
      }

      countTrackCredits(view.performance(),
                        countsFor(Vocabulary::CreditNames),
                        countsFor(Vocabulary::CreditRoles),
                        creditIds,
                        [&](library::CreditKind kind) -> DictionaryCounts&
                        { return countsFor(creditVocabulary(kind)); });
    }

    auto compress = [](DictionaryCounts const& counts)
    {
      auto frequencies = std::vector<DictionaryFrequency>{};
      frequencies.reserve(counts.size());

      for (auto const& [rawId, frequency] : counts)
      {
        frequencies.push_back(DictionaryFrequency{.id = DictionaryId{rawId}, .frequency = frequency});
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
    for (auto& cache : _vocabularies)
    {
      cache.entries.clear();
    }

    _aggregateValues.clear();

    for (auto& values : _values)
    {
      values.clear();
    }

    _titleFrequencies = std::move(titleFrequencies);

    for (std::size_t index = 0; index < vocabularyCounts.size(); ++index)
    {
      _vocabularies[index].frequencies = compress(vocabularyCounts[index]);
      _vocabularies[index].ready = false;
    }

    _valueFrequencies = std::move(valueFrequencies);

    _aggregateValuesReady = false;
    _valuesReady.fill(false);

    if (_completionAliasPolicy == nullptr)
    {
      _dictionaryAliases = std::vector<AliasRecord>{};
      _titleAliases = std::vector<AliasRecord>{};
      _snapshotDirty = false;
      return;
    }

    std::size_t entryCount = 0;

    for (auto const& cache : _vocabularies)
    {
      entryCount += cache.frequencies.size();
    }

    for (auto const source : fieldSources)
    {
      entryCount += trackFieldArrayAt(_valueFrequencies, source.field).size();
    }

    auto aliasIndices = boost::unordered_flat_map<std::uint32_t, std::uint32_t>{};
    aliasIndices.reserve(entryCount);
    auto collectAliases = [&](std::span<DictionaryFrequency> const frequencies)
    {
      for (auto& entry : frequencies)
      {
        entry.aliasIndex =
          aliasIndices.try_emplace(entry.id.raw(), static_cast<std::uint32_t>(aliasIndices.size())).first->second;
      }
    };

    for (auto& cache : _vocabularies)
    {
      collectAliases(cache.frequencies);
    }

    for (auto const source : fieldSources)
    {
      collectAliases(trackFieldArrayAt(_valueFrequencies, source.field));
    }

    _dictionaryAliases = std::vector<AliasRecord>(aliasIndices.size());
    _titleAliases = std::vector<AliasRecord>(_titleFrequencies.size());
    _snapshotDirty = false;
  }

  void CompletionService::materializeValues(TrackField field)
  {
    trackFieldArrayAt(_values, field) =
      sortedDictionaryVocabulary(trackFieldArrayAt(_valueFrequencies, field),
                                 _library.dictionary(),
                                 _textOrderingPolicy,
                                 [this](std::size_t const aliasIndex, std::string_view const text)
                                 { return aliasesForDictionary(aliasIndex, text); });
    trackFieldArrayAt(_valuesReady, field) = true;
  }

  std::span<std::string const> CompletionService::aliasesForDictionary(std::size_t const aliasIndex,
                                                                       std::string_view const text)
  {
    if (_completionAliasPolicy == nullptr)
    {
      return {};
    }

    AO_INVARIANT(aliasIndex < _dictionaryAliases.size(),
                 "Dictionary alias index {} exceeds record count {}",
                 aliasIndex,
                 _dictionaryAliases.size());
    return resolveAliases(_dictionaryAliases[aliasIndex], text);
  }

  std::span<std::string const> CompletionService::aliasesForTitle(std::size_t const titleIndex,
                                                                  std::string_view const text)
  {
    if (_completionAliasPolicy == nullptr)
    {
      return {};
    }

    AO_INVARIANT(titleIndex < _titleAliases.size(),
                 "Title alias index {} exceeds record count {}",
                 titleIndex,
                 _titleAliases.size());
    return resolveAliases(_titleAliases[titleIndex], text);
  }

  std::span<std::string const> CompletionService::aliasesFor(AliasHandle const handle, std::string_view const text)
  {
    switch (handle.source)
    {
      case AliasSource::Dictionary: return aliasesForDictionary(handle.index, text);
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
      auto const res = _completionAliasPolicy->makeAliasesInto(record.values, text);
      AO_INVARIANT(res.has_value(), "Admitted completion text failed alias derivation: {}", res.error().message);
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

    auto const& tagFrequencies = vocabularyCache(Vocabulary::Tags).frequencies;
    std::size_t entryCount = _aggregateIncludesTags ? tagFrequencies.size() : 0;

    for (auto const field : _aggregateFields)
    {
      entryCount += field == TrackField::Title ? _titleFrequencies.size() : fieldFrequencies(field).size();
    }

    auto counts = OwnedAggregateValues{};
    counts.reserve(entryCount);
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

    auto const& dictionary = _library.dictionary();

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

      for (auto const& entry : fieldFrequencies(field))
      {
        addAggregateValue(dictionary.getOrDefault(entry.id),
                          entry.frequency,
                          AliasHandle{.source = AliasSource::Dictionary, .index = entry.aliasIndex});
      }
    }

    if (_aggregateIncludesTags)
    {
      for (auto const& entry : tagFrequencies)
      {
        addAggregateValue(dictionary.getOrDefault(entry.id),
                          entry.frequency,
                          AliasHandle{.source = AliasSource::Dictionary, .index = entry.aliasIndex});
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

  void CompletionService::setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const> policyPtr)
  {
    requireOwnerThread();
    _updatedTextOrderingPolicyPtr = std::move(policyPtr);
    _textOrderingPolicy = _updatedTextOrderingPolicyPtr.get();

    // Preserve frequencies and aliases; only materialized ordering is stale.
    for (auto& cache : _vocabularies)
    {
      cache.ready = false;
    }

    _aggregateValuesReady = false;
    _valuesReady.fill(false);
  }
} // namespace ao::rt
