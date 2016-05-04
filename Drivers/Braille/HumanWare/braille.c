/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2016 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://brltty.com/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <string.h>
#include <errno.h>

#include "log.h"
#include "ascii.h"

#include "brl_driver.h"
#include "brldefs-hw.h"

BEGIN_KEY_NAME_TABLE(all)
  KEY_NAME_ENTRY(HW_KEY_Power, "Power"),

  KEY_NAME_ENTRY(HW_KEY_Dot1, "Dot1"),
  KEY_NAME_ENTRY(HW_KEY_Dot2, "Dot2"),
  KEY_NAME_ENTRY(HW_KEY_Dot3, "Dot3"),
  KEY_NAME_ENTRY(HW_KEY_Dot4, "Dot4"),
  KEY_NAME_ENTRY(HW_KEY_Dot5, "Dot5"),
  KEY_NAME_ENTRY(HW_KEY_Dot6, "Dot6"),
  KEY_NAME_ENTRY(HW_KEY_Dot7, "Dot7"),
  KEY_NAME_ENTRY(HW_KEY_Dot8, "Dot8"),
  KEY_NAME_ENTRY(HW_KEY_Space, "Space"),

  KEY_NAME_ENTRY(HW_KEY_Nav1, "Display1"),
  KEY_NAME_ENTRY(HW_KEY_Nav2, "Display2"),
  KEY_NAME_ENTRY(HW_KEY_Nav3, "Display3"),
  KEY_NAME_ENTRY(HW_KEY_Nav4, "Display4"),
  KEY_NAME_ENTRY(HW_KEY_Nav5, "Display5"),
  KEY_NAME_ENTRY(HW_KEY_Nav6, "Display6"),

  KEY_NAME_ENTRY(HW_KEY_Thumb1, "Thumb1"),
  KEY_NAME_ENTRY(HW_KEY_Thumb2, "Thumb2"),
  KEY_NAME_ENTRY(HW_KEY_Thumb3, "Thumb3"),
  KEY_NAME_ENTRY(HW_KEY_Thumb4, "Thumb4"),

  KEY_GROUP_ENTRY(HW_GRP_RoutingKeys, "RoutingKey"),
END_KEY_NAME_TABLE

BEGIN_KEY_NAME_TABLES(all)
  KEY_NAME_TABLE(all),
END_KEY_NAME_TABLES

DEFINE_KEY_TABLE(all)

BEGIN_KEY_TABLE_LIST
  &KEY_TABLE_DEFINITION(all),
END_KEY_TABLE_LIST

typedef struct {
  const char *name;
  int (*probeDisplay) (BrailleDisplay *brl);
  int (*writeCells) (BrailleDisplay *brl, const unsigned char *cells, unsigned char count);
  int (*handleKeys) (BrailleDisplay *brl);
} ProtocolEntry;

struct BrailleDataStruct {
  const ProtocolEntry *protocol;

  struct {
    unsigned char rewrite;
    unsigned char cells[0XFF];
  } text;
};

static BraillePacketVerifierResult
verifyPacket (
  BrailleDisplay *brl,
  const unsigned char *bytes, size_t size,
  size_t *length, void *data
) {
  unsigned char byte = bytes[size-1];

  switch (size) {
    case 1:
      if (byte != ESC) return BRL_PVR_INVALID;
      *length = 3;
      break;

    case 3:
      *length += byte;
      break;

    default:
      break;
  }

  return BRL_PVR_INCLUDE;
}

static int
writePacket (BrailleDisplay *brl, unsigned char type, unsigned char length, const void *data) {
  HW_Packet packet;

  packet.fields.header = ESC;
  packet.fields.type = type;
  packet.fields.length = length;

  if (data) memcpy(packet.fields.data.bytes, data, length);
  length += packet.fields.data.bytes - packet.bytes;

  return writeBraillePacket(brl, NULL, &packet, length);
}

static int
writeIdentifyRequest (BrailleDisplay *brl) {
  return writePacket(brl, HW_MSG_INIT, 0, NULL);
}

static size_t
readResponse (BrailleDisplay *brl, void *packet, size_t size) {
  return readBraillePacket(brl, NULL, packet, size, verifyPacket, NULL);
}

static BrailleResponseResult
isIdentityResponse (BrailleDisplay *brl, const void *packet, size_t size) {
  const HW_Packet *response = packet;

  return (response->fields.type == HW_MSG_INIT_RESP)? BRL_RSP_DONE: BRL_RSP_UNEXPECTED;
}

static int
probeSerialDisplay (BrailleDisplay *brl) {
  HW_Packet response;

  if (probeBrailleDisplay(brl, 0, NULL, 1000,
                          writeIdentifyRequest,
                          readResponse, &response, sizeof(response.bytes),
                          isIdentityResponse)) {
    logMessage(LOG_INFO, "detected Humanware device: model=%u cells=%u",
               response.fields.data.init.modelIdentifier,
               response.fields.data.init.cellCount);

    if (response.fields.data.init.communicationDisabled) {
      logMessage(LOG_WARNING, "communication channel not available");
    }

    brl->textColumns = response.fields.data.init.cellCount;
    return 1;
  }

  return 0;
}

static int
writeSerialCells (BrailleDisplay *brl, const unsigned char *cells, unsigned char count) {
  return writePacket(brl, HW_MSG_DISPLAY, count, cells);
}

static int
handleKeyEvent (BrailleDisplay *brl, unsigned char key, int press) {
  KeyGroup group;

  if (key < HW_KEY_ROUTING) {
    group = HW_GRP_NavigationKeys;
  } else {
    group = HW_GRP_RoutingKeys;
    key -= HW_KEY_ROUTING;
  }

  return enqueueKeyEvent(brl, group, key, press);
}

static int
handleSerialKeys (BrailleDisplay *brl) {
  HW_Packet packet;
  size_t length;

  while ((length = readBraillePacket(brl, NULL, &packet, sizeof(packet), verifyPacket, NULL))) {
    switch (packet.fields.type) {
      case HW_MSG_KEY_DOWN:
        handleKeyEvent(brl, packet.fields.data.key.id, 1);
        continue;

      case HW_MSG_KEY_UP:
        handleKeyEvent(brl, packet.fields.data.key.id, 0);
        continue;

      default:
        break;
    }

    logUnexpectedPacket(&packet, length);
  }

  return (errno == EAGAIN)? EOF: BRL_CMD_RESTARTBRL;
}

static const ProtocolEntry serialProtocol = {
  .name = "serial",
  .probeDisplay = probeSerialDisplay,
  .writeCells = writeSerialCells,
  .handleKeys = handleSerialKeys
};

static ssize_t
readFeature (
  BrailleDisplay *brl, unsigned char report,
  unsigned char *buffer, size_t size
) {
  ssize_t length = gioGetHidFeature(brl->gioEndpoint, report, buffer, size);

  if (length != -1) {
    logInputPacket(buffer, length);
  } else {
    logSystemError("HID feature read");
  }

  return length;
}

static int
writeReport (BrailleDisplay *brl, const unsigned char *data, size_t size) {
  logOutputPacket(data, size);

  {
    ssize_t result = gioWriteHidReport(brl->gioEndpoint, data, size);
    if (result != -1) return 1;
  }

  logSystemError("HID report write");
  return 0;
}

static int
probeHidDisplay (BrailleDisplay *brl) {
  const unsigned char report = HW_REP_FTR_Capabilities;
  const size_t size = gioGetHidReportSize(brl->gioEndpoint, report);

  if (size > 0) {
    unsigned char buffer[size];
    const ssize_t length = readFeature(brl, report, buffer, size);

    if (length != -1) {
      HW_CapabilitiesReport capabilities;

      {
        const size_t maximum = sizeof(capabilities);
        size_t count = MIN(length, maximum);
        memcpy(&capabilities, buffer, count);

        if (count < maximum) {
          memset(((unsigned char *)&capabilities)+count,
                 0, (maximum - count));
        }
      }

      logMessage(LOG_INFO, "Firmware Version: %c.%c.%c%c",
                 capabilities.version.major, capabilities.version.minor,
                 capabilities.version.revision[0], capabilities.version.revision[1]);

      brl->textColumns = capabilities.cellCount;
      return 1;
    }
  }

  return 0;
}

static int
writeHidCells (BrailleDisplay *brl, const unsigned char *cells, unsigned char count) {
  unsigned char buffer[4 + count];
  unsigned char *byte = buffer;

  *byte++ = HW_REP_OUT_WriteCells;
  *byte++ = 1;
  *byte++ = 0;
  *byte++ = count;
  byte = mempcpy(byte, cells, count);

  return writeReport(brl, buffer, byte-buffer);
}

static int
handleHidKeys (BrailleDisplay *brl) {
  return EOF;
}

static const ProtocolEntry hidProtocol = {
  .name = "HID",
  .probeDisplay = probeHidDisplay,
  .writeCells = writeHidCells,
  .handleKeys = handleHidKeys
};

static int
connectResource (BrailleDisplay *brl, const char *identifier) {
  static const SerialParameters serialParameters = {
    SERIAL_DEFAULT_PARAMETERS,
    .baud = 115200,
    .parity = SERIAL_PARITY_EVEN
  };

  BEGIN_USB_CHANNEL_DEFINITIONS
    { /* all models (serial protocol) */
      .vendor=0X1C71, .product=0XC005, 
      .configuration=1, .interface=1, .alternative=0,
      .inputEndpoint=2, .outputEndpoint=3,
      .serial = &serialParameters,
      .data = &serialProtocol
    },

    { /* all models (HID protocol) */
      .vendor=0X1C71, .product=0XC006,
      .configuration=1, .interface=1, .alternative=0,
      .data = &hidProtocol
    },
  END_USB_CHANNEL_DEFINITIONS

  GioDescriptor descriptor;
  gioInitializeDescriptor(&descriptor);

  descriptor.serial.parameters = &serialParameters;
  descriptor.serial.options.applicationData = &serialProtocol;
  descriptor.serial.options.readyDelay = 100;

  descriptor.usb.channelDefinitions = usbChannelDefinitions;

  descriptor.bluetooth.channelNumber = 1;
  descriptor.bluetooth.options.applicationData = &serialProtocol;
  descriptor.bluetooth.options.readyDelay = 100;

  if (connectBrailleResource(brl, identifier, &descriptor, NULL)) {
    brl->data->protocol = gioGetApplicationData(brl->gioEndpoint);
    return 1;
  }

  return 0;
}

static int
brl_construct (BrailleDisplay *brl, char **parameters, const char *device) {
  if ((brl->data = malloc(sizeof(*brl->data)))) {
    memset(brl->data, 0, sizeof(*brl->data));

    if (connectResource(brl, device)) {
      if (brl->data->protocol->probeDisplay(brl)) {
        setBrailleKeyTable(brl, &KEY_TABLE_DEFINITION(all));
        makeOutputTable(dotsTable_ISO11548_1);

        brl->data->text.rewrite = 1;
        return 1;
      }

      disconnectBrailleResource(brl, NULL);
    }

    free(brl->data);
    brl->data = NULL;
  } else {
    logMallocError();
  }

  return 0;
}

static void
brl_destruct (BrailleDisplay *brl) {
  disconnectBrailleResource(brl, NULL);

  if (brl->data) {
    free(brl->data);
    brl->data = NULL;
  }
}

static int
brl_writeWindow (BrailleDisplay *brl, const wchar_t *text) {
  const size_t count = brl->textColumns;

  if (cellsHaveChanged(brl->data->text.cells, brl->buffer, count, NULL, NULL, &brl->data->text.rewrite)) {
    unsigned char cells[count];

    translateOutputCells(cells, brl->data->text.cells, count);
    if (!brl->data->protocol->writeCells(brl, cells, count)) return 0;
  }

  return 1;
}

static int
brl_readCommand (BrailleDisplay *brl, KeyTableCommandContext context) {
  return brl->data->protocol->handleKeys(brl);
}
