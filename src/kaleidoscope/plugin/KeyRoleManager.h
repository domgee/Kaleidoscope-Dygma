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

#ifndef KEY_ROLE_MANAGER_H
#define KEY_ROLE_MANAGER_H

#include "kbd_core.h"

#include "Kaleidoscope-FocusSerial.h"
#include "Kaleidoscope-Ranges.h"
#include "Superkeys/includes.h"
#include "Superkeys/Superkey/Superkey.h"
#include <Kaleidoscope.h>
#include <cstdint>

namespace kaleidoscope
{
namespace plugin
{

class KeyRoleManager : public kaleidoscope::Plugin
{
  public:

    typedef struct PACK
    {
        Superkey::superkey_config_t superkeys[Utils::SUPER_KEY_COUNT];
    } keyrole_config_t;

  public:

    /* Record the keymap storage format, appended last to the config cache. Treat any value other than KEYMAP_FORMAT_SUPERKEYS as a keymap
     * that may still hold the DUM and DUL codes older firmware converted tap+hold Superkeys into */
    typedef struct PACK
    {
        uint8_t keymap_format;
        uint8_t reserved[3];
    } keymap_format_config_t;

    static_assert(sizeof(keymap_format_config_t) == 4, "keymap_format_config_t must stay 4 bytes");

    static constexpr uint8_t KEYMAP_FORMAT_SUPERKEYS = 1;

    KeyRoleManager();

    /**
     * Handle a key switch event and perform KeyRoleManager processing.
     *
     * This method is responsible for handling key switch events and performing KeyRoleManager processing when applicable.
     * It checks whether a key switch event corresponds to a physical key, ignores injected keys from other plugins,
     * and handles normal key events. It also manages the release of configured tap or held keys when modifiers are pressed or released.
     *
     * @param mapped_key The mapped key associated with the key switch event.
     * @param key_addr The address of the key for the key switch event.
     * @param keyState The state of the key switch event.
     * @return An EventHandlerResult indicating the result of the event handling.
     */
    EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t keyState);

    /**
     * Handle focus events and configuration commands for KeyRoleManager.
     *
     * This method processes focus events and configuration commands related to KeyRoleManager.
     * It allows you to manage and modify various settings for KeyRoleManager, such as key mappings, wait times, timeouts, hold durations, and overlap thresholds.
     *
     * @param command A character array representing the command for configuration.
     * @return An EventHandlerResult indicating the result of the event handling.
     */
    EventHandlerResult onFocusEvent(const char *command);

    /**
     * Prepare KeyRoleManager state before reporting key events.
     *
     * This method is responsible for managing the state of KeyRoleManager just before reporting key events
     *
     * @return An EventHandlerResult indicating the result of the operation.
     */
    EventHandlerResult beforeReportingState();

    /**
     * @brief Is a qukey or a superkey still waiting to resolve?
     *
     * Plugins registered before keyRoleManager (Autoshift, CapsWordTrigger)
     * consume their own key events, which makes those keys invisible to the
     * Qukeys chord detection and to the SuperKeys interruption detection --
     * re-injecting them later does not help, because both filter INJECTED.
     *
     * The result is that e.g. a qukey with hold=Ctrl followed by an Autoshift
     * key would emit a bare 'c' and only later the Ctrl, instead of Ctrl+C.
     *
     * Those plugins must call this before retaining a key and, when it returns
     * true, let the key flow through untouched as an ordinary key.
     */
    bool hasPendingRoleKey();

    /**
     * @brief Is the current event a replay of a queued one, not a fresh press?
     *
     * Qukeys re-emits the events it swallowed without the INJECTED flag, so a
     * toggle-on may be a replay. Plugins that decide something on toggle-on
     * must not re-decide on the replay.
     */
    bool isReplayingQueuedEvent();

    EventHandlerResult onSetup();

    /**
     * Initialize KeyRoleManager with storage settings.
     *
     * This method sets up the KeyRoleManager by configuring storage parameters.
     * It allocates a storage slice in EEPROM to store the KeyRoleManager settings, including size and offset.
     * After setting up the storage, it updates the KeyRoleManager cache to ensure consistency with the stored values.
     */
    void setup_superkeys(void);

    bool is_qukey(Key key);

    void setup_keys();

    /* Return true when the configured Superkey slot is dual-function, so Qukeys plays it with its full tap and hold actions */
    bool is_dual_function_slot(uint16_t slot) const;

    /* Return true with the tap and hold actions, flags included, when key is a dual-function Superkey. Leave tap and hold untouched otherwise */
    bool dual_function_superkey(Key key, Key &tap, Key &hold) const;

    bool is_dual_function_superkey(Key key) const;

  private:

    const keyrole_config_t * p_keyrole_config = nullptr;
    const keymap_format_config_t * p_keymap_format_config = nullptr;

    /* Keep one bit per configured Superkey slot that plays as a dual-function key */
    uint8_t df_slots_[(Utils::MAX_SUPER_KEYS_ACTIVE + 7) / 8];

    uint8_t sk_index;
    uint8_t configured_superkeys;

    enum class key_roles_t
    {
        NONE,
        QUKEY,
        SUPERKEY
    };

    void rebuild_dual_function_slots();

    static bool dual_function_rule(const Superkey::superkey_config_t *p_superkey_config);

    uint16_t legacy_qukey_code(uint8_t slot);

    void migrate_legacy_keymap();

//    void config();

    void send_sk_map();

    bool is_only_modifier(Key key);

    bool has_layer_change(Key key);

    void dumpKeymap();

    uint16_t setup_qukey();

    uint16_t replace_superkey_with_qukey(const Key *action_0, const Key *action_1);

    uint16_t calculate_qukey_code(uint32_t base_raw, uint16_t selected_keycode);


    void set_active_sk();

    void save_sk(const Key *action_0, const Key *action_1, const Key *action_2, const Key *action_3, const Key *action_4);

    void cfgmem_key_save( const Superkey::action_config_t * p_action_config, Superkey::action_config_t * p_action );
    void cfgmem_superkey_reset( const Superkey::superkey_config_t * p_superkey_config);
    void cfgmem_keyrole_reset( void );
};

} // namespace plugin
} // namespace kaleidoscope

extern kaleidoscope::plugin::KeyRoleManager keyRoleManager;

#endif // KEY_ROLE_MANAGER_H
