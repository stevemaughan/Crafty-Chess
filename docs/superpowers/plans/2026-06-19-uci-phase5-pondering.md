# UCI Phase 5 — Pondering (`go ponder` / `ponderhit`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Crafty think on the opponent's time over UCI: emit a predicted reply (`bestmove M ponder P`), search on `go ponder` without a time limit, and convert that search into a normal timed search on `ponderhit`.

**Architecture:** Builds directly on Phase 4's interrupt machinery. (1) `UCIGo()` emits the 2nd PV move as the `ponder` move. (2) `go ponder` sets `pondering=1` (so `TimeCheck()` never expires — exactly like `go infinite`) while still applying the GUI clock via `UCISetClock()` so `TimeSet()` computes `time_limit`. (3) `ponderhit` (handled in `Interrupt()`'s `uci_mode` branch) sets `pondering=0`, activating `TimeCheck()`; because the search's `start_time` was set when pondering began and `TimeCheck` measures `time_used = ReadClock() - start_time`, the elapsed ponder time correctly counts toward the move's budget (the UCI convention) — no timer reset needed.

**Tech Stack:** C (C99), Crafty unity build, gcc 15.2 via Git Bash. Tests are `timeout`-guarded transcript checks via `tests/uci/run_tests.sh`.

## Global Constraints

- **Conventions:** UCI logic in `source/uci.c`; the one mode-gated engine hook is in `source/interrupt.c` (the `uci_mode` branch from Phase 4 gains a `ponderhit` case). Build (from `source/`): `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`. Tests from repo root: `sh tests/uci/run_tests.sh`. Build/test via the **Bash tool (Git Bash)**. Engine writes git-ignored `log.*`/`game.*` — don't commit.
- **Ponder move:** `bestmove M ponder P` where `P = last_pv.path[2]` (the predicted opponent reply); emit `ponder P` only when `last_pv.pathl >= 3` AND `last_pv.path[2] != 0`. Otherwise emit a plain `bestmove M` (or `bestmove 0000` for terminal positions, unchanged).
- **`go ponder`:** parse the `ponder` token → set `pondering = 1` before `Iterate()` (search runs until `ponderhit`/`stop`, never times out), AND still call `UCISetClock()` if a clock was given so `TimeSet()` computes `time_limit` for the post-`ponderhit` phase. Reset `pondering = 0` after `Iterate()` (already done in Phase 4).
- **`ponderhit`:** in `Interrupt()`'s `uci_mode` branch, add a case that sets `pondering = 0` and `continue`s (does NOT set `abort_search`, does NOT reset `start_time`). The search then continues and terminates when `time_used` (from the ponder start) reaches `time_limit`. The ponder time legitimately counts toward the budget.
- **`go ponder` + `stop`** already works via Phase 4 (the `stop` branch aborts because `thinking || pondering` is true, and `bestmove` is emitted; the GUI discards it).
- **Distinguish `pondering` (state) from `ponder` (setting):** this phase manipulates the `pondering` STATE global only. The `ponder` SETTING global (data.c:669, default 1; drives the `TimeSet` ÷20 divisor) is NOT touched here — Phase 6's `Ponder` UCI option will own it.
- **Existing test to update:** `tests/uci/run_tests.sh:56` asserts `'^bestmove [a-h][1-8][a-h][1-8][nbrq]?$'` — the trailing `$` will break once `ponder P` is appended. It must be updated to allow an optional ` ponder <move>` suffix. (Other bestmove assertions use no `$` anchor, so the substring match still passes.)
- **Hang safety:** the harness already wraps the engine in `timeout 30` (Phase 4); ponder tests must terminate.

---

## Task 1: Emit the `ponder` move in `bestmove`

`bestmove M ponder P` so the GUI can ponder. Independent of `go ponder` — every search with a 2-move-deep PV now suggests a ponder move.

**Files:**
- Modify: `source/uci.c` (`UCIGo()` bestmove emission)
- Test: `tests/uci/run_tests.sh` (update the `$`-anchored test; add a ponder-suffix test)

**Interfaces:**
- Consumes: `last_pv.path[2]`, `UCIMove`.
- Produces: no new symbols; `bestmove` now optionally carries `ponder P`.

- [ ] **Step 1: Update the `$`-anchored test and add a ponder test**

In `tests/uci/run_tests.sh`, change the existing line 56 assertion pattern from:
```
'^bestmove [a-h][1-8][a-h][1-8][nbrq]?$'
```
to:
```
'^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$'
```
Then append before `exit $fail`:
```sh
# --- Phase 5 Task 1: ponder move in bestmove ---
expect "bestmove includes a ponder move" 'uci\nposition startpos\ngo depth 8\nquit\n' '^bestmove [a-h][1-8][a-h][1-8] ponder [a-h][1-8][a-h][1-8]'
```

- [ ] **Step 2: Run tests to verify the new one fails**

Run: `sh tests/uci/run_tests.sh`
Expected: the new "bestmove includes a ponder move" assertion FAILS (no ` ponder` emitted yet); all others still PASS (the updated line-56 pattern still matches a plain `bestmove`). Exit nonzero.

- [ ] **Step 3: Emit the ponder move in `UCIGo()` (`source/uci.c`)**

In `UCIGo`, replace the bestmove-emission block:
```c
  if (last_pv.pathl == 0 || best == 0)
    printf("bestmove 0000\n");
  else {
    UCIMove(best, movestr);
    printf("bestmove %s\n", movestr);
  }
```
with:
```c
  if (last_pv.pathl == 0 || best == 0)
    printf("bestmove 0000\n");
  else {
    UCIMove(best, movestr);
    if (last_pv.pathl >= 3 && last_pv.path[2]) {
      char pondermv[8];

      UCIMove(last_pv.path[2], pondermv);
      printf("bestmove %s ponder %s\n", movestr, pondermv);
    } else
      printf("bestmove %s\n", movestr);
  }
```

- [ ] **Step 4: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm` (expect exit 0).

- [ ] **Step 5: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS (including the new ponder one and the updated line-56 one), exit 0.

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): emit predicted reply as the ponder move in bestmove"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 2: `go ponder` and `ponderhit`

Search on the opponent's time and convert to a timed search when the predicted move is played.

**Files:**
- Modify: `source/uci.c` (`UCIGo()` — parse `ponder`, set `pondering`)
- Modify: `source/interrupt.c` (`Interrupt()` `uci_mode` branch — add `ponderhit`)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `pondering`, `UCISetClock`, the Phase 4 clock parsing and interrupt branch.
- Produces: `go ponder` searches until `ponderhit`/`stop`; `ponderhit` activates timing.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Phase 5 Task 2: go ponder + ponderhit ---
expect "go ponder then stop -> bestmove"                  'uci\nposition startpos\ngo ponder wtime 2000 btime 2000\nstop\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "ponderhit converts ponder to a timed search"      'uci\nposition startpos\ngo ponder wtime 2000 btime 2000\nponderhit\n' '^bestmove [a-h][1-8][a-h][1-8]'
```
(The second has no trailing `quit`: after `ponderhit` the timed search finishes, `bestmove` is emitted, and the engine exits on EOF.)

- [ ] **Step 2: Run tests to verify they fail/behave wrong**

Run: `sh tests/uci/run_tests.sh`
Expected: the new tests FAIL or behave incorrectly — `go ponder …` currently has no `ponder` handling, so it is treated as an unknown token: `wtime`/`btime` set `has_clock`, so the search runs a normal clock search and finishes WITHOUT waiting for `ponderhit`. The `ponderhit` test would then get a bestmove from the wrong (premature) search timing, and `ponderhit` itself is unrecognized. Implement to make the behavior correct. Exit may be nonzero; proceed to implement.

- [ ] **Step 3: Parse `ponder` and set `pondering` in `UCIGo()` (`source/uci.c`)**

Add an `int ponder_flag = 0;` local (next to the `infinite`/clock locals). In the token-parsing loop, add:
```c
    else if (!strcmp(args[i], "ponder"))
      ponder_flag = 1;
```
Change the limit-decision block to skip the default-depth fallback when pondering (so a clock-less `go ponder` doesn't fix a depth):
```c
  if (search_depth == 0 && search_time_limit == 0) {
    if (infinite)
      ;
    else if (has_clock)
      UCISetClock(wtime, btime, winc, binc, movestogo);
    else if (!ponder_flag)
      search_depth = UCI_DEFAULT_DEPTH;
  }
```
Change the pre-search `pondering` assignment to also cover ponder:
```c
  pondering = (infinite || ponder_flag) ? 1 : 0;
```
(`go ponder wtime …` therefore: clock applied via `UCISetClock` so `time_limit` is computed, but `pondering=1` keeps the search alive until `ponderhit`/`stop`.)

- [ ] **Step 4: Add `ponderhit` to `Interrupt()` (`source/interrupt.c`)**

In the `uci_mode` branch of `Interrupt()` (added in Phase 4), add a `ponderhit` case before the final `continue;`:
```c
        if (!strcmp(args[0], "ponderhit")) {
          pondering = 0;
          continue;
        }
```
(This activates `TimeCheck()`: the search keeps running but now terminates when `time_used` — measured from the ponder start — reaches `time_limit`. No abort, no timer reset, so the ponder time counts toward the move.)

- [ ] **Step 5: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm` (expect exit 0, no warnings).

- [ ] **Step 6: Empirically verify ponder behavior**

```bash
# go ponder must NOT finish on its own (pondering holds it); ponderhit ends it:
time (printf 'uci\nposition startpos\ngo ponder wtime 4000 btime 4000\nponderhit\n' | source/crafty_test.exe | grep -E '^bestmove')
# go ponder + stop returns a bestmove the GUI would discard:
printf 'uci\nposition startpos\ngo ponder wtime 4000 btime 4000\nstop\nquit\n' | source/crafty_test.exe | grep -E '^bestmove'
```
Expected: both print a `bestmove` and return quickly (well under the `timeout`). For the first, the time from `ponderhit` to `bestmove` should reflect the computed budget (≈ a couple hundred ms for a 4s clock), not hang. Paste outputs into your report. (If `go ponder` ever hangs because `ponderhit` is not detected mid-search, that is the same `CheckInput` pipe path verified working in Phase 4 — but report it if it regresses.)

- [ ] **Step 7: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, exit 0, suite completes quickly (no 30s timeouts).

- [ ] **Step 8: Commit**

```bash
git add source/uci.c source/interrupt.c tests/uci/run_tests.sh
git commit -m "feat(uci): support go ponder and ponderhit (think on opponent time)"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Phase 5 Definition of Done

- Every search with a ≥2-ply PV emits `bestmove M ponder P`; terminal/short PVs emit a plain `bestmove M` / `bestmove 0000`.
- `go ponder` searches without timing out; `ponderhit` converts it to a timed search that finishes on the clock (ponder time counted); `stop` during ponder still yields a `bestmove`.
- The `Interrupt` `ponderhit` case is `uci_mode`-gated (native/WinBoard unchanged); the engine still builds clean.
- `sh tests/uci/run_tests.sh` is all green and hang-free.

## Out of scope (later phases)

Wiring `Hash`/`Threads`/`SyzygyPath`/`OwnBook`/`MultiPV`/`Move Overhead` to real settings, and making the `Ponder` UCI option actually toggle the `ponder` setting global (Phase 6); cutechess/GUI hardening (Phase 7).

## Self-Review Notes

- **Spec coverage:** Phase 5 = pondering. ponder move in bestmove → Task 1; `go ponder` + `ponderhit` → Task 2. Covered.
- **Type consistency:** `ponder_flag`/`infinite`/`has_clock` are local `int`s in `UCIGo`. `pondering` is the existing state global. `ponderhit` reuses the existing `Interrupt` `uci_mode` branch and the `pondering` global. `last_pv.path[2]` formatted via the existing `UCIMove`.
- **Risk note:** the subtle part is the ponderhit timing transition, which relies on `start_time` (set at `Iterate` start = ponder start) and `TimeCheck`'s `time_used = ReadClock() - start_time` — verified in the source. The empirical check (Step 6) confirms `go ponder` doesn't self-terminate and `ponderhit` ends it. Mid-search detection reuses Phase 4's verified `CheckInput` pipe path.
