// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "AddTrackDialog.h"

AddTrackDialog::AddTrackDialog(QWidget* parent, Qt::WindowFlags f) : QDialog{parent, f} { setupUi(this); }

rs::fbs::TrackT AddTrackDialog::track() const
{
  rs::fbs::TrackT t;
  // t.title = lineEditTitle->text().toStdString();
  // t.artist = lineEditArtist->text().toStdString();
  // t.album = lineEditAlbum->text().toStdString();
  return t;
}
