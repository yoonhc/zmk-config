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

`config/tofu60_ble_v3_ansi_7u.keymap` is the compiled stock keymap source for this build. It starts from the effective OpenKBD `260511` common and ANSI keymaps, includes the personal behavior definitions from `config/behaviors/custom_behaviors.dtsi`, and defines the customized PC, Mac, function, and control-chord layers.

### Caps Multi-Role

The Caps Multi-Role behaviors are custom Studio behaviors with a 260 ms logical deadline measured from the first press timestamp. Fixed variants are zero-parameter behaviors, while Tunable variants accept one overlap parameter. Each PC/Mac output variant is available with three interrupt policies:

| Studio behavior | Tap | Hold | Interrupt policy |
| --- | --- | --- | --- |
| `Caps Multi (PC)` | `LANG1` | Left Control | Hold-preferred: another key press immediately selects Control |
| `Caps Multi (Mac)` | Left Control + Space | Left Control | Hold-preferred: another key press immediately selects Control |
| `Caps Multi - Balanced (PC)` | `LANG1` | Left Control | Release-order: Caps released first selects tap; the other key released first selects Control |
| `Caps Multi - Balanced (Mac)` | Left Control + Space | Left Control | Release-order: Caps released first selects tap; the other key released first selects Control |
| `Caps Multi - Overlap (PC)` | `LANG1` | Left Control | Hybrid: Caps released first before 38 ms selects tap; the first interrupt released first or 38 ms of overlap selects Control |
| `Caps Multi - Overlap (Mac)` | Left Control + Space | Left Control | Hybrid: Caps released first before 38 ms selects tap; the first interrupt released first or 38 ms of overlap selects Control |
| `Caps Multi - Overlap Tunable (PC)` | `LANG1` | Left Control | Same hybrid policy with a Studio-supplied 5–100 ms overlap term |
| `Caps Multi - Overlap Tunable (Mac)` | Left Control + Space | Left Control | Same hybrid policy with a Studio-supplied 5–100 ms overlap term |

For a Balanced instance, position events are buffered while release order is undecided. If Caps is released first, the tap action is emitted before the buffered key. If an interrupted key is released first, or the 260 ms deadline is reached, Control is pressed before the buffered events are replayed. This avoids accidental Control chords when a mechanical switch has not electrically released before the next key press.

Overlap instances use the same buffering but add a 38 ms simultaneous-press deadline. Before that deadline, Caps releasing first selects tap and the first interrupt key releasing first selects Control. Reaching 38 ms while both remain down irreversibly selects Control, which remains held until Caps is released. Only the first interrupt key participates in the release-order decision; later captured keys are replayed under the selected result.

The Tunable variants expose that simultaneous-press deadline as the `Overlap (ms)` behavior parameter in ZMK Studio. Enter a value from 5 through 100 when assigning the behavior; 38 is the recommended starting value. The value is stored with that individual key binding in the persistent Studio keymap. It is sampled when Caps is pressed, so changing it affects the next sequence rather than one already in progress. Invalid values from a manually authored keymap fall back to 38 ms.

See the [Caps Multi - Overlap Tunable behavior specification](docs/caps-multi-overlap-tunable.md) for the complete timing rules, state machine, boundary conditions, and examples.

For every variant, pressing Caps a second time before the deadline cancels the pending tap and activates Momentary Layer 3 until the second Caps release. The second press and release do not execute the binding at the Caps position on Layer 3. At or after the logical deadline, the previous sequence resolves first and the next Caps press starts a new sequence.

The implementation lives in `src/behaviors/behavior_caps_multi_role.c` and delegates its outputs to three Devicetree child bindings. PC instances use `<&kp LANG1>, <&kp LCTRL>, <&mo 3>`; Mac instances use `<&kp LC(SPACE)>, <&kp LCTRL>, <&mo 3>`. Timer callbacks and deferred capture ownership validate the current sequence generation so stale work cannot emit or replay input.

The following zero-parameter mod-morph behaviors are available for assignment in ZMK Studio:

| Studio name | Binding | Normal press | Modified press |
| --- | --- | --- | --- |
| `BSPC / DEL` | `&bspc_del` | Backspace | Left Shift: Delete |
| `Ctrl+; Left` | `&ctl_semi_left` | `;` | Left Ctrl: Left Arrow |
| `Ctrl+Quote Right` | `&ctl_sqt_right` | `'` | Left Ctrl: Right Arrow |
| `Ctrl+[ Up` | `&ctl_lbkt_up` | `[` | Left Ctrl: Up Arrow |
| `Ctrl+/ Down` | `&ctl_fslh_down` | `/` | Left Ctrl: Down Arrow |
| `Ctrl+] Caps` | `&ctl_rbkt_caps` | `]` | Left Ctrl: Caps Lock |

The custom behavior nodes intentionally do not use `/omit-if-no-ref/`. They must remain in the compiled Devicetree even when a stock binding does not reference them, so ZMK Studio can offer them for assignment. Only Left Ctrl triggers the Ctrl-based morphs; Right Ctrl continues to produce the original Ctrl-modified key.

The compiled stock keymap uses `Caps Multi - Overlap Tunable (PC)` on the PC base layer and `Caps Multi - Overlap Tunable (Mac)` on the Mac layer, both with a 38 ms parameter. A normal flash preserves the existing persistent Studio keymap, so it does not automatically replace an existing fixed Caps binding. Either reassign the Caps position to the Tunable behavior with 38 in Studio, or deliberately use **Restore Stock Settings** to load the new compiled stock binding. The fixed 38 ms, hold-preferred, and Balanced variants remain available for comparison.

## First-flash verification

Do not use **Restore Stock Settings** in ZMK Studio during the first smoke test. A normal firmware flash preserves the existing Studio keymap, so this first test should cover boot, USB and BLE input, Studio connectivity, LED behavior, and whether every physical ANSI 7U switch produces input.

Because the saved Studio keymap remains active, this smoke test does not verify the stock OpenKBD key assignments. Testing the stock keymap requires a separate, deliberate **Restore Stock Settings** operation, which removes the saved Studio keymap changes.
