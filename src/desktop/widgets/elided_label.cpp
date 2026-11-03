// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "elided_label.hpp"

#include <QtGui/QPainter>
#include <QtWidgets/QSizePolicy>

namespace nesva::ui {

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
    // Ignored horizontally: the text never pushes the neighbouring widgets or
    // widens the page. Fixed vertically: one line, always the same height.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMinimumWidth(1);
    setTextInteractionFlags(Qt::NoTextInteraction);
    QLabel::setText(QString());
}

void ElidedLabel::setText(const QString& text) {
    fullText_ = text;
    setToolTip(text);
    QLabel::setText(text);
    update();
}

void ElidedLabel::clearText() {
    setText(QString());
}

void ElidedLabel::clear() {
    clearText();
}

void ElidedLabel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    const QRect area = contentsRect();
    const QString elided = fontMetrics().elidedText(fullText_, Qt::ElideRight, area.width());
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(area, alignment() | Qt::TextSingleLine, elided);
}

} // namespace nesva::ui
