/*=============================================================================
   Elements' host layer for Android: a hardware keyboard's keys.

   KeyEvent reports a key as its AKEYCODE (android/keycodes.h), the same on
   every keyboard layout, as the other hosts' virtual key codes are.

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#include <elements/base_view.hpp>
#include <android/input.h>
#include <android/keycodes.h>

namespace cycfi::elements
{
   key_code translate_key(int code)
   {
      // Letters, digits, function keys and the keypad's digits run in order.
      if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
         return key_code(int(key_code::a) + (code - AKEYCODE_A));
      if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
         return key_code(int(key_code::_0) + (code - AKEYCODE_0));
      if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
         return key_code(int(key_code::f1) + (code - AKEYCODE_F1));
      if (code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9)
         return key_code(int(key_code::kp_0) + (code - AKEYCODE_NUMPAD_0));

      switch (code)
      {
         case AKEYCODE_SPACE:          return key_code::space;
         case AKEYCODE_APOSTROPHE:     return key_code::apostrophe;
         case AKEYCODE_COMMA:          return key_code::comma;
         case AKEYCODE_MINUS:          return key_code::minus;
         case AKEYCODE_PERIOD:         return key_code::period;
         case AKEYCODE_SLASH:          return key_code::slash;
         case AKEYCODE_SEMICOLON:      return key_code::semicolon;
         case AKEYCODE_EQUALS:         return key_code::equal;
         case AKEYCODE_LEFT_BRACKET:   return key_code::left_bracket;
         case AKEYCODE_BACKSLASH:      return key_code::backslash;
         case AKEYCODE_RIGHT_BRACKET:  return key_code::right_bracket;
         case AKEYCODE_GRAVE:          return key_code::grave_accent;

         case AKEYCODE_ESCAPE:         return key_code::escape;
         case AKEYCODE_ENTER:          return key_code::enter;
         case AKEYCODE_TAB:            return key_code::tab;
         case AKEYCODE_DEL:            return key_code::backspace;
         case AKEYCODE_INSERT:         return key_code::insert;
         case AKEYCODE_FORWARD_DEL:    return key_code::_delete;
         case AKEYCODE_DPAD_RIGHT:     return key_code::right;
         case AKEYCODE_DPAD_LEFT:      return key_code::left;
         case AKEYCODE_DPAD_DOWN:      return key_code::down;
         case AKEYCODE_DPAD_UP:        return key_code::up;
         case AKEYCODE_PAGE_UP:        return key_code::page_up;
         case AKEYCODE_PAGE_DOWN:      return key_code::page_down;
         case AKEYCODE_MOVE_HOME:      return key_code::home;
         case AKEYCODE_MOVE_END:       return key_code::end;
         case AKEYCODE_CAPS_LOCK:      return key_code::caps_lock;
         case AKEYCODE_SCROLL_LOCK:    return key_code::scroll_lock;
         case AKEYCODE_NUM_LOCK:       return key_code::num_lock;
         case AKEYCODE_SYSRQ:          return key_code::print_screen;
         case AKEYCODE_BREAK:          return key_code::pause;

         case AKEYCODE_NUMPAD_DOT:     return key_code::kp_decimal;
         case AKEYCODE_NUMPAD_DIVIDE:  return key_code::kp_divide;
         case AKEYCODE_NUMPAD_MULTIPLY: return key_code::kp_multiply;
         case AKEYCODE_NUMPAD_SUBTRACT: return key_code::kp_subtract;
         case AKEYCODE_NUMPAD_ADD:     return key_code::kp_add;
         case AKEYCODE_NUMPAD_ENTER:   return key_code::kp_enter;
         case AKEYCODE_NUMPAD_EQUALS:  return key_code::kp_equal;

         case AKEYCODE_SHIFT_LEFT:     return key_code::left_shift;
         case AKEYCODE_CTRL_LEFT:      return key_code::left_control;
         case AKEYCODE_ALT_LEFT:       return key_code::left_alt;
         case AKEYCODE_META_LEFT:      return key_code::left_super;
         case AKEYCODE_SHIFT_RIGHT:    return key_code::right_shift;
         case AKEYCODE_CTRL_RIGHT:     return key_code::right_control;
         case AKEYCODE_ALT_RIGHT:      return key_code::right_alt;
         case AKEYCODE_META_RIGHT:     return key_code::right_super;
         case AKEYCODE_MENU:           return key_code::menu;

         default:                      return key_code::unknown;
      }
   }

   // Control is the action modifier, as on Windows and Linux.
   int translate_meta(int meta)
   {
      int mods = 0;
      if (meta & AMETA_SHIFT_ON)
         mods |= mod_shift;
      if (meta & AMETA_CTRL_ON)
         mods |= mod_control | mod_action;
      if (meta & AMETA_ALT_ON)
         mods |= mod_alt;
      if (meta & AMETA_META_ON)
         mods |= mod_super;
      return mods;
   }
}
