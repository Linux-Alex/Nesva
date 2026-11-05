// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// Test double for "rclone config": prints the same line-based prompts and then
// stays alive until its stdin is closed or it is killed, which is what the
// guided wizard has to cope with when the user closes the dialog.

#include <cstdio>
#include <iostream>
#include <string>

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("Current remotes:");
    std::puts("");
    std::puts("Name                 Type");
    std::puts("====                 ====");
    std::puts("");
    std::puts("e) Edit existing remote");
    std::puts("n) New remote");
    std::puts("d) Delete remote");
    std::puts("r) Rename remote");
    std::puts("c) Copy remote");
    std::puts("s) Set configuration password");
    std::puts("q) Quit config");
    std::printf("e/n/d/r/c/s/q> ");
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::printf("\n%s> ", line.c_str());
        std::fflush(stdout);
    }
    return 0;
}
