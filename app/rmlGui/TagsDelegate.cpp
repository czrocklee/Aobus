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

#include "TagsDelegate.h"
#include <QtGui/QPainter>

void TagsDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  QStringList tags = index.data().value<QStringList>();
  auto frame = option.rect.marginsRemoved(QMargins{3, 3, 3, 3});
  
  for (const auto& tag: tags)
  {
    painter->save();
    auto textFrame = frame.marginsRemoved(QMargins{5, 5, 5, 5});
    painter->drawText(, tag, QTextOption{Qt::AlignVCenter});

    auto rect = painter->fontMetrics().boundingRect(tags[0]);
    frame.adjust(rect.width() + 3, 0, 0, 0);
    qDebug() << frame.topLeft() << frame.bottomLeft();
    painter->drawLine(frame.topLeft(), frame.bottomLeft());
    
    //qDebug() << adjustedRect;
    //adjustedRect.setWidth(rect.width());
    //painter->drawRect(adjustedRect);
    //painter->setBackground(QBrush{Qt::GlobalColor::lightGray});
    //painter->setBackgroundMode(Qt::BGMode::OpaqueMode);
    //painter->restore();
  }
  
}

QSize TagsDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const { return QSize{100, 20}; }