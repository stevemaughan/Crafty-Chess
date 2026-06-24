# Crafty → UCI Conversion

## What this project is

This is a fork of **Crafty**, the classic open-source chess engine written by Robert (Bob) Hyatt.
The source in this repo is **Crafty v25.6.1** — a UCI-enabled build of upstream Crafty 25.6; the
`.1` denotes the UCI conversion (see `VERSION` in [data.c](source/data.c#L445)).

Crafty is a strong, full-featured engine, but it only speaks the **WinBoard/xboard** protocol
(plus its own native console command set). It does **not** support **UCI** (Universal Chess
Interface), the protocol used by most modern GUIs (Arena, Cute Chess, Nibbler, ChessBase, etc.).

**Goal of this fork:** add UCI support while preserving every existing feature — opening books,
Syzygy/endgame tablebases, transposition (hash) tables, SMP/multi-threaded search, pondering,
and analysis — and exposing them as UCI options to the GUI. We are **adding** UCI, not removing
WinBoard.

## Guiding principles

- **UCI is a protocol adapter, not an engine rewrite.** The search, evaluation, book, tablebase,
  hash, and threading code are left intact. We translate UCI in/out and reuse the existing core.
- **Three coexisting protocols, auto-detected.** Native console, WinBoard, and UCI all live in one
  binary. The mode is chosen from the first stdin token (`uci` → UCI; `xboard` → WinBoard;
  anything else → native console).
- **Preserve cross-platform builds.** UCI code is portable C with no new dependencies, gated by the
  same platform macros Crafty already uses. Windows is the primary target, but the engine must
  still build on Unix/macOS/MinGW.
- **WinBoard must never regress.** Existing WinBoard and native behavior is a baseline we protect.

## Codebase orientation

Crafty uses a **unity build**: [crafty.c](source/crafty.c) `#include`s every other `.c` file into one
translation unit so the compiler can inline across the whole program. To add a new module, add it to
the include list in [crafty.c](source/crafty.c) and to the build.

Key files for this work:

| File | Role |
|---|---|
| [source/crafty.c](source/crafty.c) | Unity build — the `#include` manifest of all source files |
| [source/main.c](source/main.c) | `main()` + the central game loop (read line → `Option()` → else treat as move; drives `Ponder()`/`Iterate()`). ~375 KB |
| [source/option.c](source/option.c) | The giant `Option()` command dispatcher. **WinBoard protocol already lives here** as commands (`protover`, `level`, `st`, `sd`, `time`, `otim`, `hard`/`easy`, `post`, `analyze`, `setboard`, `ping`, `force`, `go`, `hint`, `undo`, `remove`, `hash`, `egtb`, `smp`/`mt`, `draw`, `result`, …) |
| [source/interrupt.c](source/interrupt.c) | `Interrupt()` — reads input **during** a search and sets `abort_search` / `input_status`. Crafty's native `?` ("move now") and `@` ("ponder move played") are the semantic equivalents of UCI `stop` and `ponderhit` |
| [source/iterate.c](source/iterate.c) | `Iterate()` — iterative-deepening driver (the search entry point) |
| [source/ponder.c](source/ponder.c) | Pondering / permanent-brain logic |
| [source/analyze.c](source/analyze.c) | Analyze mode (permanent-pondering; basis for `go infinite`) |
| [source/search.c](source/search.c), [source/quiesce.c](source/quiesce.c), [source/next.c](source/next.c) | Alpha-beta search, quiescence, move ordering |
| [source/hash.c](source/hash.c) | Transposition table (`hash` option) |
| [source/tbprobe.c](source/tbprobe.c), [source/tbcore.c](source/tbcore.c) | Syzygy tablebases (`egtb` command, `-DSYZYGY`) |
| [source/book.c](source/book.c) | Opening book |
| [source/thread.c](source/thread.c) | SMP threads (`smp`/`mt` commands, `-DCPUS=n`) |
| [source/time.c](source/time.c) | Time management |
| [source/output.c](source/output.c), [source/setboard.c](source/setboard.c) | PV/score display; FEN parsing |
| [source/data.c](source/data.c), [source/chess.h](source/chess.h), [source/data.h](source/data.h) | Globals, types, board representation |

## Build

Crafty builds via the unity file. The [Makefile](source/Makefile) is Unix-oriented (gcc/icc targets);
**Windows is the default target when `-DUNIX` is omitted**. Threads are gated by `-DCPUS=n`, Syzygy
by `-DSYZYGY`.

- Establish a clean baseline build **before** touching anything.
- New UCI code compiles on all platforms; no new external dependencies.
- Verify the build under both the primary Windows toolchain and a Unix/MinGW gcc build to confirm
  portability is preserved.

## The UCI conversion approach

### Where the code goes
- New **`source/uci.c`**, added to the include manifest in [crafty.c](source/crafty.c) and to the
  build. Following Crafty's convention, UCI prototypes go in the shared [chess.h](source/chess.h)
  (no per-module headers); the `uci_mode` global is declared in [data.c](source/data.c) with `extern`
  in [data.h](source/data.h).
- A `uci_mode` global, parallel to the existing `xboard` flag, gating output format and behavior.
- **Only two hook points** into existing code:
  1. [main.c](source/main.c) — detect `uci` as the first token and route into the UCI loop.
  2. [interrupt.c](source/interrupt.c) — recognize UCI mid-search commands (`stop`, `ponderhit`,
     `isready`, `quit`) and set the existing `abort_search` / `input_status` flags, reusing the
     proven `?` / `@` machinery.

### Command mapping (UCI → Crafty)
| UCI | Maps to |
|---|---|
| `uci` | Emit `id name`/`id author`, the `option` list, then `uciok` |
| `isready` | `readyok` immediately (even mid-search) |
| `ucinewgame` | `new`-equivalent reinit + clear hash |
| `position [startpos\|fen …] moves …` | `setboard` + replay moves (handles UCI's stateless full-position-each-`go` model; keep repetition/50-move history correct) |
| `go …` | Translate limits, then `Iterate()` (or ponder search for `go ponder`) |
| `stop` | `abort_search = 1` → emit current best as `bestmove` |
| `ponderhit` | The `@` path: ponder → think, start clock |
| `quit` | Clean exit |

`go` sub-tokens: `wtime`/`btime`/`winc`/`binc`/`movestogo` → `time`/`level` controls; `movetime` →
`st`; `depth` → `sd`; `nodes` → node limit; `infinite` → unbounded (analyze-style); `ponder` →
ponder search on the GUI-supplied move.

### UCI options (`setoption name X value Y`)
Scope agreed for this conversion:
- **Hash** (spin, MB) → hash table size
- **Threads** (spin) → `smp`/`mt`
- **Ponder** (check) → pondering on/off
- **SyzygyPath** (string) → `egtb` path + init
- **OwnBook** (check) + **BookFile** (string) → opening book toggle/path
- **Move Overhead** (spin, ms) → time buffer subtracted in time management

(**MultiPV** was dropped: Crafty has no multi-PV search, and adding real N-line MultiPV would require modifying its root search. The option is not advertised.)

Deeper Crafty personality/eval knobs are intentionally **out of scope for now** (easy to add later).

### Output translation (Crafty → UCI)
Reformat the search's PV/score/depth/node output as
`info depth … seldepth … score cp N | mate N … nodes … nps … hashfull … time … pv …`,
and the final move as `bestmove <move> [ponder <move>]`. Scores in centipawns from the side-to-move
POV; Crafty mate scores → `mate N`; moves in UCI long algebraic (`e2e4`, `e7e8q`), castling as
king-move coordinates.

### Genuinely tricky parts (handle with care)
- **Stateless `position`**: UCI resends the whole game each `go`; rebuild from FEN/startpos + replay
  while keeping repetition and 50-move history correct.
- **Ponder model differs**: in UCI the GUI tells us which move to ponder (`go ponder` + the move),
  unlike WinBoard where Crafty guesses. Honor the GUI's move; `ponderhit`/`stop` resolve it.
- **`stop` must always yield a `bestmove`**, including during `infinite`/ponder searches.
- **Coordinate & score conventions** differ from WinBoard and need their own conversion path.

## Testing & verification

- **Baseline build** unchanged first.
- **`perft`** (in-engine) to confirm no move-generation regression.
- **`cutechess-cli`**: UCI handshake + self-play, and Crafty-UCI vs Crafty-WinBoard gauntlets.
- **Scripted stdin transcript** (`uci`, `isready`, `position`, `go depth N`, `stop`) checked against
  expected `info`/`bestmove` shape.
- **Manual GUI load** (Arena / Cute Chess / Nibbler).
- **Regression guard**: WinBoard and native console modes must still behave as before.

## Roadmap — ✅ COMPLETE (all 7 phases done, merged, and validated)

1. ✅ **Skeleton** — mode detection, `uci`/`isready`/`quit` handshake, option enumeration.
2. ✅ **Search plumbing** — `position` parsing + `go depth`/`movetime` fixed search → `bestmove`.
3. ✅ **Info output** — `info depth/score(cp|mate)/nodes/nps/time/pv` streamed per iteration.
4. ✅ **Time control** — `wtime`/`btime`/`winc`/`binc`/`movestogo`, `stop`, `go infinite`.
5. ✅ **Pondering** — `go ponder` / `ponderhit`, `bestmove … ponder …`.
6. ✅ **Options** — Hash, Threads, SyzygyPath, OwnBook/BookFile, Move Overhead (MultiPV dropped — Crafty has no multi-PV).
7. ✅ **Hardening** — fastchess self-play gauntlets (50-game stability + fast-TC stress, all clean after fixing the Move Overhead default 0→30 ms), native/xboard/UCI regression pass.

**Status:** Crafty now speaks UCI (auto-detected from the first stdin token) alongside the unchanged WinBoard and native console interfaces. All UCI code lives in [source/uci.c](source/uci.c) plus small mode-gated hooks in `option.c`/`main.c` (detect `uci`), `utility.c` (`DisplayPV`→UCI info; `CheckInput` piped stdin), `interrupt.c` (`stop`/`isready`/`quit`/`ponderhit` mid-search), and `iterate.c` (one `!uci_mode` guard). Transcript tests: [tests/uci/run_tests.sh](tests/uci/run_tests.sh) (50 assertions, `timeout`-guarded). Self-play harness: [tests/gauntlet/run_gauntlet.sh](tests/gauntlet/run_gauntlet.sh) (needs fastchess or cutechess-cli).

## Conventions & gotchas

- Respect the **unity build**: new modules go into the [crafty.c](source/crafty.c) include list.
- Match the surrounding C style (the existing boxed comment headers, naming, indentation).
- Keep UCI changes **additive and mode-gated** — never alter WinBoard/native code paths in a way
  that changes their behavior.
- Don't add dependencies or platform-specific code that breaks the cross-platform build.
