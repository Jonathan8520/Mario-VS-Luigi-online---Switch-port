/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

void fatal_error(const char *fmt, ...) {
  /* Log it first: the console screen below is gone the moment A is pressed,
   * and debug.log is what gets sent back for diagnosis. */
  {
    char msg[1024];
    va_list l2;
    va_start(l2, fmt);
    vsnprintf(msg, sizeof msg, fmt, l2);
    va_end(l2);
    debugPrintf("[boot] FATAL: %s\n", msg);
    debug_log_flush();
    fsdevCommitDevice("sdmc");
  }
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\n(this message is also in sdmc:/switch/mvlo/debug.log)\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
