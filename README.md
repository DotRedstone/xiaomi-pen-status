# Xiaomi Pen Status

Small Qt tray utility for stylus power-state attributes exported by
`qcom_battmgr`. It reconstructs the detected pen address from `pen_mac_h` and
`pen_mac_l`, checks that exact address through BlueZ when the pen is docked,
and starts a short LE discovery/pair/connect attempt while the detected pen is
pairable. The UI follows the system locale and supports Chinese and English.
Set `XIAOMI_PEN_LANG=zh` or `XIAOMI_PEN_LANG=en` to override it.

Build:

```sh
qmake6
make
```

Run:

```sh
./xiaomi-pen-status
```

Closing the window keeps the tray icon running. Use the tray menu to show the
window again or quit.

Optional local desktop integration:

```sh
install -Dm755 xiaomi-pen-status ~/.local/bin/xiaomi-pen-status
install -Dm644 xiaomi-pen-status.desktop ~/.local/share/applications/xiaomi-pen-status.desktop
install -Dm644 xiaomi-pen-status.svg ~/.local/share/icons/hicolor/scalable/apps/xiaomi-pen-status.svg
```

The default sysfs path is:

```text
/sys/devices/platform/pmic-glink/pmic_glink.power-supply.0/xiaomi
```

Override it with `XIAOMI_PEN_SYSFS` when testing.
