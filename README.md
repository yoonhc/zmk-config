# Tofu60 BLE 3.0 ZMK config

ZMK firmware configuration for a Tofu60 BLE 3.0 with an ANSI layout and a 7U spacebar.

## Source

The complete `config/boards/shields/klink_kbd` shield directory was imported from the OpenKBD [`260511`](https://github.com/openkbd/zmk-config-tofu60-ble-3.0/tree/260511/config/boards/shields/klink_kbd) release:

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

## First-flash verification

Do not use **Restore Stock Settings** in ZMK Studio during the first smoke test. A normal firmware flash preserves the existing Studio keymap, so this first test should cover boot, USB and BLE input, Studio connectivity, LED behavior, and whether every physical ANSI 7U switch produces input.

Because the saved Studio keymap remains active, this smoke test does not verify the stock OpenKBD key assignments. Testing the stock keymap requires a separate, deliberate **Restore Stock Settings** operation, which removes the saved Studio keymap changes.

New mod-morph and tap-dance instances will be added separately after this baseline firmware has been built and tested.
