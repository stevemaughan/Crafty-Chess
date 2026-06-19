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
    else if (!strcmp(args[0], "quit"))
      break;
    /* Unknown commands are ignored, per the UCI specification. */
  }
  CraftyExit(0);
}
