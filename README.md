# Tofu60 BLE 3.0 ZMK config

ZMK firmware configuration for a Tofu60 BLE 3.0 with an ANSI layout and a 7U spacebar.

## Source

The complete `boards/shields/klink_kbd` shield directory was imported from the OpenKBD [`260511`](https://github.com/openkbd/zmk-config-tofu60-ble-3.0/tree/260511/config/boards/shields/klink_kbd) release:

- Repository: `openkbd/zmk-config-tofu60-ble-3.0`
- Tag: `260511`
- Commit: `11d8b7dbe90fc98d653d04a17b54134c0cda5240`

This repository combines the Tofu60 `260511` shield with verified, pinned ZMK and Klink revisions. The upstream release used moving `main` revisions, so the dependency commits below should not be interpreted as an exact record of the dependencies used when the `260511` release was originally built.

| Dependency | Pinned revision |
| --- | --- |
| [`zmkfirmware/zmk`](https://github.com/zmkfirmware/zmk) | `4ec69cb7e658590adf6354027aca789b364a70c5` |
| [`yangdigi/zmk-keyboards-klink`](https://github.com/yangdigi/zmk-keyboards-klink) | `e80a8335563aec32a8f2c47cba697077291bfb99` |

The reusable GitHub Actions workflow is pinned to the same official ZMK revision as the west manifest.

### Upstream ZMK compatibility change

The OpenKBD `260511` shield enables `CONFIG_ZMK_HID_GENERIC_DESKTOP_USAGES_BASIC`, a setting that only exists in the `yangdigi/zmk` fork. The setting has been removed from `boards/klink.conf` so this configuration can build against official ZMK. The stock Tofu60 keymap does not reference the additional Generic Desktop usages supplied by that fork. All other files in the imported shield directory remain identical to the `260511` tag.

## Build

Push a commit or run the **Build ZMK firmware** workflow manually from the repository's Actions page. The workflow builds this target:

- Board: `klink`
- Shield: `tofu60_ble_v3_ansi_7u`
- ZMK Studio transport: `studio-rpc-usb-uart`
- ZMK Studio: enabled

Download the generated firmware artifact from the completed workflow run and use the contained UF2 file to flash the keyboard.

## Personal keymap and Studio behaviors

`config/tofu60_ble_v3_ansi_7u.keymap` is the compiled stock keymap source for this build. It reproduces the effective OpenKBD `260511` common and ANSI keymaps, then includes the personal behavior definitions from `config/behaviors/custom_behaviors.dtsi`. The only changed stock binding is the base-layer Caps Lock position, which now uses `&caps_multi`.

### Caps Multi-Role

`Caps Multi-Role` is a custom, zero-parameter Studio behavior with a 260 ms logical deadline measured from the first press timestamp:

| Input | Result |
| --- | --- |
| Tap once | `LANG1`, emitted when the sequence resolves |
| Hold once, or press another key while holding | Left Control |
| Tap, then press again within 260 ms | Momentary Layer 3 while the second press is held |

If another key is pressed while a single tap is pending, `LANG1` is emitted before that key is processed. A second Caps press before the deadline cancels the pending `LANG1`; the second press and release do not execute the binding at the Caps position on Layer 3. At or after the logical deadline, the previous sequence resolves first and the next Caps press starts a new sequence.

The implementation lives in `src/behaviors/behavior_caps_multi_role.c` and delegates its outputs to the three Devicetree child bindings `<&kp LANG1>, <&kp LCTRL>, <&mo 3>`. Timer callbacks validate both the current state and sequence generation so a cancelled callback cannot emit a stale `LANG1`.

The following zero-parameter mod-morph behaviors are available for assignment in ZMK Studio:

| Studio name | Binding | Normal press | Modified press |
| --- | --- | --- | --- |
| `BSPC / DEL` | `&bspc_del` | Backspace | Left Shift: Delete; Right Shift: Right Shift + Delete |
| `Ctrl+; Left` | `&ctl_semi_left` | `;` | Left Ctrl: Left Arrow |
| `Ctrl+Quote Right` | `&ctl_sqt_right` | `'` | Left Ctrl: Right Arrow |
| `Ctrl+[ Up` | `&ctl_lbkt_up` | `[` | Left Ctrl: Up Arrow |
| `Ctrl+/ Down` | `&ctl_fslh_down` | `/` | Left Ctrl: Down Arrow |
| `Ctrl+] Caps` | `&ctl_rbkt_caps` | `]` | Left Ctrl: Caps Lock |

The custom behavior nodes intentionally do not use `/omit-if-no-ref/`. They must remain in the compiled Devicetree even though the stock bindings do not reference them, so ZMK Studio can offer them for assignment. Only Left Ctrl triggers the Ctrl-based morphs; Right Ctrl continues to produce the original Ctrl-modified key.

After flashing this firmware, reconnect ZMK Studio and assign `Caps Multi-Role` to the current Caps position, then assign the mod-morph behaviors to the desired keys. Do not use **Restore Stock Settings** for this step; the existing persistent Studio keymap and Layer 3 can be kept and edited in place.

## First-flash verification

Do not use **Restore Stock Settings** in ZMK Studio during the first smoke test. A normal firmware flash preserves the existing Studio keymap, so this first test should cover boot, USB and BLE input, Studio connectivity, LED behavior, and whether every physical ANSI 7U switch produces input.

Because the saved Studio keymap remains active, this smoke test does not verify the stock OpenKBD key assignments. Testing the stock keymap requires a separate, deliberate **Restore Stock Settings** operation, which removes the saved Studio keymap changes.
