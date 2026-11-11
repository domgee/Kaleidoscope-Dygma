/* KeyRoleManager - KeyRole support for Kaleidoscope.
 * Copyright (C) 2025 Dygma Lab S.L.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "kbdfal_ll_memory.h"
#include "KeyRoleManager.h"

#include "Kaleidoscope-Ranges.h"
#include "kaleidoscope/key_defs.h"
#include <Kaleidoscope-FocusSerial.h>
#include <cstdint>
#include <cstring>

#include "EEPROMKeymapDygma.h"
#include "QukeysDygma.h"
#include "SuperkeysHandler.h"
#include "Superkeys/Actions/ActionsDriver.h"

namespace kaleidoscope
{
namespace plugin
{

#define ACTION_IS_UNDEFINED( p_action )   ( (*(uint16_t *)p_action) == 0xFFFF )

KeyRoleManager::KeyRoleManager()
{
    sk_index = 0;
    configured_superkeys = 0;
    memset(df_slots_, 0, sizeof(df_slots_));
}

/* HELPER FUNCTIONS */

/* The boot migration in migrate_legacy_keymap is the only user of layerIndexFromRaw, hidModToDumIndex, is_only_modifier, has_layer_change,
 * calculate_qukey_code and replace_superkey_with_qukey. Keep them unchanged so the migration reproduces the codes older firmware stored */

auto is_idle = [](const Key &k) { return k.getRaw() == 1; };

static inline int layerIndexFromRaw(uint32_t raw)
{
    auto key_type = ActionsDriver::find_key_type(raw);
    auto ranges_t = static_cast<Utils::KeyRanges>(key_type);
    int layer_idx = -1;
    uint32_t base = 0;

    if (ranges_t == Utils::KeyRanges::LAYER_LOCK)
    {
        base = Utils::LAYER_LOCK_FIRST;
        layer_idx = static_cast<int>(raw - base);
        //NRF_LOG_DEBUG("layerIndexFromRaw: LAYER_LOCK - raw=0x%08lX, base=0x%08lX, layer_idx=%d", 
        //              raw, base, layer_idx);
    }
    else if (ranges_t == Utils::KeyRanges::LAYER_SHIFT)
    {
        base = Utils::LAYER_SHIFT_FIRST;
        layer_idx = static_cast<int>(raw - base);
        //NRF_LOG_DEBUG("layerIndexFromRaw: LAYER_SHIFT - raw=0x%08lX, base=0x%08lX, layer_idx=%d", 
        //              raw, base, layer_idx);
    }
    else
    {
        //NRF_LOG_DEBUG("layerIndexFromRaw: Not a layer key - raw=0x%08lX, type=%d", 
        //              raw, key_type);
    }
    
    return layer_idx;
}

static inline int hidModToDumIndex(uint16_t hid)
{
    // Check for right modifiers (0x100 bit set)
    bool is_right = (hid & 0x100) != 0;
    uint8_t mod = hid & 0xFF;  // Get just the HID code
    
    switch (mod)
    {
        case 0xE0: // Left Control
            return is_right ? 4 : 0;  // Right Ctrl is index 4, Left is 0
        case 0xE1: // Left Shift
            return is_right ? 5 : 1;  // Right Shift is index 5, Left is 1
        case 0xE2: // Left Alt
            return is_right ? 6 : 2;  // Right Alt is index 6, Left is 2
        case 0xE3: // Left GUI/OS
            return is_right ? 7 : 3;  // Right GUI is index 7, Left is 3
        case 0xE4: // Right Control
            return 4;  // Always right Ctrl (0xE4 is right control)
        case 0xE5: // Right Shift
            return 5;  // Always right Shift (0xE5 is right shift)
        case 0xE6: // Right Alt (AltGr)
            return 6;  // Always right Alt
        case 0xE7: // Right GUI/OS
            return 7;  // Always right GUI
        default:
            return -1;
    }
}

bool KeyRoleManager::is_only_modifier(Key key)
{
    uint16_t key_id = key.getRaw() & 0x00FF; // We only take the HID keycode (lower part)
//    uint16_t flags = key.getRaw() & 0xFF00;  // Get the flags part

    // HIDmodifier range: 224 (0xE0) to 231 (0xE7) for left modifiers
    // Right modifiers are in the same range but with the 0x100 bit set
    return (key_id >= 0xE0 && key_id <= 0xE7) || 
           ((key_id | 0x100) >= 0xEE0 && (key_id | 0x100) <= 0xEE7);
}

bool KeyRoleManager::has_layer_change(Key Action)
{
    auto raw = Action.getRaw();
    auto key_type = ActionsDriver::find_key_type(raw);
    auto ranges_t = static_cast<Utils::KeyRanges>(key_type);

    bool is_layer = (ranges_t == Utils::KeyRanges::LAYER_LOCK || 
                    ranges_t == Utils::KeyRanges::LAYER_SHIFT);
    
    // Debug output
    //NRF_LOG_DEBUG("has_layer_change: raw=0x%04X, type=%d, is_layer=%d", raw, key_type, is_layer);
    
    return is_layer;
}

bool KeyRoleManager::is_qukey(Key key)
{
    bool result = false;

    if (key >= ranges::DUM_FIRST && key <= ranges::DUM_LAST) result = true;
    if (key >= ranges::DUL_FIRST && key <= ranges::DUL_LAST) result = true;

    return result;
}
/* END HELPER FUNCTIONS */

uint16_t KeyRoleManager::calculate_qukey_code(uint32_t hold_action_raw, uint16_t tap_action_raw)
{
    // Extract HID keycode (lower 8 bits) and flags (upper 8 bits)
    const uint16_t tap_hid   = static_cast<uint16_t>(tap_action_raw  & 0x00FF);
    const uint16_t tap_flags = static_cast<uint16_t>((tap_action_raw >> 8) & 0x00FF);
    const uint16_t hold_hid  = static_cast<uint16_t>(hold_action_raw & 0x01FF); // Include 9th bit for right modifiers

    //NRF_LOG_DEBUG("calculate_qukey_code: hold_raw=0x%08lX, tap_raw=0x%04X, hold_hid=0x%04X, tap_hid=0x%02X, tap_flags=0x%02X",
    //              hold_action_raw, tap_action_raw, hold_hid, tap_hid, tap_flags);

    // First check if this is a layer change key using the raw action
    // Case A: modifiers HID (Ctrl/Shift/Alt/OS/AltGr)
    if (!has_layer_change(Key(hold_action_raw)) && ( (hold_hid >= 0xE0 && hold_hid <= 0xE7) || (hold_hid >= 0x1E0 && hold_hid <= 0x1E7) ) )
    {
        const int idx = hidModToDumIndex(hold_hid);
        if (idx < 0) {
            //NRF_LOG_DEBUG("  Invalid modifier index for hold_hid=0x%04X", hold_hid);
            return 0;
        }
        
        // Calculate the base address for this modifier
        uint32_t base = ranges::DUM_FIRST + (static_cast<uint32_t>(idx) << 8);
        
        // If tap has flags, use the upper half of the range (128-255)
        // Otherwise use the lower half (0-127)
        uint16_t qukey_code;
        if (tap_flags != 0) {
            qukey_code = static_cast<uint16_t>(base + 128 + (tap_hid & 0x7F));
        } else {
            qukey_code = static_cast<uint16_t>(base + (tap_hid & 0x7F));
        }
        
        //NRF_LOG_DEBUG("  Modifier qukey - idx=%d, base=0x%04X, code=0x%04X", idx, base, qukey_code);
        return qukey_code;
    }
    // Case B: layer changes - check using the raw action
    if (has_layer_change(Key(hold_action_raw)))
    {
        // Get the layer index from the raw action
        int layer_idx = layerIndexFromRaw(hold_action_raw);
        if (layer_idx < 0) {
            //NRF_LOG_DEBUG("  Invalid layer index for hold_action_raw=0x%08lX", hold_action_raw);
            return 0;
        }
        
        // Calculate the base address for this layer
        // Each layer gets 256 possible keycodes (0-255)
        uint32_t base = ranges::DUL_FIRST + (static_cast<uint32_t>(layer_idx) << 8);
        
        // Calculate the final qukey code
        uint16_t qukey_code;
        if (tap_flags != 0) {
            // If tap has flags, use the upper half of the range (128-255)
            qukey_code = static_cast<uint16_t>(base + 128 + (tap_hid & 0x7F));
        } else {
            // Otherwise use the lower half (0-127)
            qukey_code = static_cast<uint16_t>(base + (tap_hid & 0x7F));
        }
        
        // Debug output
        //NRF_LOG_DEBUG("  Layer qukey - hold=0x%04X, tap=0x%02X, layer_idx=%d, base=0x%04X, code=0x%04X", 
        //              hold_hid, tap_hid, layer_idx, base, qukey_code);
        
        return qukey_code;
    }

    // Not a modifier or layer change
    //NRF_LOG_DEBUG("  Not a modifier or layer change");
    return 0;
}

uint16_t KeyRoleManager::replace_superkey_with_qukey(const Key *action_0, const Key *action_1)
{
    if (!action_0 || !action_1) return 0;

    const uint16_t tap_raw = static_cast<uint16_t>(action_0->getRaw());
    const uint16_t hold_raw = static_cast<uint16_t>(action_1->getRaw());

    const uint16_t qukey_code = calculate_qukey_code(hold_raw, tap_raw);
    if (qukey_code == 0u)
    {
        // ERROR: Quekey base not valid.
        return 0;
    }
    return qukey_code;
}

void KeyRoleManager::setup_keys()
{
    set_active_sk();
    rebuild_dual_function_slots();
    SuperkeysHandler::save_superkey_map_from( p_keyrole_config->superkeys, this->configured_superkeys);
}

EventHandlerResult KeyRoleManager::onSetup()
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_KEYROLE, (const void **)&p_keyrole_config );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

    result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_KEYMAP_FORMAT, (const void **)&p_keymap_format_config );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

    set_active_sk();

    if (p_keymap_format_config->keymap_format != KEYMAP_FORMAT_SUPERKEYS)
    {
        migrate_legacy_keymap();
    }

    rebuild_dual_function_slots();
    return EventHandlerResult::OK;

    UNUSED( result );
}

void KeyRoleManager::setup_superkeys(void)
{
    qukeys.onSetup();         // Initialize the Qukeys plugin.
    SuperkeysHandler::setup(configured_superkeys, p_keyrole_config->superkeys); // Initialize the SuperkeysHandler plugin.
}

void KeyRoleManager::set_active_sk() 
{
    this->configured_superkeys = 0;
  
    for (uint16_t i = 0; i < Utils::SUPER_KEY_COUNT; ++i) 
    {
      uint8_t _undefined_actions = 0;
  
      for (int j = 0; j < ACTIONS_IN_SUPERKEY; ++j)
      {
        if ( ACTION_IS_UNDEFINED( &p_keyrole_config->superkeys[i].actions[j] ) == true)
        {
          ++_undefined_actions;
        }
      }
  
      if (_undefined_actions == ACTIONS_IN_SUPERKEY)
      {
        break; // First row completely empty -> end of configured superkeys.
      }
  
      ++this->configured_superkeys;
    }

    if(this->configured_superkeys > Utils::MAX_SUPER_KEYS_ACTIVE)
    {
        //NRF_LOG_ERROR("Superkey count %i is greater than %i", this->configured_superkeys, Utils::MAX_SUPER_KEYS_ACTIVE);
        //NRF_LOG_FLUSH();
        this->configured_superkeys = Utils::MAX_SUPER_KEYS_ACTIVE;
    }
}

/* Return the DUM or DUL code older firmware registered for the slot under its conditions, or 0 for none */
uint16_t KeyRoleManager::legacy_qukey_code(uint8_t slot)
{
    const Superkey::superkey_config_t * p_superkey_config = &p_keyrole_config->superkeys[slot];

    Key action_0 = Superkey::getActionAsKey(p_superkey_config, 0);
    Key action_1 = Superkey::getActionAsKey(p_superkey_config, 1);
    Key action_2 = Superkey::getActionAsKey(p_superkey_config, 2);
    Key action_3 = Superkey::getActionAsKey(p_superkey_config, 3);
    Key action_4 = Superkey::getActionAsKey(p_superkey_config, 4);

    if (is_idle(action_0) || is_idle(action_1) || !is_idle(action_2) || !is_idle(action_3) || !is_idle(action_4))
    {
        return 0;
    }

    bool tap_is_layer_lock = (action_0.getRaw() >= Utils::LAYER_LOCK_FIRST && action_0.getRaw() <= Utils::LAYER_LOCK_LAST);

    Key hold_action = action_1;
    hold_action.setFlags(0);

    if (tap_is_layer_lock || !(is_only_modifier(hold_action) || has_layer_change(action_1)))
    {
        return 0;
    }

    return replace_superkey_with_qukey(&action_0, &action_1);
}

/* Turn the DUM and DUL cells older firmware stored for tap+hold Superkeys back into the Superkey codes, once. Map a cell to the lowest slot
 * with its code, as older firmware read it back and played it. Leave unmatched cells and codes outside the DU range as stored */
void KeyRoleManager::migrate_legacy_keymap()
{
    uint16_t codes[Utils::MAX_SUPER_KEYS_ACTIVE];
    bool any_code = false;

    for (uint8_t i = 0; i < configured_superkeys; ++i)
    {
        codes[i] = legacy_qukey_code(i);

        if (is_qukey(Key(codes[i])))
        {
            any_code = true;
        }
    }

    if (any_code)
    {
        const EEPROMKeymap::keymap_config_t * p_keymap_config = EEPROMKeymap::getKeymapConfig();
        uint16_t total_keys = static_cast<uint16_t>(Runtime.device().numKeys()) * APP_LAYERS_CNT;

        for (uint16_t pos = 0; pos < total_keys; ++pos)
        {
            Key stored_key = Key(p_keymap_config->keys[pos].keyCode, p_keymap_config->keys[pos].flags);

            if (!is_qukey(stored_key))
            {
                continue;
            }

            for (uint8_t i = 0; i < configured_superkeys; ++i)
            {
                if (codes[i] == stored_key.getRaw())
                {
                    EEPROMKeymap::updateKey(pos, Key(ranges::DYNAMIC_SUPER_FIRST + i));
                    break;
                }
            }
        }
    }

    /* Join the deferred save the keymap updates requested, so a power loss before it reruns the migration on unchanged data */
    keymap_format_config_t keymap_format_config;
    keymap_format_config.keymap_format = KEYMAP_FORMAT_SUPERKEYS;
    memset(keymap_format_config.reserved, 0xFF, sizeof(keymap_format_config.reserved));

    result_t result = kbdfal_ll_memory_data_save(p_keymap_format_config, &keymap_format_config, sizeof(keymap_format_config));
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

    UNUSED( result );
}

/* Play a Superkey through Qukeys when it has a tap and a hold and nothing else, the tap is no Layer Lock and no modifier, and
 * the hold is a modifier with any modifier flags or a Layer Shift 0 to 9 */
bool KeyRoleManager::dual_function_rule(const Superkey::superkey_config_t * p_superkey_config)
{
    const uint16_t tap = Superkey::getActionRaw(&p_superkey_config->actions[0]);
    const uint16_t hold = Superkey::getActionRaw(&p_superkey_config->actions[1]);

    if (tap == 0x0000 || tap == 0x0001 || tap == 0xFFFF || hold == 0x0001 || hold == 0xFFFF)
    {
        return false;
    }

    for (uint8_t j = 2; j <= 4; ++j)
    {
        if (Superkey::getActionRaw(&p_superkey_config->actions[j]) != 0x0001)
        {
            return false;
        }
    }

    const uint8_t tap_code = static_cast<uint8_t>(tap & 0xFF);

    if ((tap >= Utils::LAYER_LOCK_FIRST && tap <= Utils::LAYER_LOCK_LAST) || (tap_code >= 0xE0 && tap_code <= 0xE7))
    {
        return false;
    }

    if (hold >= Utils::LAYER_SHIFT_FIRST && hold <= Utils::LAYER_SHIFT_LAST)
    {
        return true;
    }

    const uint8_t hold_flags = static_cast<uint8_t>(hold >> 8);
    const uint8_t hold_code = static_cast<uint8_t>(hold & 0xFF);

    return (hold_flags & (SYNTHETIC | RESERVED)) == 0 && hold_code >= 0xE0 && hold_code <= 0xE7;
}

void KeyRoleManager::rebuild_dual_function_slots()
{
    memset(df_slots_, 0, sizeof(df_slots_));

    if (p_keyrole_config == nullptr)
    {
        return;
    }

    for (uint8_t i = 0; i < configured_superkeys; ++i)
    {
        if (dual_function_rule(&p_keyrole_config->superkeys[i]))
        {
            df_slots_[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
    }
}

bool KeyRoleManager::is_dual_function_slot(uint16_t slot) const
{
    if (slot >= configured_superkeys || slot >= Utils::MAX_SUPER_KEYS_ACTIVE)
    {
        return false;
    }

    return (df_slots_[slot >> 3] & (1u << (slot & 7))) != 0;
}

bool KeyRoleManager::dual_function_superkey(Key key, Key &tap, Key &hold) const
{
    if (p_keyrole_config == nullptr || IS_OUTSIDE_DYNAMIC_SUPER_RANGE(key.getRaw()))
    {
        return false;
    }

    const uint16_t slot = static_cast<uint16_t>(key.getRaw() - ranges::DYNAMIC_SUPER_FIRST);

    if (!is_dual_function_slot(slot))
    {
        return false;
    }

    tap = Superkey::getActionAsKey(&p_keyrole_config->superkeys[slot], 0);
    hold = Superkey::getActionAsKey(&p_keyrole_config->superkeys[slot], 1);
    return true;
}

bool KeyRoleManager::is_dual_function_superkey(Key key) const
{
    Key tap;
    Key hold;

    return dual_function_superkey(key, tap, hold);
}

void KeyRoleManager::send_sk_map()
{
    Key key;

    for (uint16_t i = 0; i < Utils::SUPER_KEY_COUNT; ++i)
    {
        for (int j = 0; j < ACTIONS_IN_SUPERKEY; ++j)
        {
            key = Superkey::getActionAsKey( &p_keyrole_config->superkeys[i].actions[j] );

            ::Focus.send(key);
        }
    }
}

EventHandlerResult KeyRoleManager::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t keyState)
{
    if (qukeys.onKeyswitchEvent(mapped_key, key_addr, keyState) == EventHandlerResult::EVENT_CONSUMED)
    {
        return EventHandlerResult::EVENT_CONSUMED;
    }
    return superkeysHandler.onKeyswitchEvent(mapped_key, key_addr, keyState);
}

bool KeyRoleManager::hasPendingRoleKey()
{
    return qukeys.hasPendingQukey() || superkeysHandler.hasPendingSuperkey();
}

bool KeyRoleManager::isReplayingQueuedEvent()
{
    return qukeys.isReplayingQueuedEvent();
}

EventHandlerResult KeyRoleManager::onFocusEvent(const char *command)
{
    EventHandlerResult result = EventHandlerResult::OK;

    if (qukeys.onFocusEvent(command) == EventHandlerResult::EVENT_CONSUMED)
    {
        return EventHandlerResult::EVENT_CONSUMED;
    }
    result = superkeysHandler.onFocusEvent(command);

    if (strncmp_P(command, "superkeys.", 10) != 0) return EventHandlerResult::OK;

    if (strcmp_P(command + 10, "map") == 0)
    {

        if (::Focus.isEOL())
        {
            send_sk_map();
        }
        else
        {
            uint16_t pos = 0;
            Key key;
            Superkey::action_config_t new_action_config;
            const Superkey::superkey_config_t * p_superkey_config;
            const Superkey::action_config_t * p_action_config;

            while (!::Focus.isEOL() && pos < Utils::SUPER_KEY_COUNT * ACTIONS_IN_SUPERKEY)
            {
                ::Focus.read(key);

                new_action_config.flags = key.getFlags();
                new_action_config.keyCode = key.getKeyCode();

                p_superkey_config = &p_keyrole_config->superkeys[ pos / ACTIONS_IN_SUPERKEY ];
                p_action_config = &p_superkey_config->actions[ pos % ACTIONS_IN_SUPERKEY ];

                cfgmem_key_save( p_action_config, &new_action_config );

                pos++;
            }

            /* Rebuild the handlers and the dual-function slots. The keymap keeps the Superkey codes as written */
            setup_keys();
        }
        result = EventHandlerResult::EVENT_CONSUMED;
    }

    return result;
}

EventHandlerResult KeyRoleManager::beforeReportingState()
{
    if (qukeys.beforeReportingState() == EventHandlerResult::EVENT_CONSUMED)
    {
        return EventHandlerResult::EVENT_CONSUMED;
    }
    return superkeysHandler.beforeReportingState();
}

/****************************************************/
/*                   Config Memory                  */
/****************************************************/

void KeyRoleManager::cfgmem_key_save( const Superkey::action_config_t * p_action_config, Superkey::action_config_t * p_action )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( p_action_config, p_action, sizeof(Superkey::action_config_t) );
    ASSERT_DYGMA( result == RESULT_OK, "ConfigManager.config_item_update failed" );

    UNUSED( result );
}

void KeyRoleManager::cfgmem_superkey_reset( const Superkey::superkey_config_t * p_superkey_config)
{
    uint8_t i;

    Superkey::action_config_t idle_action =
    {
        .flags = 0xFF,
        .keyCode = 0xFF,
    };

    for (i = 0; i < ACTIONS_IN_SUPERKEY; i++)
    {
        cfgmem_key_save( &p_superkey_config->actions[i], &idle_action );
    }
}

void KeyRoleManager::cfgmem_keyrole_reset( void )
{
    uint8_t i;

    for (i = 0; i < Utils::SUPER_KEY_COUNT; i++)
    {
        cfgmem_superkey_reset( &p_keyrole_config->superkeys[i] );
    }
}

} // namespace plugin
} // namespace kaleidoscope
kaleidoscope::plugin::KeyRoleManager keyRoleManager;
