// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "flow_layout.hpp"

#include <QtWidgets/QWidget>

#include <algorithm>

namespace nesva::ui {

FlowLayout::FlowLayout(QWidget* parent, int horizontalSpacing, int verticalSpacing)
    : QLayout(parent),
      horizontalSpacing_(horizontalSpacing),
      verticalSpacing_(verticalSpacing) {
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item) {
    items_.append(item);
}

int FlowLayout::count() const {
    return static_cast<int>(items_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const {
    return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index < 0 || index >= items_.size()) {
        return nullptr;
    }
    return items_.takeAt(index);
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return size;
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    const QMargins margins = contentsMargins();
    const QRect effective = rect.adjusted(margins.left(), margins.top(), -margins.right(),
                                          -margins.bottom());
    int x = effective.x();
    int y = effective.y();
    int lineHeight = 0;

    for (QLayoutItem* item : items_) {
        if (item->isEmpty()) {
            continue;
        }
        QSize size = item->sizeHint();
        // A widget that cannot fit even on an empty line gets the line width, so
        // it wraps its own text instead of forcing a horizontal scrollbar.
        size.setWidth(std::min(size.width(), effective.width()));

        const int nextX = x + size.width() + horizontalSpacing_;
        if (nextX - horizontalSpacing_ > effective.right() + 1 && lineHeight > 0) {
            x = effective.x();
            y = y + lineHeight + verticalSpacing_;
            lineHeight = 0;
        }
        if (!testOnly) {
            item->setGeometry(QRect(QPoint(x, y), size));
        }
        x = x + size.width() + horizontalSpacing_;
        lineHeight = std::max(lineHeight, size.height());
    }
    return y + lineHeight - rect.y() + margins.bottom();
}

} // namespace nesva::ui
