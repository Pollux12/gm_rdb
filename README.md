# gm_rdb

`gm_rdb` adds remote debugging to Garry's Mod.

It provides two modules:

- `rdb` for the server
- `rdb_client` for the client

Basic Lua API:

- `rdb.activate([port])`
- `rdb.deactivate()`
- `rdb.Version`
- `rdb.VersionNum`

Useful launch flags:

- `-rdb_allow_remote` to allow non-local debugger connections
- `-rdb_pause_on_activate [seconds]` to pause when debugging starts

Default ports are `21111` for `rdb` and `21112` for `rdb_client`.

Debugger protocol `gmod-3` adds realm-specific runtime support used by the GLuaLS MCP host:

- The server module exposes bounded map, gamemode, dedicated/single-player, and player-count status.
- The client module can capture one JPEG from its attached graphical client through the engine `jpeg` command.
- Screenshot files use internal names, are limited to 1 MiB, time out after five seconds, and are deleted after success or failure.
- The server build does not expose screenshot capture, and the client build does not expose server runtime status.

## Build

This project uses CMake (`cmake/FindGarrysmodCommon.cmake`) and [`garrysmod_common`](https://github.com/danielga/garrysmod_common). 64-bit builds use a second submodule, `third-party/sourcesdk-minimal-x64`, which carries the 64-bit Source SDK link libraries.

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --target rdb rdb_client --config Release
```

On Windows pass `-A Win32` or `-A x64` at configure time to select the architecture. CI runs the exact same configuration — there are no CI-only dependency downloads.

Set `AUTOINSTALL` and `AUTOINSTALL_CLIENT` to copy the built files into a Garry's Mod folder.

## Notes

- Windows and Linux are covered by CI.
- macOS support is experimental.
- Keep the historical `.dll` suffix on macOS module names.

## Credits

This project started as a fork of [danielga/gm_rdb](https://github.com/danielga/gm_rdb).
Debugger protocol tooling: [LRDB](https://github.com/satoren/vscode-lrdb)
