// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

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

    template<typename Type>
    struct IsResult : std::false_type
    {};

    template<typename Value>
    struct IsResult<Result<Value>> : std::true_type
    {};
  } // namespace detail

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

    /**
     * Runs one root write operation inside this transaction.
     *
     * A recoverable operation error, native transaction failure, or private
     * library error carrier aborts the whole transaction before the error is
     * returned. Any other exception also aborts the transaction before it is
     * rethrown. Successful operations leave the transaction active for commit().
     * The callback cannot nest apply() or commit the transaction; savepoint
     * semantics are not provided.
     */
    template<typename Function,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Function, WriteTransaction&>>>
      requires detail::IsResult<OperationResult>::value
    OperationResult apply(Function&& function)
    {
      try
      {
        auto optResult = std::optional<OperationResult>{};
        auto boundaryRes = applyBoundary(
          [&function, &optResult](WriteTransaction& transaction) -> Result<>
          {
            optResult.emplace(std::invoke(std::forward<Function>(function), transaction));

            if (!*optResult)
            {
              return std::unexpected{std::move(optResult->error())};
            }

            return {};
          });

        if (!boundaryRes)
        {
          return std::unexpected{std::move(boundaryRes.error())};
        }

        if (optResult)
        {
          return std::move(*optResult);
        }

        abort();
        return makeError(Error::Code::InvalidState, "Library write operation produced no result");
      }
      catch (...)
      {
        // This also covers allocation or result-transfer failure outside the
        // erased callback invocation itself.
        abort();
        throw;
      }
    }

    Result<> commit();
    // Explicitly terminates an active transaction. The destructor performs the
    // same rollback when an operation unwinds without committing.
    void abort() noexcept;

  private:
    struct Impl;
    static Result<WriteTransaction> begin(lmdb::Environment& environment,
                                          DictionaryStore& dictionary,
                                          detail::LibraryIdentity const& identity,
                                          Options options,
                                          std::shared_ptr<void const> writerSessionAnchorPtr = {});
    explicit WriteTransaction(std::unique_ptr<Impl> implPtr);
    Result<> applyBoundary(std::move_only_function<Result<>(WriteTransaction&)> function);

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
