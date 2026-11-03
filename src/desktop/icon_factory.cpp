// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "icon_factory.hpp"

#include <QtCore/QRectF>
#include <QtGui/QBrush>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>

#include <cmath>

namespace nesva::ui {
namespace {

void paintGlyph(QPainter& painter, Glyph glyph, const QColor& color) {
    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case Glyph::Dashboard: {
        const QRectF tile(3.0, 3.0, 7.0, 7.0);
        painter.drawRoundedRect(tile, 1.6, 1.6);
        painter.drawRoundedRect(tile.translated(9.0, 0.0), 1.6, 1.6);
        painter.drawRoundedRect(tile.translated(0.0, 9.0), 1.6, 1.6);
        painter.drawRoundedRect(tile.translated(9.0, 9.0), 1.6, 1.6);
        break;
    }
    case Glyph::Jobs: {
        painter.drawEllipse(QRectF(3.5, 3.5, 15.0, 15.0));
        painter.drawLine(QPointF(11.0, 7.0), QPointF(11.0, 11.6));
        painter.drawLine(QPointF(11.0, 11.6), QPointF(14.4, 13.4));
        break;
    }
    case Glyph::Storage: {
        painter.drawEllipse(QRectF(3.5, 3.0, 15.0, 5.4));
        painter.drawLine(QPointF(3.5, 5.7), QPointF(3.5, 17.0));
        painter.drawLine(QPointF(18.5, 5.7), QPointF(18.5, 17.0));
        QPainterPath bottom;
        bottom.moveTo(3.5, 17.0);
        bottom.arcTo(QRectF(3.5, 14.3, 15.0, 5.4), 180.0, 180.0);
        painter.drawPath(bottom);
        painter.drawLine(QPointF(3.5, 11.3), QPointF(18.5, 11.3));
        break;
    }
    case Glyph::Archive: {
        QPainterPath folder;
        folder.moveTo(3.0, 7.0);
        folder.lineTo(3.0, 5.4);
        folder.quadTo(3.0, 4.4, 4.0, 4.4);
        folder.lineTo(9.0, 4.4);
        folder.lineTo(10.6, 6.4);
        folder.lineTo(18.0, 6.4);
        folder.quadTo(19.0, 6.4, 19.0, 7.4);
        folder.lineTo(19.0, 17.6);
        folder.quadTo(19.0, 18.6, 18.0, 18.6);
        folder.lineTo(4.0, 18.6);
        folder.quadTo(3.0, 18.6, 3.0, 17.6);
        folder.closeSubpath();
        painter.drawPath(folder);
        break;
    }
    case Glyph::Snapshots: {
        for (int layer = 0; layer < 3; ++layer) {
            const qreal offset = static_cast<qreal>(layer) * 2.6;
            painter.drawRoundedRect(QRectF(3.4 + offset * 0.4, 4.4 + offset, 13.2, 6.6), 1.8, 1.8);
        }
        break;
    }
    case Glyph::Restore: {
        QPainterPath tray;
        tray.moveTo(4.0, 13.5);
        tray.lineTo(4.0, 17.6);
        tray.quadTo(4.0, 18.6, 5.0, 18.6);
        tray.lineTo(17.0, 18.6);
        tray.quadTo(18.0, 18.6, 18.0, 17.6);
        tray.lineTo(18.0, 13.5);
        painter.drawPath(tray);
        QPainterPath arrow;
        arrow.moveTo(11.0, 14.5);
        arrow.lineTo(11.0, 4.6);
        painter.drawPath(arrow);
        painter.drawLine(QPointF(11.0, 4.6), QPointF(7.6, 8.0));
        painter.drawLine(QPointF(11.0, 4.6), QPointF(14.4, 8.0));
        break;
    }
    case Glyph::Activity: {
        QPainterPath pulse;
        pulse.moveTo(2.6, 12.0);
        pulse.lineTo(6.6, 12.0);
        pulse.lineTo(8.6, 6.4);
        pulse.lineTo(11.6, 18.0);
        pulse.lineTo(14.0, 12.0);
        pulse.lineTo(19.4, 12.0);
        painter.drawPath(pulse);
        break;
    }
    case Glyph::Mail: {
        painter.drawRoundedRect(QRectF(3.0, 6.0, 16.0, 12.0), 1.8, 1.8);
        painter.drawLine(QPointF(3.4, 6.6), QPointF(11.0, 12.4));
        painter.drawLine(QPointF(18.6, 6.6), QPointF(11.0, 12.4));
        break;
    }
    case Glyph::Settings: {
        painter.drawEllipse(QRectF(8.0, 8.0, 6.0, 6.0));
        painter.drawEllipse(QRectF(4.0, 4.0, 14.0, 14.0));
        constexpr qreal kPi = 3.14159265358979323846;
        for (int index = 0; index < 8; ++index) {
            const qreal angle = static_cast<qreal>(index) * 45.0 * kPi / 180.0;
            const QPointF inner(11.0 + 7.0 * std::cos(angle), 11.0 + 7.0 * std::sin(angle));
            const QPointF outer(11.0 + 9.2 * std::cos(angle), 11.0 + 9.2 * std::sin(angle));
            painter.drawLine(inner, outer);
        }
        break;
    }
    }
}

QPixmap renderGlyph(Glyph glyph, const QColor& color, int pixelSize) {
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal scale = static_cast<qreal>(pixelSize) / 22.0;
    painter.scale(scale, scale);
    paintGlyph(painter, glyph, color);
    painter.end();
    return pixmap;
}

} // namespace

QIcon makeGlyphIcon(Glyph glyph, const QColor& color, int pixelSize) {
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48}) {
        icon.addPixmap(renderGlyph(glyph, color, size > 0 ? size : pixelSize));
    }
    return icon;
}

QIcon applicationIcon() {
    QPixmap pixmap(256, 256);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient gradient(0, 0, 256, 256);
    gradient.setColorAt(0.0, QColor(0x2b, 0x6c, 0xe8));
    gradient.setColorAt(1.0, QColor(0x1a, 0x3f, 0x8f));
    QPainterPath background;
    background.addRoundedRect(QRectF(8, 8, 240, 240), 56, 56);
    painter.fillPath(background, gradient);

    QFont font = painter.font();
    font.setPixelSize(148);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(0xff, 0xff, 0xff));
    painter.drawText(QRectF(8, 8, 240, 240), Qt::AlignCenter, QStringLiteral("N"));
    painter.end();

    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addPixmap(pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return icon;
}

} // namespace nesva::ui
