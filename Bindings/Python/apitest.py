#!/usr/bin/env python

import sys
import sysconfig
import os

def getProgramPath ():
  return os.path.realpath(os.path.dirname(sys.argv[0]))

def getBuildPath (directory):
  name = "{prefix}.{platform}-{version[0]}.{version[1]}".format(
    prefix = directory,
    platform = sysconfig.get_platform(),
    version = sys.version_info
  )

  return os.path.join(getProgramPath(), "build", name)

sys.path.insert(0, getBuildPath("lib"))

if __name__ == "__main__":
  import brlapi
  import errno
  import Xlib.keysymdef.miscellany

  try:
    brl = brlapi.Connection()

    try:
      brl.enterTtyMode()
      brl.ignoreKeys(brlapi.rangeType_all, [0])

      # Accept the home, window up, and window down braille commands
      brl.acceptKeys(brlapi.rangeType_command, [
        brlapi.KEY_TYPE_CMD | brlapi.KEY_CMD_HOME,
        brlapi.KEY_TYPE_CMD | brlapi.KEY_CMD_WINUP,
        brlapi.KEY_TYPE_CMD | brlapi.KEY_CMD_WINDN
      ])

      # Accept the tab key
      brl.acceptKeys(brlapi.rangeType_key, [
        brlapi.KEY_TYPE_SYM | Xlib.keysymdef.miscellany.XK_Tab
      ])

      brl.writeText("Press home, winup/dn or tab to continue ...")
      key = brl.readKey()

      k = brl.expandKeyCode(key)
      brl.writeText("Key %ld (%x %x %x %x) !" % (key, k["type"], k["command"], k["argument"], k["flags"]))
      brl.writeText(None, 1)
      brl.readKey()

      underline = chr(brlapi.DOT7 + brlapi.DOT8)
      # Note: center() can take two arguments only starting from python 2.4
      brl.write(
        regionBegin = 1,
        regionSize = 40,
        text = "Press any key to exit                   ",
        orMask = "".center(21,underline) + "".center(19,chr(0))
      )

      brl.acceptKeys(brlapi.rangeType_all, [0])
      brl.readKey()

      brl.leaveTtyMode()
    finally:
      del brl
  except brlapi.ConnectionError as e:
    if e.brlerrno == brlapi.ERROR_CONNREFUSED:
      print "Connection to %s refused. BRLTTY is too busy..." % e.host
    elif e.brlerrno == brlapi.ERROR_AUTHENTICATION:
      print "Authentication with %s failed. Please check the permissions of %s" % (e.host, e.auth)
    elif e.brlerrno == brlapi.ERROR_LIBCERR and (e.libcerrno == errno.ECONNREFUSED or e.libcerrno == errno.ENOENT):
      print "Connection to %s failed. Is BRLTTY really running?" % (e.host)
    else:
      print "Connection to BRLTTY at %s failed: " % (e.host)
    print(e)
    print(e.brlerrno)
    print(e.libcerrno)
