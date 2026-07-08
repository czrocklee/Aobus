// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "../TrackField.h"
#include <ao/CoreIds.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;

  struct VocabularyEntry final
  {
    std::string value;
    std::uint32_t frequency = 0;
  };

  class CompletionService final
  {
  public:
    CompletionService(library::MusicLibrary& library, LibraryChanges const& changes);
    ~CompletionService();

    CompletionService(CompletionService const&) = delete;
    CompletionService& operator=(CompletionService const&) = delete;
    CompletionService(CompletionService&&) = delete;
    CompletionService& operator=(CompletionService&&) = delete;

    std::span<VocabularyEntry const> tags();
    std::span<VocabularyEntry const> customKeys();
    std::span<VocabularyEntry const> valuesFor(TrackField field);

    void markDirty(std::span<TrackId const> trackIds);

  private:
    // The cached vocabularies carry no synchronization: every access (lazy rebuilds and dirty
    // invalidation) must happen on the thread that constructed the service. This holds today because
    // every LibraryChanges::tracksMutated emit is marshalled onto the main thread before the
    // subscription fires. The assert turns a future off-thread emit into a loud failure instead of a
    // silent data race.
    void assertOwnerThread() const;
    void rebuildDirtyValueVocabularies(bool cold);

    library::MusicLibrary& _library;
    std::thread::id _ownerThread;
    Subscription _tracksMutatedSubscription;

    bool _tagsDirty = true;
    bool _customKeysDirty = true;
    std::array<bool, kTrackFieldCount> _valueDirty{};

    std::vector<VocabularyEntry> _tags;
    std::vector<VocabularyEntry> _customKeys;
    std::array<std::vector<VocabularyEntry>, kTrackFieldCount> _values;
  };
} // namespace ao::rt
