// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/CompletionService.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    using LoadMode = library::TrackStore::Reader::LoadMode;

    struct ValueCompletionFieldSpec final
    {
      TrackField field;
      bool coldStore;
    };

    constexpr auto kValueCompletionFields = std::to_array<ValueCompletionFieldSpec>({
      {.field = TrackField::Artist, .coldStore = false},
      {.field = TrackField::Album, .coldStore = false},
      {.field = TrackField::AlbumArtist, .coldStore = false},
      {.field = TrackField::Genre, .coldStore = false},
      {.field = TrackField::Composer, .coldStore = false},
      {.field = TrackField::Work, .coldStore = true},
    });

    using CountMap = std::unordered_map<std::string_view, std::uint32_t>;

    constexpr ValueCompletionFieldSpec const* valueCompletionSpecForField(TrackField field)
    {
      for (auto const& spec : kValueCompletionFields)
      {
        if (spec.field == field)
        {
          return &spec;
        }
      }

      return nullptr;
    }

    // kValueCompletionFields carries the per-field storage tier this service scans, while the
    // TrackField definition table's valueCompletion flag is the public predicate every caller uses
    // (trackFieldSupportsValueCompletion: GTK editors, MetadataValueCompleter, QueryExpressionCompleter).
    // The two must name the same set of fields: a field listed here but unflagged would be rebuilt yet
    // never queried, and a flagged field missing here would pass the caller gate but resolve to an empty
    // vocabulary -- both silent. Validate they agree once, instead of letting the lists drift.
    void assertValueCompletionFieldsConsistent()
    {
      for (auto const& spec : kValueCompletionFields)
      {
        assert(trackFieldSupportsValueCompletion(spec.field) &&
               "kValueCompletionFields lists a field the TrackField table does not flag for value completion");
      }

      for (auto const& def : trackFieldDefinitions())
      {
        assert((!def.valueCompletion || valueCompletionSpecForField(def.field) != nullptr) &&
               "TrackField table flags a value-completion field absent from kValueCompletionFields");
      }
    }

    std::size_t fieldIndex(TrackField field)
    {
      return static_cast<std::size_t>(field);
    }

    void addValue(CountMap& counts, std::string_view value)
    {
      if (!value.empty())
      {
        ++counts[value];
      }
    }

    std::vector<VocabularyEntry> sortedVocabulary(CountMap const& counts)
    {
      auto entries = std::vector<VocabularyEntry>{};
      entries.reserve(counts.size());

      for (auto const& [value, frequency] : counts)
      {
        entries.push_back(VocabularyEntry{.value = std::string{value}, .frequency = frequency});
      }

      std::ranges::sort(
        entries,
        [](VocabularyEntry const& lhs, VocabularyEntry const& rhs)
        { return lhs.frequency > rhs.frequency || (lhs.frequency == rhs.frequency && lhs.value < rhs.value); });

      return entries;
    }

    DictionaryId dictionaryIdForField(TrackField field, library::TrackView const& view)
    {
      switch (field)
      {
        case TrackField::Artist: return view.metadata().artistId();
        case TrackField::Album: return view.metadata().albumId();
        case TrackField::AlbumArtist: return view.metadata().albumArtistId();
        case TrackField::Genre: return view.metadata().genreId();
        case TrackField::Composer: return view.metadata().composerId();
        case TrackField::Work: return view.metadata().workId();
        default: return kInvalidDictionaryId;
      }
    }

    bool hasDirtyField(bool coldStore, std::array<bool, kTrackFieldCount> const& dirty)
    {
      return std::ranges::any_of(kValueCompletionFields,
                                 [coldStore, &dirty](ValueCompletionFieldSpec const& spec)
                                 { return spec.coldStore == coldStore && dirty.at(fieldIndex(spec.field)); });
    }
  } // namespace

  CompletionService::CompletionService(library::MusicLibrary& library, LibraryMutationService& mutation)
    : _library{library}
    , _ownerThread{std::this_thread::get_id()}
    , _tracksMutatedSubscription{
        mutation.onTracksMutated([this](std::vector<TrackId> const& trackIds) { markDirty(trackIds); })}
  {
    assertValueCompletionFieldsConsistent();
    _valueDirty.fill(true);
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

    if (_tagsDirty)
    {
      auto counts = CountMap{};
      auto const txn = _library.readTransaction();
      auto const reader = _library.tracks().reader(txn);
      auto const& dictionary = _library.dictionary();

      for (auto const& [_, view] : reader.hot())
      {
        for (auto const tagId : view.tags())
        {
          addValue(counts, dictionary.getOrDefault(tagId));
        }
      }

      _tags = sortedVocabulary(counts);
      _tagsDirty = false;
    }

    return _tags;
  }

  std::span<VocabularyEntry const> CompletionService::customKeys()
  {
    assertOwnerThread();

    if (_customKeysDirty)
    {
      auto counts = CountMap{};
      auto const txn = _library.readTransaction();
      auto const reader = _library.tracks().reader(txn);
      auto const& dictionary = _library.dictionary();

      for (auto const& [_, view] : reader.cold())
      {
        for (auto const& [dictId, _] : view.customMetadata())
        {
          addValue(counts, dictionary.getOrDefault(dictId));
        }
      }

      _customKeys = sortedVocabulary(counts);
      _customKeysDirty = false;
    }

    return _customKeys;
  }

  std::span<VocabularyEntry const> CompletionService::valuesFor(TrackField field)
  {
    assertOwnerThread();

    auto const* const spec = valueCompletionSpecForField(field);

    if (spec == nullptr)
    {
      static auto const kEmpty = std::vector<VocabularyEntry>{};
      return kEmpty;
    }

    if (auto const index = fieldIndex(field); _valueDirty.at(index))
    {
      rebuildDirtyValueVocabularies(spec->coldStore);
    }

    return _values.at(fieldIndex(field));
  }

  void CompletionService::markDirty(std::span<TrackId const> /*trackIds*/)
  {
    assertOwnerThread();

    _tagsDirty = true;
    _customKeysDirty = true;

    for (auto const& spec : kValueCompletionFields)
    {
      _valueDirty.at(fieldIndex(spec.field)) = true;
    }
  }

  void CompletionService::rebuildDirtyValueVocabularies(bool cold)
  {
    auto const mode = cold ? LoadMode::Cold : LoadMode::Hot;

    if (!hasDirtyField(cold, _valueDirty))
    {
      return;
    }

    auto counts = std::array<CountMap, kTrackFieldCount>{};
    auto const txn = _library.readTransaction();
    auto const reader = _library.tracks().reader(txn);
    auto const& dictionary = _library.dictionary();

    for (auto iter = reader.begin(mode); iter != reader.end(mode); ++iter)
    {
      auto const& [_, view] = *iter;

      for (auto const& spec : kValueCompletionFields)
      {
        if (spec.coldStore == cold && _valueDirty.at(fieldIndex(spec.field)))
        {
          addValue(counts.at(fieldIndex(spec.field)), dictionary.getOrDefault(dictionaryIdForField(spec.field, view)));
        }
      }
    }

    for (auto const& spec : kValueCompletionFields)
    {
      if (spec.coldStore == cold && _valueDirty.at(fieldIndex(spec.field)))
      {
        auto const index = fieldIndex(spec.field);
        _values.at(index) = sortedVocabulary(counts.at(index));
        _valueDirty.at(index) = false;
      }
    }
  }
} // namespace ao::rt
