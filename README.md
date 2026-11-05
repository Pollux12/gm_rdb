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

## Running Lua from the debugger

The server module's `run_lua` request runs a chunk through the `_GLUALS.runLua(realm, code)` bridge that the GLuaLS extension installs in `lua/autorun/debug.lua`.

- When the bridge returns `true, detail` and `detail` is a string, the response `result` is that string, parsed as JSON when it is valid JSON.
- With `"mode": "<name>"` (lowercase letters and `_`) the bridge is called as `_GLUALS.runLua(realm, code, mode)`. GLuaLS's MCP tools use modes to get return values, printed output and Lua errors back as JSON, and to run code on the next server tick.
- Requests are handled inside the Lua debug hook, and LuaJIT runs no hooks there, so code run directly by `run_lua` never stops at breakpoints. Code the bridge schedules for the next tick does.
- Older bridges ignore the extra argument and return no result.

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

- While no debugger is connected, the module keeps the last 2000 console lines and Lua errors and sends them to the next client that connects, so errors from before attaching still show up.
- Windows and Linux are covered by CI.
- macOS support is experimental.
- Keep the historical `.dll` suffix on macOS module names.

## Credits

This project started as a fork of [danielga/gm_rdb](https://github.com/danielga/gm_rdb).
Debugger protocol tooling: [LRDB](https://github.com/satoren/vscode-lrdb)
