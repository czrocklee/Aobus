// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>

#include <memory>
#include <optional>

namespace ao::lmdb
{
  class Environment;
  class WriteTransaction;
}

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
  }

  class FileManifestStore;
  class ListStore;
  class MetadataStore;
  class MusicLibrary;
  class ResourceStore;
  class TrackStore;

  /**
   * Owns one coherent library write, including its transaction-local dictionary.
   *
   * Dropping the object aborts the native transaction and discards staged
   * dictionary mappings. Only commit() can publish those mappings.
   */
  class [[nodiscard]] WriteTransaction final
  {
  public:
    struct Options final
    {
      // Data-only test seam that terminates the native transaction at the
      // commit boundary and returns the supplied failure. It cannot re-enter
      // library code while writer/publication locks are held.
      std::optional<Error> optInjectedCommitFailure = std::nullopt;
    };

    ~WriteTransaction();

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&&) noexcept;
    WriteTransaction& operator=(WriteTransaction&& other) noexcept;

    DictionaryStore::Writer& dictionary();
    Result<> commit();
    // Explicitly terminates an active transaction while retaining the wrapper
    // that transaction-derived store objects borrow until their destruction.
    void abort() noexcept;

  private:
    struct Impl;
    static Result<WriteTransaction> begin(lmdb::Environment& environment,
                                          DictionaryStore& dictionary,
                                          detail::LibraryIdentity const& identity,
                                          Options options,
                                          std::shared_ptr<void const> writerSessionAnchorPtr = {});
    explicit WriteTransaction(std::unique_ptr<Impl> implPtr);

    lmdb::WriteTransaction& native(detail::LibraryIdentity const& identity);
    lmdb::WriteTransaction const& native(detail::LibraryIdentity const& identity) const;

    std::unique_ptr<Impl> _implPtr;

    friend class FileManifestStore;
    friend class ListStore;
    friend class MetadataStore;
    friend class MusicLibrary;
    friend class ResourceStore;
    friend class TrackStore;
  };
} // namespace ao::library
