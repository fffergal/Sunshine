/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for macOS input handling.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUserDevice.h>
#include <mach/mach.h>

// local includes
#include "src/display_device.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

/**
 * @brief Delay for a double click, in milliseconds.
 * @todo Make this configurable.
 */
constexpr std::chrono::milliseconds MULTICLICK_DELAY_MS(500);

namespace platf {
  using namespace std::literals;

  struct macos_input_t {
  public:
    CGDirectDisplayID display {};
    CGFloat displayScaling {};

    // Virtual HID devices
    IOHIDUserDeviceRef virt_keyboard {};
    IOHIDUserDeviceRef virt_mouse {};

    // Keyboard state: modifier bitmask and up to 6 simultaneously pressed keys
    uint8_t kb_modifiers {};
    uint8_t kb_keys[6] {};
    uint8_t kb_keys_count {};

    // Mouse state
    bool mouse_down[3] {};  // mouse button status (index 0=left, 1=right, 2=middle)
    util::point_t mouse_pos {};  // internally tracked cursor position
    std::chrono::steady_clock::time_point last_mouse_event[3][2];  // timestamp of last mouse events
  };

  // A struct to hold a Windows keycode to USB HID usage code mapping.
  struct KeyCodeMap {
    int win_keycode;
    int mac_keycode;  // now contains USB HID usage codes
  };

  // Customized less operator for using std::lower_bound() on a KeyCodeMap array.
  bool operator<(const KeyCodeMap &a, const KeyCodeMap &b) {
    return a.win_keycode < b.win_keycode;
  }

  // clang-format off
// Windows virtual keycode -> USB HID keyboard usage code mapping.
// Modifier keys 0xE0-0xE7 are handled specially (set bits in modifier byte).
const KeyCodeMap kKeyCodesMap[] = {
  { 0x08 /* VKEY_BACK */,                      0x2A /* Keyboard Delete (Backspace) */    },
  { 0x09 /* VKEY_TAB */,                       0x2B /* Keyboard Tab */                  },
  { 0x0A /* VKEY_BACKTAB */,                   0x2B /* Keyboard Tab */                  },
  { 0x0C /* VKEY_CLEAR */,                     0x53 /* Keypad Num Lock and Clear */      },
  { 0x0D /* VKEY_RETURN */,                    0x28 /* Keyboard Return (ENTER) */        },
  { 0x10 /* VKEY_SHIFT */,                     0xE1 /* Keyboard Left Shift */            },
  { 0x11 /* VKEY_CONTROL */,                   0xE0 /* Keyboard Left Control */          },
  { 0x12 /* VKEY_MENU */,                      0xE2 /* Keyboard Left Alt */              },
  { 0x13 /* VKEY_PAUSE */,                     0x48 /* Keyboard Pause */                 },
  { 0x14 /* VKEY_CAPITAL */,                   0x39 /* Keyboard Caps Lock */             },
  { 0x15 /* VKEY_KANA */,                      0x88 /* Keyboard International2 (Kana) */ },
  { 0x15 /* VKEY_HANGUL */,                    -1                                        },
  { 0x17 /* VKEY_JUNJA */,                     -1                                        },
  { 0x18 /* VKEY_FINAL */,                     -1                                        },
  { 0x19 /* VKEY_HANJA */,                     -1                                        },
  { 0x19 /* VKEY_KANJI */,                     -1                                        },
  { 0x1B /* VKEY_ESCAPE */,                    0x29 /* Keyboard Escape */                },
  { 0x1C /* VKEY_CONVERT */,                   -1                                        },
  { 0x1D /* VKEY_NONCONVERT */,                -1                                        },
  { 0x1E /* VKEY_ACCEPT */,                    -1                                        },
  { 0x1F /* VKEY_MODECHANGE */,                -1                                        },
  { 0x20 /* VKEY_SPACE */,                     0x2C /* Keyboard Spacebar */              },
  { 0x21 /* VKEY_PRIOR */,                     0x4B /* Keyboard Page Up */               },
  { 0x22 /* VKEY_NEXT */,                      0x4E /* Keyboard Page Down */             },
  { 0x23 /* VKEY_END */,                       0x4D /* Keyboard End */                   },
  { 0x24 /* VKEY_HOME */,                      0x4A /* Keyboard Home */                  },
  { 0x25 /* VKEY_LEFT */,                      0x50 /* Keyboard Left Arrow */            },
  { 0x26 /* VKEY_UP */,                        0x52 /* Keyboard Up Arrow */              },
  { 0x27 /* VKEY_RIGHT */,                     0x4F /* Keyboard Right Arrow */           },
  { 0x28 /* VKEY_DOWN */,                      0x51 /* Keyboard Down Arrow */            },
  { 0x29 /* VKEY_SELECT */,                    -1                                        },
  { 0x2A /* VKEY_PRINT */,                     -1                                        },
  { 0x2B /* VKEY_EXECUTE */,                   -1                                        },
  { 0x2C /* VKEY_SNAPSHOT */,                  -1                                        },
  { 0x2D /* VKEY_INSERT */,                    0x49 /* Keyboard Insert */                },
  { 0x2E /* VKEY_DELETE */,                    0x4C /* Keyboard Delete Forward */        },
  { 0x2F /* VKEY_HELP */,                      -1                                        },
  { 0x30 /* VKEY_0 */,                         0x27 /* Keyboard 0 */                     },
  { 0x31 /* VKEY_1 */,                         0x1E /* Keyboard 1 */                     },
  { 0x32 /* VKEY_2 */,                         0x1F /* Keyboard 2 */                     },
  { 0x33 /* VKEY_3 */,                         0x20 /* Keyboard 3 */                     },
  { 0x34 /* VKEY_4 */,                         0x21 /* Keyboard 4 */                     },
  { 0x35 /* VKEY_5 */,                         0x22 /* Keyboard 5 */                     },
  { 0x36 /* VKEY_6 */,                         0x23 /* Keyboard 6 */                     },
  { 0x37 /* VKEY_7 */,                         0x24 /* Keyboard 7 */                     },
  { 0x38 /* VKEY_8 */,                         0x25 /* Keyboard 8 */                     },
  { 0x39 /* VKEY_9 */,                         0x26 /* Keyboard 9 */                     },
  { 0x41 /* VKEY_A */,                         0x04 /* Keyboard a */                     },
  { 0x42 /* VKEY_B */,                         0x05 /* Keyboard b */                     },
  { 0x43 /* VKEY_C */,                         0x06 /* Keyboard c */                     },
  { 0x44 /* VKEY_D */,                         0x07 /* Keyboard d */                     },
  { 0x45 /* VKEY_E */,                         0x08 /* Keyboard e */                     },
  { 0x46 /* VKEY_F */,                         0x09 /* Keyboard f */                     },
  { 0x47 /* VKEY_G */,                         0x0A /* Keyboard g */                     },
  { 0x48 /* VKEY_H */,                         0x0B /* Keyboard h */                     },
  { 0x49 /* VKEY_I */,                         0x0C /* Keyboard i */                     },
  { 0x4A /* VKEY_J */,                         0x0D /* Keyboard j */                     },
  { 0x4B /* VKEY_K */,                         0x0E /* Keyboard k */                     },
  { 0x4C /* VKEY_L */,                         0x0F /* Keyboard l */                     },
  { 0x4D /* VKEY_M */,                         0x10 /* Keyboard m */                     },
  { 0x4E /* VKEY_N */,                         0x11 /* Keyboard n */                     },
  { 0x4F /* VKEY_O */,                         0x12 /* Keyboard o */                     },
  { 0x50 /* VKEY_P */,                         0x13 /* Keyboard p */                     },
  { 0x51 /* VKEY_Q */,                         0x14 /* Keyboard q */                     },
  { 0x52 /* VKEY_R */,                         0x15 /* Keyboard r */                     },
  { 0x53 /* VKEY_S */,                         0x16 /* Keyboard s */                     },
  { 0x54 /* VKEY_T */,                         0x17 /* Keyboard t */                     },
  { 0x55 /* VKEY_U */,                         0x18 /* Keyboard u */                     },
  { 0x56 /* VKEY_V */,                         0x19 /* Keyboard v */                     },
  { 0x57 /* VKEY_W */,                         0x1A /* Keyboard w */                     },
  { 0x58 /* VKEY_X */,                         0x1B /* Keyboard x */                     },
  { 0x59 /* VKEY_Y */,                         0x1C /* Keyboard y */                     },
  { 0x5A /* VKEY_Z */,                         0x1D /* Keyboard z */                     },
  { 0x5B /* VKEY_LWIN */,                      0xE3 /* Keyboard Left GUI (Command) */    },
  { 0x5C /* VKEY_RWIN */,                      0xE7 /* Keyboard Right GUI (Command) */   },
  { 0x5D /* VKEY_APPS */,                      0x65 /* Keyboard Application */           },
  { 0x5F /* VKEY_SLEEP */,                     -1                                        },
  { 0x60 /* VKEY_NUMPAD0 */,                   0x62 /* Keypad 0 */                       },
  { 0x61 /* VKEY_NUMPAD1 */,                   0x59 /* Keypad 1 */                       },
  { 0x62 /* VKEY_NUMPAD2 */,                   0x5A /* Keypad 2 */                       },
  { 0x63 /* VKEY_NUMPAD3 */,                   0x5B /* Keypad 3 */                       },
  { 0x64 /* VKEY_NUMPAD4 */,                   0x5C /* Keypad 4 */                       },
  { 0x65 /* VKEY_NUMPAD5 */,                   0x5D /* Keypad 5 */                       },
  { 0x66 /* VKEY_NUMPAD6 */,                   0x5E /* Keypad 6 */                       },
  { 0x67 /* VKEY_NUMPAD7 */,                   0x5F /* Keypad 7 */                       },
  { 0x68 /* VKEY_NUMPAD8 */,                   0x60 /* Keypad 8 */                       },
  { 0x69 /* VKEY_NUMPAD9 */,                   0x61 /* Keypad 9 */                       },
  { 0x6A /* VKEY_MULTIPLY */,                  0x55 /* Keypad * */                       },
  { 0x6B /* VKEY_ADD */,                       0x57 /* Keypad + */                       },
  { 0x6C /* VKEY_SEPARATOR */,                 -1                                        },
  { 0x6D /* VKEY_SUBTRACT */,                  0x56 /* Keypad - */                       },
  { 0x6E /* VKEY_DECIMAL */,                   0x63 /* Keypad . */                       },
  { 0x6F /* VKEY_DIVIDE */,                    0x54 /* Keypad / */                       },
  { 0x70 /* VKEY_F1 */,                        0x3A /* Keyboard F1 */                    },
  { 0x71 /* VKEY_F2 */,                        0x3B /* Keyboard F2 */                    },
  { 0x72 /* VKEY_F3 */,                        0x3C /* Keyboard F3 */                    },
  { 0x73 /* VKEY_F4 */,                        0x3D /* Keyboard F4 */                    },
  { 0x74 /* VKEY_F5 */,                        0x3E /* Keyboard F5 */                    },
  { 0x75 /* VKEY_F6 */,                        0x3F /* Keyboard F6 */                    },
  { 0x76 /* VKEY_F7 */,                        0x40 /* Keyboard F7 */                    },
  { 0x77 /* VKEY_F8 */,                        0x41 /* Keyboard F8 */                    },
  { 0x78 /* VKEY_F9 */,                        0x42 /* Keyboard F9 */                    },
  { 0x79 /* VKEY_F10 */,                       0x43 /* Keyboard F10 */                   },
  { 0x7A /* VKEY_F11 */,                       0x44 /* Keyboard F11 */                   },
  { 0x7B /* VKEY_F12 */,                       0x45 /* Keyboard F12 */                   },
  { 0x7C /* VKEY_F13 */,                       0x68 /* Keyboard F13 */                   },
  { 0x7D /* VKEY_F14 */,                       0x69 /* Keyboard F14 */                   },
  { 0x7E /* VKEY_F15 */,                       0x6A /* Keyboard F15 */                   },
  { 0x7F /* VKEY_F16 */,                       0x6B /* Keyboard F16 */                   },
  { 0x80 /* VKEY_F17 */,                       0x6C /* Keyboard F17 */                   },
  { 0x81 /* VKEY_F18 */,                       0x6D /* Keyboard F18 */                   },
  { 0x82 /* VKEY_F19 */,                       0x6E /* Keyboard F19 */                   },
  { 0x83 /* VKEY_F20 */,                       0x6F /* Keyboard F20 */                   },
  { 0x84 /* VKEY_F21 */,                       -1                                        },
  { 0x85 /* VKEY_F22 */,                       -1                                        },
  { 0x86 /* VKEY_F23 */,                       -1                                        },
  { 0x87 /* VKEY_F24 */,                       -1                                        },
  { 0x90 /* VKEY_NUMLOCK */,                   0x53 /* Keypad Num Lock and Clear */      },
  { 0x91 /* VKEY_SCROLL */,                    0x47 /* Keyboard Scroll Lock */           },
  { 0xA0 /* VKEY_LSHIFT */,                    0xE1 /* Keyboard Left Shift */            },
  { 0xA1 /* VKEY_RSHIFT */,                    0xE5 /* Keyboard Right Shift */           },
  { 0xA2 /* VKEY_LCONTROL */,                  0xE0 /* Keyboard Left Control */          },
  { 0xA3 /* VKEY_RCONTROL */,                  0xE4 /* Keyboard Right Control */         },
  { 0xA4 /* VKEY_LMENU */,                     0xE2 /* Keyboard Left Alt */              },
  { 0xA5 /* VKEY_RMENU */,                     0xE6 /* Keyboard Right Alt */             },
  { 0xA6 /* VKEY_BROWSER_BACK */,              -1                                        },
  { 0xA7 /* VKEY_BROWSER_FORWARD */,           -1                                        },
  { 0xA8 /* VKEY_BROWSER_REFRESH */,           -1                                        },
  { 0xA9 /* VKEY_BROWSER_STOP */,              -1                                        },
  { 0xAA /* VKEY_BROWSER_SEARCH */,            -1                                        },
  { 0xAB /* VKEY_BROWSER_FAVORITES */,         -1                                        },
  { 0xAC /* VKEY_BROWSER_HOME */,              -1                                        },
  { 0xAD /* VKEY_VOLUME_MUTE */,               -1                                        },
  { 0xAE /* VKEY_VOLUME_DOWN */,               -1                                        },
  { 0xAF /* VKEY_VOLUME_UP */,                 -1                                        },
  { 0xB0 /* VKEY_MEDIA_NEXT_TRACK */,          -1                                        },
  { 0xB1 /* VKEY_MEDIA_PREV_TRACK */,          -1                                        },
  { 0xB2 /* VKEY_MEDIA_STOP */,                -1                                        },
  { 0xB3 /* VKEY_MEDIA_PLAY_PAUSE */,          -1                                        },
  { 0xB4 /* VKEY_MEDIA_LAUNCH_MAIL */,         -1                                        },
  { 0xB5 /* VKEY_MEDIA_LAUNCH_MEDIA_SELECT */, -1                                        },
  { 0xB6 /* VKEY_MEDIA_LAUNCH_APP1 */,         -1                                        },
  { 0xB7 /* VKEY_MEDIA_LAUNCH_APP2 */,         -1                                        },
  { 0xBA /* VKEY_OEM_1 */,                     0x33 /* Keyboard ; : */                   },
  { 0xBB /* VKEY_OEM_PLUS */,                  0x2E /* Keyboard = + */                   },
  { 0xBC /* VKEY_OEM_COMMA */,                 0x36 /* Keyboard , < */                   },
  { 0xBD /* VKEY_OEM_MINUS */,                 0x2D /* Keyboard - _ */                   },
  { 0xBE /* VKEY_OEM_PERIOD */,                0x37 /* Keyboard . > */                   },
  { 0xBF /* VKEY_OEM_2 */,                     0x38 /* Keyboard / ? */                   },
  { 0xC0 /* VKEY_OEM_3 */,                     0x35 /* Keyboard ` ~ */                   },
  { 0xDB /* VKEY_OEM_4 */,                     0x2F /* Keyboard [ { */                   },
  { 0xDC /* VKEY_OEM_5 */,                     0x31 /* Keyboard \ | */                   },
  { 0xDD /* VKEY_OEM_6 */,                     0x30 /* Keyboard ] } */                   },
  { 0xDE /* VKEY_OEM_7 */,                     0x34 /* Keyboard ' " */                   },
  { 0xDF /* VKEY_OEM_8 */,                     -1                                        },
  { 0xE2 /* VKEY_OEM_102 */,                   -1                                        },
  { 0xE5 /* VKEY_PROCESSKEY */,                -1                                        },
  { 0xE7 /* VKEY_PACKET */,                    -1                                        },
  { 0xF6 /* VKEY_ATTN */,                      -1                                        },
  { 0xF7 /* VKEY_CRSEL */,                     -1                                        },
  { 0xF8 /* VKEY_EXSEL */,                     -1                                        },
  { 0xF9 /* VKEY_EREOF */,                     -1                                        },
  { 0xFA /* VKEY_PLAY */,                      -1                                        },
  { 0xFB /* VKEY_ZOOM */,                      -1                                        },
  { 0xFC /* VKEY_NONAME */,                    -1                                        },
  { 0xFD /* VKEY_PA1 */,                       -1                                        },
  { 0xFE /* VKEY_OEM_CLEAR */,                 0x53 /* Keypad Num Lock and Clear */      }
};
  // clang-format on

  int keysym(int keycode) {
    KeyCodeMap key_map {};

    key_map.win_keycode = keycode;
    const KeyCodeMap *temp_map = std::lower_bound(
      kKeyCodesMap,
      kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]),
      key_map
    );

    if (temp_map >= kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]) ||
        temp_map->win_keycode != keycode || temp_map->mac_keycode == -1) {
      return -1;
    }

    return temp_map->mac_keycode;
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto key = keysym(modcode);

    BOOST_LOG(debug) << "got keycode: 0x"sv << std::hex << modcode << ", translated to: 0x" << std::hex << key << ", release:" << release;

    if (key < 0) {
      return;
    }

    auto macos_input = static_cast<macos_input_t *>(input.get());
    auto hid_code = static_cast<uint8_t>(key);

    if (hid_code >= 0xE0 && hid_code <= 0xE7) {
      // Modifier key: set or clear the corresponding bit in the modifier byte.
      // bit position = hid_code - 0xE0 (0=LCtrl, 1=LShift, 2=LAlt, 3=LGUI, 4=RCtrl, 5=RShift, 6=RAlt, 7=RGUI)
      const uint8_t bit = 1u << (hid_code - 0xE0u);
      if (release) {
        macos_input->kb_modifiers &= ~bit;
      } else {
        macos_input->kb_modifiers |= bit;
      }
    } else {
      // Regular key: add to or remove from the pressed-keys array (max 6).
      auto *keys = macos_input->kb_keys;
      auto &count = macos_input->kb_keys_count;
      if (release) {
        for (uint8_t i = 0; i < count; ++i) {
          if (keys[i] == hid_code) {
            --count;
            keys[i] = keys[count];
            keys[count] = 0;
            break;
          }
        }
      } else {
        // Only add if not already present and there is room.
        bool found = false;
        for (uint8_t i = 0; i < count; ++i) {
          if (keys[i] == hid_code) {
            found = true;
            break;
          }
        }
        if (!found && count < 6) {
          keys[count++] = hid_code;
        }
      }
    }

    // Build and send the 8-byte boot keyboard HID report.
    // Byte 0: modifier bitmask, Byte 1: reserved (0x00), Bytes 2-7: up to 6 key codes.
    uint8_t report[8] = {};
    report[0] = macos_input->kb_modifiers;
    for (uint8_t i = 0; i < macos_input->kb_keys_count; ++i) {
      report[2 + i] = macos_input->kb_keys[i];
    }

    IOHIDUserDeviceHandleReport(macos_input->virt_keyboard, report, sizeof(report));
  }

  void unicode(input_t &input, char *utf8, int size) {
    BOOST_LOG(info) << "unicode: Unicode input not yet implemented for MacOS."sv;
  }

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    BOOST_LOG(info) << "alloc_gamepad: Gamepad not yet implemented for MacOS."sv;
    return -1;
  }

  void free_gamepad(input_t &input, int nr) {
    BOOST_LOG(info) << "free_gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    BOOST_LOG(info) << "gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  // returns current mouse location (from internally tracked position):
  util::point_t get_mouse_loc(input_t &input) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    return macos_input->mouse_pos;
  }

  /**
   * @brief Sends a HID mouse report via the virtual mouse device.
   *
   * Mouse report layout (7 bytes matching the virtual mouse HID descriptor):
   *   Byte 0     : button bitmask (bit0=left, bit1=right, bit2=middle) + 5-bit padding
   *   Bytes 1-2  : relative X movement (16-bit little-endian signed)
   *   Bytes 3-4  : relative Y movement (16-bit little-endian signed)
   *   Byte 5     : vertical scroll (8-bit signed)
   *   Byte 6     : horizontal scroll (8-bit signed)
   */
  void send_mouse_report(input_t &input, int16_t dx, int16_t dy, int8_t scroll_v, int8_t scroll_h) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    const uint8_t buttons =
      (macos_input->mouse_down[0] ? 0x01u : 0u) |  // left button   (bit 0)
      (macos_input->mouse_down[1] ? 0x02u : 0u) |  // right button  (bit 1)
      (macos_input->mouse_down[2] ? 0x04u : 0u);   // middle button (bit 2)

    uint8_t report[7] = {};
    report[0] = buttons;
    report[1] = static_cast<uint8_t>(dx & 0xFF);
    report[2] = static_cast<uint8_t>((dx >> 8) & 0xFF);
    report[3] = static_cast<uint8_t>(dy & 0xFF);
    report[4] = static_cast<uint8_t>((dy >> 8) & 0xFF);
    report[5] = static_cast<uint8_t>(scroll_v);
    report[6] = static_cast<uint8_t>(scroll_h);

    IOHIDUserDeviceHandleReport(macos_input->virt_mouse, report, sizeof(report));
  }

  void move_mouse(
    input_t &input,
    const int deltaX,
    const int deltaY
  ) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    BOOST_LOG(debug) << "move_mouse: delta "sv << deltaX << ", "sv << deltaY;

    // Update tracked position (clamped to display bounds).
    const CGRect display_bounds = CGDisplayBounds(macos_input->display);
    macos_input->mouse_pos.x = std::clamp(
      macos_input->mouse_pos.x + deltaX,
      display_bounds.origin.x,
      display_bounds.origin.x + display_bounds.size.width - 1
    );
    macos_input->mouse_pos.y = std::clamp(
      macos_input->mouse_pos.y + deltaY,
      display_bounds.origin.y,
      display_bounds.origin.y + display_bounds.size.height - 1
    );

    send_mouse_report(input, static_cast<int16_t>(deltaX), static_cast<int16_t>(deltaY), 0, 0);
  }

  void abs_mouse(
    input_t &input,
    const touch_port_t &touch_port,
    const float x,
    const float y
  ) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto scaling = macos_input->displayScaling;
    const auto display = macos_input->display;

    // Compute the target absolute position in display coordinates.
    CGRect display_bounds = CGDisplayBounds(display);
    const double origin_x = display_bounds.origin.x;
    const double origin_y = display_bounds.origin.y;
    const double max_x = origin_x + display_bounds.size.width - 1;
    const double max_y = origin_y + display_bounds.size.height - 1;
    const double target_x = std::clamp(x * scaling + origin_x, origin_x, max_x);
    const double target_y = std::clamp(y * scaling + origin_y, origin_y, max_y);

    // Compute delta from internally tracked position.
    const auto dx = static_cast<int16_t>(target_x - macos_input->mouse_pos.x);
    const auto dy = static_cast<int16_t>(target_y - macos_input->mouse_pos.y);

    macos_input->mouse_pos.x = target_x;
    macos_input->mouse_pos.y = target_y;

    send_mouse_report(input, dx, dy, 0, 0);
  }

  void button_mouse(input_t &input, const int button, const bool release) {
    CGMouseButton mac_button;

    const auto macos_input = static_cast<macos_input_t *>(input.get());

    switch (button) {
      case 1:
        mac_button = kCGMouseButtonLeft;
        break;
      case 2:
        mac_button = kCGMouseButtonCenter;
        break;
      case 3:
        mac_button = kCGMouseButtonRight;
        break;
      default:
        BOOST_LOG(warning) << "Unsupported mouse button for MacOS: "sv << button;
        return;
    }

    macos_input->mouse_down[mac_button] = !release;

    // if the last mouse down was less than MULTICLICK_DELAY_MS, we send a double click event
    const auto now = std::chrono::steady_clock::now();

    if (now < macos_input->last_mouse_event[mac_button][release] + MULTICLICK_DELAY_MS) {
      BOOST_LOG(debug) << "button_mouse: double-click, button "sv << button << ", release: "sv << release;
    } else {
      BOOST_LOG(debug) << "button_mouse: single-click, button "sv << button << ", release: "sv << release;
    }

    macos_input->last_mouse_event[mac_button][release] = now;

    // Send a mouse report with updated button state and no movement.
    send_mouse_report(input, 0, 0, 0, 0);
  }

  void scroll(input_t &input, const int high_res_distance) {
    const int wheel = high_res_distance / 120;
    const auto scroll_v = static_cast<int8_t>(std::clamp(wheel, -127, 127));
    send_mouse_report(input, 0, 0, scroll_v, 0);
  }

  void hscroll(input_t &input, int high_res_distance) {
    const int wheel = high_res_distance / 120;
    const auto scroll_h = static_cast<int8_t>(std::clamp(wheel, -127, 127));
    send_mouse_report(input, 0, 0, 0, scroll_h);
  }

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    // Unused
    return nullptr;
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    // Unimplemented feature - platform_caps::controller_touch
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    // Unimplemented
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    // Unimplemented
  }

  input_t input() {
    input_t result {new macos_input_t()};

    const auto macos_input = static_cast<macos_input_t *>(result.get());

    // Default to main display
    macos_input->display = CGMainDisplayID();

    auto output_name = display_device::map_output_name(config::video.output_name);
    // If output_name is set, try to find the display with that display id
    if (!output_name.empty()) {
      const int MAX_DISPLAYS = 32;
      uint32_t max_display = MAX_DISPLAYS;
      uint32_t display_count;
      CGDirectDisplayID displays[MAX_DISPLAYS];
      if (CGGetActiveDisplayList(max_display, displays, &display_count) != kCGErrorSuccess) {
        BOOST_LOG(error) << "Unable to get active display list , error: "sv << std::endl;
      } else {
        for (int i = 0; i < display_count; i++) {
          CGDirectDisplayID display_id = displays[i];
          if (display_id == std::atoi(output_name.c_str())) {
            macos_input->display = display_id;
          }
        }
      }
    }

    // Input coordinates are based on the virtual resolution not the physical, so we need the scaling factor
    const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(macos_input->display);
    macos_input->displayScaling = ((CGFloat) CGDisplayPixelsWide(macos_input->display)) / ((CGFloat) CGDisplayModeGetPixelWidth(mode));
    CFRelease(mode);

    // Initialize tracked mouse position from the current system cursor position.
    {
      const auto snapshot_event = CGEventCreate(nullptr);
      const auto current = CGEventGetLocation(snapshot_event);
      CFRelease(snapshot_event);
      macos_input->mouse_pos = util::point_t {current.x, current.y};
    }

    // Standard boot keyboard HID report descriptor:
    //   - 8 modifier bits (Left/Right Control, Shift, Alt, GUI)
    //   - 1 reserved byte
    //   - 6-byte key array (USB HID usage codes)
    static const uint8_t kb_descriptor[] = {
      0x05, 0x01,  // USAGE_PAGE (Generic Desktop)
      0x09, 0x06,  // USAGE (Keyboard)
      0xA1, 0x01,  // COLLECTION (Application)
      0x05, 0x07,  //   USAGE_PAGE (Keyboard)
      0x19, 0xE0,  //   USAGE_MINIMUM (Left Control)
      0x29, 0xE7,  //   USAGE_MAXIMUM (Right GUI)
      0x15, 0x00,  //   LOGICAL_MINIMUM (0)
      0x25, 0x01,  //   LOGICAL_MAXIMUM (1)
      0x75, 0x01,  //   REPORT_SIZE (1)
      0x95, 0x08,  //   REPORT_COUNT (8)
      0x81, 0x02,  //   INPUT (Data, Variable, Absolute) -- modifier byte
      0x95, 0x01,  //   REPORT_COUNT (1)
      0x75, 0x08,  //   REPORT_SIZE (8)
      0x81, 0x03,  //   INPUT (Constant) -- reserved byte
      0x95, 0x06,  //   REPORT_COUNT (6)
      0x75, 0x08,  //   REPORT_SIZE (8)
      0x15, 0x00,  //   LOGICAL_MINIMUM (0)
      0x25, 0xFF,  //   LOGICAL_MAXIMUM (255)
      0x05, 0x07,  //   USAGE_PAGE (Keyboard)
      0x19, 0x00,  //   USAGE_MINIMUM (0)
      0x29, 0xFF,  //   USAGE_MAXIMUM (255)
      0x81, 0x00,  //   INPUT (Data, Array) -- key array
      0xC0         // END_COLLECTION
    };

    // Relative mouse HID report descriptor:
    //   - 3 buttons + 5-bit padding
    //   - 16-bit relative X and Y
    //   - 8-bit vertical scroll wheel
    //   - 8-bit horizontal scroll (AC Pan, consumer page)
    static const uint8_t mouse_descriptor[] = {
      0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
      0x09, 0x02,        // USAGE (Mouse)
      0xA1, 0x01,        // COLLECTION (Application)
      0x09, 0x01,        //   USAGE (Pointer)
      0xA1, 0x00,        //   COLLECTION (Physical)
      0x05, 0x09,        //     USAGE_PAGE (Button)
      0x19, 0x01,        //     USAGE_MINIMUM (Button 1)
      0x29, 0x03,        //     USAGE_MAXIMUM (Button 3)
      0x15, 0x00,        //     LOGICAL_MINIMUM (0)
      0x25, 0x01,        //     LOGICAL_MAXIMUM (1)
      0x75, 0x01,        //     REPORT_SIZE (1)
      0x95, 0x03,        //     REPORT_COUNT (3)
      0x81, 0x02,        //     INPUT (Data, Variable, Absolute) -- 3 buttons
      0x95, 0x01,        //     REPORT_COUNT (1)
      0x75, 0x05,        //     REPORT_SIZE (5)
      0x81, 0x03,        //     INPUT (Constant) -- 5-bit padding
      0x05, 0x01,        //     USAGE_PAGE (Generic Desktop)
      0x09, 0x30,        //     USAGE (X)
      0x09, 0x31,        //     USAGE (Y)
      0x16, 0x00, 0x80,  //     LOGICAL_MINIMUM (-32768)
      0x26, 0xFF, 0x7F,  //     LOGICAL_MAXIMUM (32767)
      0x75, 0x10,        //     REPORT_SIZE (16)
      0x95, 0x02,        //     REPORT_COUNT (2)
      0x81, 0x06,        //     INPUT (Data, Variable, Relative) -- X/Y
      0x09, 0x38,        //     USAGE (Wheel)
      0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
      0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
      0x75, 0x08,        //     REPORT_SIZE (8)
      0x95, 0x01,        //     REPORT_COUNT (1)
      0x81, 0x06,        //     INPUT (Data, Variable, Relative) -- vertical scroll
      0x05, 0x0C,        //     USAGE_PAGE (Consumer Devices)
      0x0A, 0x38, 0x02,  //     USAGE (AC Pan)
      0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
      0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
      0x75, 0x08,        //     REPORT_SIZE (8)
      0x95, 0x01,        //     REPORT_COUNT (1)
      0x81, 0x06,        //     INPUT (Data, Variable, Relative) -- horizontal scroll
      0xC0,              //   END_COLLECTION
      0xC0               // END_COLLECTION
    };

    // Helper lambda to create an IOHIDUserDevice from a raw descriptor byte array.
    const auto create_hid_device = [](const uint8_t *desc, CFIndex desc_len) -> IOHIDUserDeviceRef {
      const auto descriptor_data = CFDataCreate(kCFAllocatorDefault, desc, desc_len);
      if (!descriptor_data) {
        return nullptr;
      }

      const auto properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!properties) {
        CFRelease(descriptor_data);
        return nullptr;
      }

      CFDictionarySetValue(properties, CFSTR(kIOHIDReportDescriptorKey), descriptor_data);
      CFRelease(descriptor_data);

      const auto device = IOHIDUserDeviceCreate(kCFAllocatorDefault, properties);
      CFRelease(properties);

      return device;
    };

    macos_input->virt_keyboard = create_hid_device(kb_descriptor, sizeof(kb_descriptor));
    if (!macos_input->virt_keyboard) {
      BOOST_LOG(error) << "Failed to create virtual HID keyboard device"sv;
    }

    macos_input->virt_mouse = create_hid_device(mouse_descriptor, sizeof(mouse_descriptor));
    if (!macos_input->virt_mouse) {
      BOOST_LOG(error) << "Failed to create virtual HID mouse device"sv;
    }

    // Schedule devices on the current run loop so the system registers them.
    if (macos_input->virt_keyboard) {
      IOHIDUserDeviceScheduleWithRunLoop(macos_input->virt_keyboard, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }
    if (macos_input->virt_mouse) {
      IOHIDUserDeviceScheduleWithRunLoop(macos_input->virt_mouse, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }
    // Spin the run loop briefly to let the kernel register the virtual devices.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

    macos_input->mouse_down[0] = false;
    macos_input->mouse_down[1] = false;
    macos_input->mouse_down[2] = false;

    BOOST_LOG(debug) << "Display "sv << macos_input->display << ", pixel dimension: " << CGDisplayPixelsWide(macos_input->display) << "x"sv << CGDisplayPixelsHigh(macos_input->display);

    return result;
  }

  void freeInput(void *p) {
    const auto *input = static_cast<macos_input_t *>(p);

    if (input->virt_keyboard) {
      CFRelease(input->virt_keyboard);
    }
    if (input->virt_mouse) {
      CFRelease(input->virt_mouse);
    }

    delete input;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector gamepads {
      supported_gamepad_t {"", false, "gamepads.macos_not_implemented"}
    };

    return gamepads;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities() {
    return 0;
  }
}  // namespace platf
