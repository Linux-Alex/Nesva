// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/process_utils.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace nesva {

bool isProcessAlive(qint64 pid) {
    if (pid <= 0) {
        return false;
    }

#ifdef Q_OS_WIN
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        // Access denied means the process exists but is not inspectable.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool alive = ::GetExitCodeProcess(handle, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    ::CloseHandle(handle);
    return alive;
#else
    const auto nativePid = static_cast<pid_t>(pid);
    if (::kill(nativePid, 0) == 0) {
        return true;
    }
    // EPERM: the process exists but belongs to another user.
    return errno == EPERM;
#endif
}

} // namespace nesva
