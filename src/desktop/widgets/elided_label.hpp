// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QLabel>

namespace nesva::ui {

/// A single-line label that elides instead of growing.
///
/// Status lines sit above tables, and a wrapping label changes the row height
/// whenever its text gets longer: the table below jumps, the last row is pushed
/// under the viewport and the page layout shifts while a job runs. This label
/// never changes its height, never asks for more width than it has, and keeps
/// the full text in a tooltip so nothing is lost.
class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);

    /// Shows \p text elided to the available width; the tooltip keeps it whole.
    void setText(const QString& text);

    /// Empty text still occupies its row, so the layout never reflows.
    void clearText();

    /// Hides QLabel::clear(), which would bypass the elided rendering.
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;
    /// Keeps the row exactly one line high when a stylesheet changes the font.
    void changeEvent(QEvent* event) override;

private:
    void applyLineHeight();

    QString fullText_;
};

} // namespace nesva::ui
