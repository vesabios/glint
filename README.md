# Glint

Native host for running Glint-compatible WASM cartridges. Uses SDL2 for display, input, and audio. Embeds [wasm3](https://github.com/nicollasricas/nicollasricas/wasm3) as the WASM interpreter.

## Usage

```
./glint <cartridge.wasm> [music.xm]
./glint --fs <cartridge.wasm>          # enable file I/O (sandboxed to wasm directory)
```

**F5** to hot-reload the cartridge. **Alt+Enter** for fullscreen. Gamepad supported automatically.

## WASM Cartridge API

Your cartridge exports:

| Export              | Signature  | Description                        |
|---------------------|------------|------------------------------------|
| `game_init()`       | `void()`   | Called once at startup             |
| `game_update()`     | `void()`   | Called each frame                  |
| `game_render()`     | `void()`   | Called each frame after update     |
| `get_framebuffer()` | `→ i32`    | Returns pointer to RGBA8 framebuffer |
| `input_keys`        | global     | 256-byte array for key/mouse/gamepad state |

Optional exports for custom framebuffer size:

| Export              | Signature  |
|---------------------|------------|
| `get_fb_width()`    | `→ i32`    |
| `get_fb_height()`   | `→ i32`    |

Optional exports for audio (XM/MOD playback via ibxm):

| Export                    | Signature    |
|---------------------------|--------------|
| `get_audio_file_buffer()` | `→ i32`      |
| `audio_init(size)`        | `i32 → i32`  |
| `audio_fill()`            | `→ i32`      |
| `get_audio_out_buffer()`  | `→ i32`      |

Host-provided imports (module `"env"`):

| Import          | Signature        |
|-----------------|------------------|
| `host_random()` | `→ u32`          |
| `host_sinf(f)`  | `f32 → f32`      |
| `host_cosf(f)`  | `f32 → f32`      |
| `host_sqrtf(f)` | `f32 → f32`      |
| `host_atan2f(y,x)` | `f32,f32 → f32` |
| `host_fabsf(f)` | `f32 → f32`      |
| `host_fmodf(x,y)` | `f32,f32 → f32` |

With `--fs` enabled:

| Import                      | Signature              |
|-----------------------------|------------------------|
| `host_file_read(path,dest,max)` | `→ i32` (bytes read) |
| `host_file_write(path,src,len)` | `→ i32` (0=ok)       |
| `host_file_size(path)`          | `→ i32`              |
| `host_file_list(dir,dest,max)`  | `→ i32` (count)      |

## Building

### Linux

```
sudo apt install libsdl2-dev   # Debian/Ubuntu
make
```

### macOS

```
brew install sdl2
make
```

### Windows (MSYS2 / MinGW)

Install [MSYS2](https://www.msys2.org/), then in a MinGW64 shell:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2
make
```

### Windows (Visual Studio / CMake)

1. Install [CMake](https://cmake.org/download/) and [SDL2 development libraries](https://github.com/libsdl-org/SDL/releases) (or use vcpkg: `vcpkg install sdl2`)
2. Build:

```
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=C:/path/to/SDL2   # or skip if using vcpkg
cmake --build . --config Release
```

With vcpkg:

```
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

## Input Key Map

Keys 0-255 mapped into `input_keys[]`:

| Range     | Keys                          |
|-----------|-------------------------------|
| 0-3       | Arrow keys (L/R/U/D)         |
| 4-17      | Space, Shift, Ctrl, Alt, Tab, Enter, Backspace, Escape, Insert, Delete, Home, End, PgUp, PgDn |
| 32-57     | A-Z                          |
| 64-73     | 0-9                          |
| 80-91     | F1-F12                       |
| 96-106    | Punctuation (-, =, [, ], \, ;, ', `, ,, ., /) |
| 120-123   | Analog sticks (LX, LY, RX, RY) — 128=center |
| 128-141   | Gamepad (A/B/X/Y, LB/RB, LT/RT, Start/Select, DPad) |
| 150-157   | Mouse (X lo/hi, Y lo/hi, L/R/M buttons, scroll) |

## License

wasm3: MIT. ibxm: BSD. Glint host: do whatever you want.
