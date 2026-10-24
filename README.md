# LG Input Hook NoMute

Hold **Volume Down** on an LG Magic Remote without the TV muting, and turn the **Accessibility button**
into a real **Mute** button. For rooted LG webOS TVs.

LG's 2025 AI Magic Remote mutes the TV when you hold Volume Down, and there is no setting to turn that off.
This small webOS homebrew app adds two switches for it.

## Settings

The app has a section called **Magic Remote 2025 Settings** with two toggles:

| Toggle | What it does |
| --- | --- |
| **Volume Down: hold without mute** | Holding Volume Down keeps lowering the volume instead of muting the TV. The key repeats are sent as single presses, so the TV never detects a long press. |
| **Accessibility button works as Mute** | The top-right button of the Magic Remote (normally opens the Accessibility menu) toggles Mute instead. Each press is sent as one short tap. |

Changes are saved immediately and take effect without a restart. Below the toggles there is the usual key bind
editor and a live log of the key events, as in the original lginputhook.

## Requirements

- A **rooted** LG webOS TV with the [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) (root enabled).
- Tested on an **LG G5 (webOS 25) with the 2025 AI Magic Remote** only. Other models and remotes may work, but the
  key codes below were read from that setup and are not verified elsewhere.

## Install

1. Download `org.webosbrew.inputhook.nomute_<version>_all.ipk` from the Releases page.
2. Install it, either with
   [Glasshouse](https://github.com/rorygallagher2024/lg-webos-dashboard) (Apps page, install from file) or with
   [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) / `ares-install`.
3. **Restart the TV completely** (unplug it for about 30 seconds; standby is not enough), then open the app once.
   The hook is loaded into the input service at startup and the app registers its autostart on first launch.
4. Switch the toggles you want on.

After every update of the app, restart the TV again. Otherwise the old hook stays in memory.

Do not run it together with the original lginputhook: both would try to hook the same function. Uninstall the
original first and restart the TV.

## Uninstall

Remove the app (Glasshouse or Dev Manager) and restart the TV. Nothing is flashed, the hook only lives in RAM.
Leftovers are `/home/root/.config/lginputhook-nomute/keybinds.json` (harmless without the hook) and the autostart
script `/var/lib/webosbrew/init.d/inputhook-nomute`, which deletes itself at the next boot once the app is gone.

## How it works

The native hook (`native/`) is injected into the input service (`lginput2` and a few others) with
[ezinject](https://github.com/smx-smx/ezinject) and replaces `lginput_uinput_send_button` using Frida Gum.
It reads `/home/root/.config/lginputhook-nomute/keybinds.json` and reloads it when the file changes.

The log of a key press shows `state=1` (press), `state=2` (auto-repeat while held) and `state=0` (release).
On the 2025 Magic Remote the TV turns the repeats of Volume Down into a long press, which mutes it.

- **tap** (Volume Down): every `state=2` event is replaced by a release followed by a press. The system sees
  repeated short presses and never a long press.
- **replace with `asTap`** (Accessibility button): the key code `1124` is replaced by `113` (`KEY_MUTE`) as one
  press and release on the real press. The real release and the repeats are dropped.

### Key bind file

`keybinds.json` maps a **uinput key code** (the `uinput_code` in the log, not the `keyid`) to a rule:

```json
{
  "114":  { "action": "tap", "states": [2] },
  "1124": { "action": "replace", "keycode": 113, "asTap": true }
}
```

| Field | Meaning |
| --- | --- |
| `action` | `pass`, `ignore`, `replace`, `exec`, `launch` (from the original lginputhook) or `tap` (new) |
| `states` | Optional list of states (0-31) the rule applies to. Other states pass through unchanged |
| `asTap` | `replace` only: send the replacement as one tap on the press, drop release and repeats |

The two toggles manage the rules for `114` and `1124`. If you add your own key bind for one of these IDs, the
toggle is disabled and your rule wins.

## Building

The `.ipk` contains a native library built with the openlgtv buildroot toolchain, so building has two steps:

```sh
./scripts/build-native.sh   # downloads the toolchain (~300 MB, checksum verified), builds ezinject + libinputhookpp.so
./scripts/build-ipk.sh      # needs Node.js; builds the web part and packages the .ipk into app/
```

`scripts/build-native.sh` pins the toolchain and the ezinject commit. Building twice gives a bit-identical
`libinputhookpp.so`. `patches/inputhookpp-nomute.patch` is the diff of `native/` against upstream inputhookpp
(commit `2e808c1`).

## Repository layout

| Path | Contents | License |
| --- | --- | --- |
| `native/` | Patched [inputhookpp](https://github.com/sundermann/inputhookpp) | GPL-3.0 |
| `app/` | webOS app (UI and service), based on [lginputhook](https://github.com/Simon34545/lginputhook) | BSD-3-Clause |
| `patches/` | Changes to upstream inputhookpp | GPL-3.0 |
| `third_party/` | Notices of bundled third-party software | see files |

The distributed `.ipk` contains the GPL-3.0 licensed native library, so it is distributed under the GPL-3.0 as a
whole. The corresponding source is this repository. See [NOTICE.md](NOTICE.md).

## Credits

- [Simon34545/lginputhook](https://github.com/Simon34545/lginputhook): the app, the config UI and the idea of a
  button remapper for webOS
- [sundermann/inputhookpp](https://github.com/sundermann/inputhookpp): the native hook this is built on
- [Informatic's original inputhook](https://gist.github.com/Informatic/319bcaf94436b9136904473ca4f4ec9c)
- [smx-smx/ezinject](https://github.com/smx-smx/ezinject): library injection, built with the
  [openlgtv buildroot toolchain](https://github.com/openlgtv/buildroot-nc4)
- [webosbrew](https://github.com/webosbrew): Homebrew Channel and the rooting scene
- Rembihnutur: the app icon (as credited by lginputhook)

Built with the help of Claude (Anthropic's AI assistant).

## Disclaimer

This modifies the behavior of the TV's input service at runtime. Use it at your own risk. It is not affiliated with
or endorsed by LG.
