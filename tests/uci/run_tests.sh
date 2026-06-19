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
  out=$(printf '%b' "$transcript" | timeout 30 "$ENGINE" 2>/dev/null)
  if printf '%s\n' "$out" | grep -Eq "$pattern"; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc -- expected /$pattern/"
    fail=1
  fi
}

# reject <description> <transcript> <egrep-pattern> — assert the pattern is ABSENT
reject() {
  desc=$1; transcript=$2; pattern=$3
  out=$(printf '%b' "$transcript" | timeout 30 "$ENGINE" 2>/dev/null)
  if printf '%s\n' "$out" | grep -Eq "$pattern"; then
    echo "FAIL: $desc -- unexpected /$pattern/"
    fail=1
  else
    echo "PASS: $desc"
  fi
}

# --- Task 1: smoke (engine builds and runs) ---
expect "engine builds and runs" 'quit\n' 'Crafty v25\.2'

# --- Task 2: handshake ---
expect "uci -> uciok"     'uci\nquit\n' '^uciok'
expect "uci -> id name"   'uci\nquit\n' '^id name Crafty 25\.2'
expect "uci -> id author" 'uci\nquit\n' '^id author Robert Hyatt'

# --- Task 3: isready ---
expect "isready -> readyok" 'uci\nisready\nquit\n' '^readyok'

# --- Task 4: option enumeration ---
expect "option Hash"          'uci\nquit\n' '^option name Hash type spin default 64 min 1 max 65536'
expect "option Threads"       'uci\nquit\n' '^option name Threads type spin default 1 min 1 max'
expect "option Ponder"        'uci\nquit\n' '^option name Ponder type check default false'
expect "option SyzygyPath"    'uci\nquit\n' '^option name SyzygyPath type string'
expect "option OwnBook"       'uci\nquit\n' '^option name OwnBook type check default false'
expect "option BookFile"      'uci\nquit\n' '^option name BookFile type string'
expect "option MultiPV"       'uci\nquit\n' '^option name MultiPV type spin default 1 min 1 max 256'
expect "option Move Overhead" 'uci\nquit\n' '^option name Move Overhead type spin default 30 min 0 max 5000'

# --- Task 1 (Phase 2): go / bestmove on the start position ---
expect "go depth -> well-formed bestmove" 'uci\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$'
expect "go movetime -> bestmove"          'uci\ngo movetime 200\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
reject "no native search header leaks"    'uci\ngo depth 6\nquit\n' 'variation'
reject "no native PV ply line leaks"      'uci\ngo depth 6\nquit\n' '^\s+[0-9]+->'

# --- Task 2 (Phase 2): position setup ---
# Fool's-mate position, Black to move: Qd8-h4 is mate (unique). Expect d8h4.
expect "position fen + go finds mate-in-1" 'uci\nposition fen rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2\ngo depth 4\nquit\n' '^bestmove d8h4'
# startpos + replayed moves yields a legal reply.
expect "position startpos moves -> legal reply" 'uci\nposition startpos moves e2e4 e7e5 g1f3\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
# Stalemate (Black to move, no legal move, not in check) -> bestmove 0000.
expect "position stalemate -> bestmove 0000" 'uci\nposition fen 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1\ngo depth 2\nquit\n' '^bestmove 0000'

# --- Task 3 (Phase 2): ucinewgame ---
expect "ucinewgame then play works" 'uci\nucinewgame\nposition startpos\ngo depth 6\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'

# --- Phase 2 hardening: promotion move formatting ---
expect "promotion emits lowercase q" 'uci\nposition fen 8/P6k/8/8/8/8/7K/8 w - - 0 1\ngo depth 6\nquit\n' '^bestmove a7a8q'

# --- Phase 3 Task 1: UCI info streaming (cp scores) ---
expect "info line has all fields + coord pv" 'uci\nposition startpos\ngo depth 8\nquit\n' '^info depth 8 score cp -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv [a-h][1-8][a-h][1-8]'
expect "info streams an early depth too"      'uci\nposition startpos\ngo depth 8\nquit\n' '^info depth 1 score cp '
expect "bestmove still follows the info"      'uci\nposition startpos\ngo depth 8\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'

# --- Phase 3 Task 2: mate scores ---
# Fool's mate, Black to move: Qd8-h4 is mate in 1.
expect "mate-in-1 -> score mate 1 with pv d8h4" 'uci\nposition fen rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2\ngo depth 4\nquit\n' '^info depth [0-9]+ score mate 1 .* pv d8h4'
# Non-mate position still reports cp.
expect "normal position still uses score cp"    'uci\nposition startpos\ngo depth 6\nquit\n' '^info depth [0-9]+ score cp '

# --- Phase 3 hardening: side-to-move score perspective ---
expect "black-to-move winning -> positive cp" 'uci\nposition fen 3qk3/8/8/8/8/8/8/4K3 b - - 0 1\ngo depth 4\nquit\n' '^info depth [0-9]+ score cp [0-9]'
expect "black-to-move losing -> negative cp"  'uci\nposition fen 4k3/8/8/8/8/8/8/3QK3 b - - 0 1\ngo depth 4\nquit\n' '^info depth [0-9]+ score cp -[0-9]'

# --- Phase 4 Task 1: clock-based time control ---
expect "go wtime/btime (sudden death) -> bestmove" 'uci\nposition startpos\ngo wtime 1000 btime 1000 winc 100 binc 100\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "go with movestogo -> bestmove"             'uci\nposition startpos\ngo wtime 2000 btime 2000 movestogo 30\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "clock search still streams info"           'uci\nposition startpos\ngo wtime 2000 btime 2000\nquit\n' '^info depth [0-9]+ score cp '

# --- Phase 4 Task 2: stop and go infinite ---
expect "stop interrupts go infinite -> bestmove"   'uci\nposition startpos\ngo infinite\nstop\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "stop interrupts a long search -> bestmove" 'uci\nposition startpos\ngo movetime 60000\nstop\nquit\n' '^bestmove [a-h][1-8][a-h][1-8]'
expect "isready during search -> readyok"          'uci\nposition startpos\ngo infinite\nisready\nstop\nquit\n' '^readyok'

exit $fail
