# Nesva presentation & tutorial

Two documents, one per language:

| File | Language | Pages |
| --- | --- | --- |
| `Nesva-0.6.6-SL.pdf` | Slovenian | 10 |
| `Nesva-0.6.6-EN.pdf` | English | 10 |

Each one is a pitch followed by a complete tutorial: what Nesva is and what archive mode and
backup mode promise, installation on Windows and Linux, where the release lives on GitHub (with a
sketch of the release page), a tour of the nine GUI pages (with screenshots), the external tools
(Rclone, Restic), storage and remotes, archive and versioned backup jobs, Microsoft 365 mail,
schedules and the background agent, restore, maintenance, updates, security, the whole command
line, troubleshooting and next steps.

The HTML next to each PDF is the source. Rebuild after editing:

```bash
google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
  --print-to-pdf=Nesva-0.6.6-SL.pdf Nesva-0.6.6-SL.html
google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
  --print-to-pdf=Nesva-0.6.6-EN.pdf Nesva-0.6.6-EN.html
```

A4 page size, print colours, margins and page breaks are in the embedded stylesheet, so any
Chromium-based browser produces the same document.

## Screenshots

`img/` holds the screenshots used in the documents. They were captured from a **demo profile**,
never from a real installation:

```bash
# 1. an empty profile, then the demo jobs, storages and run history
rm -rf /tmp/nesva-demo && mkdir -p /tmp/nesva-demo
NESVA_DATA_DIR=/tmp/nesva-demo ./build/linux-release/nesva-cli status >/dev/null
sqlite3 /tmp/nesva-demo/nesva.db < docs/pitch/demo-seed.sql

# 2. the built-in GUI self-test photographs every page
QT_QPA_PLATFORM=offscreen ./build/linux-release/nesva \
  --data-dir=/tmp/nesva-demo --gui-selftest=/tmp/shots-demo --no-console-log
```

`demo-seed.sql` creates four demo jobs (two archives, one versioned backup, one mail archive),
three storages (iCloud, Backblaze B2, an external disk) and a week of run history, all with
fictional names and paths. The bottom status bar of each screenshot is cropped away because it
prints the database path.
