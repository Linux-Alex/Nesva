// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "gui_selftest.hpp"

#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>

#include <cstdio>

#include "main_window.hpp"

namespace nesva::ui {
namespace {

void pumpEventsFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QString slugify(const QString& title) {
    QString slug = title.toLower();
    slug.replace(QLatin1Char(' '), QLatin1Char('-'));
    slug.remove(QLatin1Char('/'));
    return slug;
}

} // namespace

int runGuiSelfTest(MainWindow& window, const QString& outputDirectory) {
    QDir directory(outputDirectory);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        std::fprintf(stderr, "GUI self-test: cannot create output directory %s\n",
                     qPrintable(outputDirectory));
        return 1;
    }

    window.show();
    pumpEventsFor(700);

    if (!window.isVisible()) {
        std::fprintf(stderr, "GUI self-test: main window is not visible\n");
        return 1;
    }

    int captured = 0;
    int failures = 0;
    const int pageCount = window.pageCount();

    for (int index = 0; index < pageCount; ++index) {
        window.selectPage(index);
        pumpEventsFor(150);

        const QPixmap screenshot = window.grab();
        const QString fileName = QStringLiteral("gui-selftest-%1-%2.png")
                                     .arg(index + 1)
                                     .arg(slugify(window.pageTitleAt(index)));
        bool ok = !screenshot.isNull() && screenshot.width() > 400 && screenshot.height() > 200;
        if (ok) {
            ok = screenshot.save(directory.absoluteFilePath(fileName));
        }
        if (ok) {
            ++captured;
        } else {
            ++failures;
        }
        std::fprintf(stdout, "page %d/%d: %-18s %s\n", index + 1, pageCount,
                     qPrintable(window.pageTitleAt(index)),
                     ok ? qPrintable(fileName) : "failed to capture");
    }

    std::fprintf(stdout, "GUI self-test: %d of %d pages captured into %s\n", captured, pageCount,
                 qPrintable(directory.absolutePath()));
    std::fflush(stdout);

    return failures == 0 ? 0 : 1;
}

} // namespace nesva::ui
