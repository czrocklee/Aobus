// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "CoverArtLabel.h"

CoverArtLabel::CoverArtLabel(QWidget* parent) : QLabel(parent)
{
  this->setMinimumSize(1, 1);
  setScaledContents(false);
}

void CoverArtLabel::setPixmap(QPixmap const& p)
{
  _pix = p;
  QLabel::setPixmap(scaledPixmap());
}

int CoverArtLabel::heightForWidth(int width) const { return width; }

QSize CoverArtLabel::sizeHint() const { return QSize(width(), width()); }

QPixmap CoverArtLabel::scaledPixmap() const
{
  auto scaled = _pix.scaled(this->size() * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  scaled.setDevicePixelRatio(devicePixelRatioF());
  return scaled;
}

void CoverArtLabel::resizeEvent(QResizeEvent* e)
{
  if (!_pix.isNull())
  {
    QLabel::setPixmap(scaledPixmap());
  }
}