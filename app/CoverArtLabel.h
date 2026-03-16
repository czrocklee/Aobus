// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QLabel>

class CoverArtLabel : public QLabel
{
  Q_OBJECT

public:
  explicit CoverArtLabel(QWidget* parent = 0);
  virtual int heightForWidth(int width) const;
  virtual QSize sizeHint() const;
  QPixmap scaledPixmap() const;

public slots:
  void setPixmap(QPixmap const&);
  void resizeEvent(QResizeEvent*);

private:
  QPixmap _pix;
};