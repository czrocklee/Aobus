// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "app/ui_TrackView.h"
#include <rs/core/MusicLibrary.h>

#include <QtCore/QThread>

#include <filesystem>
#include <optional>

class ImportWorker : public QThread
{
  Q_OBJECT

public:
  ImportWorker(rs::core::MusicLibrary& ml, std::vector<std::filesystem::path> const& files, QObject* parent = nullptr);

  void commit();

signals:
  void progressUpdated(QString path, int itemIndex);

  void workFinished();

protected:
  void run() override;

private:
  rs::core::MusicLibrary& _ml;
  std::optional<rs::lmdb::WriteTransaction> _txn;
  std::vector<std::filesystem::path> const& _files;
};