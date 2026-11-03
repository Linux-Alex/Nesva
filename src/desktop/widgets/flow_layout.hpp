// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QLayout>

namespace nesva::ui {

/// Lays widgets out left to right and wraps to the next line when the row is
/// full, like words in a paragraph.
///
/// The dashboard cards used to be a QHBoxLayout: on a narrow window they were
/// squeezed into unreadable slivers instead of flowing onto a second row.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int horizontalSpacing = 12,
                        int verticalSpacing = 12);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;

    QList<QLayoutItem*> items_;
    int horizontalSpacing_ = 12;
    int verticalSpacing_ = 12;
};

} // namespace nesva::ui
