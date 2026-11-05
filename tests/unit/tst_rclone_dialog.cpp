// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// Regression test: closing the guided rclone wizard while "rclone config" is
// still running must return immediately. The dialog used to leave the process
// alive when the window close button was used, so the dialog destructor waited
// for QProcess to kill it - the whole application appeared frozen.

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/qscopeguard.h>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>

#include "nesva/config/settings_store.hpp"

#include "app_context.hpp"
#include "dialogs/rclone_config_dialog.hpp"

#include <memory>

using nesva::ui::AppContext;

class RcloneDialogTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void closingTheWindowWhileRcloneRunsReturnsImmediately();
    void closingBeforeRcloneHasStartedReturnsImmediately();
    void closeButtonWhileRcloneRunsReturnsImmediately();
    void closingTheModalWizardReturnsFromExec();
    void closingTheWindowWithRealRcloneReturnsImmediately();

private:
    QString fakeRclone() const;

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<AppContext> context_;
};

QString RcloneDialogTest::fakeRclone() const {
    return qEnvironmentVariable("NESVA_FAKE_RCLONE");
}

void RcloneDialogTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    context_ = std::make_unique<AppContext>();
    nesva::ui::StartupOptions options;
    options.dataRootOverride = directory_->path();
    options.consoleLogging = false;
    const nesva::Status initialised = context_->initialise(options);
    QVERIFY2(initialised.ok(), initialised.ok() ? "" : initialised.error().message().c_str());

    const QString fake = fakeRclone();
    QVERIFY2(!fake.isEmpty(), "NESVA_FAKE_RCLONE must point at the test double");
    QVERIFY(context_->settings()
                .setString(QString::fromUtf8(nesva::config::keys::kRcloneBinaryPath), fake)
                .ok());
}

void RcloneDialogTest::cleanup() {
    context_.reset();
    directory_.reset();
}

void RcloneDialogTest::closingTheWindowWhileRcloneRunsReturnsImmediately() {
    QElapsedTimer timer;
    timer.start();
    {
        nesva::ui::RcloneConfigDialog dialog(*context_, QStringLiteral("icloud"));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog) || dialog.isVisible());
        QTest::qWait(500); // let the fake rclone print its first prompt

        // Exactly what the window close button (the title bar X) does.
        dialog.close();
    }
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 5000,
             qPrintable(QStringLiteral("closing the wizard took %1 ms; the rclone process was "
                                       "left running")
                            .arg(elapsed)));
}

void RcloneDialogTest::closingBeforeRcloneHasStartedReturnsImmediately() {
    // Opening and closing the wizard in one gesture must not wait for a process
    // that QProcess has not even created yet.
    QElapsedTimer timer;
    timer.start();
    {
        nesva::ui::RcloneConfigDialog dialog(*context_, QStringLiteral("icloud"));
        dialog.show();
        dialog.close();
    }
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(
        elapsed < 5000,
        qPrintable(QStringLiteral("closing the wizard during startup took %1 ms").arg(elapsed)));
}

void RcloneDialogTest::closeButtonWhileRcloneRunsReturnsImmediately() {
    QElapsedTimer timer;
    timer.start();
    {
        nesva::ui::RcloneConfigDialog dialog(*context_, QStringLiteral("icloud"));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog) || dialog.isVisible());
        QTest::qWait(500);

        QPushButton* close = nullptr;
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Close")) {
                close = button;
                break;
            }
        }
        QVERIFY2(close != nullptr, "the wizard must have a Close button");
        close->click();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 5000,
             qPrintable(QStringLiteral("closing the wizard took %1 ms").arg(elapsed)));
}

void RcloneDialogTest::closingTheModalWizardReturnsFromExec() {
    // Storage -> "Configure / re-authenticate..." runs the dialog with exec();
    // rejecting it must end that nested event loop immediately.
    nesva::ui::RcloneConfigDialog dialog(*context_, QStringLiteral("icloud"));
    QTimer::singleShot(500, &dialog, [&dialog] { dialog.reject(); });

    QElapsedTimer timer;
    timer.start();
    const int result = dialog.exec();
    const qint64 elapsed = timer.elapsed();

    QCOMPARE(result, int(QDialog::Rejected));
    QVERIFY2(elapsed < 5000,
             qPrintable(QStringLiteral("the modal wizard took %1 ms to close").arg(elapsed)));
}

void RcloneDialogTest::closingTheWindowWithRealRcloneReturnsImmediately() {
    // The stand-in proves the lifecycle; a real rclone proves that its own
    // startup and shutdown do not keep the dialog alive. RCLONE_CONFIG points
    // into the temporary directory so the user's remotes are never touched.
    const QString rclone = QStandardPaths::findExecutable(QStringLiteral("rclone"));
    if (rclone.isEmpty()) {
        QSKIP("rclone is not installed");
    }
    const QByteArray previousConfig = qgetenv("RCLONE_CONFIG");
    qputenv("RCLONE_CONFIG",
            QFile::encodeName(directory_->filePath(QStringLiteral("rclone.conf"))));
    const auto restoreEnvironment = qScopeGuard([previousConfig] {
        if (previousConfig.isNull()) {
            qunsetenv("RCLONE_CONFIG");
        } else {
            qputenv("RCLONE_CONFIG", previousConfig);
        }
    });
    Q_UNUSED(restoreEnvironment);

    QVERIFY(context_->settings()
                .setString(QString::fromUtf8(nesva::config::keys::kRcloneBinaryPath), rclone)
                .ok());
    QElapsedTimer timer;
    timer.start();
    {
        nesva::ui::RcloneConfigDialog dialog(*context_, QStringLiteral("icloud"));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog) || dialog.isVisible());
        QTest::qWait(500);

        dialog.close();
    }
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(
        elapsed < 5000,
        qPrintable(QStringLiteral("closing the wizard took %1 ms with real rclone").arg(elapsed)));
}

QTEST_MAIN(RcloneDialogTest)
#include "tst_rclone_dialog.moc"
