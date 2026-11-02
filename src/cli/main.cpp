// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtCore/QCoreApplication>

#include "commands.hpp"
#include "nesva/core/app_info.hpp"

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    nesva::initialiseApplicationMetadata();
    return nesva::cli::run(argc, argv);
}
