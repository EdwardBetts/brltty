/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2014 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

/* TSI/braille.c - Braille display driver for TSI displays
 *
 * Written by St�phane Doyon (s.doyon@videotron.ca)
 *
 * It attempts full support for Navigator 20/40/80 and Powerbraille 40/65/80.
 * It is designed to be compiled into BRLTTY version 3.5.
 *
 * History:
 * Version 2.74 apr2004: use message() to report low battery condition.
 * Version 2.73 jan2004: Fix key bindings for speech commands for PB80.
 *   Add CMD_SPKHOME to help.
 * Version 2.72 jan2003: brl->buffer now allocated by core.
 * Version 2.71: Added CMD_LEARN, BRL_CMD_NXPROMPT/CMD_PRPROMPT and CMD_SIXDOTS.
 * Version 2.70: Added CR_CUTAPPEND, BRL_BLK_CUTLINE, BRL_BLK_SETMARK, BRL_BLK_GOTOMARK
 *   and CR_SETLEFT. Changed binding for NXSEARCH.. Adjusted PB80 cut&paste
 *   bindings. Replaced CMD_CUT_BEG/CMD_CUT_END by CR_CUTBEGIN/CR_CUTRECT,
 *   and CMD_CSRJMP by CR_ROUTE+0. Adjusted cut_cursor for new cut&paste
 *   bindings (untested).
 * Version 2.61: Adjusted key bindings for preferences menu.
 * Version 2.60: Use TCSADRAIN when closing serial port. Slight API and
 *   name changes for BRLTTY 3.0. Argument to readbrl now ignore, instead
 *   of being validated. 
 * Version 2.59: Added bindings for CMD_LNBEG/LNEND.
 * Version 2.58: Added bindings for CMD_BACK and CR_MSGATTRIB.
 * Version 2.57: Fixed help screen/file for Nav80. We finally have a
 *   user who confirms it works!
 * Version 2.56: Added key binding for NXSEARCH.
 * Version 2.55: Added key binding for NXINDENT and NXBLNKLNS.
 * Version 2.54: Added key binding for switchvt.
 * Version 2.53: The IXOFF bit in the termios setting was inverted?
 * Version 2.52: Changed LOG_NOTICE to LOG_INFO. Was too noisy.
 * Version 2.51: Added CMD_RESTARTSPEECH.
 * Version 2.5: Added CMD_SPKHOME, sacrificed LNBEG and LNEND.
 * Version 2.4: Refresh display even if unchanged after every now and then so
 *   that it will clear up if it was garbled. Added speech key bindings (had
 *   to change a few bindings to make room). Added SKPEOLBLNK key binding.
 * Version 2.3: Reset serial port attributes at each detection attempt in
 *   initbrl. This should help BRLTTY recover if another application (such
 *   as kudzu) scrambles the serial port while BRLTTY is running.
 * Unnumbered version: Fixes for dynmically loading drivers (declare all
 *   non-exported functions and variables static).
 * Version 2.2beta3: Option to disable CTS checking. Apparently, Vario
 *   does not raise CTS when connected.
 * Version 2.2beta1: Exploring problems with emulators of TSI (PB40): BAUM
 *   and mdv mb408s. See if we can provide timing options for more flexibility.
 * Version 2.1: Help screen fix for new keys in preferences menu.
 * Version 2.1beta1: Less delays in writing braille to display for
 *   nav20/40 and pb40, delays still necessary for pb80 on probably for nav80.
 *   Additional routing keys for navigator. Cut&paste binding that combines
 *   routing key and normal key.
 * Version 2.0: Tested with Nav40 PB40 PB80. Support for functions added
 *   in BRLTTY 2.0: added key bindings for new fonctions (attributes and
 *   routing). Support for PB at 19200baud. Live detection of display, checks
 *   both at 9600 and 19200baud. RS232 wire monitoring. Ping when idle to 
 *   detect when display turned off and issue a CMD_RESTARTBRL.
 * Version 1.2 (not released) introduces support for PB65/80. Rework of key
 *   binding mechanism and readbrl(). Slight modifications to routing keys
 *   support, + corrections. May have broken routing key support for PB40.
 * Version 1.1 worked on nav40 and was reported to work on pb40.
 */

#include "prologue.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "parse.h"
#include "io_generic.h"
#include "async_wait.h"
#include "message.h"

typedef enum {
  PARM_HIGHBAUD
} DriverParameter;
#define BRLPARMS "highbaud"

#include "brl_driver.h"
#include "braille.h"
#include "brldefs-ts.h"

BEGIN_KEY_NAME_TABLE(routing)
  KEY_GROUP_ENTRY(TS_GRP_RoutingKeys, "RoutingKey"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLE(nav_small)
  KEY_NAME_ENTRY(TS_KEY_CursorLeft, "CursorLeft"),
  KEY_NAME_ENTRY(TS_KEY_CursorRight, "CursorRight"),
  KEY_NAME_ENTRY(TS_KEY_CursorUp, "CursorUp"),
  KEY_NAME_ENTRY(TS_KEY_CursorDown, "CursorDown"),

  KEY_NAME_ENTRY(TS_KEY_NavLeft, "NavLeft"),
  KEY_NAME_ENTRY(TS_KEY_NavRight, "NavRight"),
  KEY_NAME_ENTRY(TS_KEY_NavUp, "NavUp"),
  KEY_NAME_ENTRY(TS_KEY_NavDown, "NavDown"),

  KEY_NAME_ENTRY(TS_KEY_ThumbLeft, "ThumbLeft"),
  KEY_NAME_ENTRY(TS_KEY_ThumbRight, "ThumbRight"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLE(nav_large)
  KEY_NAME_ENTRY(TS_KEY_CursorLeft, "CursorLeft"),
  KEY_NAME_ENTRY(TS_KEY_CursorRight, "CursorRight"),
  KEY_NAME_ENTRY(TS_KEY_CursorUp, "CursorUp"),
  KEY_NAME_ENTRY(TS_KEY_CursorDown, "CursorDown"),

  KEY_NAME_ENTRY(TS_KEY_NavLeft, "LeftOuter"),
  KEY_NAME_ENTRY(TS_KEY_NavRight, "RightOuter"),
  KEY_NAME_ENTRY(TS_KEY_NavUp, "LeftInner"),
  KEY_NAME_ENTRY(TS_KEY_NavDown, "RightInner"),

  KEY_NAME_ENTRY(TS_KEY_ThumbLeft, "LeftThumb"),
  KEY_NAME_ENTRY(TS_KEY_ThumbRight, "RightThumb"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLE(pb_small)
  KEY_NAME_ENTRY(TS_KEY_CursorUp, "LeftRockerUp"),
  KEY_NAME_ENTRY(TS_KEY_CursorDown, "LeftRockerDown"),

  KEY_NAME_ENTRY(TS_KEY_NavLeft, "Backward"),
  KEY_NAME_ENTRY(TS_KEY_NavRight, "Forward"),
  KEY_NAME_ENTRY(TS_KEY_NavUp, "RightRockerUp"),
  KEY_NAME_ENTRY(TS_KEY_NavDown, "RightRockerDown"),

  KEY_NAME_ENTRY(TS_KEY_ThumbLeft, "Convex"),
  KEY_NAME_ENTRY(TS_KEY_ThumbRight, "Concave"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLE(pb_large)
  KEY_NAME_ENTRY(TS_KEY_Button1, "Button1"),
  KEY_NAME_ENTRY(TS_KEY_Button2, "Button2"),
  KEY_NAME_ENTRY(TS_KEY_Button3, "Button3"),
  KEY_NAME_ENTRY(TS_KEY_Button4, "Button4"),

  KEY_NAME_ENTRY(TS_KEY_Bar1, "Bar1"),
  KEY_NAME_ENTRY(TS_KEY_Bar2, "Bar2"),
  KEY_NAME_ENTRY(TS_KEY_Bar3, "Bar3"),
  KEY_NAME_ENTRY(TS_KEY_Bar4, "Bar4"),

  KEY_NAME_ENTRY(TS_KEY_Switch1Up, "Switch1Up"),
  KEY_NAME_ENTRY(TS_KEY_Switch1Down, "Switch1Down"),
  KEY_NAME_ENTRY(TS_KEY_Switch2Up, "Switch2Up"),
  KEY_NAME_ENTRY(TS_KEY_Switch2Down, "Switch2Down"),
  KEY_NAME_ENTRY(TS_KEY_Switch3Up, "Switch3Up"),
  KEY_NAME_ENTRY(TS_KEY_Switch3Down, "Switch3Down"),
  KEY_NAME_ENTRY(TS_KEY_Switch4Up, "Switch4Up"),
  KEY_NAME_ENTRY(TS_KEY_Switch4Down, "Switch4Down"),

  KEY_NAME_ENTRY(TS_KEY_LeftRockerUp, "LeftRockerUp"),
  KEY_NAME_ENTRY(TS_KEY_LeftRockerDown, "LeftRockerDown"),
  KEY_NAME_ENTRY(TS_KEY_RightRockerUp, "RightRockerUp"),
  KEY_NAME_ENTRY(TS_KEY_RightRockerDown, "RightRockerDown"),

  KEY_NAME_ENTRY(TS_KEY_Convex, "Convex"),
  KEY_NAME_ENTRY(TS_KEY_Concave, "Concave"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLES(nav20)
  KEY_NAME_TABLE(nav_small),
END_KEY_NAME_TABLES

BEGIN_KEY_NAME_TABLES(nav40)
  KEY_NAME_TABLE(nav_small),
END_KEY_NAME_TABLES

BEGIN_KEY_NAME_TABLES(nav80)
  KEY_NAME_TABLE(nav_large),
  KEY_NAME_TABLE(routing),
END_KEY_NAME_TABLES

BEGIN_KEY_NAME_TABLES(pb40)
  KEY_NAME_TABLE(pb_small),
  KEY_NAME_TABLE(routing),
END_KEY_NAME_TABLES

BEGIN_KEY_NAME_TABLES(pb65)
  KEY_NAME_TABLE(pb_large),
  KEY_NAME_TABLE(routing),
END_KEY_NAME_TABLES

BEGIN_KEY_NAME_TABLES(pb80)
  KEY_NAME_TABLE(pb_large),
  KEY_NAME_TABLE(routing),
END_KEY_NAME_TABLES

DEFINE_KEY_TABLE(nav20)
DEFINE_KEY_TABLE(nav40)
DEFINE_KEY_TABLE(nav80)
DEFINE_KEY_TABLE(pb40)
DEFINE_KEY_TABLE(pb65)
DEFINE_KEY_TABLE(pb80)

BEGIN_KEY_TABLE_LIST
  &KEY_TABLE_DEFINITION(nav20),
  &KEY_TABLE_DEFINITION(nav40),
  &KEY_TABLE_DEFINITION(nav80),
  &KEY_TABLE_DEFINITION(pb40),
  &KEY_TABLE_DEFINITION(pb65),
  &KEY_TABLE_DEFINITION(pb80),
END_KEY_TABLE_LIST

/* Braille display parameters that do not change */
#define BRLROWS 1		/* only one row on braille display */

/* Type of delay the display requires after sending it a command.
   0 -> no delay, 1 -> drain only, 2 -> drain + wait for SEND_DELAY. */
static unsigned char slowUpdate;

/* Whether multiple packets can be sent for a single update. */
static unsigned char noMultipleUpdates;

/* We periodicaly refresh the display even if nothing has changed, will clear
   out any garble... */
#define FULL_FRESHEN_EVERY 12 /* do a full update every nth writeWindow(). This
				 should be a little over every 0.5secs. */
static int fullFreshenEvery;

/* for routing keys */
#define ROUTING_BYTES_VERTICAL 4
#define ROUTING_BYTES_MAXIMUM 11
#define ROUTING_BYTES_40 9
#define ROUTING_BYTES_80 14
#define ROUTING_BYTES_81 15

static unsigned char routingKeys[ROUTING_BYTES_MAXIMUM];

/* Stabilization delay after changing baud rate */
#define BAUD_DELAY (100)

/* Normal header for sending dots, with cursor always off */
static unsigned char BRL_SEND_HEAD[] = {0XFF, 0XFF, 0X04, 0X00, 0X99, 0X00};
#define DIM_BRL_SEND_FIXED 6
#define DIM_BRL_SEND 8
/* Two extra bytes for lenght and offset */
#define BRL_SEND_LENGTH 6
#define BRL_SEND_OFFSET 7

/* Description of reply to query */
#define IDENTITY_H1 0X00
#define IDENTITY_H2 0X05

/* Bit definition of key codes returned by the display */
/* Navigator and pb40 return 2bytes, pb65/80 returns 6. Each byte has a
   different specific mask/signature in the 3 most significant bits.
   Other bits indicate whether a specific key is pressed.
   See readbrl(). */

/* We combine all key bits into one 32bit int. Each byte is masked by the
 * corresponding "mask" to extract valid bits then those are shifted by
 * "shift" and or'ed into the 32bits "code".
 */

/* bits to take into account when checking each byte's signature */
#define KEYS_BYTE_SIGNATURE_MASK 0XE0

/* how we describe each byte */
typedef struct {
  unsigned char signature; /* it's signature */
  unsigned char mask; /* bits that do represent keys */
  unsigned char shift; /* where to shift them into "code" */
} KeysByteDescriptor;

/* Description of bytes for navigator and pb40. */
static const KeysByteDescriptor keysDescriptor_Navigator[] = {
  {.signature=0X60, .mask=0X1F, .shift=0},
  {.signature=0XE0, .mask=0X1F, .shift=5}
};

/* Description of bytes for pb65/80 */
static const KeysByteDescriptor keysDescriptor_PowerBraille[] = {
  {.signature=0X40, .mask=0X0F, .shift=10},
  {.signature=0XC0, .mask=0X0F, .shift=14},
  {.signature=0X20, .mask=0X05, .shift=18},
  {.signature=0XA0, .mask=0X05, .shift=21},
  {.signature=0X60, .mask=0X1F, .shift=24},
  {.signature=0XE0, .mask=0X1F, .shift=5}
};

/* Symbolic labels for keys
   Each key has it's own bit in "code". Key combinations are ORs. */

/* For navigator and pb40 */
/* bits from byte 1: navigator right pannel keys, pb right rocker +round button
   + display forward/backward controls on the top of the display */
#define KEY_BLEFT  (1<<0)
#define KEY_BUP	   (1<<1)
#define KEY_BRIGHT (1<<2)
#define KEY_BDOWN  (1<<3)
#define KEY_BROUND (1<<4)
/* bits from byte 2: navigator's left pannel; pb's left rocker and round
   button; pb cannot produce CLEFT and CRIGHT. */
#define KEY_CLEFT  (1<<5)
#define KEY_CUP	   (1<<6)
#define KEY_CRIGHT (1<<7)
#define KEY_CDOWN  (1<<8)
#define KEY_CROUND (1<<9)

/* For pb65/80 */
/* Bits from byte 5, could be just renames of byte 1 from navigator, but
   we want to distinguish BAR1-2 from BUP/BDOWN. */
#define KEY_BAR1   (1<<24)
#define KEY_R2UP   (1<<25)
#define KEY_BAR2   (1<<26)
#define KEY_R2DN   (1<<27)
#define KEY_CNCV   (1<<28)
/* Bits from byte 6, are just renames of byte 2 from navigator */
#define KEY_BUT1   (1<<5)
#define KEY_R1UP   (1<<6)
#define KEY_BUT2   (1<<7)
#define KEY_R1DN   (1<<8)
#define KEY_CNVX   (1<<9)
/* bits from byte 1: left rocker switches */
#define KEY_S1UP   (1<<10)
#define KEY_S1DN   (1<<11)
#define KEY_S2UP   (1<<12)
#define KEY_S2DN   (1<<13)
/* bits from byte 2: right rocker switches */
#define KEY_S3UP   (1<<14)
#define KEY_S3DN   (1<<15)
#define KEY_S4UP   (1<<16)
#define KEY_S4DN   (1<<17)
/* Special mask: switches are special keys to distinguish... */
#define KEY_SWITCHMASK (KEY_S1UP|KEY_S1DN | KEY_S2UP|KEY_S2DN \
			| KEY_S3UP|KEY_S3DN | KEY_S4UP|KEY_S4DN)
/* bits from byte 3: rightmost forward bars from display top */
#define KEY_BAR3   (1<<18)
  /* one unused bit */
#define KEY_BAR4   (1<<20)
/* bits from byte 4: two buttons on the top, right side (left side buttons
   are mapped in byte 6) */
#define KEY_BUT3   (1<<21)
  /* one unused bit */
#define KEY_BUT4   (1<<23)

/* Some special case input codes */
/* input codes signaling low battery power (2bytes) */
#define BATTERY_H1 0x00
#define BATTERY_H2 0x01
/* Sensor switches/cursor routing keys information (2bytes header) */
#define ROUTING_H1 0x00
#define ROUTING_H2 0x08

/* Global variables */

typedef struct {
  const char *modelName;
  const KeyTableDefinition *keyTableDefinition;

  unsigned char routingBytes;
  signed char routingKeyCount;

  unsigned slowUpdate:2;
  unsigned highBaudSupported:1;
  unsigned isPB40:1;
} ModelEntry;

static const ModelEntry modelNavigator20 = {
  .modelName = "Navigator 20",

  .routingBytes = ROUTING_BYTES_40,
  .routingKeyCount = 20,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(nav20)
};

static const ModelEntry modelNavigator40 = {
  .modelName = "Navigator 40",

  .routingBytes = ROUTING_BYTES_40,
  .routingKeyCount = 40,

  .slowUpdate = 1,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(nav40)
};

static const ModelEntry modelNavigator80 = {
  .modelName = "Navigator 80",

  .routingBytes = ROUTING_BYTES_80,
  .routingKeyCount = 80,

  .slowUpdate = 2,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(nav80)
};

static const ModelEntry modelPowerBraille40 = {
  .modelName = "Power Braille 40",

  .routingBytes = ROUTING_BYTES_40,
  .routingKeyCount = 40,

  .highBaudSupported = 1,
  .isPB40 = 1,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(pb40)
};

static const ModelEntry modelPowerBraille65 = {
  .modelName = "Power Braille 65",

  .routingBytes = ROUTING_BYTES_81,
  .routingKeyCount = 65,

  .slowUpdate = 2,
  .highBaudSupported = 1,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(pb65)
};

static const ModelEntry modelPowerBraille80 = {
  .modelName = "Power Braille 80",

  .routingBytes = ROUTING_BYTES_81,
  .routingKeyCount = 81,

  .slowUpdate = 2,
  .highBaudSupported = 1,

  .keyTableDefinition = &KEY_TABLE_DEFINITION(pb80)
};

typedef enum {
  IPT_IDENTITY,
  IPT_ROUTING,
  IPT_BATTERY,
  IPT_KEYS
} InputPacketType;

typedef struct {
  union {
    unsigned char bytes[1];

    struct {
      unsigned char header[2];
      unsigned char columns;
      unsigned char dots;
      char version[4];
      unsigned char checksum[4];
    } identity;

    struct {
      unsigned char header[2];
      unsigned char count;
      unsigned char vertical[ROUTING_BYTES_VERTICAL];
      unsigned char horizontal[0X100 - 4];
    } routing;

    unsigned char keys[6];
  } fields;

  InputPacketType type;

  union {
    struct {
      unsigned char count;
    } routing;

    struct {
      const KeysByteDescriptor *descriptor;
      unsigned char count;
    } keys;
  } data;
} InputPacket;

static SerialParameters serialParameters = {
  SERIAL_DEFAULT_PARAMETERS
};

#define LOW_BAUD     4800
#define NORMAL_BAUD  9600
#define HIGH_BAUD   19200

static const ModelEntry *model;

static unsigned char *rawdata,	/* translated data to send to display */
                     *prevdata, /* previous data sent */
                     *dispbuf;
static unsigned char brl_cols;		/* Number of cells available for text */
static int ncells;              /* Total number of cells on display: this is
				   brl_cols cells + 1 status cell on PB80. */
static char hardwareVersion[3]; /* version of the hardware */

static ssize_t
writeBytes (BrailleDisplay *brl, const void *data, size_t size) {
  brl->writeDelay += slowUpdate * 24;
  return writeBraillePacket(brl, NULL, data, size);
}

static BraillePacketVerifierResult
verifyPacket1 (
  BrailleDisplay *brl,
  const unsigned char *bytes, size_t size,
  size_t *length, void *data
) {
  InputPacket *packet = data;
  const off_t index = size - 1;
  const unsigned char byte = bytes[index];

  if (size == 1) {
    switch (byte) {
      case IDENTITY_H1:
        packet->type = IPT_IDENTITY;
        *length = 2;
        break;

      default:
        if ((byte & KEYS_BYTE_SIGNATURE_MASK) == keysDescriptor_Navigator[0].signature) {
          packet->data.keys.descriptor = keysDescriptor_Navigator;
          packet->data.keys.count = ARRAY_COUNT(keysDescriptor_Navigator);
          goto isKeys;
        }

        if ((byte & KEYS_BYTE_SIGNATURE_MASK) == keysDescriptor_PowerBraille[0].signature) {
          packet->data.keys.descriptor = keysDescriptor_PowerBraille;
          packet->data.keys.count = ARRAY_COUNT(keysDescriptor_PowerBraille);
          goto isKeys;
        }

        return BRL_PVR_INVALID;

      isKeys:
        packet->type = IPT_KEYS;
        *length = packet->data.keys.count;
        break;
    }
  } else {
    switch (packet->type) {
      case IPT_IDENTITY:
        if (size == 2) {
          switch (byte) {
            case IDENTITY_H2:
              *length = sizeof(packet->fields.identity);
              break;

            case ROUTING_H2:
              packet->type = IPT_ROUTING;
              *length = 3;
              break;

            case BATTERY_H2:
              packet->type = IPT_BATTERY;
              break;

            default:
              return BRL_PVR_INVALID;
          }
        }
        break;

      case IPT_ROUTING:
        if (size == 3) {
          packet->data.routing.count = byte;
          *length += packet->data.routing.count;
        }
        break;

      case IPT_KEYS:
        if ((byte & KEYS_BYTE_SIGNATURE_MASK) != packet->data.keys.descriptor[index].signature) return BRL_PVR_INVALID;
        break;

      default:
        break;
    }
  }

  return BRL_PVR_INCLUDE;
}

static size_t
readPacket (BrailleDisplay *brl, InputPacket *packet) {
  return readBraillePacket(brl, NULL, &packet->fields, sizeof(packet->fields), verifyPacket1, packet);
}

static int
queryDisplay (BrailleDisplay *brl, InputPacket *reply) {
  static const unsigned char request[] = {0xFF, 0xFF, 0x0A};

  if (writeBytes(brl, request, sizeof(request))) {
    if (gioAwaitInput(brl->gioEndpoint, 100)) {
      size_t count = readPacket(brl, reply);

      if (count > 0) {
        if (reply->type == IPT_IDENTITY) return 1;
        logUnexpectedPacket(reply->fields.bytes, count);
      }
    } else {
      logMessage(LOG_DEBUG, "no response");
    }
  }

  return 0;
}


static int
resetTypematic (BrailleDisplay *brl) {
  static const unsigned char request[] = {
    0XFF, 0XFF, 0X0D, BRL_TYPEMATIC_DELAY, BRL_TYPEMATIC_REPEAT
  };

  return writeBytes(brl, request, sizeof(request));
}

static int
setBaud (BrailleDisplay *brl, int baud) {
  logMessage(LOG_DEBUG, "trying with %d baud", baud);
  serialParameters.baud = baud;
  return gioReconfigureResource(brl->gioEndpoint, &serialParameters);
}

static int
changeBaud (BrailleDisplay *brl, int baud) {
  unsigned char request[] = {0xFF, 0xFF, 0x05, 0};
  unsigned char *byte = &request[sizeof(request) - 1];

  switch (baud) {
    case LOW_BAUD:
      *byte = 2;
      break;

    case NORMAL_BAUD:
      *byte = 3;
      break;

    case HIGH_BAUD:
      *byte = 4;
      break;

    default:
      logMessage(LOG_WARNING, "display does not support %d baud", baud);
      return 0;
  }

  logMessage(LOG_WARNING, "changing display to %d baud", baud);
  return writeBraillePacket(brl, NULL, request, sizeof(request));
}

static int
connectResource (BrailleDisplay *brl, const char *identifier) {
  GioDescriptor descriptor;
  gioInitializeDescriptor(&descriptor);

  descriptor.serial.parameters = &serialParameters;

  if (connectBrailleResource(brl, identifier, &descriptor, NULL)) {
    return 1;
  }

  return 0;
}


static int
brl_construct (BrailleDisplay *brl, char **parameters, const char *device) {
  int i=0;
  InputPacket reply;
  unsigned int allowHighBaud = 1;

  {
    const char *parameter = parameters[PARM_HIGHBAUD];

    if (parameter && *parameter) {
      if (!validateYesNo(&allowHighBaud, parameter)) {
        logMessage(LOG_WARNING, "unsupported high baud setting: %s", parameter);
      }
    }
  }

  dispbuf = rawdata = prevdata = NULL;

  if (!connectResource(brl, device)) goto failure;
  if (!setBaud(brl, NORMAL_BAUD)) goto failure;

  if (!queryDisplay(brl, &reply)) {
    /* Then send the query at 19200 baud, in case a PB was left ON
     * at that speed
     */

    if (!allowHighBaud) goto failure;
    if (!setBaud(brl, HIGH_BAUD)) goto failure;
    if (!queryDisplay(brl, &reply)) goto failure;
  }

  memcpy(hardwareVersion, &reply.fields.identity.version[1], sizeof(hardwareVersion));
  ncells = reply.fields.identity.columns;
  brl_cols = ncells;
  logMessage(LOG_INFO, "display replied: %d cells, version %.*s",
             ncells, (int)sizeof(hardwareVersion), hardwareVersion);

  switch (brl_cols) {
    case 20:
      model = &modelNavigator20;
      break;

    case 40:
      model = (hardwareVersion[0] > '3')? &modelPowerBraille40: &modelNavigator40;
      break;

    case 80:
      model = &modelNavigator80;
      break;

    case 65:
      model = &modelPowerBraille65;
      break;

    case 81:
      model = &modelPowerBraille80;
      break;

    default:
      logMessage(LOG_ERR, "unrecognized braille display size: %u", brl_cols);
      goto failure;
  }

  logMessage(LOG_INFO, "detected %s", model->modelName);

  {
    const KeyTableDefinition *ktd = model->keyTableDefinition;

    brl->keyBindings = ktd->bindings;
    brl->keyNames = ktd->names;
  }

  slowUpdate = model->slowUpdate;
  noMultipleUpdates = 0;

#ifdef FORCE_DRAIN_AFTER_SEND
  slowUpdate = 1;
#endif /* FORCE_DRAIN_AFTER_SEND */

#ifdef FORCE_FULL_SEND_DELAY
  slowUpdate = 2;
#endif /* FORCE_FULL_SEND_DELAY */

#ifdef NO_MULTIPLE_UPDATES
  noMultipleUpdates = 1;
#endif /* NO_MULTIPLE_UPDATES */

  if (slowUpdate == 2) noMultipleUpdates = 1;
  fullFreshenEvery = FULL_FRESHEN_EVERY;

  if ((serialParameters.baud < HIGH_BAUD) && allowHighBaud && model->highBaudSupported) {
    /* if supported (PB) go to 19200 baud */
    if (!changeBaud(brl, HIGH_BAUD)) goto failure;
  //serialAwaitOutput(brl->gioEndpoint);
    asyncWait(BAUD_DELAY);
    if (!setBaud(brl, HIGH_BAUD)) goto failure;
    logMessage(LOG_DEBUG, "switched to %d baud - checking if display followed", HIGH_BAUD);

    if (queryDisplay(brl, &reply)) {
      logMessage(LOG_DEBUG, "display responded at %d baud", HIGH_BAUD);
    } else {
      logMessage(LOG_INFO,
                 "display did not respond at %d baud"
	         " - falling back to %d baud", NORMAL_BAUD,
                 HIGH_BAUD);

      if (!setBaud(brl, NORMAL_BAUD)) goto failure;
    //serialAwaitOutput(brl->gioEndpoint);
      asyncWait(BAUD_DELAY); /* just to be safe */

      if (queryDisplay(brl, &reply)) {
	logMessage(LOG_INFO,
                   "found display again at %d baud"
                   " - must be a TSI emulator",
                   NORMAL_BAUD);

        fullFreshenEvery = 1;
      } else {
	logMessage(LOG_ERR, "display lost after baud switch");
	goto failure;
      }
    }
  }

  memset(routingKeys, 0, sizeof(routingKeys));
  resetTypematic(brl);

  brl->textColumns = brl_cols;		/* initialise size of display */
  brl->textRows = BRLROWS;		/* always 1 */

  makeOutputTable(dotsTable_ISO11548_1);

  /* Allocate space for buffers */
  dispbuf = malloc(ncells);
  prevdata = malloc(ncells);
  rawdata = malloc(2 * ncells + DIM_BRL_SEND);
  /* 2* to insert 0s for attribute code when sending to the display */
  if (!dispbuf || !prevdata || !rawdata)
    goto failure;

  /* Initialize rawdata. It will be filled in and used directly to
     write to the display in writebrl(). */
  for (i = 0; i < DIM_BRL_SEND_FIXED; i++)
    rawdata[i] = BRL_SEND_HEAD[i];
  memset (rawdata + DIM_BRL_SEND, 0, 2 * ncells * BRLROWS);

  /* Force rewrite of display on first writebrl */
  memset(prevdata, 0xFF, ncells);

  return 1;

failure:
  brl_destruct(brl);
  return 0;
}


static void 
brl_destruct (BrailleDisplay *brl) {
  disconnectBrailleResource(brl, NULL);

  if (dispbuf) {
    free(dispbuf);
    dispbuf = NULL;
  }

  if (rawdata) {
    free(rawdata);
    rawdata = NULL;
  }

  if (prevdata) {
    free(prevdata);
    prevdata = NULL;
  }
}

static void 
display (BrailleDisplay *brl, 
         const unsigned char *pattern, 
	 unsigned int from, unsigned int to)
/* display a given dot pattern. We process only part of the pattern, from
   byte (cell) start to byte stop. That pattern should be shown at position 
   start on the display. */
{
  int i, length;

  /* Assumes BRLROWS == 1 */
  length = to - from;

  rawdata[BRL_SEND_LENGTH] = 2 * length;
  rawdata[BRL_SEND_OFFSET] = from;

  for (i = 0; i < length; i++)
    rawdata[DIM_BRL_SEND + 2 * i + 1] = translateOutputCell(pattern[from + i]);

  /* Some displays apparently don't like rapid updating. Most or all apprently
     don't do flow control. If we update the display too often and too fast,
     then the packets queue up in the send queue, the info displayed is not up
     to date, and the info displayed continues to change after we stop
     updating while the queue empties (like when you release the arrow key and
     the display continues changing for a second or two). We also risk
     overflows which put garbage on the display, or often what happens is that
     some cells from previously displayed lines will remain and not be cleared
     or replaced; also the pinging fails and the display gets
     reinitialized... To expose the problem skim/scroll through a long file
     (with long lines) holding down the up/down arrow key on the PC keyboard.

     pb40 has no problems: it apparently can take whatever we throw at
     it. Nav40 is good but we drain just to be safe.

     pb80 (twice larger but twice as fast as nav40) cannot take a continuous
     full speed flow. There is no flow control: apparently not supported
     properly on at least pb80. My pb80 is recent yet the hardware version is
     v1.0a, so this may be a hardware problem that was fixed on pb40.  There's
     some UART handshake mode that might be relevant but only seems to break
     everything (on both pb40 and pb80)...

     Nav80 is untested but as it receives at 9600, we probably need to
     compensate there too.

     Finally, some TSI emulators (at least the mdv mb408s) may have timing
     limitations.

     I no longer have access to a Nav40 and PB80 for testing: I only have a
     PB40.  */

  {
    int count = DIM_BRL_SEND + 2 * length;
    writeBytes(brl, rawdata, count);
  }
}

static void 
display_all (BrailleDisplay *brl,
             unsigned char *pattern)
{
  display (brl, pattern, 0, ncells);
}


static int 
brl_writeWindow (BrailleDisplay *brl, const wchar_t *text)
{
  static int count = 0;

  /* assert: brl->textColumns == brl_cols */

  memcpy(dispbuf, brl->buffer, brl_cols);

  if (--count<=0) {
    /* Force an update of the whole display every now and then to clear any
       garble. */
    count = fullFreshenEvery;
    memcpy(prevdata, dispbuf, ncells);
    display_all (brl, dispbuf);
  } else if (noMultipleUpdates) {
    unsigned int from, to;
    
    if (cellsHaveChanged(prevdata, dispbuf, ncells, &from, &to, NULL)) {
      display (brl, dispbuf, from, to);
    }
  }else{
    int base = 0, i = 0, collecting = 0, simil = 0;
    
    while (i < ncells)
      if (dispbuf[i] == prevdata[i])
	{
	  simil++;
	  if (collecting && 2 * simil > DIM_BRL_SEND)
	    {
	      display (brl, dispbuf, base, i - simil + 1);
	      base = i;
	      collecting = 0;
	      simil = 0;
	    }
	  if (!collecting)
	    base++;
	  i++;
	}
      else
	{
	  prevdata[i] = dispbuf[i];
	  collecting = 1;
	  simil = 0;
	  i++;
	}
    
    if (collecting)
      display (brl, dispbuf, base, i - simil );
  }
return 1;
}

static int
handleInputPacket (BrailleDisplay *brl, const InputPacket *packet) {
  switch (packet->type) {
    case IPT_KEYS: {
      KeyNumberSet keys = 0;
      unsigned int i;

      for (i=0; i<packet->data.keys.count; i+=1) {
        const KeysByteDescriptor *kbd = &packet->data.keys.descriptor[i];

        keys |= (packet->fields.keys[i] & kbd->mask) << kbd->shift;
      }

      enqueueKeys(brl, keys, TS_GRP_NavigationKeys, 0);
      break;
    }

    case IPT_ROUTING: {
      if (packet->data.routing.count != model->routingBytes) return 0;
      enqueueUpdatedKeyGroup(brl,
                             packet->fields.routing.horizontal, routingKeys,
                             model->routingKeyCount, TS_GRP_RoutingKeys);
      break;
    }

    case IPT_BATTERY:
      message(NULL, gettext("battery low"), MSG_WAITKEY);
      return 1;

    default:
      return 0;
  }

  return 1;
}

static int 
brl_readCommand (BrailleDisplay *brl, KeyTableCommandContext context) {
  /* Key press codes come in pairs of bytes for nav and pb40, in 6bytes
   * for pb65/80. Each byte has bits representing individual keys + a special
   * mask/signature in the most significant 3bits.
   *
   * The low battery warning from the display is a specific 2bytes code.
   *
   * Finally, the routing keys have a special 2bytes header followed by 9, 14
   * or 15 bytes of info (1bit for each routing key). The first 4bytes describe
   * vertical routing keys and are ignored in this driver.
   *
   * We might get a query reply, since we send queries when we don't get
   * any keys in a certain time. That a 2byte header + 10 more bytes ignored.
   */

  InputPacket packet;
  size_t size;

  while ((size = readPacket(brl, &packet))) {
    if (!handleInputPacket(brl, &packet)) {
      logUnexpectedPacket(packet.fields.bytes, size);
    }
  }

  return (errno == EAGAIN)? EOF: BRL_CMD_RESTARTBRL;
}
