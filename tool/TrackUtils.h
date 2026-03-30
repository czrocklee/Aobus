// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackRecord.h>
#include <rs/lmdb/Transaction.h>

#include <filesystem>

rs::core::TrackRecord loadTrackRecord(std::filesystem::path const& path,
                                      rs::core::DictionaryStore& dictionary,
                                      rs::core::ResourceStore::Writer& resourceWriter,
                                      rs::lmdb::WriteTransaction& txn);