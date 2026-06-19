#include <stdlib.h>
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

static void UCISendId(void) {
  printf("\nid name Crafty %s\n", version);
  printf("id author Robert Hyatt\n");
  printf("option name Hash type spin default 64 min 1 max 65536\n");
  printf("option name Threads type spin default 1 min 1 max %d\n", CPUS);
  printf("option name Ponder type check default false\n");
  printf("option name SyzygyPath type string default <empty>\n");
  printf("option name OwnBook type check default false\n");
  printf("option name BookFile type string default book.bin\n");
  printf("option name MultiPV type spin default 1 min 1 max 256\n");
  printf("option name Move Overhead type spin default 30 min 0 max 5000\n");
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
    else if (!strcmp(args[0], "isready")) {
      printf("readyok\n");
      fflush(stdout);
    }
    else if (!strcmp(args[0], "go"))
      UCIGo(nargs, args);
    else if (!strcmp(args[0], "quit"))
      break;
    /* Unknown commands are ignored, per the UCI specification. */
  }
  CraftyExit(0);
}
