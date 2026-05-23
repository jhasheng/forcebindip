# forcebindip_cpp

Minimal C++ ForceBindIP-style launcher for local testing.

It starts a target process suspended, injects `bindip_hook.dll`, then resumes the
process. The DLL patches the target process import table for selected Winsock
functions and forces IPv4 sockets to bind to the configured local IP.

This is intended for your own local processes. It does not bypass anti-cheat,
elevation, or process protection.

## Build

Build the same architecture as the target process:

```powershell
cmake -S tools/forcebindip_cpp -B tools/forcebindip_cpp/build-x64 -A x64
cmake --build tools/forcebindip_cpp/build-x64 --config Release
```

For old 32-bit Warcraft III builds:

```powershell
cmake -S tools/forcebindip_cpp -B tools/forcebindip_cpp/build-x86 -A Win32
cmake --build tools/forcebindip_cpp/build-x86 --config Release
```

## Usage

List available interfaces:

```powershell
.\tools\forcebindip_cpp\build-x64\Release\forcebindip_cpp.exe --list-ifaces
```

Start by interface name, description, or GUID:

```powershell
.\tools\forcebindip_cpp\build-x64\Release\forcebindip_cpp.exe --iface "ZeroTier" -- "C:\Path\Warcraft III\x86_64\Warcraft III.exe"
```

Use a config file:

```powershell
.\tools\forcebindip_cpp\build-x86\Release\forcebindip_cpp.exe --config .\tools\forcebindip_cpp\forcebindip_cpp.example.ini
```

If `forcebindip_cpp.ini` exists next to `forcebindip_cpp.exe`, it is loaded by
default. Command line values override config values.

Start by explicit IP:

```powershell
.\tools\forcebindip_cpp\build-x64\Release\forcebindip_cpp.exe --ip 10.27.238.154 -- "C:\Path\Warcraft III\x86_64\Warcraft III.exe"
```

For old 32-bit `war3.exe`, use the `build-x86` output.

Optional explicit DLL path:

```powershell
.\forcebindip_cpp.exe --ip 10.27.238.154 --dll .\bindip_hook.dll -- "C:\Path\war3.exe" -window
```

Config format:

```ini
[forcebindip]
iface=ZeroTier
exe=C:\Path\Warcraft III\war3.exe
cwd=C:\Path\Warcraft III
args=-window -opengl
```

## Notes

- The launcher and target process must have the same bitness.
- The hook only affects standard Winsock imports from `ws2_32.dll`.
- The most important War3 path is `bind(0.0.0.0:6112)`, which this DLL rewrites
  to `bind(<configured-ip>:6112)`.
- If the game obtains Winsock functions dynamically with `GetProcAddress`, this
  minimal IAT hook may not catch that path.
