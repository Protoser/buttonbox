#include "keyboard.h"
#include "settings.h"          // KEYMAP_N
#include "USB.h"
#include "USBHID.h"

// Hand-written NKRO keyboard report descriptor (TinyUSB has no macro for it):
//   modifier byte  — usages 0xE0..0xE7 (L/R Ctrl/Shift/Alt/Gui), 8 x 1 bit
//   key bitmap     — usages 0x00..0x77 (0..119), 120 x 1 bit
// Report = 1 + 15 = 16 bytes. Range 0..119 covers F13(0x68)..F24(0x73), letters,
// digits, and the common editing/navigation keys. The stock keyboard class isn't
// used, so report id HID_REPORT_ID_KEYBOARD (1) is free for us.
static const uint8_t report_descriptor[] = {
  0x05, 0x01,                    // Usage Page (Generic Desktop)
  0x09, 0x06,                    // Usage (Keyboard)
  0xA1, 0x01,                    // Collection (Application)
  0x85, HID_REPORT_ID_KEYBOARD,  //   Report ID (1)
  0x05, 0x07,                    //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,                    //   Usage Minimum (0xE0)
  0x29, 0xE7,                    //   Usage Maximum (0xE7)
  0x15, 0x00,                    //   Logical Minimum (0)
  0x25, 0x01,                    //   Logical Maximum (1)
  0x75, 0x01,                    //   Report Size (1)
  0x95, 0x08,                    //   Report Count (8)
  0x81, 0x02,                    //   Input (Data, Variable, Absolute) — modifier byte
  0x19, 0x00,                    //   Usage Minimum (0x00)
  0x29, 0x77,                    //   Usage Maximum (0x77)
  0x15, 0x00,                    //   Logical Minimum (0)
  0x25, 0x01,                    //   Logical Maximum (1)
  0x75, 0x01,                    //   Report Size (1)
  0x95, 0x78,                    //   Report Count (120)
  0x81, 0x02,                    //   Input (Data, Variable, Absolute) — key bitmap
  0xC0                           // End Collection
};

class KeyboardNKRO : public USBHIDDevice {
public:
  KeyboardNKRO() {
    static bool added = false;
    if (!added) { added = true; hid.addDevice(this, sizeof(report_descriptor)); }
  }
  void begin() { hid.begin(); }
  uint16_t _onGetDescriptor(uint8_t *dst) override {
    memcpy(dst, report_descriptor, sizeof(report_descriptor));
    return sizeof(report_descriptor);
  }
  bool send(const uint8_t *report16) {
    return hid.SendReport(HID_REPORT_ID_KEYBOARD, report16, 16);
  }
private:
  USBHID hid;
};

static KeyboardNKRO keeb;

// Per-output contribution, captured at press time. downMask marks which outputs
// are currently held; the report is rebuilt from these on every change.
static uint8_t  curMod[KEYMAP_N] = {0};
static uint8_t  curKey[KEYMAP_N] = {0};
static uint32_t downMask         = 0;

static void rebuild() {
  uint8_t report[16] = {0};
  for (uint8_t o = 0; o < KEYMAP_N; o++) {
    if (!(downMask & (1u << o))) continue;
    report[0] |= curMod[o];
    uint8_t k = curKey[o];
    if (k && k < 120) report[1 + (k >> 3)] |= (uint8_t)(1u << (k & 7));
  }
  keeb.send(report);
}

void kbBegin() {
  keeb.begin();
  USB.begin();
}

void kbPress(uint8_t out, uint8_t mod, uint8_t key) {
  if (out >= KEYMAP_N) return;
  curMod[out] = mod;
  curKey[out] = key;
  downMask |= (1u << out);
  rebuild();
}

void kbRelease(uint8_t out) {
  if (out >= KEYMAP_N || !(downMask & (1u << out))) return;
  downMask &= ~(1u << out);
  curMod[out] = 0;
  curKey[out] = 0;
  rebuild();
}
