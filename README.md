# Glint

Native host for running Glint-compatible WASM cartridges. Uses SDL2 for display, input, and audio. Embeds [wasm3](https://github.com/wasm3/wasm3) as the WASM interpreter.

## Usage

```
./glint <cartridge.wasm> [music.xm]
./glint --fs <cartridge.wasm>          # enable file I/O (sandboxed to wasm directory)
./glint --fs /path/to/root cart.wasm   # custom file I/O root
```

- **F5** — hot-reload the cartridge (re-runs `game_init`)
- **Alt+Enter** — toggle fullscreen
- Gamepad detected and mapped automatically

## Framebuffer & Resolution

Cartridges render into a flat RGBA8 framebuffer in WASM linear memory. The host reads it each frame via `get_framebuffer()` and blits it to the window.

### Default resolution

The default framebuffer is **640x360** (16:9). The host window opens at **2x** pixel scale (1280x720 physical).

### Cartridge-defined resolution

If the cartridge exports `get_fb_width()` and `get_fb_height()`, the host calls them at startup and uses the returned values instead. This is how cartridges like the editor (1280x720) or painter run at non-default resolutions.

On the cartridge side, `platform.h` defines `WIDTH`/`HEIGHT` with `#ifndef` guards so they can be overridden at compile time:

```c
// platform.h
#ifndef WIDTH
#define WIDTH  640
#endif
#ifndef HEIGHT
#define HEIGHT 360
#endif
#define FB_SIZE (WIDTH * HEIGHT * 4)  // RGBA8
```

### Overriding resolution via make flags

When compiling a cartridge with the wscii Makefile, pass `-DWIDTH=X -DHEIGHT=Y` in CFLAGS:

```
# Default game (640x360)
make game.wasm

# Editor at 1280x720
make editor/editor.wasm   # already has -DWIDTH=1280 -DHEIGHT=720

# Custom resolution
make CFLAGS="-DWIDTH=960 -DHEIGHT=540" game.wasm
```

The cartridge must also export `get_fb_width()` and `get_fb_height()` so the native host knows the actual size. Without these exports, the host falls back to 640x360 regardless of what the cartridge allocated.

### Window scaling

The host opens at `fb_width * 2` by `fb_height * 2` (the `SCALE` constant in `glint.c`). The window is resizable and snaps to integer scales — dragging the window edge rounds to the nearest whole multiple of the framebuffer size, so pixels stay crisp. `SDL_RenderSetLogicalSize` handles the upscale.

### Framebuffer format

The framebuffer is a contiguous array of `width * height * 4` bytes in **ABGR8888** order (matches `SDL_PIXELFORMAT_ABGR8888`):

| Byte | Channel |
|------|---------|
| 0    | R       |
| 1    | G       |
| 2    | B       |
| 3    | A       |

Pixel at (x, y): `fb[(y * width + x) * 4]`

### Browser hosting

Cartridges also run in-browser via `host.html`, which does the same thing in JS/WebGL: reads `WIDTH`/`HEIGHT` from the compiled constants, allocates a canvas, and uploads the framebuffer as a texture each frame. The browser host hardcodes 640x360 and auto-scales to fit the window. For custom resolutions in-browser, edit the `W`/`H` constants in `host.html` or query `get_fb_width()`/`get_fb_height()` from the WASM exports.

## Cartridge API

### Required exports

| Export              | Signature  | Description                        |
|---------------------|------------|------------------------------------|
| `game_init()`       | `void()`   | Called once at startup             |
| `game_update()`     | `void()`   | Called each frame                  |
| `game_render()`     | `void()`   | Called each frame after update     |
| `get_framebuffer()` | `→ i32`    | Returns pointer to RGBA8 framebuffer |
| `input_keys`        | global     | 256-byte array, written by host each frame |

### Optional exports

| Export              | Signature  | Description                        |
|---------------------|------------|------------------------------------|
| `get_fb_width()`    | `→ i32`    | Framebuffer width (default: 640)   |
| `get_fb_height()`   | `→ i32`    | Framebuffer height (default: 360)  |

Audio (XM/MOD/S3M playback via ibxm):

| Export                    | Signature    | Description                        |
|---------------------------|--------------|------------------------------------|
| `get_audio_file_buffer()` | `→ i32`      | Pointer to buffer for loading XM data |
| `audio_init(size)`        | `i32 → i32`  | Initialize audio from buffer (0=ok) |
| `audio_fill()`            | `→ i32`      | Generate next chunk of samples     |
| `get_audio_out_buffer()`  | `→ i32`      | Pointer to generated audio samples |

### Host-provided imports (module `"env"`)

Math:

| Import             | Signature        |
|--------------------|------------------|
| `host_random()`    | `→ u32`          |
| `host_sinf(f)`     | `f32 → f32`      |
| `host_cosf(f)`     | `f32 → f32`      |
| `host_sqrtf(f)`    | `f32 → f32`      |
| `host_atan2f(y,x)` | `f32,f32 → f32` |
| `host_fabsf(f)`    | `f32 → f32`      |
| `host_fmodf(x,y)`  | `f32,f32 → f32` |

File I/O (only available with `--fs`):

| Import                           | Signature | Description                              |
|----------------------------------|-----------|------------------------------------------|
| `host_file_read(path,dest,max)`  | `→ i32`   | Read file into dest, returns bytes read  |
| `host_file_write(path,src,len)`  | `→ i32`   | Write src to file, returns 0 on success  |
| `host_file_size(path)`           | `→ i32`   | Returns file size in bytes               |
| `host_file_list(dir,dest,max)`   | `→ i32`   | List directory, null-separated names     |

All file paths are relative to the cartridge directory (or `--fs` root). Absolute paths and `..` traversal are rejected.

## Building

### Linux

```
sudo apt install libsdl2-dev   # Debian/Ubuntu
# or: sudo dnf install SDL2-devel   # Fedora
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

1. Install [CMake](https://cmake.org/download/) and SDL2 via [vcpkg](https://vcpkg.io/):

```
vcpkg install sdl2
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

Or point CMake at a manual SDL2 install:

```
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=C:/SDL2
cmake --build . --config Release
```

## Input Key Map

The host writes into `input_keys[256]` each frame. Cartridge reads it directly.

| Range     | Keys                          |
|-----------|-------------------------------|
| 0-3       | Arrow keys (L, R, U, D)      |
| 4-17      | Space, Shift, Ctrl, Alt, Tab, Enter, Backspace, Escape, Insert, Delete, Home, End, PgUp, PgDn |
| 32-57     | A-Z                          |
| 64-73     | 0-9                          |
| 80-91     | F1-F12                       |
| 96-106    | Punctuation: `-` `=` `[` `]` `\` `;` `'` `` ` `` `,` `.` `/` |
| 120-123   | Analog sticks: LX, LY, RX, RY (128=center, 0=full neg, 255=full pos) |
| 128-141   | Gamepad: A, B, X, Y, LB, RB, LT, RT, Start, Select, DPad U/D/L/R |
| 150-157   | Mouse: X lo/hi, Y lo/hi, L/R/M buttons, scroll (128=none, <128=up, >128=down) |

Mouse position is split across two bytes each:
```c
int mx = input_keys[150] | (input_keys[151] << 8);
int my = input_keys[152] | (input_keys[153] << 8);
```

## Hot Reload

Press **F5** to reload the `.wasm` file from disk. The host tears down the WASM runtime, reloads the file, re-links all imports, and calls `game_init()`. Game state is lost (the cartridge starts fresh). This is useful during development — rebuild the cartridge, hit F5, and see changes immediately without restarting the host.

## License

wasm3: MIT. ibxm: BSD. Glint host: do whatever you want.
