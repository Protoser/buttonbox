// NKRO USB HID keyboard (native TinyUSB). Replaces the gamepad as the box's HID
// output. A custom bitmap report (1 modifier byte + 120-key bitmap) gives true
// n-key rollover — no 6-key limit even when many buttons are held at once.
//
// The chord/HID engine drives this per OUTPUT index (0..31): each output carries
// its own modifier + HID usage (settings.keyMod / settings.keyKey). kbPress captures
// them at press time so a rebind mid-hold can't corrupt the release. The aggregate
// report is the OR of every held output's modifier + the union of their key bits.
#pragma once
#include <Arduino.h>

void kbBegin();                                            // register + start the HID device
void kbPress(uint8_t out, uint8_t mod, uint8_t key);       // out down: contribute mod+key (key 0 = nothing)
void kbRelease(uint8_t out);                               // out up: drop its contribution
