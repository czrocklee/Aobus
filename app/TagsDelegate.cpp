// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagsDelegate.h"
#include <QtGui/QPainter>

void TagsDelegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
  QStringList tags = index.data().value<QStringList>();
  auto frame = option.rect.marginsRemoved(QMargins{3, 3, 3, 3});

  for (auto const& tag : tags)
  {
    painter->save();
    auto textFrame = frame.marginsRemoved(QMargins{5, 5, 5, 5});
    painter->drawText(textFrame, tag, QTextOption{Qt::AlignVCenter});

    auto rect = painter->fontMetrics().boundingRect(tags[0]);
    frame.adjust(rect.width() + 3, 0, 0, 0);
    qDebug() << frame.topLeft() << frame.bottomLeft();
    painter->drawLine(frame.topLeft(), frame.bottomLeft());

    // qDebug() << adjustedRect;
    // adjustedRect.setWidth(rect.width());
    // painter->drawRect(adjustedRect);
    // painter->setBackground(QBrush{Qt::GlobalColor::lightGray});
    // painter->setBackgroundMode(Qt::BGMode::OpaqueMode);
    // painter->restore();
  }
}

QSize TagsDelegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
  return QSize{100, 20};
}