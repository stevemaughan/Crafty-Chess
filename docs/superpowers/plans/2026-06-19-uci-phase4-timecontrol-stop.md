# UCI Phase 4 — Clock Time Control, `stop`, and `go infinite` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make UCI `go` honor the GUI clock (`wtime`/`btime`/`winc`/`binc`/`movestogo`), support `go infinite` (search until told to stop), and support `stop` (interrupt a running search and play the best move so far).

**Architecture:** `UCIGo()` maps the GUI clock onto Crafty's time-control globals (`tc_*`, centiseconds) so `TimeSet()` computes a per-move time, and sets `pondering=1` for `go infinite` (which makes `TimeCheck()` never expire). `stop` needs no threads: the running search already polls `CheckInput()` every N nodes (search.c) and routes detected input through `Interrupt()`. Two small mode-gated hooks make this work for UCI: (1) `CheckInput()` is taught to detect piped stdin when `uci_mode` is set (it currently only does so for `xboard`); (2) `Interrupt()` gets a `uci_mode` branch that handles `stop`/`isready`/`quit` mid-search and sets `abort_search=1` for `stop`.

**Tech Stack:** C (C99), Crafty unity build, gcc 15.2 via Git Bash. Tests are transcript checks via `tests/uci/run_tests.sh`, now hang-guarded with `timeout`.

## Global Constraints

- **Conventions:** UCI logic in `source/uci.c`. Build (from `source/`): `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`. Tests from repo root: `sh tests/uci/run_tests.sh`. Build/test via the **Bash tool (Git Bash)**. Engine writes git-ignored `log.*`/`game.*` — don't commit.
- **Mode-gated hooks into shared engine code** (in addition to Phase 3's `DisplayPV` hook): `CheckInput()` in `source/utility.c` and `Interrupt()` in `source/interrupt.c`. Both MUST be inert when `uci_mode == 0`. Only `uci.c`, `utility.c`, `interrupt.c`, and the test file change.
- **Time-control globals (verified):** all centiseconds (1 cs = 10 ms); arrays indexed `black=0, white=1` (`game_wtm` is the side to move). `int tc_time_remaining[2]`, `int tc_increment`, `int tc_moves_remaining[2]`, `int tc_sudden_death`, `int tc_safety_margin`, `int tc_moves`, `int tc_time`, `int tc_secondary_moves`, `int tc_secondary_time` (all in data.h:207-215). `Flip(x)` = `x^1` (chess.h:572).
- **Clock vs fixed:** clock-based timing requires `search_time_limit == 0` AND `search_depth == 0`; then `TimeSet()` (called from iterate.c:181) computes `time_limit`/`absolute_time_limit` from the `tc_*` globals. `movestogo M` → `tc_sudden_death = 0` + `tc_moves_remaining[stm] = M`; no movestogo → `tc_sudden_death = 1` (sudden death + increment). Fixed depth/movetime take precedence over clock.
- **`go infinite`:** set `pondering = 1` before `Iterate()` (TimeCheck returns 0 when `pondering`), reset `pondering = 0` after. The search runs until `stop` sets `abort_search = 1`.
- **`stop` mechanism:** the search self-polls `CheckInput()` every N nodes (search.c:46-57, gated only by `thread_id==0`). With the `uci_mode` `CheckInput` fix it detects piped stdin; `Interrupt()` reads the line and (uci_mode branch) sets `abort_search = 1`. `Iterate()` breaks (iterate.c:347/557), returns, and `tree->pv[0].path[1]` holds the best move from the last completed iteration (valid). `abort_search` is reset to 0 at each `Iterate()` start (iterate.c:76) — no cross-search poisoning. No threads.
- **Hang safety:** the test harness wraps the engine in `timeout 30`; every interrupt test must terminate (and would FAIL visibly, not hang, if `stop` detection is broken).

---

## Task 1: Clock-based time control (`wtime`/`btime`/`winc`/`binc`/`movestogo`)

Maps the GUI clock onto Crafty's time allocator. No mid-search input needed — fully and safely testable.

**Files:**
- Modify: `source/uci.c` (add `UCISetClock()`; extend `UCIGo()` parsing + limit decision)
- Test: `tests/uci/run_tests.sh` (add `timeout` hang-guard to helpers; add clock tests)

**Interfaces:**
- Consumes: `tc_*` globals, `game_wtm`, `Flip`.
- Produces: `static void UCISetClock(int wtime, int btime, int winc, int binc, int movestogo)` — sets the `tc_*` globals from a UCI clock (ms). `UCIGo` parses the clock tokens.

- [ ] **Step 1: Add a hang-guard to the test harness**

In `tests/uci/run_tests.sh`, in BOTH the `expect()` and `reject()` functions, change the engine invocation to add a 30-second timeout. Replace each occurrence of:
```sh
  out=$(printf '%b' "$transcript" | "$ENGINE" 2>/dev/null)
```
with:
```sh
  out=$(printf '%b' "$transcript" | timeout 30 "$ENGINE" 2>/dev/null)
```
(This protects the whole suite from a hung search. `timeout` is at /usr/bin/timeout in Git Bash.)

- [ ] **Step 2: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Phase 4 Task 1: clock-based time control ---
expect "go wtime/btime (sudden death) -> bestmove" 'uci\nposition startpos\ngo wtime 1000 btime 1000 winc 100 binc 100\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "go with movestogo -> bestmove"             'uci\nposition startpos\ngo wtime 2000 btime 2000 movestogo 30\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "clock search still streams info"           'uci\nposition startpos\ngo wtime 2000 btime 2000\nquit\n' '^info depth [0-9]+ score cp '
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the three clock tests currently produce a `bestmove` anyway? No — before this task, `go wtime …` matches no recognized token, so `UCIGo` falls back to default depth 8 and DOES emit a bestmove. So these tests may PASS vacuously. That's acceptable: their purpose is to confirm clock timing works once implemented (a clock-based search must still terminate quickly and emit a bestmove/info). To make the RED meaningful, temporarily confirm the behavior changes by checking the search actually uses the clock in Step 6's manual check. Proceed to implement.

- [ ] **Step 4: Add `UCISetClock()` to `source/uci.c`**

Add above `UCIGo` (after `UCIInfo` or `UCIMove`):
```c
/*
 *  UCISetClock() maps a UCI "go" clock (wtime/btime/winc/binc in ms, movestogo
 *  a move count) onto Crafty's time-control globals (centiseconds, indexed
 *  black=0/white=1).  game_wtm is the side to move (the engine's side).  With
 *  movestogo it uses an N-moves-in-T model; otherwise sudden death + increment.
 *  TimeSet() then computes the per-move time when Iterate() runs.
 */
static void UCISetClock(int wtime, int btime, int winc, int binc,
    int movestogo) {
  int mytime = (game_wtm ? wtime : btime) / 10;
  int opptime = (game_wtm ? btime : wtime) / 10;
  int myinc = (game_wtm ? winc : binc) / 10;

  tc_time_remaining[game_wtm] = mytime;
  tc_time_remaining[Flip(game_wtm)] = opptime;
  tc_increment = myinc;
  tc_safety_margin = 0;
  if (movestogo > 0) {
    tc_sudden_death = 0;
    tc_moves = movestogo;
    tc_time = mytime;
    tc_secondary_moves = movestogo;
    tc_secondary_time = mytime;
    tc_moves_remaining[game_wtm] = movestogo;
    tc_moves_remaining[Flip(game_wtm)] = movestogo;
  } else {
    tc_sudden_death = 1;
    tc_moves = 1000;
    tc_moves_remaining[white] = 1000;
    tc_moves_remaining[black] = 1000;
  }
}
```

- [ ] **Step 5: Extend `UCIGo()` parsing and limit decision (`source/uci.c`)**

In `UCIGo`, add clock locals and parse the clock tokens, and replace the limit fallback. Add to the locals: `int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0, has_clock = 0;`. In the token-parsing `for` loop, add these `else if` branches after the existing `depth`/`movetime` ones:
```c
    else if (!strcmp(args[i], "wtime") && i + 1 < nargs) {
      wtime = atoi(args[++i]);
      has_clock = 1;
    } else if (!strcmp(args[i], "btime") && i + 1 < nargs) {
      btime = atoi(args[++i]);
      has_clock = 1;
    } else if (!strcmp(args[i], "winc") && i + 1 < nargs)
      winc = atoi(args[++i]);
    else if (!strcmp(args[i], "binc") && i + 1 < nargs)
      binc = atoi(args[++i]);
    else if (!strcmp(args[i], "movestogo") && i + 1 < nargs)
      movestogo = atoi(args[++i]);
```
Then replace the existing fallback line `if (!search_depth && !search_time_limit) search_depth = UCI_DEFAULT_DEPTH;` with:
```c
  if (search_depth == 0 && search_time_limit == 0) {
    if (has_clock)
      UCISetClock(wtime, btime, winc, binc, movestogo);
    else
      search_depth = UCI_DEFAULT_DEPTH;
  }
```

- [ ] **Step 6: Rebuild and manually confirm clock timing**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm` (expect exit 0).
Then run two transcripts and confirm a clock-based search scales with the clock (more time → deeper search):
```bash
printf 'uci\nposition startpos\ngo wtime 200 btime 200\nquit\n' | source/crafty_test.exe | grep -E '^(info depth|bestmove)' | tail -2
printf 'uci\nposition startpos\ngo wtime 5000 btime 5000\nquit\n' | source/crafty_test.exe | grep -E '^(info depth|bestmove)' | tail -2
```
Expected: both end in a `bestmove`; the 5000ms search reaches a noticeably higher `info depth` than the 200ms one (confirming time, not a fixed depth, governs termination). Paste both into your report.

- [ ] **Step 7: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, exit 0. (The clock searches terminate in tens of ms.)

- [ ] **Step 8: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): map GUI clock (wtime/btime/winc/binc/movestogo) to Crafty time control"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 2: `stop` and `go infinite` (mid-search interruption)

Adds the two mode-gated engine hooks and `go infinite`. **Higher risk:** depends on `CheckInput()` detecting piped stdin mid-search. Verify empirically.

**Files:**
- Modify: `source/utility.c` (`CheckInput()` — detect piped stdin in `uci_mode`)
- Modify: `source/interrupt.c` (`Interrupt()` — `uci_mode` branch for `stop`/`isready`/`quit`)
- Modify: `source/uci.c` (`UCIGo()` — parse `infinite`, set `pondering`)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `uci_mode`, `abort_search`, `thinking`, `pondering`, `CraftyExit`.
- Produces: no new symbols; `go infinite` searches until `stop`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Phase 4 Task 2: stop and go infinite ---
expect "stop interrupts go infinite -> bestmove"   'uci\nposition startpos\ngo infinite\nstop\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "stop interrupts a long search -> bestmove" 'uci\nposition startpos\ngo movetime 60000\nstop\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "isready during search -> readyok"          'uci\nposition startpos\ngo infinite\nisready\nstop\nquit\n' '^readyok'
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the three new tests FAIL — currently `go infinite` is unrecognized (falls back to default depth 8, finishes WITHOUT waiting for `stop`, but the test for `readyok`/the long-search interruption behavior is not yet implemented). With the `timeout 30` guard, the `go movetime 60000` test would time out (30s) and FAIL since `stop` is not yet honored. Exit nonzero.

- [ ] **Step 3: Teach `CheckInput()` to detect piped stdin in UCI mode (`source/utility.c`)**

In `source/utility.c`, make three mode-gated edits:

In the **Windows** `CheckInput()` (the `#if !defined(UNIX)` version), change the guard:
```c
  if (!xboard && !isatty(fileno(stdin)))
```
to:
```c
  if (!xboard && !uci_mode && !isatty(fileno(stdin)))
```
and change the pipe-handling branch condition:
```c
  if (xboard) {
```
to:
```c
  if (xboard || uci_mode) {
```

In the **UNIX** `CheckInput()` (the `#if defined(UNIX)` version), change its guard:
```c
  if (!xboard && !isatty(fileno(stdin)))
```
to:
```c
  if (!xboard && !uci_mode && !isatty(fileno(stdin)))
```
(The UNIX `select()` path is not gated by `xboard`, so only the guard needs changing.)

- [ ] **Step 4: Add a `uci_mode` branch to `Interrupt()` (`source/interrupt.c`)**

In `source/interrupt.c`, in `Interrupt()`, immediately after the empty-input block:
```c
      if (nargs == 0) {
        Print(32, "ok.\n");
        break;
      }
```
insert:
```c
/*
 *  In UCI mode, handle the mid-search commands directly and never fall through
 *  to Crafty's native command processing.  "stop" aborts the search to play the
 *  best move so far; we break so the bestmove is emitted before any following
 *  command (e.g. "quit") is read.  "isready" must be answered even mid-search.
 */
      if (uci_mode) {
        if (!strcmp(args[0], "stop")) {
          if (thinking || pondering)
            abort_search = 1;
          break;
        }
        if (!strcmp(args[0], "isready")) {
          printf("readyok\n");
          fflush(stdout);
          continue;
        }
        if (!strcmp(args[0], "quit"))
          CraftyExit(0);
        continue;
      }
```

- [ ] **Step 5: Add `go infinite` to `UCIGo()` (`source/uci.c`)**

Add an `infinite` local: change the clock-locals line to also declare `int infinite = 0;`. In the token-parsing loop, add:
```c
    else if (!strcmp(args[i], "infinite"))
      infinite = 1;
```
Change the limit-decision block to:
```c
  if (search_depth == 0 && search_time_limit == 0) {
    if (infinite)
      ;                         /* pondering=1 below makes the search run until stop */
    else if (has_clock)
      UCISetClock(wtime, btime, winc, binc, movestogo);
    else
      search_depth = UCI_DEFAULT_DEPTH;
  }
```
In the pre-search block, change the existing `pondering = 0;` to:
```c
  pondering = (infinite) ? 1 : 0;
```
and after `Iterate(...)` returns (next to the `thinking = 0;` line), add:
```c
  pondering = 0;
```

- [ ] **Step 6: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 7: Empirically verify mid-search interruption (CRITICAL)**

Mid-search `stop` depends on `CheckInput()` detecting bytes on the piped stdin (via `PeekNamedPipe` on Windows). Verify it actually works in this environment:
```bash
time (printf 'uci\nposition startpos\ngo infinite\nstop\nquit\n' | source/crafty_test.exe | grep -E '^bestmove')
```
Expected: a `bestmove` line is printed and the command returns in well under a second (the search aborts on `stop`). If instead it hangs (you'd have to Ctrl-C / it returns no bestmove), then `CheckInput()` is not seeing the piped input — **STOP and report this as BLOCKED with the observed behavior**, because the automated `stop` tests cannot pass. (The code may still be correct for real GUIs that use native pipes; the controller will decide how to handle the test-environment limitation.) Paste the timing/output into your report.

- [ ] **Step 8: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS (including the three interrupt tests), exit 0, and the whole suite completes quickly (no 30s timeouts).

- [ ] **Step 9: Commit**

```bash
git add source/uci.c source/utility.c source/interrupt.c tests/uci/run_tests.sh
git commit -m "feat(uci): support stop and go infinite via mid-search input interrupt"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Phase 4 Definition of Done

- `go wtime/btime/winc/binc [movestogo]` runs a clock-appropriate search (more time → deeper) and returns `bestmove`; fixed `depth`/`movetime` still take precedence; a bare `go` still falls back to a default depth.
- `go infinite` searches until `stop`; `stop` interrupts any running search and emits the best move so far; `isready` is answered `readyok` mid-search; `quit` mid-search exits.
- The `CheckInput`/`Interrupt` hooks are inert when `uci_mode == 0` (WinBoard/native unaffected); the engine still builds cross-platform.
- `sh tests/uci/run_tests.sh` is all green and completes without hangs.

## Out of scope (later phases)

Pondering (`go ponder`/`ponderhit`) — Phase 5; wiring `Hash`/`Threads`/`SyzygyPath`/`OwnBook`/`MultiPV`/`Move Overhead` (Move Overhead will refine `tc_safety_margin`) — Phase 6; cutechess/GUI hardening — Phase 7.

## Self-Review Notes

- **Spec coverage:** Phase 4 = "full time control + stop". Clock mapping → Task 1; `stop`/`infinite` + the two engine hooks → Task 2. Covered.
- **Type consistency:** `UCISetClock(int,int,int,int,int)` static in uci.c. `tc_*` are `int` centiseconds, indexed by color (`game_wtm`/`Flip`). `abort_search`/`thinking`/`pondering` are the existing `int` globals. CheckInput edits add `uci_mode` to existing boolean guards only.
- **Risk note:** the load-bearing uncertainty is whether `CheckInput()` detects the Git-Bash pipe mid-search (Step 7 verifies). The `timeout 30` guard ensures a failure surfaces as a failed test, not a hung suite. The two engine hooks are strictly `uci_mode`-gated, so native/WinBoard behavior cannot change.
