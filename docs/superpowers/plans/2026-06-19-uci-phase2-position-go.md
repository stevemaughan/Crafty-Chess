# UCI Phase 2 — Position Setup & Fixed-Limit Search Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Crafty's UCI mode accept `position [startpos|fen …] moves …`, run a fixed-limit search on `go depth N` / `go movetime T`, and report `bestmove <coordinate-move>` — with Crafty's native search output suppressed and `ucinewgame` clearing state for a fresh game.

**Architecture:** Extends `source/uci.c` (built in Phase 1). The UCI layer calls Crafty's engine functions directly: `SetBoard()` + `InputMove()`/`MakeMoveRoot()` to build the position, then `Iterate(game_wtm, think, 0)` to search, extracting the best move from `last_pv.path[1]` and formatting it as UCI coordinate notation. This mirrors the exact sequence `main()`'s game loop uses, but does NOT make the engine's move on the board (UCI is stateless — the GUI resends the full move list each turn) and does NOT consult the opening book (correct while `OwnBook` defaults off).

**Tech Stack:** C (C99), Crafty unity build (`source/crafty.c`), gcc 15.2 via Git Bash. Tests are black-box stdin→stdout transcript checks via `tests/uci/run_tests.sh`.

## Global Constraints

- **Unity build / conventions (from Phase 1):** all UCI code lives in `source/uci.c` (already in the `crafty.c` manifest). No new files, no new dependencies. UCI command dispatch uses exact `strcmp`. Build (from `source/`): `gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`. Run tests from repo root: `sh tests/uci/run_tests.sh`. Build/test via the **Bash tool (Git Bash)**.
- **Additive and mode-gated:** no WinBoard/native code path is altered. All new behavior is inside `UCI()` and its helpers. Engine writes git-ignored `log.*`/`game.*` at runtime — don't commit them.
- **`go` must NOT mutate the board.** After `Iterate()`, leave `block[0]`'s root position unchanged. Do NOT `MakeMoveRoot()` the engine's chosen move. (The GUI resends the full position + move list before the next `go`.)
- **Suppress native search output in UCI mode.** Save and zero `display_options`, `kibitz`, and `post` immediately before `Iterate()`, and restore them immediately after. (All of Crafty's streaming search output goes through `Print(level,…)` gated by `display_options`, plus `Kibitz()` gated by `kibitz`; zeroing both silences it.) Verify empirically that a `go` emits only the `bestmove` line.
- **Move output = pure UCI coordinate notation:** from-square + to-square + lowercase promotion letter (`e2e4`, `g1f3`, `e7e8q`). Castling is emitted as the king's two-square move (`e1g1`), which falls out of from/to automatically because Crafty stores castling that way. Do NOT use `OutputMove()` (it produces SAN). Promotion encoding: `Promote(move)` ∈ {2=knight,3=bishop,4=rook,5=queen}; map via `" pnbrqk"[Promote(move)]`.
- **Terminal position** (no legal move; `last_pv.pathl == 0` or `last_pv.path[1] == 0`): emit `bestmove 0000`.
- **Search limits:** `go depth N` → `search_depth = N; search_time_limit = 0;`. `go movetime T` (T in ms) → `search_time_limit = T / 10; search_depth = 0;` (`search_time_limit` is in centiseconds). If neither is given (e.g. bare `go`, `go infinite`, or `go wtime …`), fall back to `search_depth = UCI_DEFAULT_DEPTH` (8). Full time controls (`wtime`/`btime`/…), `stop`, and `infinite` are later phases — this phase just guarantees a `bestmove` is always returned.
- **Engine function contracts (verified, with signatures):**
  - `void SetBoard(TREE*, int nargs, char *args[], int special)` — `args` = {piece-placement, side("w"/"b"), castling, en-passant}; `special=0`. Sets the board, `game_wtm`, repetition list, bitboards. Caller sets `move_number = 1`.
  - `int InputMove(TREE*, int ply, int wtm, int silent, int ponder_list, char *text)` — accepts UCI coordinate moves incl. case-insensitive promotion (`e7e8q`). Returns 0 on illegal/garbled.
  - `void MakeMoveRoot(TREE*, int wtm, int move)` — applies a root move; maintains repetition (`rep_list`/`rep_index`) and 50-move state.
  - `int Iterate(int wtm, int search_type, int root_list_done)` — search; use `think` (=1) and `0`. Best move = `tree->pv[0].path[1]` after copying `last_pv = tree->pv[0]`. Does not change the root board.
  - `void InitializeChessBoard(TREE*)` — reset to start position (sets `game_wtm=1`, castle/ep, repetition).
  - `void InitializeHashTables(int fault_in)` — clears all hash entries; pass `0`.
  - Macros (chess.h): `From(m)&63`, `To(m)`, `Promote(m)`, `File(sq)=sq&7`, `Rank(sq)=sq>>3`, square a1=0/h8=63, `Flip(x)`. Root tree = `block[0]`.
  - Relevant globals (data.h): `game_wtm`, `move_number`, `search_depth`, `search_time_limit`, `thinking`, `pondering`, `display`, `last_pv` (`PATH`, `.path[1]` is best move, `.pathl` is length), `display_options`, `kibitz`, `post`.

---

## Task 1: UCI coordinate move formatter + `go` (fixed depth / movetime) → `bestmove`

Implements the search path on the engine's *current* board (which is the standard start position right after startup), with native output suppressed. `position` setup comes in Task 2.

**Files:**
- Modify: `source/uci.c` (add `UCIMove()` and `UCIGo()` helpers; wire `go` into the `UCI()` loop; add `<stdlib.h>` for `atoi` if not already available via headers)
- Test: `tests/uci/run_tests.sh` (add a `reject` helper + assertions)

**Interfaces:**
- Consumes: the `UCI()` loop and `block[0]`, `Iterate`, `last_pv`, search globals (all already available).
- Produces: `static void UCIMove(int move, char *out)` — writes UCI coordinate notation (≥6-byte buffer). `static void UCIGo(int nargs, char *args[])` — parses limits, runs the search with output suppressed, prints `bestmove`. `#define UCI_DEFAULT_DEPTH 8`.

- [ ] **Step 1: Add a `reject` helper to the harness**

In `tests/uci/run_tests.sh`, immediately after the `expect()` function definition (before the first test), add:
```sh
# reject <description> <transcript> <egrep-pattern> — assert the pattern is ABSENT
reject() {
  desc=$1; transcript=$2; pattern=$3
  out=$(printf '%b' "$transcript" | "$ENGINE" 2>/dev/null)
  if printf '%s\n' "$out" | grep -Eq "$pattern"; then
    echo "FAIL: $desc -- unexpected /$pattern/"
    fail=1
  else
    echo "PASS: $desc"
  fi
}
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Task 1 (Phase 2): go / bestmove on the start position ---
expect "go depth -> well-formed bestmove" 'uci\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$'
expect "go movetime -> bestmove"          'uci\ngo movetime 200\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
reject "no native search header leaks"    'uci\ngo depth 6\nquit\n' 'variation'
reject "no native PV ply line leaks"      'uci\ngo depth 6\nquit\n' '^\s+[0-9]+->'
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the two `expect` lines FAIL (no `bestmove` — `go` is currently ignored as unknown). The `reject` lines may pass vacuously; that's fine. Exit nonzero.

- [ ] **Step 4: Implement `UCIMove` and `UCIGo` in `source/uci.c`**

If `source/uci.c` does not already include `<stdlib.h>`, add it after the existing includes at the top (needed for `atoi`). Then add these two helpers above `void UCI(void)` (after `UCISendId`):
```c
#define UCI_DEFAULT_DEPTH 8

/*
 *  UCIMove() converts an internal move to UCI long-algebraic coordinate
 *  notation (e2e4, g1f3, e7e8q).  Castling is encoded by Crafty as the king's
 *  two-square move, so from/to already yields e1g1 / e1c1.  out must be >= 6
 *  bytes.
 */
static void UCIMove(int move, char *out) {
  int from = From(move), to = To(move), promo = Promote(move);

  out[0] = 'a' + File(from);
  out[1] = '1' + Rank(from);
  out[2] = 'a' + File(to);
  out[3] = '1' + Rank(to);
  if (promo) {
    out[4] = " pnbrqk"[promo];
    out[5] = 0;
  } else
    out[4] = 0;
}

/*
 *  UCIGo() handles the UCI "go" command for fixed-limit searches.  It parses
 *  "depth N" and "movetime T" (ms); anything else (bare go, infinite, clock
 *  limits) falls back to a default fixed depth so a bestmove is always
 *  produced.  Crafty's native search output is suppressed for the duration of
 *  the search (display_options/kibitz/post zeroed), and the engine's move is
 *  NOT played on the board (UCI is stateless).
 */
static void UCIGo(int nargs, char *args[]) {
  TREE *const tree = block[0];
  int i, best, saved_display_options, saved_kibitz, saved_post;
  char movestr[8];

  search_depth = 0;
  search_time_limit = 0;
  for (i = 1; i < nargs; i++) {
    if (!strcmp(args[i], "depth") && i + 1 < nargs)
      search_depth = atoi(args[++i]);
    else if (!strcmp(args[i], "movetime") && i + 1 < nargs)
      search_time_limit = atoi(args[++i]) / 10;
  }
  if (!search_depth && !search_time_limit)
    search_depth = UCI_DEFAULT_DEPTH;
/*
 *  Suppress Crafty's native streaming search output while we search.
 */
  saved_display_options = display_options;
  saved_kibitz = kibitz;
  saved_post = post;
  display_options = 0;
  kibitz = 0;
  post = 0;
/*
 *  Set the pre-search state the same way main()'s game loop does, then search.
 */
  pondering = 0;
  thinking = 1;
  last_pv.pathd = 0;
  last_pv.pathl = 0;
  display = tree->position;
  tree->status[1] = tree->status[0];
  Iterate(game_wtm, think, 0);
  thinking = 0;
  display_options = saved_display_options;
  kibitz = saved_kibitz;
  post = saved_post;
/*
 *  Report the best move (the first move of the principal variation).
 */
  last_pv = tree->pv[0];
  best = last_pv.path[1];
  if (last_pv.pathl == 0 || best == 0)
    printf("bestmove 0000\n");
  else {
    UCIMove(best, movestr);
    printf("bestmove %s\n", movestr);
  }
  fflush(stdout);
  search_depth = 0;
  search_time_limit = 0;
}
```

- [ ] **Step 5: Wire `go` into the `UCI()` loop**

In `source/uci.c`, in the `UCI()` command loop, add a branch (after the `isready` branch, before `quit`):
```c
    else if (!strcmp(args[0], "go"))
      UCIGo(nargs, args);
```

- [ ] **Step 6: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 7: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS (Phase 1 + the four new ones), exit 0.

- [ ] **Step 8: Manually confirm output cleanliness**

Run: `printf 'uci\ngo depth 8\nquit\n' | source/crafty_test.exe`
Expected: after the handshake block, the ONLY additional line is `bestmove <move>` — no Crafty depth/score/PV table, no "time used", no kibitz lines. If any native line leaks, identify whether it comes from a `Print(level,…)` (raise the masked level into the saved/zeroed set) or a raw `printf` (gate it on `!uci_mode`), and re-verify. Record the raw output in your report.

- [ ] **Step 9: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): add go (fixed depth/movetime) search and bestmove output"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 2: `position` command (startpos / fen + moves)

Builds the board from a UCI `position` command so `go` searches the requested position. This is where the strong behavioral correctness tests live (find a forced mate; detect stalemate).

**Files:**
- Modify: `source/uci.c` (add `UCIPosition()`; wire `position` into the `UCI()` loop; add the start-position FEN constant)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `SetBoard`, `InputMove`, `MakeMoveRoot`, `block[0]`, `game_wtm`, `move_number`, `Flip`, and `UCIGo`/`UCIMove` from Task 1.
- Produces: `static void UCIPosition(int nargs, char *args[])` — sets the board from `startpos`/`fen` and replays the `moves` list.

- [ ] **Step 1: Write the failing tests**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Task 2 (Phase 2): position setup ---
# Fool's-mate position, Black to move: Qd8-h4 is mate (unique). Expect d8h4.
expect "position fen + go finds mate-in-1" 'uci\nposition fen rnb1kbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2\ngo depth 4\nquit\n' '^bestmove d8h4'
# startpos + replayed moves yields a legal reply.
expect "position startpos moves -> legal reply" 'uci\nposition startpos moves e2e4 e7e5 g1f3\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
# Stalemate (Black to move, no legal move, not in check) -> bestmove 0000.
expect "position stalemate -> bestmove 0000" 'uci\nposition fen 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1\ngo depth 2\nquit\n' '^bestmove 0000'
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `sh tests/uci/run_tests.sh`
Expected: the three new lines FAIL (`position` is currently ignored, so `go` searches the start position — the mate/stalemate assertions won't match). Exit nonzero.

- [ ] **Step 3: Implement `UCIPosition` in `source/uci.c`**

Add the start-position piece-placement constant near the top of `source/uci.c` (after includes):
```c
static const char uci_start_fen[] =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
```
Add the helper above `void UCI(void)`:
```c
/*
 *  UCIPosition() sets the board from a UCI "position" command:
 *  "position startpos [moves ...]" or "position fen <6 fields> [moves ...]".
 *  Only the first four FEN fields are used (SetBoard ignores half/full-move
 *  counters).  The moves list is replayed exactly as main()'s game loop applies
 *  opponent moves, keeping repetition/50-move state correct.
 */
static void UCIPosition(int nargs, char *args[]) {
  TREE *const tree = block[0];
  int i, move, wtm, moves_at = -1;
  char *fen_args[4];

  if (nargs < 2)
    return;
  if (!strcmp(args[1], "startpos")) {
    fen_args[0] = (char *) uci_start_fen;
    fen_args[1] = "w";
    fen_args[2] = "KQkq";
    fen_args[3] = "-";
    SetBoard(tree, 4, fen_args, 0);
  } else if (!strcmp(args[1], "fen")) {
    if (nargs < 6)               /* need piece, side, castle, ep */
      return;
    fen_args[0] = args[2];
    fen_args[1] = args[3];
    fen_args[2] = args[4];
    fen_args[3] = args[5];
    SetBoard(tree, 4, fen_args, 0);
  } else
    return;
  move_number = 1;
/*
 *  Locate the "moves" keyword (it cannot appear inside a FEN), then replay.
 */
  for (i = 2; i < nargs; i++)
    if (!strcmp(args[i], "moves")) {
      moves_at = i + 1;
      break;
    }
  if (moves_at < 0)
    return;
  for (i = moves_at; i < nargs; i++) {
    wtm = game_wtm;
    move = InputMove(tree, 0, wtm, 1, 0, args[i]);
    if (!move)                   /* illegal/garbled: stop replaying */
      break;
    MakeMoveRoot(tree, wtm, move);
    tree->curmv[0] = move;
    game_wtm = Flip(wtm);
    if (game_wtm)
      move_number++;
  }
}
```

- [ ] **Step 4: Wire `position` into the `UCI()` loop**

In `source/uci.c`, in the `UCI()` loop, add a branch (next to the `go` branch):
```c
    else if (!strcmp(args[0], "position"))
      UCIPosition(nargs, args);
```

- [ ] **Step 5: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 6: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, including the mate (`bestmove d8h4`) and stalemate (`bestmove 0000`). Exit 0.

- [ ] **Step 7: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): add position command (startpos/fen + moves replay)"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Task 3: `ucinewgame` (clear hash + reset board)

GUIs send `ucinewgame` before a new game. Clear the transposition table and reset the board to the start position so stale scores don't leak between games.

**Files:**
- Modify: `source/uci.c` (add the `ucinewgame` branch to the `UCI()` loop)
- Test: `tests/uci/run_tests.sh`

**Interfaces:**
- Consumes: `InitializeHashTables`, `InitializeChessBoard`, `block[0]`, `move_number`.
- Produces: no new symbols; adds `ucinewgame` handling.

- [ ] **Step 1: Write the failing test**

Append to `tests/uci/run_tests.sh` before `exit $fail`:
```sh
# --- Task 3 (Phase 2): ucinewgame ---
expect "ucinewgame then play works" 'uci\nucinewgame\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
```

- [ ] **Step 2: Run test to verify it fails or passes vacuously**

Run: `sh tests/uci/run_tests.sh`
Expected: this assertion likely already PASSES vacuously (because `ucinewgame` is ignored as unknown, then `position startpos` + `go` still work). That is acceptable — this task's value is the explicit hash/board reset; proceed to implement it so the behavior is correct and intentional, not incidental. (If it FAILS, that indicates an unknown-command handling problem to fix.)

- [ ] **Step 3: Implement the `ucinewgame` branch**

In `source/uci.c`, in the `UCI()` loop, add (next to the `position`/`go` branches):
```c
    else if (!strcmp(args[0], "ucinewgame")) {
      InitializeHashTables(0);
      InitializeChessBoard(block[0]);
      move_number = 1;
    }
```

- [ ] **Step 4: Rebuild**

Run: `cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_test.exe -lwinmm`
Expected: exit 0, no warnings.

- [ ] **Step 5: Run tests to verify they pass**

Run (from repo root): `sh tests/uci/run_tests.sh`
Expected: all assertions PASS, exit 0.

- [ ] **Step 6: Commit**

```bash
git add source/uci.c tests/uci/run_tests.sh
git commit -m "feat(uci): add ucinewgame (clear hash and reset board)"
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## Phase 2 Definition of Done

- `position startpos moves …` and `position fen …` set the board correctly; the move list is replayed with repetition/50-move state intact.
- `go depth N` and `go movetime T` run a search and return a single `bestmove <coordinate-move>`; a bare/unsupported `go` falls back to a default depth and still returns a move; terminal positions return `bestmove 0000`.
- A forced mate is found (`position fen <fool's-mate> … go` → `bestmove d8h4`); stalemate returns `bestmove 0000`.
- No Crafty native search output leaks into the UCI stream (only `bestmove` is emitted for a `go`).
- `go` does not alter the board; `ucinewgame` clears the hash and resets to the start position.
- WinBoard and native console modes remain unchanged; `sh tests/uci/run_tests.sh` is all green.

## Out of scope (later phases)

`info depth/score/nodes/pv …` streaming (Phase 3); `wtime`/`btime`/`winc`/`binc`/`movestogo` clock management, `stop`, and `go infinite` interruption (Phase 4); pondering (Phase 5); wiring `Hash`/`Threads`/`SyzygyPath`/`OwnBook`/`MultiPV`/`Move Overhead` to real settings (Phase 6).

## Self-Review Notes

- **Spec coverage:** Phase 2 = "`position` parsing + `go depth`/`movetime` fixed search → `bestmove`." position → Task 2; go (depth/movetime) + bestmove + output suppression → Task 1; ucinewgame (supporting state reset) → Task 3. Covered.
- **Type consistency:** `UCIMove(int, char*)`, `UCIGo(int, char*[])`, `UCIPosition(int, char*[])` are `static` in `uci.c`; all engine calls use the verified signatures in the Global Constraints block. `last_pv.path[1]`/`.pathl`, `From`/`To`/`Promote`/`File`/`Rank`, and the search globals match their `chess.h`/`data.h` declarations.
- **Risk note:** the only empirical unknown is output cleanliness (Task 1 Step 8). The verified gating (`Print` ← `display_options`, `Kibitz` ← `kibitz`) makes zeroing both the expected-sufficient fix; the manual check and the two `reject` assertions guard it.
