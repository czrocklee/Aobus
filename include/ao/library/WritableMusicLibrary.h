// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/WriteTransaction.h>

#include <memory>

namespace ao::library
{
  class MusicLibrary;

  /**
   * Process-scoped authority to commit writes to one MusicLibrary.
   *
   * Acquisition is non-blocking and fails while another process holds the
   * writer session for the same database. Transactions retain the session
   * anchor, so destroying this capability cannot release the lease early.
   * The borrowed MusicLibrary must outlive this capability and every
   * WriteTransaction created from it.
   */
  class [[nodiscard]] WritableMusicLibrary final
  {
  public:
    static Result<WritableMusicLibrary> acquire(MusicLibrary& library);

    ~WritableMusicLibrary();

    WritableMusicLibrary(WritableMusicLibrary const&) = delete;
    WritableMusicLibrary& operator=(WritableMusicLibrary const&) = delete;
    WritableMusicLibrary(WritableMusicLibrary&&) noexcept;
    WritableMusicLibrary& operator=(WritableMusicLibrary&&) noexcept;

    WriteTransaction writeTransaction(WriteTransaction::Options options = {});
    MusicLibrary& library() const noexcept;

  private:
    struct Impl;
    explicit WritableMusicLibrary(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::library
