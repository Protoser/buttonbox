// Two-way link to the PC companion app over USB CDC serial.
//
// Reads newline-terminated lines and dispatches on the first token:
//   <key:value ...>            -> PC telemetry (pcStatsApply)
//   get                        -> reply with current config (cfg + chd lines)
//   set <key>:<val>            -> change a setting, persist, reply cfg
//   chord add <mask>:<output>  -> append a chord (raw 0-based values), reply
//   chord del <index>          -> remove a chord, reply
//   flash                      -> enter the bootloader
//
// Replies (device -> app):
//   cfg flip:.. labels:.. idle:.. chord:.. boot:.. pcorder:i,i,i,i,i apporder:i,.. apphidden:mask nchords:..
//   chd <i>:<membersMask>:<output>      (one per chord)
#pragma once
#include <Arduino.h>

void hostlinkUpdate(uint32_t now);   // drain serial, parse + dispatch complete lines

// Bounded, non-blocking serial write for ALL device->app output. The TinyUSB
// USBCDC write() busy-spins forever when the host has the port open but isn't
// reading, which would hang the input loop ("brick"). This writes only what the TX
// FIFO can accept and gives up after a short deadline, so the loop never stalls.
// Every module that talks back to the companion must send through this, not Serial.
void hostlinkSend(const char *s);
