# Kindle Terminal

Extract the ZIP into the Kindle's internal storage so KUAL can find
`extensions/kindle-terminal/config.xml`. Eject the Kindle, reopen KUAL, and tap
**Terminal**. The shell starts in `/mnt/us`.

- The top row has Esc, Tab, Ctrl, Alt, and all four arrows.
- Tap Ctrl or Alt, then the next key. For example, Ctrl then C interrupts a command.
- Tap Fn to show Home, End, PgUp, and PgDn on the arrows and F1–F12 on the number row.
- Type `exit` and press Enter to close the terminal.

This extension uses the unmodified ARM hard-float build of kTerm 2.6 for Kindle
firmware with GTK2 and X11. Launch diagnostics go to `extensions/kindle-terminal/launch.log`.

## Build

Download the [official ARMhf ZIP](https://github.com/bfabiszewski/kterm/releases/download/v2.6/kterm-kindle-2.6-armhf.zip)
and the [pinned kTerm source archive](https://codeload.github.com/bfabiszewski/kterm/tar.gz/2a0622a7a1d5a7796848505e97be49465db934ea).
Run from the repository root.

```sh
python3 kits/kindle-terminal/build.py kterm-kindle-2.6-armhf.zip kterm-v2.6-source.tar.gz out/kindle-terminal.zip
```

The builder checks both input hashes. kTerm is distributed under GPLv3 or
later. See [kTerm 2.6 source and license](https://github.com/bfabiszewski/kterm/tree/v2.6).
