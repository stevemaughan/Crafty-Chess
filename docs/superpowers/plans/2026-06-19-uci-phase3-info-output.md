# UCI Phase 3 — `info` Search Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While a UCI search runs, stream `info depth D score cp X|mate Y nodes N nps Q time T pv <moves…>` lines — one per completed iteration — reusing the data Crafty already computes, then the existing `bestmove`.

**Architecture:** A new `UCIInfo()` function in `source/uci.c` formats and prints one UCI info line from a Crafty `PATH` (the principal variation). It is invoked by a single mode-gated hook at the top of `DisplayPV()` (utility.c) — the one place Crafty reports a new PV — which, when `uci_mode` is set, calls `UCIInfo()` and returns instead of producing native output. `UCIGo()` sets `noise_level = 0` during the search so `DisplayPV()` fires on every iteration (streaming), and keeps `display_options`/`kibitz`/`post` zeroed to suppress Crafty's other native search prints.

**Tech Stack:** C (C99), Crafty unity build, gcc 15.2 via Git Bash. Tests are black-box stdin→stdout transcript checks via `tests/uci/run_tests.sh`.

## Global Constraints

- **Unity build / conventions:** UCI formatting lives in `source/uci.c`; the `UCIInfo` prototype goes in the shared `source/chess.h`. Build (from `source/`): `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`. Tests from repo root: `sh tests/uci/run_tests.sh`. Build/test via the **Bash tool (Git Bash)**. Engine writes git-ignored `log.*`/`game.*` — don't commit them.
- **Exactly one hook into shared engine code:** a mode-gated `if (uci_mode) { UCIInfo(wtm, time, pv); return; }` at the top of `DisplayPV()` in `source/utility.c` (after its variable declarations, before the `nskip` loop). When `uci_mode == 0`, `DisplayPV()` must be behaviorally identical to before. No other engine file is modified except `uci.c`, `chess.h`, `utility.c`, and the test file.
- **Streaming:** `UCIGo()` saves and sets `noise_level = 0` for the search (restored after), so `DisplayPV()` is called once per completed iteration (Site A in iterate.c, gated by `elapsed >= noise_level`). Keep `display_options`/`kibitz`/`post` zeroed (they suppress the other native iterate.c prints: the `…variation` header, per-iteration stats, and `Kibitz`).
- **Score conversion:** Crafty's `pv->pathv` is **White-relative centipawns** (`PAWN_VALUE == 100`, so no scaling). UCI wants **side-to-move-relative**: `cp = wtm ? pv->pathv : -pv->pathv`. Mate: `MateScore(s)` is `Abs(s) > 32000`; `MATE == 32768`; mate distance in full moves = `(MATE - Abs(pv->pathv) + 1) / 2`; emit `score mate N` when the side to move is mating (converted score > 0), `score mate -N` when being mated.
- **Fields & units:** `depth` = `pv->pathd`. `nodes` = `block[0]->nodes_searched` (`uint64_t`). The `time` parameter of `DisplayPV` is **centiseconds**; UCI `time` is ms → `time * 10`; UCI `nps` is nodes/sec → `(time > 0) ? nodes * 100 / time : nodes`. `pv` = `pv->path[1] … pv->path[pv->pathl - 1]`, each formatted with the existing `UCIMove()` (coordinate notation) and space-separated.
- **Omitted this phase (documented limitations):** `seldepth` (Crafty tracks no max-ply high-water mark) and `hashfull` (no fullness counter). Both are optional in UCI; candidates for a later hardening phase.
- **Stream integrity:** a `go` must emit only well-formed `info …` lines followed by exactly one `bestmove`. The Phase 2 cleanliness assertions (no `variation` header, no native `…->` PV rows) must still pass.
- **Relevant declarations (verified):** `void DisplayPV(TREE *RESTRICT, int, int, int, PATH *, int)` (chess.h:380); `PATH { int path[]; int pathh; int pathl; int pathd; int pathv; }` (chess.h:159-165); `MATE` (chess.h:86), `Abs` (chess.h:534), `MateScore` (chess.h:617); `unsigned noise_level` (data.h:205, default 100); `extern int uci_mode` (data.h); `block[0]` is the root `TREE*`; `nodes_searched` is `uint64_t`; `inttypes.h`/`PRIu64` available via chess.h. `UCIMove(int, char*)` is `static` in uci.c (UCIInfo is in the same file, so it can call it).

---

## Task 1: `UCIInfo` (cp scores) + `DisplayPV` hook + per-iteration streaming

Implements the info line for non-mate scores and wires it into the search. Mate-score formatting is added in Task 2.

**Files:**
- Modify: `source/uci.c` (add `UCIInfo()`; save/zero/restore `noise_level` in `UCIGo()`)
- Modify: `source/chess.h` (add the `UCIInfo` prototype near the other display prototypes, e.g. after `DisplayPV`)
- Modify: `source/utility.c` (add the mode-gated hook at the top of `DisplayPV()`)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `block[0]`, `pv->pathd/pathv/pathl/path[]`, `UCIMove`, the search globals.
- Produces: `void UCIInfo(int wtm, int etime, PATH *pv)` — prints one UCI info line (cp scores in this task). Declared in chess.h.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Phase 3 Task 1: UCI info streaming (cp scores) ---
expect "info line has all fields + coord pv" 'uci\nposition startpos\ngo depth 8\nquit\n' '^info depth 8 score cp -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv [a-h][1-8][a-h][1-8]'
expect "info streams an early depth too"      'uci\nposition startpos\ngo depth 8\nquit\n' '^info depth 1 score cp '
expect "bestmove still follows the info"      'uci\nposition startpos\ngo depth 8\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the two `info …` assertions FAIL (no info lines emitted yet — search output is suppressed). The `bestmove` line still passes. Exit nonzero.

- [ ] **Step 3: Add the `UCIInfo` prototype to `chess.h`**

In `source/chess.h`, find the `DisplayPV` prototype (`void DisplayPV(TREE *RESTRICT, int, int, int, PATH *, int);`, ~line 380) and add directly after it:
```c
void UCIInfo(int, int, PATH *);
```

- [ ] **Step 4: Implement `UCIInfo` in `source/uci.c`**

Add this function in `source/uci.c` above `void UCI(void)` (after `UCIGo`). This task handles `score cp` only; the mate branch is added in Task 2:
```c
/*
 *  UCIInfo() emits one UCI "info" line for a completed search iteration.  It is
 *  called from DisplayPV() when uci_mode is set, replacing Crafty's native PV
 *  display.  Crafty's pv->pathv is White-relative centipawns (PAWN_VALUE==100);
 *  UCI wants side-to-move-relative, so the score is negated when Black is to
 *  move.  The DisplayPV "time" argument is centiseconds; UCI uses ms and nps in
 *  nodes/sec.  Moves are formatted with UCIMove (coordinate notation).
 */
void UCIInfo(int wtm, int etime, PATH *pv) {
  TREE *const tree = block[0];
  uint64_t nodes = tree->nodes_searched;
  uint64_t nps = (etime > 0) ? (nodes * 100 / (uint64_t) etime) : nodes;
  int i, n = 0;
  char line[4096];

  n += sprintf(line + n, "info depth %d score cp %d nodes %" PRIu64
      " nps %" PRIu64 " time %d pv", pv->pathd, (wtm) ? pv->pathv : -pv->pathv,
      nodes, nps, etime * 10);
  for (i = 1; i < pv->pathl; i++) {
    char mv[8];

    UCIMove(pv->path[i], mv);
    n += sprintf(line + n, " %s", mv);
  }
  printf("%s\n", line);
  fflush(stdout);
}
```

- [ ] **Step 5: Add the mode-gated hook to `DisplayPV()` in `source/utility.c`**

In `source/utility.c`, in `DisplayPV()`, immediately after the variable declarations at the top (after the `unsigned int idle_time;` line, before the `for (i = 0; i < n_root_moves; i++)` nskip loop), add:
```c
  if (uci_mode) {
    UCIInfo(wtm, time, pv);
    return;
  }
```

- [ ] **Step 6: Enable per-iteration streaming in `UCIGo()` (`source/uci.c`)**

In `UCIGo()`, extend the existing save/zero/restore of the display globals to also cover `noise_level`. Add a saved variable and set `noise_level = 0` alongside the existing `display_options = 0; kibitz = 0; post = 0;`, and restore it alongside them. Concretely: declare `unsigned saved_noise;` with the other saved vars; before `Iterate`, add `saved_noise = noise_level; noise_level = 0;` next to the existing zeroing; after `Iterate`, add `noise_level = saved_noise;` next to the existing restores. (Setting `noise_level = 0` makes `DisplayPV()` fire on every completed iteration instead of only once at the end.)

- [ ] **Step 7: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 8: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, including the new info-line ones and the still-present `bestmove`, and the Phase 2 cleanliness rejects (`variation`, `…->`). Exit 0.

- [ ] **Step 9: Manually confirm the stream shape**

Run: `printf 'uci\nposition startpos\ngo depth 10\nquit\n' | source/crafty_test.exe`
Expected: a sequence of `info depth 1 …`, `info depth 2 …`, … `info depth 10 …` lines (increasing depth, each with score/nodes/nps/time and a coordinate `pv`), then exactly one `bestmove …`. No native Crafty PV/score/header lines. Paste the output into your report.

- [ ] **Step 10: Commit**

```bash
git add source/uci.c source/chess.h source/utility.c tests/uci/run_tests.sh
git commit -m "feat(uci): stream UCI info lines (depth/score cp/nodes/nps/time/pv) during search"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 2: mate scores (`score mate N`)

Extends `UCIInfo` to report forced mates as `score mate N` (side-to-move relative) instead of a raw centipawn value.

**Files:**
- Modify: `source/uci.c` (`UCIInfo` — add the mate branch)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `MateScore`, `MATE`, `Abs` (chess.h); the `UCIInfo` from Task 1.
- Produces: no new symbols.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Phase 3 Task 2: mate scores ---
# Fool's mate, Black to move: Qd8-h4 is mate in 1.
expect "mate-in-1 -> score mate 1 with pv d8h4" 'uci\nposition fen rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2\ngo depth 4\nquit\n' '^info depth [0-9]+ score mate 1 .* pv d8h4'
# Non-mate position still reports cp.
expect "normal position still uses score cp"    'uci\nposition startpos\ngo depth 6\nquit\n' '^info depth [0-9]+ score cp '
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the `score mate 1` assertion FAILS (the mate currently prints as a huge `score cp` value); the `score cp` assertion passes. Exit nonzero.

- [ ] **Step 3: Add the mate branch to `UCIInfo` (`source/uci.c`)**

Replace the single `score cp` `sprintf` (the first `n += sprintf(...)` for the `info depth … score cp …` prefix) with a mate/cp split. The function becomes:
```c
void UCIInfo(int wtm, int etime, PATH *pv) {
  TREE *const tree = block[0];
  uint64_t nodes = tree->nodes_searched;
  uint64_t nps = (etime > 0) ? (nodes * 100 / (uint64_t) etime) : nodes;
  int i, n = 0, score = (wtm) ? pv->pathv : -pv->pathv;
  char line[4096];

  if (MateScore(pv->pathv)) {
    int moves = (MATE - Abs(pv->pathv) + 1) / 2;

    n += sprintf(line + n, "info depth %d score mate %d", pv->pathd,
        (score > 0) ? moves : -moves);
  } else
    n += sprintf(line + n, "info depth %d score cp %d", pv->pathd, score);
  n += sprintf(line + n, " nodes %" PRIu64 " nps %" PRIu64 " time %d pv",
      nodes, nps, etime * 10);
  for (i = 1; i < pv->pathl; i++) {
    char mv[8];

    UCIMove(pv->path[i], mv);
    n += sprintf(line + n, " %s", mv);
  }
  printf("%s\n", line);
  fflush(stdout);
}
```

- [ ] **Step 4: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 5: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, including `score mate 1` with `pv d8h4` and the `score cp` cases. Exit 0.

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): report forced mates as score mate N in info lines"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Phase 3 Definition of Done

- A `go` search streams `info depth D score cp X|mate Y nodes N nps Q time T pv <coords…>` lines, one per iteration, then exactly one `bestmove`.
- Scores are side-to-move-relative centipawns; forced mates report `score mate N` (negative when being mated).
- The PV is coordinate notation; nodes/nps/time are populated with correct units.
- No native Crafty search output leaks (Phase 2 cleanliness assertions still pass); WinBoard/native modes unchanged (the `DisplayPV` hook is inert when `uci_mode == 0`).
- `sh tests/uci/run_tests.sh` is all green.

## Out of scope (later phases)

`seldepth`/`hashfull` (engine doesn't track them); `wtime`/`btime`/`winc`/`binc`/`movestogo` clock management, `stop`, `go infinite` interruption (Phase 4); pondering (Phase 5); wiring options to real settings (Phase 6); `info` bound flags (`lowerbound`/`upperbound`) and `multipv` (Phase 6 MultiPV).

## Self-Review Notes

- **Spec coverage:** Phase 3 = "translate PV/score/depth/nodes to `info` lines." UCIInfo + DisplayPV hook + streaming → Task 1; mate scores → Task 2. Covered. seldepth/hashfull explicitly deferred.
- **Type consistency:** `UCIInfo(int wtm, int etime, PATH *pv)` declared in chess.h, defined in uci.c, called from utility.c with `UCIInfo(wtm, time, pv)`. `nodes`/`nps` are `uint64_t` (`PRIu64`). Score/mate use `pv->pathv` (White-relative) converted to stm via `score`/the `wtm ? … : …` form consistently.
- **Risk note:** the mate sign/perspective is the subtle part — guarded by the behavioral test asserting `score mate 1` (positive, mating side to move) for the fool's-mate position. The streaming hook's correctness (fires every iteration, no native leak, board untouched) is guarded by the field assertions, the early-depth assertion, and the carried-over Phase 2 cleanliness rejects.
