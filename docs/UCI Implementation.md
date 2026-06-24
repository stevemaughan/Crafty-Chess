# UCI Implementation for Crafty

This document describes how the **Universal Chess Interface (UCI)** protocol was
added to Crafty, which historically supported only the WinBoard/xboard protocol
and its own native console commands. It covers the design approach, the
architecture, the implementation of each UCI command and option, the small
mode-gated hooks into the engine core, the technical problems encountered and
how they were solved, and how the result was tested.

The work was done in seven incremental phases, each independently planned,
implemented, reviewed, and merged. The result is a single binary that speaks
**three protocols** — UCI, WinBoard, and native console — auto-selected at
runtime, with the original two interfaces left behaviorally unchanged.

---

## 1. Goals and constraints

- **Add UCI without removing anything.** WinBoard and the native console must
  keep working exactly as before. The engine auto-detects which protocol the
  GUI is speaking.
- **Preserve every feature.** Opening books, Syzygy/endgame tablebases,
  transposition (hash) tables, SMP threads, pondering, and analysis are all
  exposed through UCI options.
- **Adapter, not rewrite.** The search, evaluation, time management, book,
  tablebase, hash, and threading code are reused as-is. UCI is purely a
  *protocol translation layer* in front of the existing engine.
- **Cross-platform.** The UCI code is portable C with no new dependencies,
  gated behind the same platform macros Crafty already uses. Windows is the
  primary target; the engine still builds on Unix/MinGW.
- **Additive and mode-gated.** Every change is guarded so that when the engine
  is not in UCI mode, behavior is identical to before.

---

## 2. Architecture overview

### 2.1 Where the code lives

Crafty uses a **unity build**: [`source/crafty.c`](../source/crafty.c)
`#include`s every other `.c` file into one translation unit. Almost all UCI
logic lives in a single new file, **[`source/uci.c`](../source/uci.c)**, added
to that include list. Following Crafty's convention, function prototypes go in
the shared `chess.h`; the one new global goes in `data.c`/`data.h`.

The only changes to *existing* engine files are small, strictly `uci_mode`-gated
hooks (Section 7).

### 2.2 The `uci_mode` flag

A single global, `int uci_mode` (in `data.c`, `extern` in `data.h`), parallels
the existing `xboard` flag. It is set to 1 when the engine enters UCI mode and
gates every piece of UCI behavior. When `uci_mode == 0`, all hooks are inert.

### 2.3 Mode detection and the command loop

When the engine starts, `main()` reads commands and passes each to the giant
`Option()` dispatcher in `option.c`. WinBoard protocol already lives there as
ordinary commands. The UCI entry point is one additional branch:

```c
else if (!strcmp(*args, "uci")) {     /* in Option() */
  uci_mode = 1;
  UCI();
}
```

`UCI()` (in `uci.c`) then **takes over the input loop** — it never returns to
the native loop; it reads UCI commands, dispatches them, and exits on `quit`.
This mirrors how Crafty's existing `Analyze()` takes over input. Dispatch uses
exact `strcmp` (not Crafty's prefix-matching `OptionMatch`) so UCI commands
never collide with native command abbreviations:

```c
void UCI(void) {
  uci_mode = 1;
  UCISendId();                       /* respond to the initial "uci" */
  while (FOREVER) {
    if (quit) break;                 /* mid-search quit set by Interrupt() */
    if (Read(1, buffer) < 0) break;  /* EOF */
    nargs = ReadParse(buffer, args, " \t");
    if (nargs == 0) continue;
    if      (!strcmp(args[0], "uci"))        UCISendId();
    else if (!strcmp(args[0], "isready"))    { printf("readyok\n"); fflush(stdout); }
    else if (!strcmp(args[0], "ucinewgame")) { InitializeHashTables(0); InitializeChessBoard(block[0]); move_number = 1; }
    else if (!strcmp(args[0], "position"))   UCIPosition(nargs, args);
    else if (!strcmp(args[0], "go"))         UCIGo(nargs, args);
    else if (!strcmp(args[0], "setoption"))  UCISetOption(nargs, args);
    else if (!strcmp(args[0], "quit"))       break;
    /* unknown commands are ignored, per the UCI spec */
  }
  CraftyExit(0);
}
```

### 2.4 The functions in `uci.c`

| Function | Responsibility |
|---|---|
| `UCI()` | the UCI command loop; entered from `Option()` |
| `UCISendId()` | emits `id name`/`id author`, the option list, and `uciok` |
| `UCIPosition()` | sets the board from `position [startpos\|fen …] moves …` |
| `UCIGo()` | parses `go` limits, runs the search, prints `bestmove` |
| `UCIInfo()` | formats one `info …` line from a principal variation |
| `UCISetClock()` | maps the GUI clock onto Crafty's time-control globals |
| `UCISetOption()` | parses and dispatches `setoption` |
| `UCIOpenBook()` | opens/closes the opening book handle |
| `UCIMove()` | converts an internal move to UCI coordinate notation |

---

## 3. The handshake

On `uci`, the engine identifies itself and lists its options, terminated by
`uciok`. A leading newline is emitted first because, at startup, Crafty prints
its native prompt (`White(1): `) with no trailing newline before reading the
first command — the leading `\n` guarantees `id name` starts on its own line.

```
id name Crafty 25.6.1
id author Robert Hyatt
option name Hash type spin default 64 min 1 max 65536
option name Threads type spin default 1 min 1 max <CPUS>
option name Ponder type check default false
option name SyzygyPath type string default <empty>
option name OwnBook type check default false
option name BookFile type string default book.bin
option name Move Overhead type spin default 30 min 0 max 5000
uciok
```

`isready` → `readyok` (answered immediately, even during a search).
`ucinewgame` clears the hash and resets the board. `quit` exits cleanly.

---

## 4. Position setup

UCI is **stateless**: the GUI resends the whole game on every move
(`position startpos moves e2e4 e7e5 …` or `position fen <6 fields> moves …`).
`UCIPosition()` rebuilds the position each time:

1. For `startpos`, set the board from the standard starting FEN; for `fen`, pass
   the first four FEN fields (piece placement, side, castling, en passant) to
   Crafty's `SetBoard()` (the half-move/full-move counters are ignored).
2. Set `move_number = 1`.
3. Replay each move in the `moves` list **exactly as `main()`'s game loop applies
   an opponent move**: `InputMove()` (which accepts coordinate notation including
   lowercase promotion like `e7e8q`), then `MakeMoveRoot()`, then flip
   `game_wtm` and increment `move_number` after Black's move. `MakeMoveRoot()`
   maintains Crafty's repetition list and 50-move state, so three-fold and
   fifty-move detection stay correct.

---

## 5. Searching: `go`, `stop`, and `go infinite`

### 5.1 `UCIGo()` and search limits

`UCIGo()` parses the `go` arguments and chooses a search limit, then calls
`Iterate(game_wtm, think, 0)` — the same iterative-deepening entry point the
native engine uses. **The engine's move is never played on the board** (UCI is
stateless; the GUI resends it next time), and the **opening book** is handled as
described in Section 9.2.

| `go` argument | Effect |
|---|---|
| `depth N` | fixed depth: `search_depth = N` |
| `movetime T` (ms) | fixed time: `search_time_limit = T/10` (centiseconds) |
| `wtime/btime/winc/binc/movestogo` | clock-based timing via `UCISetClock()` |
| `infinite` | `pondering = 1` (search until `stop`) |
| `ponder` | `pondering = 1` + clock prepared (Section 8) |
| (none of the above) | a default fixed depth, so a `bestmove` always results |

Fixed `depth`/`movetime` take precedence over the clock.

### 5.2 Clock → Crafty time control

`UCISetClock()` maps the UCI clock onto Crafty's time-control globals, which are
in **centiseconds** and indexed by colour (`black=0`, `white=1`):

```c
tc_time_remaining[game_wtm]       = my_ms / 10;     /* side-to-move clock  */
tc_time_remaining[Flip(game_wtm)] = opp_ms / 10;
tc_increment                      = my_inc_ms / 10;
tc_safety_margin                  = uci_move_overhead;   /* Move Overhead   */
/* movestogo M -> N-moves model (tc_sudden_death = 0, tc_moves_remaining = M) */
/* otherwise   -> sudden death + increment (tc_sudden_death = 1)              */
```

With `search_time_limit == 0` and `search_depth == 0`, Crafty's `TimeSet()` then
computes the per-move time from these globals exactly as in a native game.

### 5.3 `stop` and `go infinite` — interrupting a running search

This was the most delicate integration. UCI requires the engine to react to
`stop` (and `isready`) *while a search is running*. Crafty's search already
polls input itself: every N nodes the search calls `CheckInput()` and, if input
is waiting, `Interrupt()`. This is the same machinery behind the native `?`
("move now") key — **so no threads are needed**.

Two small `uci_mode` hooks make it work for UCI (Section 7):

- `CheckInput()` is taught to detect piped stdin in UCI mode (it previously did
  so only for `xboard`).
- `Interrupt()` gains a `uci_mode` branch:

```c
if (uci_mode) {
  if (!strcmp(args[0], "stop"))      { if (thinking || pondering) abort_search = 1; break; }
  if (!strcmp(args[0], "isready"))   { printf("readyok\n"); fflush(stdout); continue; }
  if (!strcmp(args[0], "ponderhit")) { pondering = 0; continue; }     /* Section 8 */
  if (!strcmp(args[0], "quit"))      { quit = 1; abort_search = 1; break; }
  continue;                          /* ignore other commands mid-search */
}
```

`go infinite` sets `pondering = 1`, which makes `TimeCheck()` never expire, so
the search runs until `stop` sets `abort_search = 1`. On any interruption the
best move from the last completed iteration (`tree->pv[0].path[1]`) is valid, so
a `bestmove` is always emitted.

---

## 6. Output translation

### 6.1 `bestmove` and coordinate notation

After a search, the chosen move is the first move of the principal variation,
`last_pv.path[1]`. `UCIMove()` formats moves in pure UCI coordinate notation —
from-square + to-square + lowercase promotion letter (`e2e4`, `g1f3`, `e7e8q`).
Crafty stores castling internally as the king's two-square move, so a plain
from/to formatter yields `e1g1`/`e8g8` automatically. A terminal position emits
`bestmove 0000`. (Crafty's native `OutputMove()` produces SAN and is therefore
unsuitable for UCI.)

### 6.2 `info` lines

Crafty reports a new principal variation through `DisplayPV()`. In UCI mode a
single hook at the top of `DisplayPV()` calls `UCIInfo()` instead, which emits:

```
info depth D score cp X|mate Y nodes N nps Q time T pv <coord moves…>
```

`UCIGo()` sets `noise_level = 0` for the search so `DisplayPV()` fires on every
iteration, streaming progress.

### 6.3 Scores (a subtle correctness point)

Crafty's score scale is already centipawns (`PAWN_VALUE == 100`). Crucially,
**`pv->pathv` is side-to-move-relative** (positive = good for the side to move) —
which is exactly what UCI wants — so the score is used directly with **no
colour negation**. (An early implementation wrongly assumed White-relative and
negated for Black; the bug was caught by a behavioral test asserting a known
mate and confirmed empirically across winning/losing positions for both
colours.) Forced mates use `MateScore(s) = |s| > 32000` and `MATE = 32768`:
the distance is `(MATE - |pathv| + 1) / 2` full moves, positive when the side to
move is mating, negative when being mated.

### 6.4 Units

Crafty's internal time is centiseconds; UCI uses milliseconds (`time = cs * 10`)
and `nps = nodes/second` (`nodes * 100 / cs`). Node counts come from
`block[0]->nodes_searched`.

---

## 7. The mode-gated hooks into the engine core

Besides `source/uci.c`, only a handful of lines were added to existing engine
files. Each is guarded by `uci_mode` so native/WinBoard behavior is unchanged.

| File | Hook | Purpose |
|---|---|---|
| `option.c` | one `else if (!strcmp(*args, "uci"))` branch | enter UCI mode |
| `chess.h` | prototypes for `UCI()` and `UCIInfo()` | unity + separate-compile builds |
| `data.c` / `data.h` | `int uci_mode` | the mode flag |
| `crafty.c` | `#include "uci.c"` | add the module to the unity build |
| `utility.c` | `DisplayPV()`: `if (uci_mode) { UCIInfo(...); return; }` | UCI info lines instead of native PV |
| `utility.c` | `CheckInput()`: add `uci_mode` to the piped-stdin guards (Windows `PeekNamedPipe` and Unix `select`) | detect `stop`/`isready` mid-search |
| `interrupt.c` | a `uci_mode` branch in `Interrupt()` | handle `stop`/`isready`/`quit`/`ponderhit` mid-search |
| `iterate.c` | `if (quit && !uci_mode) CraftyExit(0)` | let UCI emit `bestmove` before exiting on a mid-search quit |

When `uci_mode == 0`, every one of these expressions evaluates exactly as the
original code — verified both by inspection and by running the native and
xboard interfaces.

---

## 8. Pondering (`go ponder` / `ponderhit`)

UCI pondering: the engine suggests a reply (`bestmove M ponder P`), the GUI plays
the predicted position and sends `go ponder`, and on a correct prediction sends
`ponderhit` (the engine then finishes on its clock, counting the ponder time).

- **`bestmove M ponder P`** — `P` is the second PV move, `last_pv.path[2]`, emitted
  only when a predicted reply exists.
- **`go ponder`** — sets `pondering = 1` (search held open, like `infinite`) while
  still applying the GUI clock via `UCISetClock()`, so `TimeSet()` pre-computes
  the move's time budget.
- **`ponderhit`** — simply sets `pondering = 0` in the `Interrupt()` branch.
  Because the search's `start_time` was set when pondering began and `TimeCheck`
  measures elapsed time from it, the time already spent pondering correctly counts
  toward the budget — **no timer reset needed** (verified: a long ponder followed
  by `ponderhit` moves almost immediately; a short one leaves time to think).
- `go ponder` + `stop` discards the ponder via the existing `stop` path.

---

## 9. Options (`setoption`)

`UCISetOption()` parses `setoption name <name> value <value>` (names may contain
spaces, e.g. "Move Overhead") and dispatches. Simple options set a global;
subsystem options reuse Crafty's tested native commands by writing the command
into `buffer` and calling `Option()` with output suppressed.

| Option | Wiring |
|---|---|
| **Ponder** | `ponder = (value == "true")` — the time-allocation setting |
| **Move Overhead** | `uci_move_overhead = value_ms / 10`, applied as `tc_safety_margin` in `UCISetClock()` (default **30 ms**) |
| **Hash** (MB) | `Option("hash NM")` (rounds down to a power of two), output suppressed |
| **Threads** | clamp to `[1, CPUS]`, then `Option("mt N")`; `Threads 1 → mt 0` (Crafty's serial mode; `mt 1` would print an error) |
| **SyzygyPath** | `strncpy(tb_path, …)` always; under `#if defined(SYZYGY)` run `Option("egtb on")` to initialise |
| **OwnBook** / **BookFile** | open/close `book_file` via `UCIOpenBook()` |

### 9.2 The opening book (a non-obvious detail)

`Iterate()` **already calls `Book()` internally** (in `iterate.c`); when a book
move exists it skips the search entirely. So enabling the book is just a matter
of opening `book_file` — normal `go` then plays book moves with no search. The
catch: because `Iterate()` books unconditionally, `go infinite`/`go ponder` with
a book open would *instantly* return a book move instead of analysing.
`UCIGo()` therefore temporarily sets `book_file = 0` around the `Iterate()` call
for analysis searches (`infinite`/`ponder`), restoring it afterwards.

### 9.3 MultiPV

MultiPV was **dropped**. Crafty has no multi-PV search — its root search fully
evaluates only the best move each iteration (the other root moves get
null-window confirmations whose scores are not meaningful), and adding real
N-line MultiPV would require modifying the core search. Rather than advertise a
feature it does not have, the option is not offered.

---

## 10. Testing

### 10.1 Transcript tests

[`tests/uci/run_tests.sh`](../tests/uci/run_tests.sh) is a portable POSIX-shell
harness (runs under Git Bash and Unix). Each test pipes a UCI command transcript
to the engine and greps its stdout for the expected `info`/`bestmove`/option
shape. It grew to **50 assertions** covering the handshake, position setup,
fixed/clock/infinite/ponder searches, score sign and mate distances (both
colours, both directions), `stop`/`ponderhit`/`quit` mid-search, output
cleanliness (no native chatter leaks), and every option. The engine is wrapped
in `timeout` so a hang surfaces as a failed test rather than blocking the suite.

### 10.2 Self-play gauntlets

Transcript tests cannot catch protocol/stability issues that only appear over
real games. [`tests/gauntlet/run_gauntlet.sh`](../tests/gauntlet/run_gauntlet.sh)
drives Crafty-UCI vs Crafty-UCI through **fastchess** (or cutechess-cli), which
detects illegal moves, time forfeits, crashes, and disconnects.

- A **50-game stability gauntlet** at 8s+0.08 finished with zero failures.
- A **fast-TC stress test** at 2s+0.02 surfaced one flag-fall (1/40). Root cause:
  the internal Move Overhead default was 0 while *advertised* as 30 ms — so
  unless a GUI set it, the engine reserved no buffer against process/pipe
  latency. The default was set to 30 ms; a re-run produced **0 time losses in 60
  games**.

### 10.3 Regression

The native console and xboard interfaces were re-verified to behave as before
(native search, full `feature` handshake), confirming the UCI work is purely
additive.

---

## 11. Build

The engine builds via the unity file. For development/testing on Windows with
gcc (MinGW), single-threaded:

```sh
cd source
gcc -std=gnu17 -O2 -DCPUS=1 crafty.c -o crafty.exe -lwinmm
```

For a tablebase-capable build, add `-DSYZYGY`; for SMP, set `-DCPUS=N`. Windows
is the default target when `-DUNIX` is omitted. The UCI code adds no new
dependencies and compiles under all of these configurations.

---

## 12. Known cosmetic limitations (not protocol or play bugs)

- The reported PV may continue past a draw-by-rule (three-fold / fifty-move);
  GUIs and fastchess tolerate it, and many engines do the same.
- A short banner prints at startup before `uci` is received (the engine cannot
  yet know the protocol); GUIs ignore pre-handshake output.
- `nps 0` is reported for sub-centisecond moves (the time floor is one
  centisecond).

---

## 13. Implementation history

The conversion was delivered in seven reviewed phases (each on its own branch,
merged to `master`):

1. **Handshake & mode detection** — `uci`/`isready`/`quit`, option list.
2. **Position & fixed-limit search** — `position`, `go depth`/`movetime` → `bestmove`.
3. **Info output** — streamed `info` lines (caught and fixed a score-perspective bug).
4. **Time control, stop, infinite** — clocks + mid-search interruption (fixed a quit-hang).
5. **Pondering** — `go ponder`/`ponderhit`, ponder move in `bestmove`.
6. **Options** — `setoption` wiring (found the book-in-`Iterate()` behavior; fixed a `-DSYZYGY` crash).
7. **Hardening** — fastchess gauntlets and regression (fixed the Move Overhead default).

The full design notes and per-phase implementation plans are in
[`docs/superpowers/plans/`](superpowers/plans/), and the project overview is in
[`CLAUDE.md`](../CLAUDE.md).
