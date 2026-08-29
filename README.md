# Xiaomi Pen Status

Touch-friendly Qt control panel and tray utility for stylus power-state attributes exported by
`qcom_battmgr`. It reconstructs the detected pen address from `pen_mac_h` and
`pen_mac_l`, checks that exact address through BlueZ when the pen is docked,
and starts a short LE discovery/pair/connect attempt while the detected pen is
pairable. The UI follows the system locale and supports Chinese and English.
Set `XIAOMI_PEN_LANG=zh` or `XIAOMI_PEN_LANG=en` to override it.

On Xiaomi Sheng, the Buttons page stores separate mappings for writing/hover
and air-pointer use. Primary and secondary buttons can retain their native pen
behavior or trigger mouse clicks, navigation, undo/redo, screenshot, desktop
overview, or no action. Mappings are sent to `xiaomi-sheng-thp` through
`/run/xiaomi-sheng-thp/button-mapping.sock` and are reapplied after service or
session restarts.

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

When the detected pen is a connected Xiaomi Focus Pen Pro, the window also
shows five pinch-force levels. The selected level is stored per user and is
applied after each Bluetooth reconnection, once the touch service has completed
its Focus Pen Pro initialization.

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
