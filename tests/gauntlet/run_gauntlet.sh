#!/usr/bin/env sh
# Self-play gauntlet for the Crafty UCI build, using fastchess (or cutechess-cli).
# Plays Crafty-UCI vs Crafty-UCI over many games to surface protocol/stability
# issues that the transcript tests cannot: illegal moves, time forfeits, crashes,
# and disconnects.  Run from the repo root.
#
# Usage:
#   FASTCHESS=/path/to/fastchess.exe sh tests/gauntlet/run_gauntlet.sh [TC] [GAMES]
#
#   TC     time control, fastchess syntax (default: 8+0.08)
#   GAMES  total games, even number (default: 50)
#
# Env:
#   FASTCHESS   path to fastchess(.exe); defaults to a known local install.
#   CONCURRENCY parallel games (default: 8).
#
# Notes:
#   - The engine is run from a scratch dir so its log.*/game.* files stay out of
#     the repo.  Uses absolute Windows-style paths (fastchess on Windows does not
#     resolve POSIX/relative engine paths reliably).
#   - A fast TC (e.g. 2+0.02) stresses time management — watch for "loses on time".
set -u

FASTCHESS="${FASTCHESS:-/c/Users/steve/Dropbox/Chess/Development/FastChess/fastchess.exe}"
TC="${1:-8+0.08}"
GAMES="${2:-50}"
CONCURRENCY="${CONCURRENCY:-8}"
ROUNDS=$((GAMES / 2))

ROOT=$(pwd)
SCRATCH="/c/Temp/crafty-gauntlet"
WINSCRATCH="C:/Temp/crafty-gauntlet"

if [ ! -x "$FASTCHESS" ]; then
  echo "fastchess not found at: $FASTCHESS (set FASTCHESS=...)" >&2
  exit 1
fi

echo "Building UCI binary..."
( cd source && gcc -O2 -DCPUS=1 crafty.c -o crafty_uci.exe -lwinmm ) || exit 1

mkdir -p "$SCRATCH"
cp source/crafty_uci.exe "$SCRATCH/"
cp tests/gauntlet/openings.epd "$SCRATCH/"
rm -f "$SCRATCH"/log.* "$SCRATCH"/game.* 2>/dev/null

echo "Gauntlet: $GAMES games @ $TC, concurrency $CONCURRENCY ..."
"$FASTCHESS" \
  -engine cmd="$WINSCRATCH/crafty_uci.exe" name=CraftyA dir="$WINSCRATCH" \
  -engine cmd="$WINSCRATCH/crafty_uci.exe" name=CraftyB dir="$WINSCRATCH" \
  -each proto=uci tc="$TC" \
  -openings file="$WINSCRATCH/openings.epd" format=epd order=random \
  -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
  -pgnout file="$WINSCRATCH/games.pgn" \
  > "$SCRATCH/gauntlet.log" 2>&1

echo ""
echo "=== summary ==="
echo "games finished : $(grep -c '^Finished game' "$SCRATCH/gauntlet.log")"
echo "time forfeits  : $(grep -c 'loses on time' "$SCRATCH/gauntlet.log")"
echo "illegal/crash  : $(grep -ciE 'illegal|crashed: [1-9]|disconn' "$SCRATCH/gauntlet.log")"
echo "--- endings ---"
grep '^Finished game' "$SCRATCH/gauntlet.log" | sed -E 's/.*\{(.*)\}/\1/' | sort | uniq -c | sort -rn
echo "(full log: $SCRATCH/gauntlet.log ; PGN: $SCRATCH/games.pgn)"
