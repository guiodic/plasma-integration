# Native Qt Wayland session restoration

With Qt 6.12 or newer, the platform theme persists the native Qt session ID in
the application's configuration, under `XdgSessionManagement`. Before the first
top-level surface is mapped, it passes the saved ID to Qt's platform session
manager factory. Qt owns the Wayland session and all toplevel associations.

Qt 6.12 does not provide a public API to set this initial ID. The bridge therefore
uses QtGui private APIs to replace the initial, unused platform session manager.
This dependency is isolated in `kwaylandsessionmanagement.cpp`. Older Qt versions
keep their existing behavior. Explicit `-session` restores and
`Qt::AA_DisableSessionManager` are respected.

Applications must provide stable window identities before showing their windows.
For widgets, use `QWidget::setWindowRole()`; `KMainWindow` already provides roles.
Window titles are not used as identities. A per-application runtime lock prevents
a second live process from restoring or overwriting the first process's session.
The compositor decides which state to preserve and how long it remains available.

## Manual integration test

Run in a Plasma Wayland session with Qt 6.12+, KWin supporting
`xdg-session-management-v1`, and the newly built platform theme on the plugin path.
The helper uses KWin scripting over the session bus to move and inspect the actual
compositor window geometry. It is not suitable for an Xvfb or offscreen CTest run.

```sh
export QT_QPA_PLATFORM=wayland QT_QPA_PLATFORMTHEME=kde
export QT_PLUGIN_PATH=/path/to/build/bin
test_bin=/path/to/build/bin/sessionrestorationtest6
result_dir=$(mktemp -d)
app_id=plasma-session-test-$$

"$test_bin" --app-id "$app_id" --move --output "$result_dir/first.json"
"$test_bin" --app-id "$app_id" --expected "$result_dir/first.json" \
    --output "$result_dir/restored.json"
```

Both commands must exit successfully and report `"passed": true`. The first moves
the frame to `(180, 140)`; the second checks the same native session ID and complete
frame geometry after closing and reopening the application. Each process uses a
different title but the same stable window role, `main`.

For a concurrent-instance check, keep the first process alive using `--hold 45000`,
then run another `--move` process with the same application ID. Its session ID must
differ. After both exit, reopening with `--expected first.json` must still restore
the first session. `WAYLAND_DEBUG=client` can additionally confirm that the window
association is made by Qt's native session manager.
