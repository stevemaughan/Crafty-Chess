# UCI Phase 1 — Handshake & Mode Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Crafty enter UCI mode when the GUI sends `uci`, respond with the engine identity, the option list, and `uciok`, answer `isready` with `readyok`, and exit cleanly on `quit` — without affecting WinBoard or native console behavior.

**Architecture:** UCI is a mode-gated protocol adapter layered over the existing engine. A new `source/uci.c` holds a dedicated `UCI()` input loop. The only hook into existing code is one branch in `Option()` that recognizes `uci` as the first token and hands control to `UCI()`. This phase implements the handshake only — no `position`/`go`/search yet (those are Phase 2+).

**Tech Stack:** C (C99), Crafty's unity build (`crafty.c` `#include`s every `.c`), gcc 15.2 on Windows for the test build. Tests are black-box stdin→stdout transcript checks via a POSIX shell harness (runs in Git Bash and on Unix). No new dependencies.

## Global Constraints

- **Unity build:** new modules are added to the `#include` manifest in `source/crafty.c`. (Active build is `objects = crafty.o`; the separate-compilation list in `source/Makefile` is commented out.)
- **No new dependencies; portable C; cross-platform preserved.** Windows is the primary target but the engine must still build on Unix/MinGW. Gate any platform specifics behind the macros Crafty already uses.
- **Additive and mode-gated.** All UCI behavior is gated by the new global `int uci_mode`. Never change a WinBoard or native code path in a way that alters its behavior.
- **Prototypes go in `source/chess.h`** (Crafty's convention — no per-module headers). **Globals go in `source/data.c`** with `extern` in `source/data.h`.
- **Engine identity strings (exact):** name `Crafty <version>` where `<version>` is the `version` global (currently `25.2`); author `Robert Hyatt`.
- **Test build command (verified on this machine), run from `source/`:**
  `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
  Produces `source/crafty_test.exe` (gitignored via `*.exe`). Exit 0 on success.
- **Protocol triggers use exact `strcmp`** (not Crafty's prefix-matching `OptionMatch`), so the UCI command set never collides with native abbreviations.
- **Pre-handshake output quirk (verified):** at startup Crafty prints a banner and the native prompt `White(1): ` with **no trailing newline** before reading the first command. The UCI `id` block therefore begins with a leading `\n` so `id name…` starts on its own line. Unknown UCI commands must be **silently ignored** (per the UCI spec), never error.

---

## Task 1: Baseline build + transcript-test harness

Establishes a known-good build and the reusable test harness every later task uses. Reviewer can gate this independently: "does the engine build and does the harness run a smoke check?"

**Files:**
- Create: `tests/uci/run_tests.sh`
- Build artifact (not committed): `source/crafty_test.exe`

**Interfaces:**
- Produces: shell harness with two functions — `expect "<desc>" "<transcript>" "<egrep-pattern>"` (asserts the pattern appears in engine stdout) and the convention that every transcript ends in `quit\n`. Engine path defaults to `source/crafty_test.exe`, overridable as `$1`.

- [ ] **Step 1: Build the baseline engine**

Run (from repo root):
```bash
cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm
```
Expected: exit 0; `source/crafty_test.exe` exists.

- [ ] **Step 2: Write the harness (this is the failing-test scaffold — it has one real assertion)**

Create `tests/uci/run_tests.sh`:
```sh
#!/usr/bin/env sh
# UCI protocol transcript tests for Crafty.
# Usage (from repo root):  sh tests/uci/run_tests.sh [path-to-engine]
# Each test pipes a transcript to the engine and greps its stdout.
# Every transcript must end with "quit\n" so the engine terminates.
set -u
ENGINE="${1:-source/crafty_test.exe}"
fail=0

# expect <description> <transcript-with-\n-escapes> <egrep-pattern>
expect() {
  desc=$1; transcript=$2; pattern=$3
  out=$(printf '%b' "$transcript" | "$ENGINE" 2>/dev/null)
  if printf '%s\n' "$out" | grep -Eq "$pattern"; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc -- expected /$pattern/"
    fail=1
  fi
}

# --- Task 1: smoke (engine builds and runs) ---
expect "engine builds and runs" 'quit\n' 'Crafty v25\.2'

exit $fail
```

- [ ] **Step 3: Run the harness against the baseline**

Run (from repo root):
```bash
sh tests/uci/run_tests.sh
```
Expected: `PASS: engine builds and runs` and exit 0.

- [ ] **Step 4: Commit**

```bash
git add tests/uci/run_tests.sh
git commit -m "test: add UCI transcript-test harness and baseline smoke test"
```

---

## Task 2: UCI mode entry + handshake (`uci` → id + uciok, `quit`)

The core of Phase 1: auto-detect UCI, take over the input loop, respond to `uci` with identity + `uciok`, and exit on `quit`/EOF. Options are added in Task 4; `isready` in Task 3.

**Files:**
- Create: `source/uci.c`
- Modify: `source/data.c` (add the `uci_mode` global, next to `int xboard = 0;` ~line 488)
- Modify: `source/data.h` (add `extern int uci_mode;` next to `extern int xboard;` ~line 79)
- Modify: `source/chess.h` (add `void UCI(void);` near `void Analyze(void);` ~line 329)
- Modify: `source/crafty.c` (add `#include "uci.c"` to the manifest)
- Modify: `source/option.c` (add the `uci` trigger branch after the xboard branch ~line 3912)
- Modify: `source/Makefile` (add `uci.o` to the commented separate-compilation list, line ~175)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Produces: `void UCI(void)` — entered from `Option()` when the first token is exactly `uci`. Sets `uci_mode = 1`, sends the handshake, runs the UCI command loop reading via `Read(1, buffer)` + `ReadParse(buffer, args, " \t")`, and on `quit`/EOF calls `CraftyExit(0)` (never returns).
- Produces: `int uci_mode` (global; 0 = not in UCI mode).
- Consumes (existing, already declared): `version` (data.h), `buffer`/`args`/`nargs` (data.h), `Read`/`ReadParse`/`CraftyExit` (chess.h), `FOREVER` macro (chess.h).

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` immediately before the `exit $fail` line:
```sh
# --- Task 2: handshake ---
expect "uci -> uciok"     'uci\nquit\n' '^uciok'
expect "uci -> id name"   'uci\nquit\n' '^id name Crafty 25\.2'
expect "uci -> id author" 'uci\nquit\n' '^id author Robert Hyatt'
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the three Task 2 lines report `FAIL` (the engine does not yet recognize `uci`), exit nonzero.

- [ ] **Step 3: Add the `uci_mode` global**

In `source/data.c`, find `int xboard = 0;` and add directly after it:
```c
int uci_mode = 0;
```

In `source/data.h`, find `extern int xboard;` and add directly after it:
```c
extern int uci_mode;
```

- [ ] **Step 4: Declare `UCI()` in `chess.h`**

In `source/chess.h`, find `void Analyze(void);` and add directly after it:
```c
void UCI(void);
```

- [ ] **Step 5: Create `source/uci.c`**

```c
#include "chess.h"
#include "data.h"
/*
 *******************************************************************************
 *                                                                             *
 *   UCI() implements the Universal Chess Interface protocol.  It is entered   *
 *   from Option() when the first input token is "uci", at which point it      *
 *   takes over the input loop, translating UCI commands into Crafty's         *
 *   internal machinery and Crafty's output back into UCI responses.  The      *
 *   WinBoard and native-console interfaces are unaffected; all UCI behavior   *
 *   is gated by the uci_mode flag.                                            *
 *                                                                             *
 *   This module currently implements the protocol handshake (uci, isready,    *
 *   quit) and option enumeration.  Position setup, search (go), info output   *
 *   and pondering are added in later phases.                                  *
 *                                                                             *
 *******************************************************************************
 */

/*
 *  UCISendId() sends the engine identity and the uciok terminator in response
 *  to the "uci" command.  A leading newline guarantees "id name" starts on its
 *  own line, since at startup Crafty has already printed the native prompt
 *  ("White(1): ") with no trailing newline.
 */
static void UCISendId(void) {
  printf("\nid name Crafty %s\n", version);
  printf("id author Robert Hyatt\n");
  /* (UCI options are enumerated here in a later task.) */
  printf("uciok\n");
  fflush(stdout);
}

void UCI(void) {
  uci_mode = 1;
  UCISendId();
  while (FOREVER) {
    if (Read(1, buffer) < 0)
      break;
    nargs = ReadParse(buffer, args, " \t");
    if (nargs == 0)
      continue;
    if (!strcmp(args[0], "uci"))
      UCISendId();
    else if (!strcmp(args[0], "quit"))
      break;
    /* Unknown commands are ignored, per the UCI specification. */
  }
  CraftyExit(0);
}
```

- [ ] **Step 6: Add `uci.c` to the unity build**

In `source/crafty.c`, find the line `#include "option.c"` and add directly after it:
```c
#include "uci.c"
```

- [ ] **Step 7: Add the `uci` trigger to `Option()`**

In `source/option.c`, find the end of the xboard branch (the closing `}` of the `else if (OptionMatch("xboard", *args) || OptionMatch("winboard", *args)) { ... }` block, ~line 3912) and add a new branch directly after it:
```c
/*
 ************************************************************
 *                                                          *
 *  "uci" command switches Crafty into UCI mode and hands   *
 *  control to the UCI() input loop, which does not return  *
 *  (it exits via CraftyExit on "quit").                    *
 *                                                          *
 ************************************************************
 */
  else if (!strcmp(*args, "uci")) {
    uci_mode = 1;
    UCI();
  }
```

- [ ] **Step 8: Keep the non-unity build list complete**

In `source/Makefile`, in the commented-out separate-compilation `objects` list (the block starting `#objects = main.o ...`), add `uci.o` to the list — e.g. change the final line `annotate.o analyze.o evtest.o bench.o edit.o data.o` to:
```
        annotate.o analyze.o evtest.o bench.o edit.o uci.o data.o
```
(The active unity build `objects = crafty.o` is unchanged.)

- [ ] **Step 9: Rebuild**

Run (from repo root): `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings about `UCI`/`uci_mode`.

- [ ] **Step 10: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all four assertions (`engine builds and runs`, `uci -> uciok`, `uci -> id name`, `uci -> id author`) report `PASS`; exit 0.

- [ ] **Step 11: Commit**

```bash
git add source/uci.c source/crafty.c source/option.c source/data.c source/data.h source/chess.h source/Makefile tests/uci/run_tests.sh
git commit -m "feat(uci): add UCI mode detection and uci/quit handshake"
```

---

## Task 3: `isready` → `readyok`

**Files:**
- Modify: `source/uci.c` (add the `isready` branch to the `UCI()` loop)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `void UCI(void)` loop from Task 2.
- Produces: no new symbols; adds the `isready`→`readyok` behavior.

- [ ] **Step 1: Write the failing test**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Task 3: isready ---
expect "isready -> readyok" 'uci\nisready\nquit\n' '^readyok'
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/uci/run_tests.sh`
Expected: `FAIL: isready -> readyok` (currently ignored as unknown), exit nonzero.

- [ ] **Step 3: Implement the `isready` branch**

In `source/uci.c`, in the `UCI()` loop, add a branch between the `uci` branch and the `quit` branch:
```c
    else if (!strcmp(args[0], "isready")) {
      printf("readyok\n");
      fflush(stdout);
    }
```

- [ ] **Step 4: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0.

- [ ] **Step 5: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions `PASS`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): respond to isready with readyok"
```

---

## Task 4: Option enumeration

Advertise the agreed UCI option set in the `uci` response. These are static advertisement strings only — they are **wired to engine behavior in Phase 6**, not here. Reviewer gate: "does `uci` list exactly the agreed options, before `uciok`?"

**Files:**
- Modify: `source/uci.c` (`UCISendId()` — insert option lines between the `id` lines and `uciok`)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `UCISendId()` from Task 2; the `CPUS` compile-time macro (already used in `chess.h`).
- Produces: no new symbols.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Task 4: option enumeration ---
expect "option Hash"          'uci\nquit\n' '^option name Hash type spin default 64 min 1 max 65536'
expect "option Threads"       'uci\nquit\n' '^option name Threads type spin default 1 min 1 max'
expect "option Ponder"        'uci\nquit\n' '^option name Ponder type check default false'
expect "option SyzygyPath"    'uci\nquit\n' '^option name SyzygyPath type string'
expect "option OwnBook"       'uci\nquit\n' '^option name OwnBook type check default false'
expect "option BookFile"      'uci\nquit\n' '^option name BookFile type string'
expect "option MultiPV"       'uci\nquit\n' '^option name MultiPV type spin default 1 min 1 max 256'
expect "option Move Overhead" 'uci\nquit\n' '^option name Move Overhead type spin default 30 min 0 max 5000'
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: all eight Task 4 lines report `FAIL`, exit nonzero.

- [ ] **Step 3: Implement the option lines**

In `source/uci.c`, replace the placeholder comment line `  /* (UCI options are enumerated here in a later task.) */` inside `UCISendId()` with:
```c
  printf("option name Hash type spin default 64 min 1 max 65536\n");
  printf("option name Threads type spin default 1 min 1 max %d\n", CPUS);
  printf("option name Ponder type check default false\n");
  printf("option name SyzygyPath type string default <empty>\n");
  printf("option name OwnBook type check default false\n");
  printf("option name BookFile type string default book.bin\n");
  printf("option name MultiPV type spin default 1 min 1 max 256\n");
  printf("option name Move Overhead type spin default 30 min 0 max 5000\n");
```
(With the `-DCPUS=1` test build, the Threads line prints `max 1`; the test asserts `max` without a number. Phase 6 refines the max to reflect available hardware.)

- [ ] **Step 4: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0.

- [ ] **Step 5: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: every assertion `PASS`, exit 0.

- [ ] **Step 6: Manually confirm clean ordering (id → options → uciok)**

Run (from repo root):
```bash
printf 'uci\nquit\n' | source/crafty_test.exe | grep -E '^(id|option|uciok)'
```
Expected: `id name` and `id author` first, then the eight `option name …` lines, then `uciok` last — with no banner/prompt text on any of those lines.

- [ ] **Step 7: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): enumerate UCI options in the uci response"
```

---

## Phase 1 Definition of Done

- `printf 'uci\nquit\n' | crafty` emits, on their own lines: `id name Crafty 25.2`, `id author Robert Hyatt`, the eight `option name …` lines, then `uciok`.
- `isready` returns `readyok`; `quit`/EOF exits cleanly (exit 0).
- Unknown commands are ignored without error.
- WinBoard mode (`xboard`) and native console mode are unchanged (no edits to their code paths; the only `Option()` change is the additive `uci` branch).
- `sh tests/uci/run_tests.sh` is all-green.

## Out of scope (later phases)

`position`/`go`/search → `bestmove` (Phase 2), `info` output (Phase 3), time control + `stop` (Phase 4), pondering (Phase 5), wiring the options to real engine settings + a cutechess/GUI regression pass (Phases 6–7). Each gets its own plan.

## Self-Review Notes

- **Spec coverage:** Phase 1 of the CLAUDE.md roadmap = "mode detection, `uci`/`isready`/`quit` handshake, option enumeration." Mode detection → Task 2 (Option hook + `UCI()`); `uci` handshake → Task 2; `isready` → Task 3; `quit` → Task 2; option enumeration → Task 4; test scaffold → Task 1. Covered.
- **Type consistency:** `UCI(void)` declared in `chess.h`, defined in `uci.c`, called from `option.c` and self-referenced — signature consistent throughout. `uci_mode` defined in `data.c`, externed in `data.h`, set in both `option.c` and `uci.c`. `UCISendId()` is `static` in `uci.c`, used only there.
- **No placeholders:** every code and command step is concrete and the build command is verified on this machine.
