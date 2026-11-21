# USB ⇄ Bluetooth Serial Bridge for Flipper Zero

Send text between a computer and your Android phone, with the Flipper in the middle.

```
 PC terminal / script  ⇄ USB serial port ⇄  Flipper Zero  ⇄ Bluetooth ⇄  Phone (Chrome)
```

Plugged in with the app open, the Flipper shows up on the PC as an ordinary serial (COM) port.
Anything a program writes to that port appears on the phone, and anything you type on the phone
comes out of the port. Both directions work at the same time.

**Open the phone terminal:** https://carlantaya.github.io/phonekb/serial/

## Setup

1. **Flipper app:** copy [`release/bt_serial_mntm-012.fap`](../release/bt_serial_mntm-012.fap) to the SD card at `apps/USB/`.
   It appears as **Apps → USB → USB BT Serial**. (Built for Momentum mntm-012, API 87.1.)
2. **Phone:** open the link above in Chrome on Android. Menu → **Install app** makes it work offline.
3. Plug the Flipper into the PC and open **USB BT Serial** on it.
4. On the phone tap **Connect** and pick **SerialBT &lt;your Flipper's name&gt;**.
   The first time, Android asks for a PIN: type the 6 digits shown on the Flipper.
5. On the PC, open the Flipper's port in any serial terminal, or run the included script:

   ```
   pip install pyserial
   python pc_chat.py           # finds the Flipper automatically
   ```

   Other options: PuTTY (Serial, the COM port from Device Manager), Arduino IDE's Serial Monitor,
   `screen /dev/ttyACM0` on Linux, `screen /dev/tty.usbmodemflip_*` on Mac. The baud rate doesn't matter.

## Phone terminal

- Received text in white, your sent text in blue (turn **Echo** off to hide it)
- **Line ending** added to each message: none, LF, CR or CR+LF
- **Hex** view, **Time** stamps, **Copy**, **Save** to a .txt file, **Clear**
- **Ctrl+C** button (sends byte 0x03, handy for interrupting a program on a device console)
- **⋯** → send a pasted block of text, or raw bytes typed as hex (`48 65 6C 6C 6F 0D 0A`)
- Shows whether a program on the PC currently has the port open

## Flipper screen

Shows the phone connection, whether the PC port is open, and byte counters each way.
**Back** exits (the Flipper's normal USB/qFlipper connection comes back). **Hold Left** forgets the paired phone.

## Good to know

- While the app runs, qFlipper and the Flipper CLI can't use the USB port, and the official Flipper phone app can't connect.
- Text from the PC is only forwarded while the phone is connected; anything sent before that is dropped (counted as "drop" on the Flipper).
- Text from the phone only reaches the PC while a program has the port open (DTR on). Otherwise it's dropped too.
- Speed: a few KB per second, which is plenty for text and logs but slow for files.
- The Bluetooth link is PIN-paired and encrypted.

## How it works

The Flipper app (`flipper_app/`) switches the USB port to a single CDC serial port and runs its own
Bluetooth profile, using the Flipper serial GATT service under a separate name and address.
Every Bluetooth packet starts with one type byte:

| Direction | Type | Meaning |
|---|---|---|
| phone → Flipper | `01` + data | text for the PC |
| phone → Flipper | `02` | hello: phone just connected |
| phone → Flipper | `03` + n | size report: n bytes of the probe arrived |
| Flipper → phone | `01` + data | text from the PC |
| Flipper → phone | `02` + 242 bytes | probe, to measure how big a packet the link carries |
| Flipper → phone | `03` + flags | status: bit 0 = PC port open |

Phone → Flipper uses the serial service's flow-control characteristic: the Flipper hands out 1024 bytes of
credit and tops it up once that data has gone out over USB, so big pastes never overflow its buffer.

### Building
```
pip install ufbt
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json
cd serial/flipper_app && ufbt        # output in dist/bt_serial.fap
```
