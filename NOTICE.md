# Notice

LG Input Hook NoMute is a derivative work. This file lists what it is based on and what was changed.

## Based on

| Project | Author | License | Used for |
| --- | --- | --- | --- |
| [lginputhook](https://github.com/Simon34545/lginputhook) | Simon34545 (Copyright (c) 2023, simon_34545) | BSD-3-Clause (`app/LICENSE.md`) | The webOS app, config UI, service and packaging in `app/`. The original README is kept as `app/README.upstream.md`, the button list as `app/Button IDs.md` |
| [inputhookpp](https://github.com/sundermann/inputhookpp) | sundermann | GPL-3.0 (`LICENSE`, `native/LICENSE`) | The native hook in `native/` (upstream commit `2e808c1`) |
| [inputhook script](https://gist.github.com/Informatic/319bcaf94436b9136904473ca4f4ec9c) | Informatic | see gist | The original idea, credited by lginputhook |
| [ezinject](https://github.com/smx-smx/ezinject) | Stefano Moioli (smx-smx) | see `third_party/ezinject-COPYING.txt` | Injects the hook library; the `ezinject` binary is built from commit `607055c` and shipped in the `.ipk` |

The app icon (`app/assets/`) comes from lginputhook, where it is credited to Rembihnutur.

The native library is linked statically with Frida Gum and the libraries of the openlgtv buildroot toolchain
(for example GLib, libffi, PCRE2, libdwarf, Capstone) and with nlohmann/json. These stay under their own licenses,
see the upstream projects. Toolchain: [openlgtv/buildroot-nc4](https://github.com/openlgtv/buildroot-nc4).

## Changes

Changes to `native/` (see `patches/inputhookpp-nomute.patch`):

- New optional rule field `states`: a rule only applies to events with the listed states.
- New action `tap`: auto-repeat events (`state=2`) are replaced by a release followed by a press.
- New `replace` option `asTap`: the replacement key is sent as one tap on the real press; the real release and the
  repeats are dropped.
- The configuration file moved to `/home/root/.config/lginputhook-nomute/keybinds.json`, so the app can coexist on
  disk with the original (do not run both at once).

Changes to `app/`:

- New app ID `org.webosbrew.inputhook.nomute`, port `1843`, own autostart script, log and marker files.
- New "Magic Remote 2025 Settings" section with two toggles that manage the rules for key codes `114` and `1124`.
- The UI layout was made compact so the logs fit on one TV screen; a "Repeat as taps" action was added to the key
  bind editor.
- Default configuration: Volume Down "hold without mute" enabled.

Modifications are released under the same terms as the files they modify (BSD-3-Clause for `app/`, GPL-3.0 for
`native/` and `patches/`). Modifications Copyright (c) 2026 frankthecrank.
