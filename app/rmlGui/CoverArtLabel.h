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

#pragma once

#include <QtWidgets/QLabel>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>

class CoverArtLabel : public QLabel
{
  Q_OBJECT

public:
  explicit CoverArtLabel(QWidget* parent = 0);
  virtual int heightForWidth(int width) const;
  virtual QSize sizeHint() const;
  QPixmap scaledPixmap() const;

public slots:
  void setPixmap(const QPixmap&);
  void resizeEvent(QResizeEvent*);

private:
  QPixmap _pix;
};