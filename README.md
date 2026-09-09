# F-Zero VS for Ares

A plain C11 UDP race server and a pinned Ares v148 integration porting the original
F-Zero VS position-replication patches. macOS Apple Silicon is the tested desktop;
the networking code uses POSIX APIs for macOS/Linux.

```sh
make all
make test
make test-4p RUN_ARGS='--auto-drive --capture-frames --duration 35'
```

For manual play, use `make test-2p` and assign separate controllers with
`RUN_ARGS='--input keyboard --input gamepad:0'`. Keyboard mappings and gamepad
configuration are described in [local testing](docs/local-testing.md).

Open **Multiplayer → F-Zero VS…** to host a room, browse LAN rooms,
or join a hostname/IP address. The host runs the C server inside Ares and owns P1.
The same window provides race controls, roster, and client/server diagnostics. A compact overlay also appears in fullscreen. Each local
instance has an isolated profile and log directory under `.local-tests/`.

The US original-revision ROM must be supplied locally; the adapter validates its
SHA-256 before patching. Patches modify the loaded ROM only and are restored on
unload, disconnect and the next race.

- [Hosting, joining and lifecycle](docs/hosting.md)
- [Build and patch workflow](docs/building.md)
- [Desktop harness, controls and remote server](docs/local-testing.md)
- [Protocol, memory map and validation scope](docs/protocol.md)

Source: `server/` (C server), `client/` (C++ game/network state machine), `shared/`
(wire codec), `patches/ares/` (emulator integration). `original/` is the legacy
reference. `vendor/` and `build/` are generated.

This preserves the original client-owned physics model. Remote cars use adaptive
snapshot interpolation, but there is no rollback, velocity extrapolation, or
authoritative collision/lap validation. Local rendering and network tests do not
replace longer manual races or testing across the internet.
