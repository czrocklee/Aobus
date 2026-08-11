// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <semaphore>
#include <thread>

namespace ao::lmdb::detail
{
  /** Observes one contended database-open admission on the current thread. */
  class DatabaseOpenAdmissionProbe final
  {
  public:
    explicit DatabaseOpenAdmissionProbe(std::binary_semaphore& contentionSignal);
    ~DatabaseOpenAdmissionProbe();

    DatabaseOpenAdmissionProbe(DatabaseOpenAdmissionProbe const&) = delete;
    DatabaseOpenAdmissionProbe& operator=(DatabaseOpenAdmissionProbe const&) = delete;
    DatabaseOpenAdmissionProbe(DatabaseOpenAdmissionProbe&&) = delete;
    DatabaseOpenAdmissionProbe& operator=(DatabaseOpenAdmissionProbe&&) = delete;

  private:
    std::binary_semaphore& _contentionSignal;
    std::thread::id _ownerThreadId;
    bool _observed = false;

    friend void recordDatabaseOpenAdmissionContention() noexcept;
  };

  void recordDatabaseOpenAdmissionContention() noexcept;
} // namespace ao::lmdb::detail
