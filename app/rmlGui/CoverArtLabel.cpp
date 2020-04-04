/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "CoverArtLabel.h"

CoverArtLabel::CoverArtLabel(QWidget* parent) : QLabel(parent)
{
  this->setMinimumSize(1, 1);
  setScaledContents(false);
}

void CoverArtLabel::setPixmap(const QPixmap& p)
{
  _pix = p;
  QLabel::setPixmap(scaledPixmap());
}

int CoverArtLabel::heightForWidth(int width) const
{
  return width;
}

QSize CoverArtLabel::sizeHint() const
{
  return QSize(width(), width());
}

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