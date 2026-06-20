# UCI Phase 6 — Wire `setoption` to Real Engine Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the advertised UCI options actually take effect via `setoption name X value Y`: Hash, Threads, Ponder, SyzygyPath, OwnBook, BookFile, Move Overhead. **MultiPV is dropped** (Crafty has no multi-PV capability) — its advertisement and test are removed.

**Architecture:** A new `UCISetOption()` in `source/uci.c` parses `setoption name <name> value <value>` (the name may contain spaces) and dispatches. Simple options set globals directly (`Ponder`→`ponder`, `Move Overhead`→a UCI overhead static read by `UCISetClock`). Subsystem options reuse Crafty's tested native commands by writing the command into the global `buffer` and calling `Option(block[0])` with output suppressed (`Hash`→`hash NM`, `Threads`→`mt N`). File/path options set globals and manage handles (`SyzygyPath`→`tb_path` + `#if SYZYGY` init; `OwnBook`/`BookFile`→open/close `book_file`). `UCIGo()` consults `Book()` before searching when a book is open.

**Tech Stack:** C (C99), Crafty unity build, gcc 15.2 via Git Bash. `timeout`-guarded transcript tests via `tests/uci/run_tests.sh`.

## Global Constraints

- **Conventions:** UCI logic in `source/uci.c`; the only shared-engine touch is reusing `Option()`/`Book()` (no new edits to engine files except as noted). Build (from `source/`): `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm` (NOTE: this build has **no `-DSYZYGY`** and `CPUS=1`). Tests from repo root: `sh tests/uci/run_tests.sh`. Build/test via the **Bash tool (Git Bash)**. Engine writes git-ignored `log.*`/`game.*` — don't commit.
- **`setoption` parsing:** `args` = the line split on spaces (`setoption name Move Overhead value 30` → tokens). Collect name tokens between `name` and `value` (join with spaces → `"Move Overhead"`); value tokens after `value` (join with spaces). Dispatch by exact `strcmp` on the joined name. Unknown options are silently ignored.
- **`Option()` delegation:** `Option(TREE*)` re-parses the global `char buffer[4096]` (option.c:32). To run a native command: save+zero `display_options`, `strcpy(buffer, "<cmd>")`, `Option(block[0])`, restore `display_options`. (This suppresses the command's `Print(level,…)` output; note `Print(4095,…)` error lines bypass suppression — avoid triggering them by clamping inputs.)
- **Verified mappings & globals:**
  - `Ponder` (check) → `int ponder` (data.h:128; the `TimeSet` ÷20 setting): `ponder = !strcmp(value, "true");`
  - `Move Overhead` (spin ms) → a file-static `int uci_move_overhead` (centiseconds) = `atoi(value) / 10`; `UCISetClock()` must set `tc_safety_margin = uci_move_overhead` (instead of the current `= 0`).
  - `Hash` (spin MB) → `Option()` with `"hash %dM"` (Crafty's `atoiKMB` understands `M`; rounds down to a power of two).
  - `Threads` (spin) → clamp to `[1, CPUS]`, then `Option()` with `"mt %d"` (CPUS=1 → `mt 1` → serial, no error spew).
  - `SyzygyPath` (string) → `strncpy(tb_path, value, sizeof(tb_path)-1)` (always; `char tb_path[128]`, data.h:85), then under `#if defined(SYZYGY)` run `Option()` with `"egtb"` to init. No-SYZYGY build: just stores the path, no crash.
  - `OwnBook`/`BookFile` → `FILE *book_file` (data.h:26), `char book_path[128]` (data.h:83), `int Book(TREE*, int)` (chess.h:346, returns 1 and sets `tree->pv[0].path[1]` if a book move exists, else 0; needs only `book_file` — `books_file` is optional). `int moves_out_of_book` (data.h:220).
  - `CPUS` macro and `smp_max_threads` (data.h:173) are available.
- **Drop MultiPV:** remove the `option name MultiPV …` line in `UCISendId()` (uci.c:284) and the assertion at `tests/uci/run_tests.sh:52`.
- **Phase 5 follow-up (clock-less ponder guard):** a clock-less `go ponder` + `ponderhit` currently runs unbounded on Crafty's stale default time control. Add a bound: when `ponder_flag && !has_clock && search_depth==0 && search_time_limit==0`, set a default `search_time_limit` (e.g. `UCI_PONDER_FALLBACK = 500` cs). Because `TimeCheck` returns 0 while `pondering` (checked before `search_time_limit`), the search is still held open during ponder, but `ponderhit` (sets `pondering=0`) then bounds it to that fixed time.
- **Hang safety:** the harness wraps the engine in `timeout 30`; all tests must terminate.

---

## Task 1: `setoption` parser + drop MultiPV + Ponder + Move Overhead + ponder guard

The parser foundation plus the two direct-set options and the Phase-5 follow-up.

**Files:**
- Modify: `source/uci.c` (add `UCISetOption()` + `uci_move_overhead` static; wire `setoption` into `UCI()`; drop the MultiPV advert; `UCISetClock` reads the overhead; `UCIGo` ponder-guard)
- Test: `tests/uci/run_tests.sh` (remove the MultiPV assertion; add option tests + the ponder-guard test)

**Interfaces:**
- Produces: `static void UCISetOption(int nargs, char *args[])`; `static int uci_move_overhead`.

- [ ] **Step 1: Write/adjust the tests**

In `tests/uci/run_tests.sh`: **delete** the line 52 assertion `expect "option MultiPV" …`. Then append before `exit $fail`:
```sh
# --- Phase 6 Task 1: setoption (Ponder, Move Overhead) + drop MultiPV ---
reject "MultiPV no longer advertised"               'uci\nquit\n' '^option name MultiPV'
expect "setoption Ponder accepted, engine still plays" 'uci\nsetoption name Ponder value false\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "setoption Move Overhead accepted"             'uci\nsetoption name Move Overhead value 50\nposition startpos\ngo wtime 2000 btime 2000\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "clock-less go ponder + ponderhit terminates"  'uci\nposition startpos\ngo ponder\nponderhit\n' '^bestmove [a-h][1-8][a-h][1-8]'
```

- [ ] **Step 2: Run tests to verify the state**

Run: `sh tests/uci/run_tests.sh`
Expected: the `MultiPV no longer advertised` reject currently FAILS (still advertised); the `clock-less go ponder` test currently times out/FAILS (runs unbounded — the `timeout 30` guard turns the hang into a failure); the Ponder/Move Overhead tests pass vacuously (unknown options ignored, engine still plays). Exit nonzero. Proceed to implement.

- [ ] **Step 3: Drop the MultiPV advertisement (`source/uci.c`)**

In `UCISendId()`, delete the line:
```c
  printf("option name MultiPV type spin default 1 min 1 max 256\n");
```

- [ ] **Step 4: Add the `uci_move_overhead` static and `UCISetOption()` (`source/uci.c`)**

Near the top of `source/uci.c` (after the includes / `uci_start_fen`), add:
```c
static int uci_move_overhead = 0;       /* centiseconds, from "Move Overhead" */
```
Add `UCISetOption()` above `void UCI(void)`:
```c
/*
 *  UCISetOption() handles "setoption name <name> value <value>".  The name may
 *  contain spaces (e.g. "Move Overhead"); it is the tokens between "name" and
 *  "value".  Simple options set a global; subsystem options (Hash, Threads) are
 *  delegated to Crafty's native commands via Option() with output suppressed.
 *  Unknown options are ignored, per the UCI specification.
 */
static void UCISetOption(int nargs, char *args[]) {
  TREE *const tree = block[0];
  int i, ni = -1, vi = -1, saved_display_options;
  char name[128], value[256];

  for (i = 1; i < nargs; i++) {
    if (ni < 0 && !strcmp(args[i], "name"))
      ni = i + 1;
    else if (!strcmp(args[i], "value")) {
      vi = i;
      break;
    }
  }
  if (ni < 0)
    return;
  name[0] = 0;
  for (i = ni; i < ((vi < 0) ? nargs : vi); i++) {
    if (i > ni)
      strncat(name, " ", sizeof(name) - strlen(name) - 1);
    strncat(name, args[i], sizeof(name) - strlen(name) - 1);
  }
  value[0] = 0;
  for (i = vi + 1; vi >= 0 && i < nargs; i++) {
    if (i > vi + 1)
      strncat(value, " ", sizeof(value) - strlen(value) - 1);
    strncat(value, args[i], sizeof(value) - strlen(value) - 1);
  }
  saved_display_options = display_options;
  if (!strcmp(name, "Ponder"))
    ponder = (!strcmp(value, "true")) ? 1 : 0;
  else if (!strcmp(name, "Move Overhead"))
    uci_move_overhead = atoi(value) / 10;
  /* (Hash, Threads, SyzygyPath added in Task 2; OwnBook/BookFile in Task 3) */
  display_options = saved_display_options;
}
```

- [ ] **Step 5: Wire `setoption` into the `UCI()` loop, and have `UCISetClock` use the overhead**

In `UCI()`'s command loop, add a branch (next to `position`/`go`):
```c
    else if (!strcmp(args[0], "setoption"))
      UCISetOption(nargs, args);
```
In `UCISetClock()`, change the existing `tc_safety_margin = 0;` to:
```c
  tc_safety_margin = uci_move_overhead;
```

- [ ] **Step 6: Add the clock-less ponder guard (`source/uci.c`)**

Add near the top of `source/uci.c`:
```c
#define UCI_PONDER_FALLBACK 500          /* cs; bounds a clock-less go ponder */
```
In `UCIGo()`'s limit-decision block, change the `ponder_flag` branch so a clock-less ponder gets a bounded fallback. The block becomes:
```c
  if (search_depth == 0 && search_time_limit == 0) {
    if (infinite)
      ;
    else if (has_clock)
      UCISetClock(wtime, btime, winc, binc, movestogo);
    else if (ponder_flag)
      search_time_limit = UCI_PONDER_FALLBACK;
    else
      search_depth = UCI_DEFAULT_DEPTH;
  }
```
(While `pondering`, `TimeCheck` returns 0 first, so the search is held open; on `ponderhit` it is bounded by `search_time_limit`.)

- [ ] **Step 7: Rebuild and run tests**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm` (expect exit 0, no warnings), then `sh tests/uci/run_tests.sh` from repo root.
Expected: all assertions PASS (MultiPV gone, Ponder/Move Overhead accepted, clock-less ponder now terminates), exit 0, no timeouts.

- [ ] **Step 8: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): add setoption (Ponder, Move Overhead), drop MultiPV, bound clock-less ponder"
```
End with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 2: Hash, Threads, SyzygyPath

Subsystem options delegated to Crafty's native commands / the tablebase path.

**Files:**
- Modify: `source/uci.c` (`UCISetOption()` dispatch — add Hash, Threads, SyzygyPath)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `Option()`, `tb_path`, `CPUS`, the Task 1 parser.

- [ ] **Step 1: Write the failing/again-vacuous tests**

Append before `exit $fail`:
```sh
# --- Phase 6 Task 2: Hash, Threads, SyzygyPath ---
expect "setoption Hash accepted, engine still plays"     'uci\nsetoption name Hash value 64\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "setoption Threads accepted, engine still plays"  'uci\nsetoption name Threads value 1\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "setoption SyzygyPath accepted, engine still plays" 'uci\nsetoption name SyzygyPath value C:/tb\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
reject "setoption emits no stray output before bestmove" 'uci\nsetoption name Hash value 64\nsetoption name Threads value 1\nquit\n' 'hash|threads|error|ERROR'
```

- [ ] **Step 2: Run tests (they pass vacuously — unknown options ignored)**

Run: `sh tests/uci/run_tests.sh`. The `expect` tests likely pass already (option ignored, engine plays); the `reject` passes (no output). Proceed to implement so the options actually take effect and confirm no stray output.

- [ ] **Step 3: Add Hash, Threads, SyzygyPath to `UCISetOption()` (`source/uci.c`)**

In `UCISetOption`, add these branches (after `Move Overhead`, before the closing restore):
```c
  else if (!strcmp(name, "Hash")) {
    display_options = 0;
    sprintf(buffer, "hash %dM", atoi(value));
    Option(tree);
  } else if (!strcmp(name, "Threads")) {
    int n = atoi(value);

    if (n < 1)
      n = 1;
    if (n > CPUS)
      n = CPUS;
    display_options = 0;
    sprintf(buffer, "mt %d", n);
    Option(tree);
  } else if (!strcmp(name, "SyzygyPath")) {
    strncpy(tb_path, value, sizeof(tb_path) - 1);
    tb_path[sizeof(tb_path) - 1] = 0;
#if defined(SYZYGY)
    display_options = 0;
    strcpy(buffer, "egtb");
    Option(tree);
#endif
  }
```
(The `display_options` is restored by the existing `display_options = saved_display_options;` at the end of the function, which already brackets all branches.)

- [ ] **Step 4: Rebuild and run tests**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`, then `sh tests/uci/run_tests.sh`.
Expected: exit 0; all PASS; the `reject` confirms no stray `hash`/`mt` output leaks into the UCI stream.

- [ ] **Step 5: Manually confirm Hash actually resized and output is clean**

```bash
printf 'uci\nsetoption name Hash value 128\nisready\nposition startpos\ngo depth 8\nquit\n' | source/crafty_test.exe | grep -vE '^(id|option|uciok|readyok|info|bestmove)'
```
Expected: NO lines printed (the `grep -v` filters out all legitimate UCI lines; anything left would be stray native output). Paste the result (should be empty) into your report.

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): wire Hash, Threads, and SyzygyPath options via setoption"
```
End with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 3: OwnBook and BookFile

Use Crafty's opening book: open the book on `OwnBook`/`BookFile`, and consult `Book()` in `UCIGo()` before searching.

**Files:**
- Modify: `source/uci.c` (`UCISetOption()` — OwnBook/BookFile + a book-open helper; `UCIGo()` — book probe)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `book_file`, `book_path`, `Book()`, `moves_out_of_book`.

- [ ] **Step 1: Implement the book-open helper and OwnBook/BookFile (`source/uci.c`)**

Add a static for the desired book path and an opener, near the other UCI statics:
```c
static char uci_book_file[256] = "";     /* path from "BookFile" */
```
Add a helper above `UCISetOption`:
```c
/*
 *  UCIOpenBook() (re)opens the opening book named by uci_book_file as book_file.
 *  Closing it (path empty) disables the book — Book() returns 0 when book_file
 *  is NULL.  The optional start-weights file (books_file) is left as-is.
 */
static void UCIOpenBook(void) {
  if (book_file) {
    fclose(book_file);
    book_file = 0;
  }
  if (uci_book_file[0]) {
    book_file = fopen(uci_book_file, "rb+");
    if (!book_file)
      book_file = fopen(uci_book_file, "rb");
  }
}
```
In `UCISetOption`, add branches:
```c
  else if (!strcmp(name, "OwnBook")) {
    if (!strcmp(value, "true"))
      UCIOpenBook();
    else if (book_file) {
      fclose(book_file);
      book_file = 0;
    }
  } else if (!strcmp(name, "BookFile")) {
    strncpy(uci_book_file, value, sizeof(uci_book_file) - 1);
    uci_book_file[sizeof(uci_book_file) - 1] = 0;
    if (book_file)                       /* a book is open: switch to the new file */
      UCIOpenBook();
  }
```

- [ ] **Step 2: Probe the book in `UCIGo()` (`source/uci.c`)**

At the very start of `UCIGo()` (before parsing limits), add a book probe for normal moves only (not ponder/infinite). First parse must run to know `ponder_flag`/`infinite`; simplest is to probe right after the parsing loop and the limit decision, just before the suppression/search block:
```c
/*
 *  If an opening book is loaded and this is a real move request (not ponder /
 *  infinite analysis), try a book move first and skip the search if found.
 */
  if (book_file && !infinite && !ponder_flag) {
    if (Book(tree, game_wtm) && tree->pv[0].path[1]) {
      char bmove[8];

      UCIMove(tree->pv[0].path[1], bmove);
      printf("bestmove %s\n", bmove);
      fflush(stdout);
      return;
    }
  }
```
(Place this AFTER the token-parse loop and limit-decision so `infinite`/`ponder_flag` are known, but BEFORE the `display_options`/`Iterate` block.)

- [ ] **Step 3: Verify the repo book loads (decides the test)**

Build, then run:
```bash
cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm
cd .. && printf 'uci\nsetoption name BookFile value books/book.bin\nsetoption name OwnBook value true\nposition startpos\ngo wtime 5000 btime 5000\nquit\n' | source/crafty_test.exe | grep -E '^(info depth|bestmove)' | head -3
```
- If the output is a `bestmove` with NO preceding `info depth` line → the book loaded and `Book()` short-circuited the search. Use the strict tests in Step 4.
- If `info depth` lines appear before `bestmove` → the book did not produce a move (incompatible/not found). Report this as DONE_WITH_CONCERNS, use only the softer tests (the two `expect "… accepted"` ones), and note the repo book did not load.

Paste the result into your report.

- [ ] **Step 4: Write the tests**

Append before `exit $fail`:
```sh
# --- Phase 6 Task 3: OwnBook / BookFile ---
expect "OwnBook off -> engine searches"           'uci\nposition startpos\ngo depth 6\nquit\n' '^info depth'
expect "OwnBook on + book -> bestmove"            'uci\nsetoption name BookFile value books/book.bin\nsetoption name OwnBook value true\nposition startpos\ngo wtime 5000 btime 5000\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
```
If Step 3 confirmed the book loads, ALSO append (the strict check that the book short-circuits the search):
```sh
reject "book move skips the search"               'uci\nsetoption name BookFile value books/book.bin\nsetoption name OwnBook value true\nposition startpos\ngo wtime 5000 btime 5000\nquit\n' '^info depth'
```

- [ ] **Step 5: Rebuild and run tests**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`, then `sh tests/uci/run_tests.sh`.
Expected: all PASS, exit 0. (If the book did not load, you will have omitted the strict `reject` per Step 3/4.)

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): wire OwnBook and BookFile (consult opening book before search)"
```
End with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Phase 6 Definition of Done

- `setoption` is parsed (multi-word names) and dispatched; unknown options are ignored.
- `Ponder`, `Move Overhead`, `Hash`, `Threads`, `SyzygyPath`, `OwnBook`, `BookFile` all take effect (or are harmless no-ops where the build lacks support, e.g. SyzygyPath with no `-DSYZYGY`), with no stray native output leaking into the UCI stream.
- `MultiPV` is no longer advertised or referenced.
- A clock-less `go ponder` + `ponderhit` now terminates (Phase 5 follow-up fixed).
- WinBoard/native modes unchanged; the engine still builds cross-platform; `sh tests/uci/run_tests.sh` is all green and hang-free.

## Out of scope (later phase)

Phase 7 — cutechess-cli gauntlet, real-GUI testing (Arena/Nibbler), full regression pass; updating `CLAUDE.md`'s option list to drop MultiPV (do as part of Phase 7 docs, or note here).

## Self-Review Notes

- **Spec coverage:** 7 options wired (MultiPV dropped per decision); parser → Task 1; Hash/Threads/Syzygy → Task 2; OwnBook/BookFile → Task 3; Phase-5 ponder guard → Task 1. Covered.
- **Type consistency:** `UCISetOption(int, char*[])`, `UCIOpenBook(void)`, `uci_move_overhead`/`uci_book_file` statics — all in uci.c. `Option()`/`Book()` use existing signatures. `tc_safety_margin`/`tb_path`/`book_file`/`ponder` are existing globals.
- **Risk notes:** (1) the `Option()`-delegation output suppression relies on `display_options=0`; `Print(4095,…)` error lines bypass it, mitigated by clamping Threads to `[1,CPUS]` and using valid Hash sizes (Step 5 manual check confirms no leak). (2) OwnBook functional testing depends on the repo `books/book.bin` loading — Step 3 verifies empirically and chooses the strict-vs-soft test accordingly. (3) `book_file` is left NULL at startup (no book.bin in cwd), so OwnBook defaults effectively off until set.
