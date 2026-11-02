// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <memory>

#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"
#include "support/test_fixtures.hpp"

namespace nesva::test {

/// Creates a throw-away database plus source/destination trees for archive
/// tests. Nothing here touches a real user directory.
struct ArchiveFixture {
    std::unique_ptr<QTemporaryDir> root;
    std::unique_ptr<db::Database> database;
    std::unique_ptr<jobs::JobStore> store;
    qint64 jobId = 0;
    QString sourceDir;
    QString destinationDir;

    bool setup(const QString& jobName = QStringLiteral("Unit archive job")) {
        root = std::make_unique<QTemporaryDir>();
        if (!root->isValid()) {
            return false;
        }
        sourceDir = root->filePath(QStringLiteral("source"));
        destinationDir = root->filePath(QStringLiteral("destination"));
        if (!QDir().mkpath(sourceDir) || !QDir().mkpath(destinationDir)) {
            return false;
        }

        database = std::make_unique<db::Database>();
        if (!database->open(root->filePath(QStringLiteral("nesva.db"))).ok() ||
            !database->migrate().ok()) {
            return false;
        }
        store = std::make_unique<jobs::JobStore>(*database);

        jobs::ArchiveJob job;
        job.name = jobName;
        job.mode = QStringLiteral("archive");
        job.destinationPath = destinationDir;
        job.sources = {{0, sourceDir, QStringLiteral("source")}};

        const Result<qint64> created = store->createJob(job);
        if (!created) {
            return false;
        }
        jobId = created.value();
        return true;
    }

    jobs::ArchiveJob job() const {
        Result<jobs::ArchiveJob> loaded = store->getJob(jobId);
        return loaded ? loaded.value() : jobs::ArchiveJob{};
    }

    QString sourcePath(const QString& relative) const {
        return QDir(sourceDir).absoluteFilePath(relative);
    }

    QString destinationPath(const QString& relative) const {
        // Archived files live under the source label directory.
        return QDir(destinationDir).absoluteFilePath(QStringLiteral("source/") + relative);
    }

    bool writeSource(const QString& relative, const QByteArray& content) const {
        return writeFile(sourcePath(relative), content);
    }
};

} // namespace nesva::test
