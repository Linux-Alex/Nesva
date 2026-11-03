# 0014. Installing external tools from inside Nesva

- Status: Accepted
- Date: 2026-11-03
- Deciders: project maintainers

## Context

Nesva depends on two external programs, Rclone and Restic, for cloud destinations and versioned
backups. [ADR 0006](0006-external-tool-integration.md) decided that they are user-supplied and
invoked without a shell, but it did not answer the obvious follow-up: a user who does not have
Rclone has to leave the application, find the download page, choose the right file, unpack it and
put it on `PATH`. The usual convenience answer - `curl … | sh` or a package manager run on the
user's behalf - is exactly the pattern that turns a desktop application into a supply-chain
foot-gun.

## Decision

Nesva installs Rclone itself, under four constraints.

1. **The plan is data, and the user sees it first.** `tools::planRcloneInstall` produces a
   `ToolInstallPlan`: the resolved version, the download URL, the checksum URL, the target path
   and, for every step that runs a program, the exact argument vector. The desktop dialog and the
   CLI print *that same object*, and `executeRcloneInstall` consumes it, so the preview cannot
   describe something different from what runs.

2. **Verify before unpacking, unpack before installing.** The archive is downloaded into a
   private temporary directory, hashed and compared with the publisher's `SHA256SUMS`. A mismatch
   aborts with `IntegrityError` and nothing is installed. Only the single verified binary is
   copied into place; the archive is never unpacked as root.

3. **Per-user by default, elevation only as a reviewed exception.** The default target is
   `~/.local/bin` (`%LOCALAPPDATA%\Programs\rclone` on Windows), which needs no privileges. An
   all-users install shows the exact elevated command - `pkexec install -m 0755 <binary> /usr/local/bin/rclone`
   - before the password prompt. On Windows this build does not automate elevation at all: the
   plan states that the user must copy the binary from an Administrator terminal.

4. **No shell, ever.** Download, verification and installation are in-process; unpacking uses the
   first available unpacker (`unzip`, `bsdtar`, `python3 -m zipfile`, `7z`; `tar.exe` on
   Windows) as a program with an argument vector. No command is ever assembled as a string, so
   there is nothing to quote and nothing to inject into.

After a successful install the absolute path is written to `tools.rclone_path`, so Nesva works
even when the install directory is not on the user's `PATH`.

## Consequences

Positive: installing the dependency no longer requires leaving the application or trusting an
installer script; the preview, the checksum and the scope choice are auditable; tests can drive
the whole flow through a scripted transport, including the refusal path.

Negative and accepted:

- The checksum list is fetched from the same HTTPS origin as the archive. It detects a truncated
  or corrupted download and a broken mirror - it does not protect against a compromised server.
  Rclone publishes no signature, and the application says so in the plan instead of implying a
  guarantee it cannot make.
- The archive is buffered in memory (about 30 MB) during verification; streaming verification is
  follow-up work.
- Unpacking depends on one of several common tools being present. When none is, the plan says so
  and refuses rather than degrading into a shell pipeline.
- The tool is installed outside any package manager, so distribution security updates do not
  apply to it; `nesva-cli tools` always reports the version in use.

## Alternatives considered

- **Bundle Rclone with Nesva.** Rejected in ADR 0006 and still rejected: it makes the binary
  unattributable, inflates the package and hides which version is in use.
- **Run the vendor's install script (`curl … | sh`).** Rejected outright: it executes arbitrary
  remote content with no review and no verification.
- **Shell out to `apt`/`brew`/winget.** Rejected: it needs root for the system package manager,
  differs per distribution and cannot be tested hermetically.
- **Always install system-wide.** Rejected: it forces a password prompt on the common case for no
  benefit, and it hides the elevated step instead of showing it.
