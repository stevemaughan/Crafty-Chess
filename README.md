# Crafty 25.2.1 — UCI Edition

[![Version](https://img.shields.io/badge/version-25.2.1-blue)](source/data.c)
[![Protocols](https://img.shields.io/badge/protocols-UCI%20%7C%20XBoard%20%7C%20Native-brightgreen)](#using-the-engine)
[![Language](https://img.shields.io/badge/language-C-orange)](source/)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#building)
[![License](https://img.shields.io/badge/license-Crafty%20(personal%20use)-yellow)](#license)

**Crafty** is the classic, full-strength open-source chess engine written by Dr. **Robert (Bob) Hyatt** — the direct descendant of the legendary *Cray Blitz*. This repository hosts Crafty **v25.2.1**, a build of upstream Crafty 25.2 that **adds full [UCI](https://en.wikipedia.org/wiki/Universal_Chess_Interface) protocol support** so the engine works out of the box in modern graphical interfaces, while preserving everything that made Crafty great.

> The `.1` in the version denotes the UCI conversion. Upstream Crafty spoke only the WinBoard/xboard protocol plus its own console commands; this edition speaks **UCI as well** — and the original interfaces still work unchanged.

---

## Contents

- [Why this edition](#why-this-edition)
- [Features](#features)
- [What's new in 25.2.1](#whats-new-in-2521)
- [Quick start](#quick-start)
- [Building](#building)
- [Using the engine](#using-the-engine)
- [UCI options](#uci-options)
- [Opening books & tablebases](#opening-books--tablebases)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Version history](#version-history)
- [Acknowledgements](#acknowledgements)
- [License](#license)

---

## Why this edition

Most modern GUIs — **Arena, Cute Chess, Nibbler, BanksiaGUI, ChessBase/Fritz**, and others — speak UCI. Stock Crafty does not, so using it meant a WinBoard adapter or the raw console. This fork closes that gap:

- **UCI is added as a protocol adapter, not an engine rewrite.** The search, evaluation, opening book, tablebase, hash-table, SMP, and pondering code are the proven upstream implementations. UCI simply translates commands in and results out.
- **Three protocols coexist and auto-detect.** The mode is chosen from the first token on stdin: `uci` → UCI, `xboard` → WinBoard, anything else → Crafty's native console.
- **WinBoard and native behavior are untouched.** Every UCI change is additive and mode-gated, so existing setups keep working exactly as before.

## Features

- Strong bitboard alpha-beta search: iterative deepening, aspiration windows, null-move pruning, late-move reductions, futility/razoring, and check extensions.
- Quiescence search with SEE-based pruning and full move-ordering machinery (hash move, MVV/LVA, killers, counter-moves, history).
- **SMP / multi-threaded** search (YBWC) — scale across cores with the `Threads` option.
- **Opening book** support (Crafty's own `book.bin` format).
- **Syzygy endgame tablebases** (WDL + DTZ) via `-DSYZYGY`.
- **Pondering** ("permanent brain") — thinks on the opponent's clock.
- Transposition (hash) table with configurable size.
- Cross-platform: **Windows** (primary target), **Linux**, **macOS**, and MinGW.

## What's new in 25.2.1

- ✅ **Full UCI support**, auto-detected alongside WinBoard and native modes.
- ✅ `position` (startpos / FEN + move replay), `go` (`depth`, `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `infinite`, `ponder`), `stop`, `ponderhit`, `isready`, `ucinewgame`, `quit`.
- ✅ Streaming `info` output (`depth`, `score cp`/`mate`, `nodes`, `nps`, `time`, `pv`) and `bestmove … [ponder …]`.
- ✅ UCI options wired to the engine: **Hash, Threads, Ponder, SyzygyPath, OwnBook, BookFile, Move Overhead** (see [below](#uci-options)).
- ✅ **Time-management & robustness hardening** for UCI play — correct GUI-clock mapping, sane default time budgeting, FEN half-move-clock (50-move rule) tracking, well-behaved `go infinite`/ponder, and `volatile` cross-thread abort flags. Validated with transcript tests and self-play gauntlets.

Full design notes: [`docs/UCI Implementation.md`](docs/UCI%20Implementation.md).

## Quick start

```sh
# build (Windows, gcc / MinGW-w64)
cd source
gcc -O2 -DCPUS=1 crafty.c -o crafty.exe -lwinmm

# sanity-check the UCI handshake
printf 'uci\nisready\nquit\n' | ./crafty.exe
```

You should see `id name Crafty 25.2.1`, the `option …` list, `uciok`, then `readyok`. Point your GUI at the resulting executable and you're playing.

## Building

Crafty uses a **unity build**: [`source/crafty.c`](source/crafty.c) `#include`s every other `.c` file into a single translation unit, so you only ever compile `crafty.c`. Windows is the default target when `-DUNIX` is omitted.

### Windows (gcc / MinGW-w64)

```sh
cd source

# Single-threaded (quick test build)
gcc -O2 -DCPUS=1 crafty.c -o crafty.exe -lwinmm

# Recommended release: multi-threaded + Syzygy tablebases
gcc -O2 -DCPUS=8 -DSYZYGY -mpopcnt crafty.c -o crafty.exe -lwinmm
```

> `tbprobe.c` is already part of the unity build, so **do not** add it on the command line. Drop `-mpopcnt` if your CPU lacks a hardware popcount instruction.

### Linux / macOS

The [`Makefile`](source/Makefile) is the easiest route:

```sh
cd source
make help          # list available targets
make unix-gcc      # Linux with gcc
make unix-clang    # macOS / clang (also the default target)
```

Edit the `opt =` line in the Makefile to set build options. Common ones:

| Option | Effect |
| --- | --- |
| `-DCPUS=n` | Maximum threads for SMP search (use the `Threads` UCI option / `mt n` to engage them). |
| `-DSYZYGY` | Enable Syzygy endgame tablebases. |
| `-DTBDIR="path"` | Default tablebase directory. |
| `-DBOOKDIR="path"` | Default opening-book directory. |
| `-DUNIX` | Build for a Unix-like OS (omit for Windows). |
| `-DSKILL` | Enable the `skill` command to dial Crafty's strength down. |

## Using the engine

Crafty picks its protocol from the **first command it reads**:

| First input | Mode |
| --- | --- |
| `uci` | **UCI** — for Arena, Cute Chess, Nibbler, BanksiaGUI, ChessBase, etc. |
| `xboard` | **WinBoard/xboard** protocol. |
| anything else | **Native console** (Crafty's own command set). |

In a UCI GUI, just register `crafty.exe` as a UCI engine — the GUI sends `uci` automatically. From a terminal you can drive it directly:

```text
uci
setoption name Hash value 256
setoption name Threads value 4
position startpos moves e2e4 e7e5
go wtime 60000 btime 60000 winc 1000 binc 1000
```

## UCI options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `Hash` | spin (MB) | 64 | Transposition-table size (1 – 65536 MB). |
| `Threads` | spin | 1 | Search threads (1 – build's `CPUS`). |
| `Ponder` | check | false | Think on the opponent's time. The GUI drives pondering via `go ponder`. |
| `SyzygyPath` | string | *(empty)* | Path to Syzygy tablebase files (requires a `-DSYZYGY` build). |
| `OwnBook` | check | false | Use the engine's opening book. |
| `BookFile` | string | `book.bin` | Path to the opening-book file. |
| `Move Overhead` | spin (ms) | 30 | Time buffer subtracted per move to absorb GUI/network lag and avoid flag-fall. |

> *MultiPV is intentionally not advertised — Crafty has no multi-PV search.* Deeper personality/eval knobs remain available through the native console command set.

## Opening books & tablebases

- **Opening book** — a prebuilt [`books/book.bin`](books/) is included. Enable it in UCI with `setoption name OwnBook value true` and (optionally) point `BookFile` at another book.
- **Endgame tablebases** — build with `-DSYZYGY`, then set `SyzygyPath` to your Syzygy directory.

## Testing

Two harnesses live under [`tests/`](tests/):

```sh
# UCI protocol transcript tests (black-box; pipes transcripts, greps the output)
sh tests/uci/run_tests.sh

# Self-play stability gauntlet (needs fastchess or cutechess-cli)
FASTCHESS=/path/to/fastchess.exe sh tests/gauntlet/run_gauntlet.sh "8+0.08" 50
```

The transcript suite covers the handshake, position setup, search/`bestmove`, `info` output, time control, `stop`/`ponderhit`, options, and book behaviour. The gauntlet plays Crafty against itself to surface illegal moves, time forfeits, crashes, and disconnects. Move generation can be spot-checked in-engine with `perft N`.

## Repository layout

```text
source/      Engine source (unity build; UCI lives in uci.c)
books/       Opening book (book.bin) and PGN sources
docs/        UCI implementation notes, Crafty help/HTML docs
tests/       UCI transcript tests and the self-play gauntlet
Makefile     in source/ — Unix/macOS build targets
```

Key source files: [`uci.c`](source/uci.c) (UCI adapter), [`main.c`](source/main.c) (game loop), [`option.c`](source/option.c) (command dispatch, incl. WinBoard), [`search.c`](source/search.c)/[`quiesce.c`](source/quiesce.c)/[`next.c`](source/next.c) (search), [`evaluate.c`](source/evaluate.c) (evaluation), [`hash.c`](source/hash.c), [`thread.c`](source/thread.c) (SMP), [`time.c`](source/time.c), [`book.c`](source/book.c), [`tbprobe.c`](source/tbprobe.c)/[`tbcore.c`](source/tbcore.c) (Syzygy).

## Version history

This repository also serves as an archive of Crafty releases — historical versions are preserved as **git tags** (e.g. `v25.2`, `v24.1`, …) so you can check out and compare any version. `master` carries the current UCI edition (25.2.1).

## Acknowledgements

Crafty is the work of **Robert M. Hyatt** and contributors (Mike Byrne, Tracy Riegle, Peter Skinner), with:

- **Magic bitboard** move generation by *Pradu Kaanan*
- **Syzygy** tablebase probing by *Ronald de Man*
- **EPD** support by *Steven Edwards*

This edition adds the UCI protocol layer; all engine credit belongs to the original authors.

## License

Crafty is **copyright © 1996–2016 Robert M. Hyatt, Ph.D. All rights reserved.** It is **free for personal use** but is **not** released under an OSI/open-source license. In summary (the authoritative text is in the header of [`source/main.c`](source/main.c)):

- Free for your **personal use**; anyone you let play it must see it clearly identified as **"Crafty"**.
- It **may not be entered into any computer-chess competition** without the authors' written permission — and only then under the name "Crafty", so its ancestry is known.
- The **original copyright notice must be kept intact** in all copies of the source.
- **Any changes to the software must be made public**, whether distributed free or commercially.

In keeping with that last requirement, the UCI changes in this fork are published here in full.
