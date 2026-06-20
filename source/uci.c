#include <stdlib.h>
#include "chess.h"
#include "data.h"

static const char uci_start_fen[] =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
static int uci_move_overhead = 0;       /* centiseconds, from "Move Overhead" */
static char uci_book_file[256] = "";     /* path from "BookFile" */
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
#define UCI_PONDER_FALLBACK 500          /* cs; bounds a clock-less go ponder */

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
  tc_safety_margin = uci_move_overhead;
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

/*
 *  UCIGo() handles the UCI "go" command.  It parses "depth N" and
 *  "movetime T" (ms); clock parameters (wtime/btime/winc/binc/movestogo) are
 *  mapped to Crafty's time-control globals by UCISetClock(); "infinite" runs
 *  the search in pondering mode until a "stop" command is received.  A bare
 *  "go" with none of these falls back to a default fixed depth so a bestmove
 *  is always produced.  Crafty's native search output is suppressed for the
 *  duration of the search (display_options/kibitz/post zeroed), and the
 *  engine's move is NOT played on the board (UCI is stateless).
 */
static void UCIGo(int nargs, char *args[]) {
  TREE *const tree = block[0];
  int i, best, saved_display_options, saved_kibitz, saved_post;
  int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0, has_clock = 0;
  int infinite = 0, ponder_flag = 0;
  unsigned saved_noise;
  FILE *saved_book_file;
  char movestr[8];

  search_depth = 0;
  search_time_limit = 0;
  for (i = 1; i < nargs; i++) {
    if (!strcmp(args[i], "depth") && i + 1 < nargs)
      search_depth = atoi(args[++i]);
    else if (!strcmp(args[i], "movetime") && i + 1 < nargs)
      search_time_limit = atoi(args[++i]) / 10;
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
    else if (!strcmp(args[i], "infinite"))
      infinite = 1;
    else if (!strcmp(args[i], "ponder"))
      ponder_flag = 1;
  }
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
/*
 *  Suppress Crafty's native streaming search output while we search.
 */
  saved_display_options = display_options;
  saved_kibitz = kibitz;
  saved_post = post;
  saved_noise = noise_level;
  display_options = 0;
  kibitz = 0;
  post = 0;
  noise_level = 0;
/*
 *  Set the pre-search state the same way main()'s game loop does, then search.
 */
  pondering = (infinite || ponder_flag) ? 1 : 0;
  thinking = 1;
  last_pv.pathd = 0;
  last_pv.pathl = 0;
  display = tree->position;
  tree->status[1] = tree->status[0];
  saved_book_file = book_file;
  if (infinite || ponder_flag)
    book_file = 0;                 /* analysis: search, don't return a book move */
  Iterate(game_wtm, think, 0);
  book_file = saved_book_file;
  thinking = 0;
  pondering = 0;
  display_options = saved_display_options;
  kibitz = saved_kibitz;
  post = saved_post;
  noise_level = saved_noise;
/*
 *  Report the best move (the first move of the principal variation).
 */
  last_pv = tree->pv[0];
  best = last_pv.path[1];
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
  fflush(stdout);
  search_depth = 0;
  search_time_limit = 0;
}

/*
 *  UCIPosition() sets the board from a UCI "position" command:
 *  "position startpos [moves ...]" or "position fen <FEN> [moves ...]".
 *  The FEN can include all 6 fields, but only the first four are used:
 *  piece placement, side to move, castling rights, and en passant square.
 *  Half-move and full-move counters are ignored. The moves list is replayed
 *  exactly as main()'s game loop applies opponent moves, keeping repetition/50-move
 *  state correct.
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

/*
 *  UCIInfo() emits one UCI "info" line for a completed search iteration.  It is
 *  called from DisplayPV() when uci_mode is set, replacing Crafty's native PV
 *  display.  Crafty's pv->pathv is already side-to-move-relative centipawns
 *  (positive = good for the side to move; PAWN_VALUE==100), which is exactly
 *  what UCI wants, so the score is used directly with no color negation.  The
 *  DisplayPV "time" argument is centiseconds; UCI uses ms and nps in nodes/sec.
 *  Moves are formatted with UCIMove (coordinate notation).
 */
void UCIInfo(int wtm, int etime, PATH *pv) {
  TREE *const tree = block[0];
  uint64_t nodes = tree->nodes_searched;
  uint64_t nps = (etime > 0) ? (nodes * 100 / (uint64_t) etime) : 0;
  int i, n = 0, score = pv->pathv;
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

static void UCISendId(void) {
  printf("\nid name Crafty %s\n", version);
  printf("id author Robert Hyatt\n");
  printf("option name Hash type spin default 64 min 1 max 65536\n");
  printf("option name Threads type spin default 1 min 1 max %d\n", CPUS);
  printf("option name Ponder type check default false\n");
  printf("option name SyzygyPath type string default <empty>\n");
  printf("option name OwnBook type check default false\n");
  printf("option name BookFile type string default book.bin\n");
  printf("option name Move Overhead type spin default 30 min 0 max 5000\n");
  printf("uciok\n");
  fflush(stdout);
}

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
    /* Crafty's mt command rejects 1 (must be 0=disabled or >1); map 1->0 */
    sprintf(buffer, "mt %d", (n == 1) ? 0 : n);
    Option(tree);
  } else if (!strcmp(name, "SyzygyPath")) {
    strncpy(tb_path, value, sizeof(tb_path) - 1);
    tb_path[sizeof(tb_path) - 1] = 0;
#if defined(SYZYGY)
    display_options = 0;
    strcpy(buffer, "egtb");
    Option(tree);
#endif
  } else if (!strcmp(name, "OwnBook")) {
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
  display_options = saved_display_options;
}

void UCI(void) {
  uci_mode = 1;
  UCISendId();
  while (FOREVER) {
    if (quit)
      break;
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
    else if (!strcmp(args[0], "setoption"))
      UCISetOption(nargs, args);
    else if (!strcmp(args[0], "position"))
      UCIPosition(nargs, args);
    else if (!strcmp(args[0], "go"))
      UCIGo(nargs, args);
    else if (!strcmp(args[0], "ucinewgame")) {
      InitializeHashTables(0);
      InitializeChessBoard(block[0]);
      move_number = 1;
    }
    else if (!strcmp(args[0], "quit"))
      break;
    /* Unknown commands are ignored, per the UCI specification. */
  }
  CraftyExit(0);
}
