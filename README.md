# Phone Keyboard for Flipper Zero (Momentum)

Phone (Chrome, Android) --Bluetooth--> Flipper Zero --USB cable--> PC (sees a normal keyboard + mouse)

## Files
- `release/phone_kb_mntm-012.fap` – Flipper app for Momentum mntm-012 (API 87)
- `release/phone_kb_mntm-010_011.fap` – Flipper app for Momentum mntm-010 / mntm-011 (API 86)
- `release/phone_kb_mntm-007.fap` – Flipper app for Momentum mntm-007 (API 72); built with `cdefines=["PHONE_KB_LEGACY_GAP"]` in application.fam
- Repo root – the phone controller (a web page using Web Bluetooth): `index.html`, `sw.js`, `manifest.json`, two icons. Live at https://carlantaya.github.io/phonekb/
- `flipper_app/` – Flipper app source (C)

## 1. Install the Flipper app
Check your version on the Flipper (Settings > System Info / About), pick the matching `.fap`, and copy it to the SD card at `apps/USB/`.
If the Flipper says "App Too New" or "App Too Old", don't press Continue: that build doesn't match your firmware. (qFlipper, or the Flipper mobile app's file manager).
It shows up under Apps > USB > Phone Keyboard.

If you're on a different Momentum release and it says "API mismatch", rebuild from source:
```
pip install ufbt
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json
cd flipper_app && ufbt        # output in dist/phone_kb.fap
```

## 2. Host the phone page (Web Bluetooth needs https)
Easiest options:
- GitHub Pages: new repo, upload all 5 files from `phone/`, Settings > Pages > deploy from main.
- Netlify Drop (app.netlify.com/drop): drag the `phone` folder in from a computer.

Open the URL in **Chrome on Android** once with internet, then menu > Install app (or Add to Home screen).
After that it opens and works with no internet at all. If you edit the files later, bump `VERSION` in `sw.js`.

## 3. Use it
1. Plug the Flipper into the PC by USB and open Phone Keyboard on the Flipper.
2. On the phone, tap Connect and pick "PhoneKB <your flipper name>".
3. The first time, Android asks for a PIN; type the 6 digits shown on the Flipper.

Touchpad: drag = move, tap = left click, two-finger tap = right click,
two-finger drag = scroll, tap then touch-and-hold = drag.
Ctrl/Alt/Win/Shift are sticky: tap one, then a key or letter (e.g. Ctrl, then c).
The ⋯ button shows F-keys, media keys, shortcuts, a "paste a block of text" box and speed settings.

On the Flipper: Back = exit, hold Left = forget the paired phone.

## Notes
- While the app runs, qFlipper and the official Flipper phone app can't connect (USB is a keyboard, BLE is in use).
- Text goes out as US-layout key codes. Non-ASCII characters (emoji, accents) are dropped.
- The link is PIN-paired and encrypted, so nearby devices can't inject keystrokes.

## Protocol (phone -> Flipper, serial characteristic 19ed82ae-ed21-4c9d-4145-228e62fe0000)
| bytes | meaning |
|---|---|
| 01 mods key | tap key (HID usage + modifier bitmask) |
| 02 dx dy | mouse move (int8) |
| 03 btn / 04 btn | mouse button down / up (1 L, 2 R, 4 M) |
| 05 d | scroll (int8) |
| 06 | release all |
| 07 lo hi | consumer key (volume, play...) |
| 08 len ascii... | type text |
| 09 mods key / 0A mods key | key down / key up |
