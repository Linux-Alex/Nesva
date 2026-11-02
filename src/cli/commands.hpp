// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

namespace nesva::cli {

/// Entry point shared by main(): parses arguments, dispatches the command and
/// returns the process exit code.
int run(int argc, char** argv);

} // namespace nesva::cli
