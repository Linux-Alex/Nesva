// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/app_info.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QSysInfo>
#include <QtCore/QtVersion>

#include "nesva/version.hpp"

namespace nesva {

void initialiseApplicationMetadata() {
    QCoreApplication::setOrganizationName(QString::fromUtf8(kOrganisationName));
    QCoreApplication::setOrganizationDomain(QString::fromUtf8(kOrganisationDomain));
    QCoreApplication::setApplicationName(QString::fromUtf8(kApplicationName));
    QCoreApplication::setApplicationVersion(ApplicationInfo::version());
}

QString ApplicationInfo::version() {
    return QString::fromUtf8(NESVA_VERSION_STRING);
}

QString ApplicationInfo::displayVersion() {
    return QStringLiteral("%1 %2").arg(QString::fromUtf8(kApplicationName), version());
}

QString ApplicationInfo::gitCommit() {
    return QString::fromUtf8(NESVA_GIT_COMMIT_STRING);
}

QString ApplicationInfo::qtVersion() {
    return QString::fromUtf8(qVersion());
}

QString ApplicationInfo::platformDescription() {
    return QStringLiteral("%1 / %2").arg(QSysInfo::prettyProductName(),
                                        QSysInfo::currentCpuArchitecture());
}

QJsonObject ApplicationInfo::toJson() {
    QJsonObject object;
    object.insert(QStringLiteral("name"), QString::fromUtf8(kApplicationName));
    object.insert(QStringLiteral("version"), version());
    object.insert(QStringLiteral("git_commit"), gitCommit());
    object.insert(QStringLiteral("qt_version"), qtVersion());
    object.insert(QStringLiteral("platform"), platformDescription());
    object.insert(QStringLiteral("build_compiler"), QString::fromUtf8(NESVA_COMPILER_STRING));
    return object;
}

} // namespace nesva
