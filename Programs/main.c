/*
 * BRLTTY - A background process providing access to the Linux console (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2004 by The BRLTTY Team. All rights reserved.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation.  Please see the file COPYING for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

/*
 * main.c - Main processing loop plus signal handling
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "misc.h"
#include "message.h"
#include "tunes.h"
#include "ctb.h"
#include "route.h"
#include "cut.h"
#include "at2.h"
#include "scr.h"
#include "brl.h"
#ifdef ENABLE_SPEECH_SUPPORT
#include "spk.h"
#endif /* ENABLE_SPEECH_SUPPORT */
#include "brltty.h"
#include "defaults.h"

int updateInterval = DEFAULT_UPDATE_INTERVAL;
int messageDelay = DEFAULT_MESSAGE_DELAY;

/*
 * Misc param variables
 */
Preferences prefs;                /* environment (i.e. global) parameters */
BrailleDisplay brl;                        /* For the Braille routines */
short fwinshift;                /* Full window horizontal distance */
short hwinshift;                /* Half window horizontal distance */
short vwinshift;                /* Window vertical distance */
ScreenDescription scr;                        /* For screen state infos */
static short dispmd = LIVE_SCRN;        /* freeze screen on/off */
static unsigned char infmode = 0;                /* display screen image or info */

static int contracted = 0;
#ifdef ENABLE_CONTRACTED_BRAILLE
static int contractedLength;
static int contractedStart;
static int contractedOffsets[0X100];
static int contractedTrack = 0;
#endif /* ENABLE_CONTRACTED_BRAILLE */

static unsigned int updateIntervals = 0;        /* incremented each main loop cycle */


/*
 * useful macros
 */

#define BRL_ISUPPER(c) \
  (isupper((c)) || (c)=='@' || (c)=='[' || (c)=='^' || (c)==']' || (c)=='\\')

unsigned char *curtbl = textTable;        /* active translation table */

static void
setTranslationTable (int attributes) {
  curtbl = attributes? attributesTable: textTable;
}


/* 
 * Structure definition for volatile screen state variables.
 */
typedef struct {
  short column;
  short row;
} ScreenMark;
typedef struct {
  unsigned char trackCursor;		/* tracking mode */
  unsigned char hideCursor;		/* For temporarily hiding cursor */
  unsigned char showAttributes;		/* text or attributes display */
  int winx, winy;	/* upper-left corner of braille window */
  int motx, moty;	/* user motion of braille window */
  int trkx, trky;	/* tracked cursor position */
  int ptrx, ptry;	/* mouse pointer position */
  ScreenMark marks[0X100];
} ScreenState;
static ScreenState initialScreenState = {
  DEFAULT_TRACK_CURSOR, DEFAULT_HIDE_CURSOR, 0,
  0, 0, /* winx/y */
  0, 0, /* motx/y */
  0, 0, /* trkx/y */
  0, 0, /* ptrx/y */
  {[0X00 ... 0XFF]={0, 0}}
};

/* 
 * Array definition containing pointers to ScreenState structures for 
 * each screen.  Each structure is dynamically allocated when its 
 * screen is used for the first time.
 * Screen 0 is reserved for the help screen; its structure is static.
 */
#define MAX_SCR 0X3F              /* actual number of separate screens */
static ScreenState scr0;        /* at least one is statically allocated */
static ScreenState *scrparam[MAX_SCR+1] = {&scr0, };
static ScreenState *p = &scr0;        /* pointer to current state structure */
static int curscr;                        /* current screen number */

static void
switchto (unsigned int scrno) {
  curscr = scrno;
  if (scrno > MAX_SCR)
    scrno = 0;
  if (!scrparam[scrno]) {         /* if not already allocated... */
    {
      if (!(scrparam[scrno] = malloc(sizeof(*p))))
        scrno = 0;         /* unable to allocate a new structure */
      else
        *scrparam[scrno] = initialScreenState;
    }
  }
  p = scrparam[scrno];
  setTranslationTable(p->showAttributes);
}

static void
setDigitUpper (unsigned char *cell, int digit) {
  *cell |= portraitDigits[digit];
}

static void
setDigitLower (unsigned char *cell, int digit) {
  *cell |= portraitDigits[digit] << 4;
}

static void
setNumberUpper (unsigned char *cells, int number) {
  setDigitUpper(&cells[0], (number / 10) % 10);
  setDigitUpper(&cells[1], number % 10);
}

static void
setNumberLower (unsigned char *cells, int number) {
  setDigitLower(&cells[0], (number / 10) % 10);
  setDigitLower(&cells[1], number % 10);
}

static void
setNumberVertical (unsigned char *cell, int number) {
  setDigitUpper(cell, (number / 10) % 10);
  setDigitLower(cell, number % 10);
}

static void
setCoordinateUpper (unsigned char *cells, int x, int y) {
  setNumberUpper(&cells[0], x);
  setNumberUpper(&cells[2], y);
}

static void
setCoordinateLower (unsigned char *cells, int x, int y) {
  setNumberLower(&cells[0], x);
  setNumberLower(&cells[2], y);
}

static void
setCoordinateVertical (unsigned char *cells, int x, int y) {
  setNumberUpper(&cells[0], y);
  setNumberLower(&cells[0], x);
}

static void
setCoordinateAlphabetic (unsigned char *cell, int x, int y) {
  /* The coords are given with letters as the DOS tsr */
  *cell = ((updateIntervals / 16) % (y / 25 + 1))? 0:
          textTable[y % 25 + 'a'] |
          ((x / brl.x) << 6);
}

static void
setStateLetter (unsigned char *cell) {
  *cell = textTable[(p->showAttributes)? 'a':
                    ((dispmd & FROZ_SCRN) == FROZ_SCRN)? 'f':
                    p->trackCursor? 't':
                    ' '];
}

static void
setStateDots (unsigned char *cell) {
  *cell = ((dispmd & FROZ_SCRN) == FROZ_SCRN? BRL_DOT1: 0) |
          (prefs.showCursor                 ? BRL_DOT4: 0) |
          (p->showAttributes                ? BRL_DOT2: 0) |
          (prefs.cursorStyle                ? BRL_DOT5: 0) |
          (prefs.alertTunes                 ? BRL_DOT3: 0) |
          (prefs.blinkingCursor             ? BRL_DOT6: 0) |
          (p->trackCursor                   ? BRL_DOT7: 0) |
          (prefs.slidingWindow              ? BRL_DOT8: 0);
}

static void
setStatusCellsNone (unsigned char *status) {
}

static void
setStatusCellsAlva (unsigned char *status) {
  if ((dispmd & HELP_SCRN) == HELP_SCRN) {
    status[0] = textTable['h'];
    status[1] = textTable['l'];
    status[2] = textTable['p'];
  } else {
    setCoordinateAlphabetic(&status[0], scr.posx, scr.posy);
    setCoordinateAlphabetic(&status[1], p->winx, p->winy);
    setStateLetter(&status[2]);
  }
}

static void
setStatusCellsTieman (unsigned char *status) {
  setCoordinateUpper(&status[0], scr.posx, scr.posy);
  setCoordinateLower(&status[0], p->winx, p->winy);
  setStateDots(&status[4]);
}

static void
setStatusCellsPB80 (unsigned char *status) {
  setNumberVertical(&status[0], p->winy+1);
}

static void
setStatusCellsGeneric (unsigned char *status) {
  status[BRL_firstStatusCell] = BRL_STATUS_CELLS_GENERIC;
  status[BRL_GSC_BRLCOL] = p->winx+1;
  status[BRL_GSC_BRLROW] = p->winy+1;
  status[BRL_GSC_CSRCOL] = scr.posx+1;
  status[BRL_GSC_CSRROW] = scr.posy+1;
  status[BRL_GSC_SCRNUM] = scr.no;
  status[BRL_GSC_FREEZE] = (dispmd & FROZ_SCRN) == FROZ_SCRN;
  status[BRL_GSC_DISPMD] = p->showAttributes;
  status[BRL_GSC_SIXDOTS] = prefs.textStyle;
  status[BRL_GSC_SLIDEWIN] = prefs.slidingWindow;
  status[BRL_GSC_SKPIDLNS] = prefs.skipIdenticalLines;
  status[BRL_GSC_SKPBLNKWINS] = prefs.skipBlankWindows;
  status[BRL_GSC_CSRVIS] = prefs.showCursor;
  status[BRL_GSC_CSRHIDE] = p->hideCursor;
  status[BRL_GSC_CSRTRK] = p->trackCursor;
  status[BRL_GSC_CSRSIZE] = prefs.cursorStyle;
  status[BRL_GSC_CSRBLINK] = prefs.blinkingCursor;
  status[BRL_GSC_ATTRVIS] = prefs.showAttributes;
  status[BRL_GSC_ATTRBLINK] = prefs.blinkingAttributes;
  status[BRL_GSC_CAPBLINK] = prefs.blinkingCapitals;
  status[BRL_GSC_TUNES] = prefs.alertTunes;
  status[BRL_GSC_HELP] = (dispmd & HELP_SCRN) != 0;
  status[BRL_GSC_INFO] = infmode;
  status[BRL_GSC_AUTOREPEAT] = prefs.autorepeat;
  status[BRL_GSC_AUTOSPEAK] = prefs.autospeak;
}

static void
setStatusCellsMDV (unsigned char *status) {
  setCoordinateVertical(&status[0], p->winx+1, p->winy+1);
}

static void
setStatusCellsVoyager (unsigned char *status) {
  setNumberVertical(&status[0], p->winy);
  setNumberVertical(&status[1], scr.posy);
  if ((dispmd & FROZ_SCRN) == FROZ_SCRN) {
    status[2] = textTable['F'];
  } else {
    setNumberVertical(&status[2], scr.posx);
  }
}

typedef void (*SetStatusCellsHandler) (unsigned char *status);
typedef struct {
  SetStatusCellsHandler set;
  unsigned char count;
} StatusStyleEntry;
static const StatusStyleEntry statusStyleTable[] = {
  {setStatusCellsNone, 0},
  {setStatusCellsAlva, 3},
  {setStatusCellsTieman, 5},
  {setStatusCellsPB80, 1},
  {setStatusCellsGeneric, 0},
  {setStatusCellsMDV, 2},
  {setStatusCellsVoyager, 3}
};
static const int statusStyleCount = sizeof(statusStyleTable) / sizeof(statusStyleTable[0]);

static void
setStatusCells (void) {
  unsigned char status[BRL_MAX_STATUS_CELL_COUNT];        /* status cell buffer */
  memset(status, 0, sizeof(status));
  if (prefs.statusStyle < statusStyleCount)
    statusStyleTable[prefs.statusStyle].set(status);
  braille->writeStatus(&brl, status);
}

static void
showInfo (void) {
  /* Here we must be careful. Some displays (e.g. Braille Lite 18)
   * are very small, and others (e.g. Bookworm) are even smaller.
   */
  unsigned char status[22];
  setStatusText(&brl, "info");
  if (brl.x*brl.y >= 21) {
    sprintf(status, "%02d:%02d %02d:%02d %02d %c%c%c%c%c%c",
            p->winx, p->winy, scr.posx, scr.posy, curscr, 
            p->trackCursor? 't': ' ',
            prefs.showCursor? (prefs.blinkingCursor? 'B': 'v'):
                              (prefs.blinkingCursor? 'b': ' '),
            p->showAttributes? 'a': 't',
            ((dispmd & FROZ_SCRN) == FROZ_SCRN)? 'f': ' ',
            prefs.textStyle? '6': '8',
            prefs.blinkingCapitals? 'B': ' ');
    writeBrailleString(&brl, status);
  } else {
    int i;
    sprintf(status, "xxxxx %02d %c%c%c%c%c%c     ",
            curscr,
            p->trackCursor? 't': ' ',
            prefs.showCursor? (prefs.blinkingCursor? 'B': 'v'):
                              (prefs.blinkingCursor? 'b': ' '),
            p->showAttributes? 'a': 't',
            ((dispmd & FROZ_SCRN) == FROZ_SCRN) ?'f': ' ',
            prefs.textStyle? '6': '8',
            prefs.blinkingCapitals? 'B': ' ');
    if (braille->writeVisual) {
      memcpy(brl.buffer, status, brl.x*brl.y);
      braille->writeVisual(&brl);
    }

    memset(&status, 0, 5);
    setCoordinateUpper(&status[0], scr.posx, scr.posy);
    setCoordinateLower(&status[0], p->winx, p->winy);
    setStateDots(&status[4]);

    /* We have to do the Braille translation ourselves since we
     * don't want the first five characters to be translated.
     */
    for (i=5; status[i]; i++) status[i] = textTable[status[i]];
    memcpy(brl.buffer, status, brl.x*brl.y);
    braille->writeWindow(&brl);
  }
}

static void
handleSignal (int number, void (*handler) (int))
{
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_handler = handler;
  if (sigaction(number, &action, NULL) == -1) {
    LogError("signal set");
  }
}

static void
exitLog (void) {
  /* Reopen syslog (in case -e closed it) so that there will
   * be a "stopped" message to match the "starting" message.
   */
  LogOpen(0);
  LogPrint(LOG_INFO, "Terminated.");
  LogClose();
}

static void
exitScreenParameters (void) {
  int i;
  /* don't forget that scrparam[0] is staticaly allocated */
  for (i = 1; i <= MAX_SCR; i++) 
    free(scrparam[i]);
}

static void
terminateProgram (int quickly) {
  int flags = MSG_NODELAY;

#ifdef ENABLE_SPEECH_SUPPORT
  int silently = quickly;
  if (speech == &noSpeech) silently = 1;
  if (silently) flags |= MSG_SILENT;
#endif /* ENABLE_SPEECH_SUPPORT */

  clearStatusCells(&brl);
  message("BRLTTY exiting.", flags);

#ifdef ENABLE_SPEECH_SUPPORT
  if (!silently) {
    int awaitSilence = speech->isSpeaking();
    int i;
    for (i=0; i<messageDelay; i+=updateInterval) {
      approximateDelay(updateInterval);
      if (readCommand(BRL_CTX_MESSAGE) != EOF) break;
      if (awaitSilence) {
        speech->doTrack();
        if (!speech->isSpeaking()) break;
      }
    }
  }
#endif /* ENABLE_SPEECH_SUPPORT */

  exit(0);
}

static void 
terminationHandler (int signalNumber) {
  terminateProgram(signalNumber == SIGINT);
}

static void 
childDeathHandler (int signalNumber) {
  pid_t process;
  int status;
  while ((process = waitpid(-1, &status, WNOHANG)) > 0) {
    if (process == routingProcess) {
      routingProcess = 0;
      routingStatus = WIFEXITED(status)? WEXITSTATUS(status): ROUTE_ERROR;
    }
  }
}

static void 
slideWindowVertically (int y) {
  if (y < p->winy)
    p->winy = y;
  else if  (y >= (p->winy + brl.y))
    p->winy = y - (brl.y - 1);
}

static void 
placeWindowHorizontally (int x) {
  p->winx = x / brl.x * brl.x;
}

static void 
trackCursor (int place) {
#ifdef ENABLE_CONTRACTED_BRAILLE
  if (contracted) {
    p->winy = scr.posy;
    if (scr.posx < p->winx) {
      int length = scr.posx + 1;
      unsigned char buffer[length];
      int onspace = 1;
      readScreen(0, p->winy, length, 1, buffer, SCR_TEXT);
      while (length) {
        if ((isspace(buffer[--length]) != 0) != onspace) {
          if (onspace) {
            onspace = 0;
          } else {
            ++length;
            break;
          }
        }
      }
      p->winx = length;
    }
    contractedTrack = 1;
    return;
  }
#endif /* ENABLE_CONTRACTED_BRAILLE */

  if (place)
    if ((scr.posx < p->winx) || (scr.posx >= (p->winx + brl.x)) ||
        (scr.posy < p->winy) || (scr.posy >= (p->winy + brl.y)))
      placeWindowHorizontally(scr.posx);

  if (prefs.slidingWindow) {
    int reset = brl.x * 3 / 10;
    int trigger = prefs.eagerSlidingWindow? brl.x*3/20: 0;
    if (scr.posx < (p->winx + trigger))
      p->winx = MAX(scr.posx-reset, 0);
    else if (scr.posx >= (p->winx + brl.x - trigger))
      p->winx = MAX(MIN(scr.posx+reset+1, scr.cols)-brl.x, 0);
  } else if (scr.posx < p->winx) {
    p->winx -= ((p->winx - scr.posx - 1) / brl.x + 1) * brl.x;
    if (p->winx < 0) p->winx = 0;
  } else
    p->winx += (scr.posx - p->winx) / brl.x * brl.x;

  slideWindowVertically(scr.posy);
}

#ifdef ENABLE_SPEECH_SUPPORT
static int speechTracking = 0;
static int speechScreen = -1;
static int speechLine = 0;
static int speechIndex = -1;

static void
trackSpeech (int index) {
  placeWindowHorizontally(index % scr.cols);
  slideWindowVertically((index / scr.cols) + speechLine);
}

static void
sayRegion (int left, int top, int width, int height, int track, SayMode mode) {
  /* OK heres a crazy idea: why not send the attributes with the
   * text, in case some inflection or marking can be added...! The
   * speech driver's say function will receive a buffer of text
   * and a length, but in reality the buffer will contain twice
   * len bytes: the text followed by the video attribs data.
   */
  int length = width * height;
  unsigned char buffer[length * 2];

  if (mode == sayImmediate) speech->mute();
  readScreen(left, top, width, height, buffer, SCR_TEXT);
  if (speech->express) {
    readScreen(left, top, width, height, buffer+length, SCR_ATTRIB);
    speech->express(buffer, length);
  } else {
    speech->say(buffer, length);
  }

  speechTracking = track;
  speechScreen = scr.no;
  speechLine = top;
}

static void
sayLines (int line, int count, int track, SayMode mode) {
  sayRegion(0, line, scr.cols, count, track, mode);
}
#endif /* ENABLE_SPEECH_SUPPORT */

static int
upDifferentLine (short mode) {
   if (p->winy > 0) {
      char buffer1[scr.cols], buffer2[scr.cols];
      int skipped = 0;
      if (mode==SCR_TEXT && p->showAttributes)
         mode = SCR_ATTRIB;
      readScreen(0, p->winy, scr.cols, 1, buffer1, mode);
      do {
         readScreen(0, --p->winy, scr.cols, 1, buffer2, mode);
         if (memcmp(buffer1, buffer2, scr.cols) ||
             (mode==SCR_TEXT && prefs.showCursor && p->winy==scr.posy))
            return 1;

         /* lines are identical */
         if (skipped == 0)
            playTune(&tune_skip_first);
         else if (skipped <= 4)
            playTune(&tune_skip);
         else if (skipped % 4 == 0)
            playTune(&tune_skip_more);
         skipped++;
      } while (p->winy > 0);
   }

   playTune(&tune_bounce);
   return 0;
}

static int
downDifferentLine (short mode) {
   if (p->winy < (scr.rows - brl.y)) {
      char buffer1[scr.cols], buffer2[scr.cols];
      int skipped = 0;
      if (mode==SCR_TEXT && p->showAttributes)
         mode = SCR_ATTRIB;
      readScreen(0, p->winy, scr.cols, 1, buffer1, mode);
      do {
         readScreen(0, ++p->winy, scr.cols, 1, buffer2, mode);
         if (memcmp(buffer1, buffer2, scr.cols) ||
             (mode==SCR_TEXT && prefs.showCursor && p->winy==scr.posy))
            return 1;

         /* lines are identical */
         if (skipped == 0)
            playTune(&tune_skip_first);
         else if (skipped <= 4)
            playTune(&tune_skip);
         else if (skipped % 4 == 0)
            playTune(&tune_skip_more);
         skipped++;
      } while (p->winy < (scr.rows - brl.y));
   }

   playTune(&tune_bounce);
   return 0;
}

static void
upOneLine (short mode) {
   if (p->winy > 0)
      p->winy--;
   else
      playTune(&tune_bounce);
}

static void
downOneLine (short mode) {
   if (p->winy < (scr.rows - brl.y))
      p->winy++;
   else
      playTune(&tune_bounce);
}

static void
upLine (short mode) {
   if (prefs.skipIdenticalLines)
      upDifferentLine(mode);
   else
      upOneLine(mode);
}

static void
downLine (short mode) {
   if (prefs.skipIdenticalLines)
      downDifferentLine(mode);
   else
      downOneLine(mode);
}

static void
overlayAttributes (const unsigned char *attributes, int width, int height) {
  int row;
  for (row=0; row<height; row++) {
    int column;
    for (column=0; column<width; column++) {
      switch (attributes[row*width + column]) {
        /* Experimental! Attribute values are hardcoded... */
        case 0x08: /* dark-gray on black */
        case 0x07: /* light-gray on black */
        case 0x17: /* light-gray on blue */
        case 0x30: /* black on cyan */
          break;
        case 0x70: /* black on light-gray */
          brl.buffer[row*brl.x + column] |= (BRL_DOT7 | BRL_DOT8);
          break;
        case 0x0F: /* white on black */
        default:
          brl.buffer[row*brl.x + column] |= (BRL_DOT8);
          break;
      }
    }
  }
}


static int
insertCharacter (unsigned char character, int flags) {
  if (islower(character)) {
    if (flags & (BRL_FLG_CHAR_SHIFT | BRL_FLG_CHAR_UPPER)) character = toupper(character);
  } else if (flags & BRL_FLG_CHAR_SHIFT) {
    switch (character) {
      case '1': character = '!'; break;
      case '2': character = '@'; break;
      case '3': character = '#'; break;
      case '4': character = '$'; break;
      case '5': character = '%'; break;
      case '6': character = '^'; break;
      case '7': character = '&'; break;
      case '8': character = '*'; break;
      case '9': character = '('; break;
      case '0': character = ')'; break;
      case '-': character = '_'; break;
      case '=': character = '+'; break;
      case '[': character = '{'; break;
      case ']': character = '}'; break;
      case'\\': character = '|'; break;
      case ';': character = ':'; break;
      case'\'': character = '"'; break;
      case '`': character = '~'; break;
      case ',': character = '<'; break;
      case '.': character = '>'; break;
      case '/': character = '?'; break;
    }
  }

  if (flags & BRL_FLG_CHAR_CONTROL) {
    if ((character & 0X6F) == 0X2F)
      character |= 0X50;
    else
      character &= 0X9F;
  }

  {
    ScreenKey key = character;
    if (flags & BRL_FLG_CHAR_META) key |= SCR_KEY_MOD_META;
    return insertKey(key);
  }
}

typedef int (*RowTester) (int column, int row, void *data);
static void
findRow (int column, int increment, RowTester test, void *data) {
  int row = p->winy + increment;
  while ((row >= 0) && (row <= scr.rows-brl.y)) {
    if (test(column, row, data)) {
      p->winy = row;
      return;
    }
    row += increment;
  }
  playTune(&tune_bounce);
}

static int
testIndent (int column, int row, void *data) {
  int count = column+1;
  char buffer[count];
  readScreen(0, row, count, 1, buffer, SCR_TEXT);
  while (column >= 0) {
    if ((buffer[column] != ' ') && (buffer[column] != 0)) return 1;
    --column;
  }
  return 0;
}

static int
testPrompt (int column, int row, void *data) {
  const char *prompt = data;
  int count = column+1;
  char buffer[count];
  readScreen(0, row, count, 1, buffer, SCR_TEXT);
  return memcmp(buffer, prompt, count) == 0;
}

static int
getRightShift (void) {
#ifdef ENABLE_CONTRACTED_BRAILLE
  if (contracted) return contractedLength;
#endif /* ENABLE_CONTRACTED_BRAILLE */
  return fwinshift;
}

static int
getOffset (int arg, int end) {
#ifdef ENABLE_CONTRACTED_BRAILLE
  if (contracted) {
    int result = 0;
    int index;
    for (index=0; index<contractedLength; ++index) {
      int offset = contractedOffsets[index];
      if (offset != -1) {
        if (offset > arg) {
          if (end) result = index - 1;
          break;
        }
        result = index;
      }
    }
    return result;
  }
#endif /* ENABLE_CONTRACTED_BRAILLE */
  return arg;
}

unsigned char
cursorDots (void) {
  return prefs.cursorStyle?  (BRL_DOT1 | BRL_DOT2 | BRL_DOT3 | BRL_DOT4 | BRL_DOT5 | BRL_DOT6 | BRL_DOT7 | BRL_DOT8): (BRL_DOT7 | BRL_DOT8);
}

static void
setBlinkingState (int *state, int *timer, int visible, unsigned char invisibleTime, unsigned char visibleTime) {
  *timer = PREFERENCES_TIME((*state = visible)? visibleTime: invisibleTime);
}

static int cursorState;                /* display cursor on (toggled during blink) */
static int cursorTimer;
static void
setBlinkingCursor (int visible) {
  setBlinkingState(&cursorState, &cursorTimer, visible,
                   prefs.cursorInvisibleTime, prefs.cursorVisibleTime);
}

static int attributesState;
static int attributesTimer;
static void
setBlinkingAttributes (int visible) {
  setBlinkingState(&attributesState, &attributesTimer, visible,
                   prefs.attributesInvisibleTime, prefs.attributesVisibleTime);
}

static int capitalsState;                /* display caps off (toggled during blink) */
static int capitalsTimer;
static void
setBlinkingCapitals (int visible) {
  setBlinkingState(&capitalsState, &capitalsTimer, visible,
                   prefs.capitalsInvisibleTime, prefs.capitalsVisibleTime);
}

static void
resetBlinkingStates (void) {
  setBlinkingCursor(0);
  setBlinkingAttributes(1);
  setBlinkingCapitals(1);
}

static int
toggleFlag (unsigned char *flag, int command, const TuneDefinition *off, const TuneDefinition *on) {
  const TuneDefinition *tune;
  if ((command & BRL_FLG_TOGGLE_MASK) != BRL_FLG_TOGGLE_MASK)
    *flag = (command & BRL_FLG_TOGGLE_ON)? 1: ((command & BRL_FLG_TOGGLE_OFF)? 0: !*flag);
  if ((tune = *flag? on: off)) playTune(tune);
  return *flag;
}
#define TOGGLE(flag, off, on) toggleFlag(&flag, command, off, on)
#define TOGGLE_NOPLAY(flag) TOGGLE(flag, NULL, NULL)
#define TOGGLE_PLAY(flag) TOGGLE(flag, &tune_toggle_off, &tune_toggle_on)

int
main (int argc, char *argv[]) {
  short oldwinx, oldwiny;
  int i;                        /* loop counter */

#ifdef INIT_PATH
  if ((getpid() == 1) || (strstr(argv[0], "linuxrc") != NULL)) {
    fprintf(stderr, "BRLTTY started as %s\n", argv[0]);
    fflush(stderr);
    switch (fork()) {
      case -1: /* failed */
        fprintf(stderr, "Fork for BRLTTY failed: %s\n", strerror(errno));
        fflush(stderr);
      default: /* parent */
        fprintf(stderr, "Executing the real INIT: %s\n", INIT_PATH);
        fflush(stderr);
      exec_init:
        execv(INIT_PATH, argv);
        /* execv() shouldn't return */
        fprintf(stderr, "Execution of the real INIT failed: %s\n", strerror(errno));
        fflush(stderr);
        exit(1);
      case 0: { /* child */
        static char *arguments[] = {"brltty", "-E", "-n", "-e", "-linfo", NULL};
        argv = arguments;
        argc = (sizeof(arguments) / sizeof(arguments[0])) - 1;
        break;
      }
    }
  } else if (strstr(argv[0], "brltty") == NULL) {
    /* 
     * If we are substituting the real init binary, then we may consider
     * when someone might want to call that binary even when pid != 1.
     * One example is /sbin/telinit which is a symlink to /sbin/init.
     */
    goto exec_init;
  }
#endif /* INIT_PATH */

  /* Open the system log. */
  LogOpen(0);
  LogPrint(LOG_INFO, "Starting.");
  atexit(exitLog);

  /* Initialize global data assumed to be ready by the termination handler. */
  *p = initialScreenState;
  scrparam[0] = p;
  for (i = 1; i <= MAX_SCR; i++)
    scrparam[i] = NULL;
  curscr = 0;
  atexit(exitScreenParameters);
  
  /* We install SIGPIPE handler before startup() so that drivers which
   * use pipes can't cause program termination (the call to message() in
   * startup() in particular).
   */
  handleSignal(SIGPIPE, SIG_IGN);

  /* Install the program termination handler. */
  handleSignal(SIGTERM, terminationHandler);
  handleSignal(SIGINT, terminationHandler);

  /* Setup everything required on startup */
  startup(argc, argv);

  /* Install the handler which monitors the death of child processes. */
  handleSignal(SIGCHLD, childDeathHandler);

  describeScreen(&scr);
  /* NB: screen size can sometimes change, f.e. the video mode may be changed
   * when installing a new font. Will be detected by another call to
   * describeScreen in the main loop. Don't assume that scr.rows
   * and scr.cols are constants across loop iterations.
   */
  switchto(scr.no);                        /* allocate current screen params */
  p->trkx = scr.posx; p->trky = scr.posy;
  trackCursor(1);        /* set initial window position */
  p->motx = p->winx; p->moty = p->winy;
  oldwinx = p->winx; oldwiny = p->winy;
  if (prefs.pointerFollowsWindow) setPointer(p->winx, p->winy);
  getPointer(&p->ptrx, &p->ptry);

  /*
   * Main program loop 
   */
  resetBlinkingStates();
  while (1) {
    int pointerMoved = 0;

    /* The braille display can stick out by brl.x-offr columns from the
     * right edge of the screen.
     */
    short offr = scr.cols % brl.x;
    if (!offr) offr = brl.x;

    closeTuneDevice(0);

    if (routingStatus >= 0) {
      const TuneDefinition *tune;
      switch (routingStatus) {
        default:
          tune = &tune_routing_failed;
          break;
        case ROUTE_OK:
          tune = &tune_routing_succeeded;
          break;
      }
      playTune(tune);
      routingStatus = -1;
    }

    /*
     * Process any Braille input 
     */
    while (1) {
      static int command = EOF;
      int oldmotx = p->winx;
      int oldmoty = p->winy;

      {
        static int repeatTimer = 0;
        static int repeatStarted = 0;
        int next = readBrailleCommand(&brl,
                                      infmode? BRL_CTX_STATUS:
                                      ((dispmd & HELP_SCRN) == HELP_SCRN)? BRL_CTX_HELP:
                                      BRL_CTX_SCREEN);
        if (!prefs.autorepeat) repeatTimer = 0;
        if (!repeatTimer) repeatStarted = 0;

        if (next == EOF) {
          if (!repeatTimer) break;
          if ((repeatTimer -= updateInterval) > 0) break;
          repeatTimer = PREFERENCES_TIME(prefs.autorepeatInterval);
          repeatStarted = 1;
        } else {
          int repeatFlags = next & BRL_FLG_REPEAT_MASK;
          LogPrint(LOG_DEBUG, "Command: %06X", next);
          next &= ~BRL_FLG_REPEAT_MASK;

          if (prefs.skipIdenticalLines) {
            int real;
            switch (next & BRL_MSK_CMD) {
              default:
                real = next;
                break;
              case BRL_CMD_LNUP:
                real = BRL_CMD_PRDIFLN;
                break;
              case BRL_CMD_LNDN:
                real = BRL_CMD_NXDIFLN;
                break;
              case BRL_CMD_PRDIFLN:
                real = BRL_CMD_LNUP;
                break;
              case BRL_CMD_NXDIFLN:
                real = BRL_CMD_LNDN;
                break;
            }
            if (real != next) next = (next & ~BRL_MSK_CMD) | real;
          }

          switch (next & BRL_MSK_BLK) {
            default:
              switch (next & BRL_MSK_CMD) {
                default:
                  if (IS_DELAYED_COMMAND(repeatFlags)) next = BRL_CMD_NOOP;
                  repeatFlags = 0;

                case BRL_CMD_LNUP:
                case BRL_CMD_LNDN:
                case BRL_CMD_PRDIFLN:
                case BRL_CMD_NXDIFLN:
                case BRL_CMD_CHRLT:
                case BRL_CMD_CHRRT:

                case BRL_CMD_MENU_PREV_ITEM:
                case BRL_CMD_MENU_NEXT_ITEM:
                case BRL_CMD_MENU_PREV_SETTING:
                case BRL_CMD_MENU_NEXT_SETTING:

                case BRL_BLK_PASSKEY + BRL_KEY_BACKSPACE:
                case BRL_BLK_PASSKEY + BRL_KEY_DELETE:
                case BRL_BLK_PASSKEY + BRL_KEY_PAGE_UP:
                case BRL_BLK_PASSKEY + BRL_KEY_PAGE_DOWN:
                case BRL_BLK_PASSKEY + BRL_KEY_CURSOR_UP:
                case BRL_BLK_PASSKEY + BRL_KEY_CURSOR_DOWN:
                case BRL_BLK_PASSKEY + BRL_KEY_CURSOR_LEFT:
                case BRL_BLK_PASSKEY + BRL_KEY_CURSOR_RIGHT:
                  break;
              }

            case BRL_BLK_PASSCHAR:
            case BRL_BLK_PASSDOTS:
              break;
          }

          if (repeatStarted) {
            repeatStarted = 0;
            if (next == command) {
              next = BRL_CMD_NOOP;
              repeatFlags = 0;
            }
          }
          command = next;

          if (repeatFlags & BRL_FLG_REPEAT_DELAY) {
            repeatTimer = PREFERENCES_TIME(prefs.autorepeatDelay);
            if (!(repeatFlags & BRL_FLG_REPEAT_INITIAL)) break;
            repeatStarted = 1;
          } else if (repeatFlags & BRL_FLG_REPEAT_INITIAL) {
            repeatTimer = PREFERENCES_TIME(prefs.autorepeatInterval);
            repeatStarted = 1;
          } else {
            repeatTimer = 0;
          }     
        }
      }

    doCommand:
      if (!executeScreenCommand(command)) {
        switch (command & BRL_MSK_CMD) {
          case BRL_CMD_NOOP:        /* do nothing but loop */
            if (command & BRL_FLG_TOGGLE_ON)
              playTune(&tune_toggle_on);
            else if (command & BRL_FLG_TOGGLE_OFF)
              playTune(&tune_toggle_off);
            break;

          case BRL_CMD_TOP_LEFT:
            p->winx = 0;
          case BRL_CMD_TOP:
            p->winy = 0;
            break;
          case BRL_CMD_BOT_LEFT:
            p->winx = 0;
          case BRL_CMD_BOT:
            p->winy = scr.rows - brl.y;
            break;

          case BRL_CMD_WINUP:
            if (p->winy == 0)
              playTune (&tune_bounce);
            p->winy = MAX (p->winy - vwinshift, 0);
            break;
          case BRL_CMD_WINDN:
            if (p->winy == scr.rows - brl.y)
              playTune (&tune_bounce);
            p->winy = MIN (p->winy + vwinshift, scr.rows - brl.y);
            break;

          case BRL_CMD_LNUP:
            upOneLine(SCR_TEXT);
            break;
          case BRL_CMD_LNDN:
            downOneLine(SCR_TEXT);
            break;

          case BRL_CMD_PRDIFLN:
            upDifferentLine(SCR_TEXT);
            break;
          case BRL_CMD_NXDIFLN:
            downDifferentLine(SCR_TEXT);
            break;

          case BRL_CMD_ATTRUP:
            upDifferentLine(SCR_ATTRIB);
            break;
          case BRL_CMD_ATTRDN:
            downDifferentLine(SCR_ATTRIB);
            break;

          {
            int increment;
          case BRL_CMD_PRPGRPH:
            increment = -1;
            goto findParagraph;
          case BRL_CMD_NXPGRPH:
            increment = 1;
          findParagraph:
            {
              int found = 0;
              unsigned char buffer[scr.cols];
              int findBlank = 1;
              int line = p->winy;
              int i;
              while ((line >= 0) && (line <= (scr.rows - brl.y))) {
                readScreen(0, line, scr.cols, 1, buffer, SCR_TEXT);
                for (i=0; i<scr.cols; i++)
                  if ((buffer[i] != ' ') && (buffer[i] != 0))
                    break;
                if ((i == scr.cols) == findBlank) {
                  if (!findBlank) {
                    found = 1;
                    p->winy = line;
                    p->winx = 0;
                    break;
                  }
                  findBlank = 0;
                }
                line += increment;
              }
              if (!found) playTune(&tune_bounce);
            }
            break;
          }

          {
            int increment;
          case BRL_CMD_PRPROMPT:
            increment = -1;
            goto findPrompt;
          case BRL_CMD_NXPROMPT:
            increment = 1;
          findPrompt:
            {
              unsigned char buffer[scr.cols];
              unsigned char *blank;
              readScreen(0, p->winy, scr.cols, 1, buffer, SCR_TEXT);
              if ((blank = memchr(buffer, ' ', scr.cols))) {
                findRow(blank-buffer, increment, testPrompt, buffer);
              } else {
                playTune(&tune_command_rejected);
              }
            }
            break;
          }

          {
            int increment;
          case BRL_CMD_PRSEARCH:
            increment = -1;
            goto doSearch;
          case BRL_CMD_NXSEARCH:
            increment = 1;
          doSearch:
            if (cut_buffer) {
              int length = strlen(cut_buffer);
              int found = 0;
              if (length <= scr.cols) {
                int line = p->winy;
                unsigned char buffer[scr.cols+1];
                unsigned char string[length+1];
                for (i=0; i<length; i++) string[i] = tolower(cut_buffer[i]);
                string[length] = 0;
                while ((line >= 0) && (line <= (scr.rows - brl.y))) {
                  unsigned char *address = buffer;
                  readScreen(0, line, scr.cols, 1, buffer, SCR_TEXT);
                  for (i=0; i<scr.cols; i++) buffer[i] = tolower(buffer[i]);
                  buffer[scr.cols] = 0;
                  if (line == p->winy) {
                    if (increment < 0) {
                      int end = p->winx + length - 1;
                      if (end < scr.cols) buffer[end] = 0;
                    } else {
                      int start = p->winx + brl.x;
                      if (start > scr.cols) start = scr.cols;
                      address = buffer + start;
                    }
                  }
                  if ((address = strstr(address, string))) {
                    if (increment < 0) {
                      while (1) {
                        unsigned char *next = strstr(address+1, string);
                        if (!next) break;
                        address = next;
                      }
                    }
                    p->winy = line;
                    p->winx = (address - buffer) / brl.x * brl.x;
                    found = 1;
                    break;
                  }
                  line += increment;
                }
              }
              if (!found) playTune(&tune_bounce);
            } else {
              playTune(&tune_command_rejected);
            }
            break;
          }

          case BRL_CMD_LNBEG:
            if (p->winx)
              p->winx = 0;
            else
              playTune(&tune_bounce);
            break;
          case BRL_CMD_LNEND:
            if (p->winx == (scr.cols - brl.x))
              playTune(&tune_bounce);
            else
              p->winx = scr.cols - brl.x;
            break;

          case BRL_CMD_CHRLT:
            if (p->winx == 0)
              playTune (&tune_bounce);
            p->winx = MAX (p->winx - 1, 0);
            break;
          case BRL_CMD_CHRRT:
            if (p->winx < (scr.cols - 1))
              p->winx++;
            else
              playTune(&tune_bounce);
            break;

          case BRL_CMD_HWINLT:
            if (p->winx == 0)
              playTune(&tune_bounce);
            else
              p->winx = MAX(p->winx-hwinshift, 0);
            break;
          case BRL_CMD_HWINRT:
            if (p->winx < (scr.cols - hwinshift))
              p->winx += hwinshift;
            else
              playTune(&tune_bounce);
            break;

          case BRL_CMD_FWINLT:
            if (!(prefs.skipBlankWindows && (prefs.blankWindowsSkipMode == sbwAll))) {
              int oldX = p->winx;
              if (p->winx > 0) {
                p->winx = MAX(p->winx-fwinshift, 0);
                if (prefs.skipBlankWindows) {
                  if (prefs.blankWindowsSkipMode == sbwEndOfLine)
                    goto skipEndOfLine;
                  if (!prefs.showCursor ||
                      (scr.posy != p->winy) ||
                      (scr.posx >= (p->winx + brl.x))) {
                    int charCount = MIN(scr.cols, p->winx+brl.x);
                    int charIndex;
                    char buffer[charCount];
                    readScreen(0, p->winy, charCount, 1, buffer, SCR_TEXT);
                    for (charIndex=0; charIndex<charCount; ++charIndex)
                      if ((buffer[charIndex] != ' ') && (buffer[charIndex] != 0))
                        break;
                    if (charIndex == charCount)
                      goto wrapUp;
                  }
                }
                break;
              }
            wrapUp:
              if (p->winy == 0) {
                playTune(&tune_bounce);
                p->winx = oldX;
                break;
              }
              playTune(&tune_wrap_up);
              p->winx = MAX((scr.cols-offr)/fwinshift*fwinshift, 0);
              upLine(SCR_TEXT);
            skipEndOfLine:
              if (prefs.skipBlankWindows && (prefs.blankWindowsSkipMode == sbwEndOfLine)) {
                int charIndex;
                char buffer[scr.cols];
                readScreen(0, p->winy, scr.cols, 1, buffer, SCR_TEXT);
                for (charIndex=scr.cols-1; charIndex>=0; --charIndex)
                  if ((buffer[charIndex] != ' ') && (buffer[charIndex] != 0))
                    break;
                if (prefs.showCursor && (scr.posy == p->winy))
                  charIndex = MAX(charIndex, scr.posx);
                charIndex = MAX(charIndex, 0);
                if (charIndex < p->winx)
                  p->winx = charIndex / fwinshift * fwinshift;
              }
              break;
            }
          case BRL_CMD_FWINLTSKIP: {
            int oldX = p->winx;
            int oldY = p->winy;
            int tuneLimit = 3;
            int charCount;
            int charIndex;
            char buffer[scr.cols];
            while (1) {
              if (p->winx > 0) {
                p->winx = MAX(p->winx-fwinshift, 0);
              } else {
                if (p->winy == 0) {
                  playTune(&tune_bounce);
                  p->winx = oldX;
                  p->winy = oldY;
                  break;
                }
                if (tuneLimit-- > 0)
                  playTune(&tune_wrap_up);
                p->winx = MAX((scr.cols-offr)/fwinshift*fwinshift, 0);
                upLine(SCR_TEXT);
              }
              charCount = MIN(brl.x, scr.cols-p->winx);
              readScreen(p->winx, p->winy, charCount, 1, buffer, SCR_TEXT);
              for (charIndex=(charCount-1); charIndex>=0; charIndex--)
                if ((buffer[charIndex] != ' ') && (buffer[charIndex] != 0))
                  break;
              if (prefs.showCursor &&
                  (scr.posy == p->winy) &&
                  (scr.posx < (p->winx + charCount)))
                charIndex = MAX(charIndex, scr.posx-p->winx);
              if (charIndex >= 0) {
                if (prefs.slidingWindow)
                  p->winx = MAX(p->winx+charIndex-brl.x+1, 0);
                break;
              }
            }
            break;
          }

          case BRL_CMD_FWINRT:
            if (!(prefs.skipBlankWindows && (prefs.blankWindowsSkipMode == sbwAll))) {
              int oldX = p->winx;
              int rwinshift = getRightShift();
              if (p->winx < (scr.cols - rwinshift)) {
                p->winx += rwinshift;
                if (prefs.skipBlankWindows) {
                  if (!prefs.showCursor ||
                      (scr.posy != p->winy) ||
                      (scr.posx < p->winx)) {
                    int charCount = scr.cols - p->winx;
                    int charIndex;
                    char buffer[charCount];
                    readScreen(p->winx, p->winy, charCount, 1, buffer, SCR_TEXT);
                    for (charIndex=0; charIndex<charCount; ++charIndex)
                      if ((buffer[charIndex] != ' ') && (buffer[charIndex] != 0))
                        break;
                    if (charIndex == charCount)
                      goto wrapDown;
                  }
                }
                break;
              }
            wrapDown:
              if (p->winy >= (scr.rows - brl.y)) {
                playTune(&tune_bounce);
                p->winx = oldX;
                break;
              }
              playTune(&tune_wrap_down);
              p->winx = 0;
              downLine(SCR_TEXT);
              break;
            }
          case BRL_CMD_FWINRTSKIP: {
            int oldX = p->winx;
            int oldY = p->winy;
            int tuneLimit = 3;
            int charCount;
            int charIndex;
            char buffer[scr.cols];
            while (1) {
              int rwinshift = getRightShift();
              if (p->winx < (scr.cols - rwinshift)) {
                p->winx += rwinshift;
              } else {
                if (p->winy >= (scr.rows - brl.y)) {
                  playTune(&tune_bounce);
                  p->winx = oldX;
                  p->winy = oldY;
                  break;
                }
                if (tuneLimit-- > 0)
                  playTune(&tune_wrap_down);
                p->winx = 0;
                downLine(SCR_TEXT);
              }
              charCount = MIN(brl.x, scr.cols-p->winx);
              readScreen(p->winx, p->winy, charCount, 1, buffer, SCR_TEXT);
              for (charIndex=0; charIndex<charCount; charIndex++)
                if ((buffer[charIndex] != ' ') && (buffer[charIndex] != 0))
                  break;
              if (prefs.showCursor &&
                  (scr.posy == p->winy) &&
                  (scr.posx >= p->winx))
                charIndex = MIN(charIndex, scr.posx-p->winx);
              if (charIndex < charCount) {
                if (prefs.slidingWindow)
                  p->winx = MIN(p->winx+charIndex, scr.cols-offr);
                break;
              }
            }
            break;
          }

          case BRL_CMD_RETURN:
            if ((p->winx != p->motx) || (p->winy != p->moty)) {
          case BRL_CMD_BACK:
              p->winx = p->motx;
              p->winy = p->moty;
              break;
            }
          case BRL_CMD_HOME:
            trackCursor(1);
            break;

          case BRL_CMD_RESTARTBRL:
            restartBrailleDriver();
            break;
          case BRL_CMD_PASTE:
            if ((dispmd & HELP_SCRN) != HELP_SCRN && !routingProcess)
              if (cut_paste())
                break;
            playTune(&tune_command_rejected);
            break;
          case BRL_CMD_CSRJMP_VERT:
            playTune(routeCursor(-1, p->winy, curscr)?
                     &tune_routing_started:
                     &tune_command_rejected);
            break;

          case BRL_CMD_CSRVIS:
            /* toggles the preferences option that decides whether cursor
               is shown at all */
            TOGGLE_PLAY(prefs.showCursor);
            break;
          case BRL_CMD_CSRHIDE:
            /* This is for briefly hiding the cursor */
            TOGGLE_NOPLAY(p->hideCursor);
            /* no tune */
            break;
          case BRL_CMD_CSRSIZE:
            TOGGLE_PLAY(prefs.cursorStyle);
            break;
          case BRL_CMD_CSRTRK:
            if (TOGGLE(p->trackCursor, &tune_cursor_unlinked, &tune_cursor_linked)) {
#ifdef ENABLE_SPEECH_SUPPORT
              if (speech->isSpeaking()) {
                speechIndex = -1;
              } else
#endif /* ENABLE_SPEECH_SUPPORT */
                trackCursor(1);
            }
            break;
          case BRL_CMD_CSRBLINK:
            setBlinkingCursor(1);
            if (TOGGLE_PLAY(prefs.blinkingCursor)) {
              setBlinkingAttributes(1);
              setBlinkingCapitals(0);
            }
            break;

          case BRL_CMD_ATTRVIS:
            TOGGLE_PLAY(prefs.showAttributes);
            break;
          case BRL_CMD_ATTRBLINK:
            setBlinkingAttributes(1);
            if (TOGGLE_PLAY(prefs.blinkingAttributes)) {
              setBlinkingCapitals(1);
              setBlinkingCursor(0);
            }
            break;

          case BRL_CMD_CAPBLINK:
            setBlinkingCapitals(1);
            if (TOGGLE_PLAY(prefs.blinkingCapitals)) {
              setBlinkingAttributes(0);
              setBlinkingCursor(0);
            }
            break;

          case BRL_CMD_SKPIDLNS:
            TOGGLE_PLAY(prefs.skipIdenticalLines);
            break;
          case BRL_CMD_SKPBLNKWINS:
            TOGGLE_PLAY(prefs.skipBlankWindows);
            break;
          case BRL_CMD_SLIDEWIN:
            TOGGLE_PLAY(prefs.slidingWindow);
            break;

          case BRL_CMD_DISPMD:
            setTranslationTable(TOGGLE_NOPLAY(p->showAttributes));
            break;
          case BRL_CMD_SIXDOTS:
            TOGGLE_PLAY(prefs.textStyle);
            break;

          case BRL_CMD_AUTOREPEAT:
            TOGGLE_PLAY(prefs.autorepeat);
            break;
          case BRL_CMD_TUNES:
            TOGGLE_PLAY(prefs.alertTunes);        /* toggle sound on/off */
            break;
          case BRL_CMD_FREEZE: {
            unsigned char frozen = (dispmd & FROZ_SCRN) != 0;
            if (TOGGLE(frozen, &tune_screen_unfrozen, &tune_screen_frozen)) {
              dispmd = selectDisplay(dispmd | FROZ_SCRN);
            } else {
              dispmd = selectDisplay(dispmd & ~FROZ_SCRN);
            }
            break;
          }

#ifdef ENABLE_PREFERENCES_MENU
          case BRL_CMD_PREFMENU:
            updatePreferences();
            break;
          case BRL_CMD_PREFSAVE:
            if (savePreferences()) {
              playTune(&tune_command_done);
            }
            break;
#endif /* ENABLE_PREFERENCES_MENU */
          case BRL_CMD_PREFLOAD:
            if (loadPreferences(1)) {
              resetBlinkingStates();
              playTune(&tune_command_done);
            }
            break;

          case BRL_CMD_HELP: {
            unsigned char help = (dispmd & HELP_SCRN) != 0;
            infmode = 0;        /* ... and not in info mode */
            if (TOGGLE_NOPLAY(help)) {
              dispmd = selectDisplay(dispmd | HELP_SCRN);
              if (dispmd & HELP_SCRN) { /* help screen selection successful */
                switchto(0);        /* screen 0 for help screen */
              } else {      /* help screen selection failed */
                message("help not available", 0);
              }
            } else {
              dispmd = selectDisplay(dispmd & ~HELP_SCRN);
            }
            break;
          }
          case BRL_CMD_INFO:
            TOGGLE_NOPLAY(infmode);
            break;

#ifdef ENABLE_LEARN_MODE
          case BRL_CMD_LEARN:
            learnMode(&brl, updateInterval, 10000);
            break;
#endif /* ENABLE_LEARN_MODE */

          case BRL_CMD_SWITCHVT_PREV:
            if (!switchVirtualTerminal(scr.no-1))
              playTune(&tune_command_rejected);
            break;
          case BRL_CMD_SWITCHVT_NEXT:
            if (!switchVirtualTerminal(scr.no+1))
              playTune(&tune_command_rejected);
            break;

#ifdef ENABLE_SPEECH_SUPPORT
          case BRL_CMD_RESTARTSPEECH:
            restartSpeechDriver();
            break;
          case BRL_CMD_SPKHOME:
            if (scr.no == speechScreen) {
              trackSpeech(speech->getTrack());
            } else {
              playTune(&tune_command_rejected);
            }
            break;
          case BRL_CMD_AUTOSPEAK:
            TOGGLE_PLAY(prefs.autospeak);
            break;
          case BRL_CMD_MUTE:
            speech->mute();
            break;

          case BRL_CMD_SAY_LINE:
            sayLines(p->winy, 1, 0, prefs.sayLineMode);
            break;
          case BRL_CMD_SAY_ABOVE:
            sayLines(0, p->winy+1, 1, sayImmediate);
            break;
          case BRL_CMD_SAY_BELOW:
            sayLines(p->winy, scr.rows-p->winy, 1, sayImmediate);
            break;

          case BRL_CMD_SAY_SLOWER:
            if (speech->rate && (prefs.speechRate > 0)) {
              setSpeechRate(--prefs.speechRate);
            } else {
              playTune(&tune_command_rejected);
            }
            break;
          case BRL_CMD_SAY_FASTER:
            if (speech->rate && (prefs.speechRate < SPK_MAXIMUM_RATE)) {
              setSpeechRate(++prefs.speechRate);
            } else {
              playTune(&tune_command_rejected);
            }
            break;

          case BRL_CMD_SAY_SOFTER:
            if (speech->volume && (prefs.speechVolume > 0)) {
              setSpeechVolume(--prefs.speechVolume);
            } else {
              playTune(&tune_command_rejected);
            }
            break;
          case BRL_CMD_SAY_LOUDER:
            if (speech->volume && (prefs.speechVolume < SPK_MAXIMUM_VOLUME)) {
              setSpeechVolume(++prefs.speechVolume);
            } else {
              playTune(&tune_command_rejected);
            }
            break;
#endif /* ENABLE_SPEECH_SUPPORT */

          default: {
            int blk = command & BRL_MSK_BLK;
            int arg = command & BRL_MSK_ARG;
            int flags = command & BRL_MSK_FLG;

            switch (blk) {
              case BRL_BLK_PASSKEY: {
                unsigned short key;
                switch (arg) {
                  case BRL_KEY_ENTER:
                    key = SCR_KEY_ENTER;
                    break;
                  case BRL_KEY_TAB:
                    key = SCR_KEY_TAB;
                    break;
                  case BRL_KEY_BACKSPACE:
                    key = SCR_KEY_BACKSPACE;
                    break;
                  case BRL_KEY_ESCAPE:
                    key = SCR_KEY_ESCAPE;
                    break;
                  case BRL_KEY_CURSOR_LEFT:
                    key = SCR_KEY_CURSOR_LEFT;
                    break;
                  case BRL_KEY_CURSOR_RIGHT:
                    key = SCR_KEY_CURSOR_RIGHT;
                    break;
                  case BRL_KEY_CURSOR_UP:
                    key = SCR_KEY_CURSOR_UP;
                    break;
                  case BRL_KEY_CURSOR_DOWN:
                    key = SCR_KEY_CURSOR_DOWN;
                    break;
                  case BRL_KEY_PAGE_UP:
                    key = SCR_KEY_PAGE_UP;
                    break;
                  case BRL_KEY_PAGE_DOWN:
                    key = SCR_KEY_PAGE_DOWN;
                    break;
                  case BRL_KEY_HOME:
                    key = SCR_KEY_HOME;
                    break;
                  case BRL_KEY_END:
                    key = SCR_KEY_END;
                    break;
                  case BRL_KEY_INSERT:
                    key = SCR_KEY_INSERT;
                    break;
                  case BRL_KEY_DELETE:
                    key = SCR_KEY_DELETE;
                    break;
                  default:
                    if (arg < BRL_KEY_FUNCTION) goto badKey;
                    key = SCR_KEY_FUNCTION + (arg - BRL_KEY_FUNCTION);
                    break;
                }
                if (!insertKey(key))
                badKey:
                  playTune(&tune_command_rejected);
                break;
              }

              case BRL_BLK_PASSCHAR:
                if (!insertCharacter(arg, flags)) {
                  playTune(&tune_command_rejected);
                }
                break;

              case BRL_BLK_PASSDOTS:
                if (!insertCharacter(untextTable[arg], flags)) {
                  playTune(&tune_command_rejected);
                }
                break;

              case BRL_BLK_PASSAT2:
                if (AT2_interpretCode(&command, arg)) goto doCommand;
                break;

              case BRL_BLK_ROUTE:
                if (arg < brl.x) {
                  arg = getOffset(arg, 0);
                  if (routeCursor(MIN(p->winx+arg, scr.cols-1), p->winy, curscr)) {
                    playTune(&tune_routing_started);
                    break;
                  }
                }
                playTune(&tune_command_rejected);
                break;
              case BRL_BLK_CUTBEGIN:
                if (arg < brl.x && p->winx+arg < scr.cols) {
                  arg = getOffset(arg, 0);
                  cut_begin(p->winx+arg, p->winy);
                } else
                  playTune(&tune_command_rejected);
                break;
              case BRL_BLK_CUTAPPEND:
                if (arg < brl.x && p->winx+arg < scr.cols) {
                  arg = getOffset(arg, 0);
                  cut_append(p->winx+arg, p->winy);
                } else
                  playTune(&tune_command_rejected);
                break;
              case BRL_BLK_CUTRECT:
                if (arg < brl.x) {
                  arg = getOffset(arg, 1);
                  if (cut_rectangle(MIN(p->winx+arg, scr.cols-1), p->winy))
                    break;
                }
                playTune(&tune_command_rejected);
                break;
              case BRL_BLK_CUTLINE:
                if (arg < brl.x) {
                  arg = getOffset(arg, 1);
                  if (cut_line(MIN(p->winx+arg, scr.cols-1), p->winy))
                    break;
                }
                playTune(&tune_command_rejected);
                break;
              case BRL_BLK_DESCCHAR:
                if (arg < brl.x && p->winx+arg < scr.cols) {
                  static char *colours[] = {
                    "black",     "blue",          "green",       "cyan",
                    "red",       "magenta",       "brown",       "light grey",
                    "dark grey", "light blue",    "light green", "light cyan",
                    "light red", "light magenta", "yellow",      "white"
                  };
                  char buffer[0X40];
                  unsigned char character, attributes;
                  arg = getOffset(arg, 0);
                  readScreen(p->winx+arg, p->winy, 1, 1, &character, SCR_TEXT);
                  readScreen(p->winx+arg, p->winy, 1, 1, &attributes, SCR_ATTRIB);
                  sprintf(buffer, "char %d (0x%02x): %s on %s",
                          character, character,
                          colours[attributes & 0X0F],
                          colours[(attributes & 0X70) >> 4]);
                  if (attributes & 0X80) strcat(buffer, " blink");
                  message(buffer, 0);
                } else
                  playTune(&tune_command_rejected);
                break;
              case BRL_BLK_SETLEFT:
                if (arg < brl.x && p->winx+arg < scr.cols) {
                  arg = getOffset(arg, 0);
                  p->winx += arg;
                } else
                  playTune(&tune_command_rejected);
                break;
              case BRL_BLK_SETMARK: {
                ScreenMark *mark = &p->marks[arg];
                mark->column = p->winx;
                mark->row = p->winy;
                playTune(&tune_mark_set);
                break;
              }
              case BRL_BLK_GOTOMARK: {
                ScreenMark *mark = &p->marks[arg];
                p->winx = mark->column;
                p->winy = mark->row;
                break;
              }
              case BRL_BLK_SWITCHVT:
                  if (!switchVirtualTerminal(arg+1))
                       playTune(&tune_command_rejected);
                  break;
              {
                int increment;
              case BRL_BLK_PRINDENT:
                increment = -1;
                goto findIndent;
              case BRL_BLK_NXINDENT:
                increment = 1;
              findIndent:
                arg = getOffset(arg, 0);
                findRow(MIN(p->winx+arg, scr.cols-1),
                        increment, testIndent, NULL);
                break;
              }
              default:
                playTune(&tune_command_rejected);
                LogPrint(LOG_WARNING, "Unrecognized command: %04X", command);
            }
            break;
          }
        }
      }

      if ((p->winx != oldmotx) || (p->winy != oldmoty)) {
        /* The window has been manually moved. */
        p->motx = p->winx;
        p->moty = p->winy;
        contracted = 0;
      }
    }
        
    /*
     * Update blink counters: 
     */
    if (prefs.blinkingCursor)
      if ((cursorTimer -= updateInterval) <= 0)
        setBlinkingCursor(!cursorState);
    if (prefs.blinkingAttributes)
      if ((attributesTimer -= updateInterval) <= 0)
        setBlinkingAttributes(!attributesState);
    if (prefs.blinkingCapitals)
      if ((capitalsTimer -= updateInterval) <= 0)
        setBlinkingCapitals(!capitalsState);

    /*
     * Update Braille display and screen information.  Switch screen 
     * params if screen number has changed.
     */
    describeScreen(&scr);
    if (!(dispmd & (HELP_SCRN|FROZ_SCRN)) && curscr != scr.no) switchto(scr.no);

    /* NB: This should also accomplish screen resizing: scr.rows and
     * scr.cols may have changed.
     */
    {
      int maximum = scr.rows - brl.y;
      int *table[] = {&p->winy, &p->moty, NULL};
      int **value = table;
      while (*value) {
        if (**value > maximum) **value = maximum;
        ++value;
      }
    }
    {
      int maximum = scr.cols - 1;
      int *table[] = {&p->winx, &p->motx, NULL};
      int **value = table;
      while (*value) {
        if (**value > maximum) **value = maximum;
        ++value;
      }
    }

#ifdef ENABLE_SPEECH_SUPPORT
    /* called continually even if we're not tracking so that the pipe doesn't fill up. */
    speech->doTrack();
#endif /* ENABLE_SPEECH_SUPPORT */

    if (p->trackCursor) {
#ifdef ENABLE_SPEECH_SUPPORT
      if (speechTracking) {
        if ((scr.no == speechScreen) && speech->isSpeaking()) {
          int index = speech->getTrack();
          if (index != speechIndex) {
            trackSpeech(speechIndex = index);
          }
        } else {
          speechTracking = 0;
        }
      }
      if (!speechTracking)
#endif /* ENABLE_SPEECH_SUPPORT */
      {
        /* If cursor moves while blinking is on */
        if (prefs.blinkingCursor) {
          if (scr.posy != p->trky) {
            /* turn off cursor to see what's under it while changing lines */
            setBlinkingCursor(0);
          } else if (scr.posx != p->trkx) {
            /* turn on cursor to see it moving on the line */
            setBlinkingCursor(1);
          }
        }
        /* If the cursor moves in cursor tracking mode: */
        if (!routingProcess && (scr.posx != p->trkx || scr.posy != p->trky)) {
          trackCursor(0);
          p->trkx = scr.posx;
          p->trky = scr.posy;
        } else if (prefs.windowFollowsPointer) {
          int x, y;
          if (getPointer(&x, &y)) {
            if ((x != p->ptrx)) {
              p->ptrx = x;
              if (x < p->winx)
                p->winx = x;
              else if (x >= (p->winx + brl.x))
                p->winx = x + 1 - brl.x;
              pointerMoved = 1;
            }

            if ((y != p->ptry)) {
              p->ptry = y;
              if (y < p->winy)
                p->winy = y;
              else if (y >= (p->winy + brl.y))
                p->winy = y + 1 - brl.y;
              pointerMoved = 1;
            }
          }
        }
      }
    }

#ifdef ENABLE_SPEECH_SUPPORT
    if (prefs.autospeak) {
      static int oldScreen = -1;
      static int oldX = -1;
      static int oldY = -1;
      static int oldLength = 0;
      static unsigned char *oldText = NULL;

      int newScreen = scr.no;
      int newX = scr.posx;
      int newY = scr.posy;
      int newLength = scr.cols;
      unsigned char newText[newLength];

      readScreen(0, p->winy, newLength, 1, newText, SCR_TEXT);

      if (!speechTracking) {
        int column = 0;
        int count = newLength;
        const unsigned char *text = newText;

        if (oldText) {
          if ((newScreen == oldScreen) && (p->winy == oldwiny) && (newLength == oldLength)) {
            if (memcmp(newText, oldText, newLength) != 0) {
              if ((newY == p->winy) && (newY == oldY)) {
                if ((newX > oldX) &&
                    (memcmp(newText, oldText, oldX) == 0) &&
                    (memcmp(newText+newX, oldText+oldX, newLength-newX) == 0)) {
                  column = oldX;
                  count = newX - oldX;
                  goto speak;
                }

                if ((newX < oldX) &&
                    (memcmp(newText, oldText, newX) == 0) &&
                    (memcmp(newText+newX, oldText+oldX, newLength-oldX) == 0)) {
                  column = newX;
                  count = oldX - newX;
                  text = oldText;
                  goto speak;
                }

                if ((newX == oldX) &&
                    (memcmp(newText, oldText, newX) == 0)) {
                  int x;
                  for (x=newX+1; x<newLength; ++x) {
                    if (memcmp(newText+x, oldText+oldX, newLength-x) == 0) {
                      column = oldX;
                      count = x - oldX;
                      goto speak;
                    }

                    if (memcmp(newText+newX, oldText+x, oldLength-x) == 0) {
                      column = newX;
                      count = x - newX;
                      text = oldText;
                      goto speak;
                    }
                  }
                }

                while (newText[column] == oldText[column]) ++column;
                while (newText[count-1] == oldText[count-1]) --count;
                count -= column;
              speak:;
              }
            } else if ((newY == p->winy) && ((newX != oldX) || (newY != oldY))) {
              column = newX;
              count = 1;
            } else {
              count = 0;
            }
          }
        }

        if (count) {
          speech->mute();
          speech->say(text+column, count);
        }
      }

      oldText = reallocWrapper(oldText, newLength);
      memcpy(oldText, newText, newLength);
      oldLength = newLength;

      oldScreen = newScreen;
      oldX = newX;
      oldY = newY;
    }

    processSpeechFifo();
#endif /* ENABLE_SPEECH_SUPPORT */

    /* There are a few things to take care of if the display has moved. */
    if ((p->winx != oldwinx) || (p->winy != oldwiny)) {
      if (prefs.pointerFollowsWindow && !pointerMoved) setPointer(p->winx, p->winy);

      if (prefs.showAttributes && prefs.blinkingAttributes) {
        /* Attributes are blinking.
           We could check to see if we changed screen, but that doesn't
           really matter... this is mainly for when you are hunting up/down
           for the line with attributes. */
        setBlinkingAttributes(1);
        /* problem: this still doesn't help when the braille window is
           stationnary and the attributes themselves are moving
           (example: tin). */
      }

      oldwinx = p->winx;
      oldwiny = p->winy;
    }

    if (infmode) {
      showInfo();
    } else {
      int cursorLocation = -1;
      contracted = 0;
#ifdef ENABLE_CONTRACTED_BRAILLE
      if (prefs.textStyle && contractionTable) {
        int windowLength = brl.x * brl.y;
        while (1) {
          int cursorOffset = cursorLocation;
          int inputLength = scr.cols - p->winx;
          int outputLength = windowLength;
          unsigned char inputBuffer[inputLength];
          unsigned char outputBuffer[outputLength];

          if ((scr.posy == p->winy) && (scr.posx >= p->winx)) cursorOffset = scr.posx - p->winx;
          readScreen(p->winx, p->winy, inputLength, 1, inputBuffer, SCR_TEXT);
          for (i=0; i<inputLength; ++i) contractedOffsets[i] = -1;
          if (!contractText(contractionTable,
                            inputBuffer, &inputLength,
                            outputBuffer, &outputLength,
                            contractedOffsets, cursorOffset))
            break;

          if (contractedTrack) {
            int inputEnd = inputLength;
            if (outputLength == windowLength) {
              int inputIndex = inputEnd;
              while (inputIndex) {
                int offset = contractedOffsets[--inputIndex];
                if (offset != -1) {
                  if (offset != outputLength) break;
                  inputEnd = inputIndex;
                }
              }
            }
            if (scr.posx >= (p->winx + inputEnd)) {
              int offset = 0;
              int onspace = 0;
              int length = scr.cols - p->winx;
              unsigned char buffer[length];
              readScreen(p->winx, p->winy, length, 1, buffer, SCR_TEXT);
              while (offset < length) {
                if ((isspace(buffer[offset]) != 0) != onspace) {
                  if (onspace) break;
                  onspace = 1;
                }
                ++offset;
              }
              if ((offset += p->winx) > scr.posx)
                p->winx = (p->winx + scr.posx) / 2;
              else
                p->winx = offset;
              continue;
            }
          }

          memcpy(brl.buffer, outputBuffer, outputLength);
          memset(brl.buffer+outputLength, 0, windowLength-outputLength);
          while (cursorOffset >= 0) {
            int offset = contractedOffsets[cursorOffset];
            if (offset >= 0) {
              cursorLocation = offset;
              break;
            }
            --cursorOffset;
          }
          contractedStart = p->winx;
          contractedLength = inputLength;
          contractedTrack = 0;
          contracted = 1;

          if (p->showAttributes || (prefs.showAttributes && (!prefs.blinkingAttributes || attributesState))) {
            int inputOffset;
            int outputOffset = 0;
            unsigned char attributes = 0;
            readScreen(contractedStart, p->winy, contractedLength, 1, inputBuffer, SCR_ATTRIB);
            for (inputOffset=0; inputOffset<contractedLength; ++inputOffset) {
              int offset = contractedOffsets[inputOffset];
              if (offset >= 0) {
                while (outputOffset < offset) outputBuffer[outputOffset++] = attributes;
                attributes = 0;
              }
              attributes |= inputBuffer[inputOffset];
            }
            while (outputOffset < outputLength) outputBuffer[outputOffset++] = attributes;
            if (p->showAttributes) {
              for (outputOffset=0; outputOffset<outputLength; ++outputOffset)
                brl.buffer[outputOffset] = attributesTable[outputBuffer[outputOffset]];
            } else {
              overlayAttributes(outputBuffer, outputLength, 1);
            }
          }

          break;
        }
      }
#endif /* ENABLE_CONTRACTED_BRAILLE */
      if (!contracted) {
        int winlen = MIN(brl.x, scr.cols-p->winx);
        readScreen(p->winx, p->winy, winlen, brl.y, brl.buffer,
                   p->showAttributes? SCR_ATTRIB: SCR_TEXT);
        if (braille->writeVisual) braille->writeVisual(&brl);

        /* blank out capital letters if they're blinking and should be off */
        if (prefs.blinkingCapitals && !capitalsState)
          for (i=0; i<winlen*brl.y; i++)
            if (BRL_ISUPPER(brl.buffer[i]))
              brl.buffer[i] = ' ';

        /* convert to dots using the current translation table */
        if ((curtbl == attributesTable) || !prefs.textStyle) {
          for (
            i = 0;
            i < (winlen * brl.y);
            brl.buffer[i] = curtbl[brl.buffer[i]], i++
          );
        } else {
          for (
            i = 0;
            i < (winlen * brl.y);
            brl.buffer[i] = curtbl[brl.buffer[i]] & (BRL_DOT1 | BRL_DOT2 | BRL_DOT3 | BRL_DOT4 | BRL_DOT5 | BRL_DOT6), i++
          );
        }

        if (winlen < brl.x) {
          /* We got a rectangular piece of text with readScreen but the display
             is in an off-right position with some cells at the end blank
             so we'll insert these cells and blank them. */
          for (i=brl.y-1; i>0; i--)
            memmove(brl.buffer+i*brl.x, brl.buffer+i*winlen, winlen);
          for (i=0; i<brl.y; i++)
            memset(brl.buffer+i*brl.x+winlen, 0, brl.x-winlen);
        }

        /* Attribute underlining: if viewing text (not attributes), attribute
           underlining is active and visible and we're not in help, then we
           get the attributes for the current region and OR the underline. */
        if (!p->showAttributes && prefs.showAttributes && (!prefs.blinkingAttributes || attributesState)) {
          unsigned char attrbuf[winlen*brl.y];
          readScreen(p->winx, p->winy, winlen, brl.y, attrbuf, SCR_ATTRIB);
          overlayAttributes(attrbuf, winlen, brl.y);
        }

        /*
         * If the cursor is visible and in range, and help is off: 
         */
        if ((scr.posx >= p->winx) && (scr.posx < (p->winx + brl.x)) &&
            (scr.posy >= p->winy) && (scr.posy < (p->winy + brl.y)))
          cursorLocation = (scr.posy - p->winy) * brl.x + scr.posx - p->winx;
      }
      if (cursorLocation >= 0) {
        if (prefs.showCursor && !p->hideCursor && (!prefs.blinkingCursor || cursorState)) {
          brl.buffer[cursorLocation] |= cursorDots();
        }
      }

      setStatusCells();
      braille->writeWindow(&brl);
    }

    drainBrailleOutput(&brl, updateInterval);
    updateIntervals++;
  }

  terminateProgram(0);
  return 0;
}

void 
message (const char *text, short flags) {
   int length = strlen(text);

#ifdef ENABLE_SPEECH_SUPPORT
   if (prefs.alertTunes && !(flags & MSG_SILENT)) {
      speech->mute();
      speech->say(text, length);
   }
#endif /* ENABLE_SPEECH_SUPPORT */

   if (braille && brl.buffer) {
      while (length) {
         int count;
         int index;

         /* strip leading spaces */
         while (*text == ' ')  text++, length--;

         if (length <= brl.x*brl.y) {
            count = length; /* the whole message fits on the braille window */
         } else {
            /* split the message across multiple windows on space characters */
            for (count=brl.x*brl.y-2; count>0 && text[count]!=' '; count--);
            if (!count) count = brl.x * brl.y - 1;
         }

         memset(brl.buffer, ' ', brl.x*brl.y);
         for (index=0; index<count; brl.buffer[index++]=*text++);
         if (length -= count) {
            while (index < brl.x*brl.y) brl.buffer[index++] = '-';
            brl.buffer[brl.x*brl.y - 1] = '>';
         }

         /*
          * Do Braille translation using text table. * Six-dot mode is
          * ignored, since case can be important, and * blinking caps won't 
          * work ... 
          */
         writeBrailleBuffer(&brl);

         if (flags & MSG_WAITKEY)
            getCommand(BRL_CTX_MESSAGE);
         else if (length || !(flags & MSG_NODELAY)) {
            int i;
            for (i=0; i<messageDelay; i+=updateInterval) {
               int command;
               drainBrailleOutput(&brl, updateInterval);
               while ((command = readCommand(BRL_CTX_MESSAGE)) == BRL_CMD_NOOP);
               if (command != EOF) break;
            }
         }
      }
   }
}

void
showDotPattern (unsigned char dots, unsigned char duration) {
  unsigned char status[BRL_MAX_STATUS_CELL_COUNT];        /* status cell buffer */
  memset(status, dots, sizeof(status));
  memset(brl.buffer, dots, brl.x*brl.y);
  braille->writeStatus(&brl, status);
  braille->writeWindow(&brl);
  drainBrailleOutput(&brl, duration);
}
