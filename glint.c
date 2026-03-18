/*
 * Glint Native Host — runs any Glint-compatible .wasm cartridge
 *
 * Usage: ./glint <cartridge.wasm> [music.xm]
 *
 * Expected WASM exports:
 *   game_init()          — called once at startup
 *   game_update()        — called each frame
 *   game_render()        — called each frame after update
 *   get_framebuffer()    — returns pointer to RGBA8 framebuffer
 *   input_keys           — 256-byte array for key state
 *
 * Host-provided imports (module "env"):
 *   host_random()  -> u32
 *   host_sinf(f32) -> f32
 *   host_cosf(f32) -> f32
 *   host_sqrtf(f32) -> f32
 *   host_atan2f(f32,f32) -> f32
 *   host_fabsf(f32) -> f32
 *   host_fmodf(f32,f32) -> f32
 *
 * Audio: optional XM/MOD/S3M file loaded alongside cartridge.
 * Uses ibxm for playback at 48kHz stereo via SDL audio.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
 #include <windows.h>
 #include <direct.h>
#else
 #include <dirent.h>
 #include <sys/stat.h>
#endif

#include <SDL2/SDL.h>
#include "wasm3/wasm3.h"

// --- Constants ---
#define FB_WIDTH_DEFAULT  640
#define FB_HEIGHT_DEFAULT 360
#define SCALE     2

static int fb_width  = FB_WIDTH_DEFAULT;
static int fb_height = FB_HEIGHT_DEFAULT;
#define STACK_SIZE (4 * 1024 * 1024)

// --- Key mapping (matches platform.h) ---
#define KEY_LEFT      0
#define KEY_RIGHT     1
#define KEY_UP        2
#define KEY_DOWN      3
#define KEY_SPACE     4
#define KEY_SHIFT     5
#define KEY_CTRL      6
#define KEY_ALT       7
#define KEY_TAB       8
#define KEY_ENTER     9
#define KEY_BACKSPACE 10
#define KEY_ESCAPE    11
#define KEY_INSERT    12
#define KEY_DELETE    13
#define KEY_HOME      14
#define KEY_END       15
#define KEY_PAGEUP    16
#define KEY_PAGEDOWN  17
#define KEY_A         32
#define KEY_PERIOD    105
#define KEY_COMMA     104
#define KEY_SLASH     106
#define KEY_SEMICOLON 101
#define KEY_QUOTE     102
#define KEY_MINUS     96
#define KEY_EQUALS    97
#define KEY_LBRACKET  98
#define KEY_RBRACKET  99
#define KEY_BACKSLASH 100
#define KEY_BACKTICK  103

// Mouse
#define KEY_MOUSE_X_LO   150
#define KEY_MOUSE_X_HI   151
#define KEY_MOUSE_Y_LO   152
#define KEY_MOUSE_Y_HI   153
#define KEY_MOUSE_LEFT    154
#define KEY_MOUSE_RIGHT   155
#define KEY_MOUSE_MIDDLE  156
#define KEY_MOUSE_SCROLL  157

// Gamepad
#define KEY_PAD_A      128
#define KEY_PAD_B      129
#define KEY_PAD_X      130
#define KEY_PAD_Y      131
#define KEY_PAD_LB     132
#define KEY_PAD_RB     133
#define KEY_PAD_LT     134
#define KEY_PAD_RT     135
#define KEY_PAD_START  136
#define KEY_PAD_SELECT 137
#define KEY_PAD_UP     138
#define KEY_PAD_DOWN   139
#define KEY_PAD_LEFT   140
#define KEY_PAD_RIGHT  141

// --- WASM state ---
static IM3Environment env;
static IM3Runtime     runtime;
static IM3Module      module;
static IM3Function    fn_init;
static IM3Function    fn_update;
static IM3Function    fn_render;
static IM3Function    fn_get_fb;
static IM3Function    fn_audio_file_buf;
static IM3Function    fn_audio_init;
static IM3Function    fn_audio_fill;
static IM3Function    fn_audio_out_buf;

static uint8_t *wasm_mem;
static uint32_t wasm_mem_size;
static uint8_t *input_keys_ptr;
static int32_t  input_keys_offset = -1;  // offset into WASM memory
static const char *wasm_path_g;   // file-scope for reload
static uint8_t *wasm_data_g;      // current loaded wasm data

// --- RNG state ---
static uint32_t rng_state;

static uint32_t xorshift32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// --- Host functions ---
m3ApiRawFunction(host_random) {
    m3ApiReturnType(uint32_t);
    m3ApiReturn(xorshift32());
}

m3ApiRawFunction(host_sinf) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, x);
    m3ApiReturn(sinf(x));
}

m3ApiRawFunction(host_cosf) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, x);
    m3ApiReturn(cosf(x));
}

m3ApiRawFunction(host_sqrtf) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, x);
    m3ApiReturn(sqrtf(x));
}

m3ApiRawFunction(host_atan2f) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, y);
    m3ApiGetArg(float, x);
    m3ApiReturn(atan2f(y, x));
}

m3ApiRawFunction(host_fabsf) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, x);
    m3ApiReturn(fabsf(x));
}

m3ApiRawFunction(host_fmodf) {
    m3ApiReturnType(float);
    m3ApiGetArg(float, x);
    m3ApiGetArg(float, y);
    m3ApiReturn(fmodf(x, y));
}

// --- File I/O ---
// Base directory for file access. Set to directory containing the .wasm file.
// All paths from the guest are resolved relative to this.
static char fs_base[512] = ".";
static int fs_enabled = 0;

static int fs_resolve(const char *guest_path, char *out, int out_len) {
    /* Reject absolute paths and parent traversal */
    if (!guest_path || guest_path[0] == '/' || guest_path[0] == '\\') return -1;
    if (strstr(guest_path, "..")) return -1;
    int n = snprintf(out, out_len, "%s/%s", fs_base, guest_path);
    if (n >= out_len) return -1;
    return 0;
}

m3ApiRawFunction(host_file_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArgMem(char *, dest);
    m3ApiGetArg(int32_t, max_len);

    if (!fs_enabled) { m3ApiReturn(-1); }

    char resolved[1024];
    if (fs_resolve(path, resolved, sizeof(resolved)) < 0) { m3ApiReturn(-1); }

    FILE *f = fopen(resolved, "rb");
    if (!f) { m3ApiReturn(-1); }

    size_t n = fread(dest, 1, max_len, f);
    fclose(f);
    m3ApiReturn((int32_t)n);
}

m3ApiRawFunction(host_file_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArgMem(const char *, src);
    m3ApiGetArg(int32_t, len);

    if (!fs_enabled) { m3ApiReturn(-1); }

    char resolved[1024];
    if (fs_resolve(path, resolved, sizeof(resolved)) < 0) { m3ApiReturn(-1); }

    FILE *f = fopen(resolved, "wb");
    if (!f) { m3ApiReturn(-1); }

    size_t n = fwrite(src, 1, len, f);
    fclose(f);
    m3ApiReturn(n == (size_t)len ? 0 : -1);
}

m3ApiRawFunction(host_file_size) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char *, path);

    if (!fs_enabled) { m3ApiReturn(-1); }

    char resolved[1024];
    if (fs_resolve(path, resolved, sizeof(resolved)) < 0) { m3ApiReturn(-1); }

#ifdef _WIN32
    FILE *f = fopen(resolved, "rb");
    if (!f) { m3ApiReturn(-1); }
    fseek(f, 0, SEEK_END);
    int32_t sz = (int32_t)ftell(f);
    fclose(f);
    m3ApiReturn(sz);
#else
    struct stat st;
    if (stat(resolved, &st) < 0) { m3ApiReturn(-1); }
    m3ApiReturn((int32_t)st.st_size);
#endif
}

m3ApiRawFunction(host_file_list) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const char *, dir);
    m3ApiGetArgMem(char *, dest);
    m3ApiGetArg(int32_t, max_len);

    if (!fs_enabled) { m3ApiReturn(-1); }

    char resolved[1024];
    if (fs_resolve(dir, resolved, sizeof(resolved)) < 0) { m3ApiReturn(-1); }

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", resolved);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) { m3ApiReturn(-1); }

    int count = 0, pos = 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        int nlen = (int)strlen(fd.cFileName);
        if (pos + nlen + 1 > max_len) break;
        memcpy(dest + pos, fd.cFileName, nlen);
        pos += nlen;
        dest[pos++] = '\0';
        count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    m3ApiReturn(count);
#else
    DIR *d = opendir(resolved);
    if (!d) { m3ApiReturn(-1); }

    int count = 0, pos = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden + . + .. */
        int nlen = strlen(ent->d_name);
        if (pos + nlen + 1 > max_len) break;
        memcpy(dest + pos, ent->d_name, nlen);
        pos += nlen;
        dest[pos++] = '\0';
        count++;
    }
    closedir(d);
    m3ApiReturn(count);
#endif
}

// --- SDL scancode to Glint key index ---
static int sdl_to_key(SDL_Scancode sc) {
    // Arrow keys
    if (sc == SDL_SCANCODE_LEFT)      return KEY_LEFT;
    if (sc == SDL_SCANCODE_RIGHT)     return KEY_RIGHT;
    if (sc == SDL_SCANCODE_UP)        return KEY_UP;
    if (sc == SDL_SCANCODE_DOWN)      return KEY_DOWN;

    // Modifiers and special
    if (sc == SDL_SCANCODE_SPACE)     return KEY_SPACE;
    if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) return KEY_SHIFT;
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL)   return KEY_CTRL;
    if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)     return KEY_ALT;
    if (sc == SDL_SCANCODE_TAB)       return KEY_TAB;
    if (sc == SDL_SCANCODE_RETURN)    return KEY_ENTER;
    if (sc == SDL_SCANCODE_BACKSPACE) return KEY_BACKSPACE;
    if (sc == SDL_SCANCODE_ESCAPE)    return KEY_ESCAPE;
    if (sc == SDL_SCANCODE_INSERT)    return KEY_INSERT;
    if (sc == SDL_SCANCODE_DELETE)    return KEY_DELETE;
    if (sc == SDL_SCANCODE_HOME)      return KEY_HOME;
    if (sc == SDL_SCANCODE_END)       return KEY_END;
    if (sc == SDL_SCANCODE_PAGEUP)    return KEY_PAGEUP;
    if (sc == SDL_SCANCODE_PAGEDOWN)  return KEY_PAGEDOWN;

    // Letters a-z -> 32..57
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return KEY_A + (sc - SDL_SCANCODE_A);

    // Numbers 0-9 -> 64..73
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 65 + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return 64;

    // F1-F12 -> 80..91
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 80 + (sc - SDL_SCANCODE_F1);

    // Punctuation
    if (sc == SDL_SCANCODE_MINUS)        return KEY_MINUS;
    if (sc == SDL_SCANCODE_EQUALS)       return KEY_EQUALS;
    if (sc == SDL_SCANCODE_LEFTBRACKET)  return KEY_LBRACKET;
    if (sc == SDL_SCANCODE_RIGHTBRACKET) return KEY_RBRACKET;
    if (sc == SDL_SCANCODE_BACKSLASH)    return KEY_BACKSLASH;
    if (sc == SDL_SCANCODE_SEMICOLON)    return KEY_SEMICOLON;
    if (sc == SDL_SCANCODE_APOSTROPHE)   return KEY_QUOTE;
    if (sc == SDL_SCANCODE_GRAVE)        return KEY_BACKTICK;
    if (sc == SDL_SCANCODE_COMMA)        return KEY_COMMA;
    if (sc == SDL_SCANCODE_PERIOD)       return KEY_PERIOD;
    if (sc == SDL_SCANCODE_SLASH)        return KEY_SLASH;

    return -1;
}

// --- Gamepad mapping ---
static SDL_GameController *gamepad = NULL;

static void update_gamepad(void) {
    if (!gamepad || !input_keys_ptr) return;

    input_keys_ptr[KEY_PAD_A]      = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_A);
    input_keys_ptr[KEY_PAD_B]      = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_B);
    input_keys_ptr[KEY_PAD_X]      = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_X);
    input_keys_ptr[KEY_PAD_Y]      = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_Y);
    input_keys_ptr[KEY_PAD_LB]     = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    input_keys_ptr[KEY_PAD_RB]     = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    input_keys_ptr[KEY_PAD_START]  = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_START);
    input_keys_ptr[KEY_PAD_SELECT] = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_BACK);
    input_keys_ptr[KEY_PAD_UP]     = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    input_keys_ptr[KEY_PAD_DOWN]   = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    input_keys_ptr[KEY_PAD_LEFT]   = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    input_keys_ptr[KEY_PAD_RIGHT]  = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    // Analog sticks — match browser: deadzone -> 128, active -> 1..255
    // Game treats 0 as "no gamepad", 128 as center
    #define STICK_DEADZONE 6553  // ~0.2 of 32767, matches browser DEADZONE=0.2
    int16_t lx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
    int32_t lmag_sq = (int32_t)lx * lx + (int32_t)ly * ly;
    if (lmag_sq > (int32_t)STICK_DEADZONE * STICK_DEADZONE) {
        int alx = (lx / 258) + 128; // /258 maps -32768..32767 to ~1..255
        int aly = (ly / 258) + 128;
        if (alx < 1) alx = 1; if (alx > 255) alx = 255;
        if (aly < 1) aly = 1; if (aly > 255) aly = 255;
        input_keys_ptr[120] = (uint8_t)alx;
        input_keys_ptr[121] = (uint8_t)aly;
    } else {
        input_keys_ptr[120] = 128;
        input_keys_ptr[121] = 128;
    }

    // Right analog stick
    int16_t rx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
    int32_t rmag_sq = (int32_t)rx * rx + (int32_t)ry * ry;
    if (rmag_sq > (int32_t)STICK_DEADZONE * STICK_DEADZONE) {
        int arx = (rx / 258) + 128;
        int ary = (ry / 258) + 128;
        if (arx < 1) arx = 1; if (arx > 255) arx = 255;
        if (ary < 1) ary = 1; if (ary > 255) ary = 255;
        input_keys_ptr[122] = (uint8_t)arx;
        input_keys_ptr[123] = (uint8_t)ary;
    } else {
        input_keys_ptr[122] = 128;
        input_keys_ptr[123] = 128;
    }

    #undef STICK_DEADZONE

    // Triggers as LT/RT
    int16_t lt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int16_t rt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    input_keys_ptr[KEY_PAD_LT] = lt > 8000 ? 1 : 0;
    input_keys_ptr[KEY_PAD_RT] = rt > 8000 ? 1 : 0;
}

// --- Load WASM file ---
static uint8_t* load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *out_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*out_size);
    fread(buf, 1, *out_size, f);
    fclose(f);
    return buf;
}

#define CHECK(result, msg) do { \
    if (result) { fprintf(stderr, "Glint error: %s — %s\n", msg, result); return 1; } \
} while(0)

// --- Hot reload WASM ---
static int reload_wasm(void) {
    printf("Glint: Reloading %s...\n", wasm_path_g);

    // Load new file first — if it fails, keep running old code
    size_t new_size;
    uint8_t *new_data = load_file(wasm_path_g, &new_size);
    if (!new_data) {
        fprintf(stderr, "Glint: Reload failed — could not read file\n");
        return 0;
    }

    // Tear down old runtime (frees module too)
    m3_FreeRuntime(runtime);
    free(wasm_data_g);

    wasm_data_g = new_data;

    // Re-create runtime
    runtime = m3_NewRuntime(env, STACK_SIZE, NULL);
    if (!runtime) { fprintf(stderr, "Glint: Reload failed — runtime\n"); return -1; }

    M3Result result;
    result = m3_ParseModule(env, &module, wasm_data_g, new_size);
    if (result) { fprintf(stderr, "Glint: Reload parse error: %s\n", result); return -1; }

    result = m3_LoadModule(runtime, module);
    if (result) { fprintf(stderr, "Glint: Reload load error: %s\n", result); return -1; }

    // Re-link host functions
    m3_LinkRawFunction(module, "env", "host_random", "i()",     host_random);
    m3_LinkRawFunction(module, "env", "host_sinf",   "f(f)",    host_sinf);
    m3_LinkRawFunction(module, "env", "host_cosf",   "f(f)",    host_cosf);
    m3_LinkRawFunction(module, "env", "host_sqrtf",  "f(f)",    host_sqrtf);
    m3_LinkRawFunction(module, "env", "host_atan2f", "f(ff)",   host_atan2f);
    m3_LinkRawFunction(module, "env", "host_fabsf",  "f(f)",    host_fabsf);
    m3_LinkRawFunction(module, "env", "host_fmodf",  "f(ff)",   host_fmodf);
    m3_LinkRawFunction(module, "env", "host_file_read",  "i(**i)",  host_file_read);
    m3_LinkRawFunction(module, "env", "host_file_write", "i(**i)",  host_file_write);
    m3_LinkRawFunction(module, "env", "host_file_size",  "i(*)",    host_file_size);
    m3_LinkRawFunction(module, "env", "host_file_list",  "i(**i)",  host_file_list);

    // Re-find exports
    result = m3_FindFunction(&fn_init,   runtime, "game_init");
    if (result) { fprintf(stderr, "Glint: Reload error: %s\n", result); return -1; }
    result = m3_FindFunction(&fn_update, runtime, "game_update");
    if (result) { fprintf(stderr, "Glint: Reload error: %s\n", result); return -1; }
    result = m3_FindFunction(&fn_render, runtime, "game_render");
    if (result) { fprintf(stderr, "Glint: Reload error: %s\n", result); return -1; }
    result = m3_FindFunction(&fn_get_fb, runtime, "get_framebuffer");
    if (result) { fprintf(stderr, "Glint: Reload error: %s\n", result); return -1; }

    // Re-find optional audio exports
    m3_FindFunction(&fn_audio_file_buf, runtime, "get_audio_file_buffer");
    m3_FindFunction(&fn_audio_init, runtime, "audio_init");
    m3_FindFunction(&fn_audio_fill, runtime, "audio_fill");
    m3_FindFunction(&fn_audio_out_buf, runtime, "get_audio_out_buffer");

    // Re-query framebuffer dimensions
    {
        IM3Function fn_fbw, fn_fbh;
        if (!m3_FindFunction(&fn_fbw, runtime, "get_fb_width") &&
            !m3_FindFunction(&fn_fbh, runtime, "get_fb_height")) {
            uint32_t w = 0, h = 0;
            const void *w_ret[] = { &w };
            const void *h_ret[] = { &h };
            if (!m3_Call(fn_fbw, 0, NULL) && !m3_GetResults(fn_fbw, 1, w_ret) &&
                !m3_Call(fn_fbh, 0, NULL) && !m3_GetResults(fn_fbh, 1, h_ret)) {
                if (w > 0 && w <= 4096 && h > 0 && h <= 4096) {
                    fb_width = w;
                    fb_height = h;
                }
            }
        }
    }

    // Get WASM memory and input_keys
    wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
    IM3Global g_input = m3_FindGlobal(module, "input_keys");
    if (g_input) {
        M3TaggedValue val;
        m3_GetGlobal(g_input, &val);
        input_keys_offset = val.value.i32;
        input_keys_ptr = wasm_mem + input_keys_offset;
    } else {
        input_keys_ptr = NULL;
        input_keys_offset = -1;
    }

    // Call game_init
    result = m3_CallV(fn_init);
    if (result) { fprintf(stderr, "Glint: Reload game_init error: %s\n", result); return -1; }

    wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
    // Refresh input_keys_ptr after init (memory may have grown)
    if (input_keys_offset >= 0)
        input_keys_ptr = wasm_mem + input_keys_offset;
    printf("Glint: Reload complete.\n");
    return 1;
}

// --- Audio ring buffer ---
#define AUDIO_RING_SIZE (48000 * 2)  /* 1 second stereo */
static int16_t audio_ring[AUDIO_RING_SIZE];
static int audio_ring_read = 0;
static int audio_ring_write = 0;
static int audio_active = 0;
static SDL_mutex *audio_mutex = NULL;

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int samples = len / sizeof(int16_t);  /* total int16s (L+R interleaved) */

    SDL_LockMutex(audio_mutex);
    for (int i = 0; i < samples; i++) {
        if (audio_ring_read != audio_ring_write) {
            out[i] = audio_ring[audio_ring_read];
            audio_ring_read = (audio_ring_read + 1) % AUDIO_RING_SIZE;
        } else {
            out[i] = 0;
        }
    }
    SDL_UnlockMutex(audio_mutex);
}

/* Fill the ring buffer from WASM audio. Called from main thread. */
static void audio_pump(void) {
    if (!audio_active) return;

    /* How much space in ring buffer? */
    SDL_LockMutex(audio_mutex);
    int used = (audio_ring_write - audio_ring_read + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
    SDL_UnlockMutex(audio_mutex);

    /* Keep at least ~4000 stereo samples buffered (8000 int16s) */
    while (used < 8000) {
        M3Result res = m3_CallV(fn_audio_fill);
        if (res) break;
        uint32_t count = 0;
        m3_GetResultsV(fn_audio_fill, &count);
        if (count <= 0) break;

        /* Get output buffer pointer */
        res = m3_CallV(fn_audio_out_buf);
        if (res) break;
        uint32_t out_ptr = 0;
        m3_GetResultsV(fn_audio_out_buf, &out_ptr);

        wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
        int16_t *src = (int16_t *)(wasm_mem + out_ptr);
        int n = count * 2; /* stereo pairs -> int16 count */

        SDL_LockMutex(audio_mutex);
        for (int i = 0; i < n; i++) {
            int next = (audio_ring_write + 1) % AUDIO_RING_SIZE;
            if (next == audio_ring_read) break; /* full */
            audio_ring[audio_ring_write] = src[i];
            audio_ring_write = next;
        }
        used = (audio_ring_write - audio_ring_read + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
        SDL_UnlockMutex(audio_mutex);
    }
}

// --- Main ---
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: glint [--fs [root]] <cartridge.wasm> [music.xm]\n");
        return 1;
    }

    /* Parse flags */
    int arg_idx = 1;
    const char *fs_root_override = NULL;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "--fs") == 0) {
            fs_enabled = 1;
            arg_idx++;
            // Optional: next arg is fs root if it doesn't look like a .wasm file
            if (arg_idx < argc && argv[arg_idx][0] != '-') {
                int len = strlen(argv[arg_idx]);
                if (len < 5 || strcmp(argv[arg_idx] + len - 5, ".wasm") != 0) {
                    fs_root_override = argv[arg_idx];
                    arg_idx++;
                }
            }
        } else {
            fprintf(stderr, "Unknown flag: %s\n", argv[arg_idx]);
            return 1;
        }
    }
    if (arg_idx >= argc) {
        fprintf(stderr, "Usage: glint [--fs [root]] <cartridge.wasm> [music.xm]\n");
        return 1;
    }
    const char *wasm_path = argv[arg_idx];
    wasm_path_g = wasm_path;

    /* Set fs_base: use --fs override, or directory containing the wasm file */
    if (fs_root_override) {
        strncpy(fs_base, fs_root_override, sizeof(fs_base) - 1);
        fs_base[sizeof(fs_base) - 1] = '\0';
    } else {
        const char *last_slash = strrchr(wasm_path, '/');
        if (last_slash) {
            int dir_len = (int)(last_slash - wasm_path);
            if (dir_len >= (int)sizeof(fs_base)) dir_len = sizeof(fs_base) - 1;
            memcpy(fs_base, wasm_path, dir_len);
            fs_base[dir_len] = '\0';
        } else {
            strcpy(fs_base, ".");
        }
    }
    if (fs_enabled) {
        printf("Glint: File I/O enabled (base: %s)\n", fs_base);
    }

    // Seed RNG
    rng_state = (uint32_t)time(NULL) ^ 0xDEADBEEF;

    // Load WASM
    size_t wasm_size;
    uint8_t *wasm_data = load_file(wasm_path, &wasm_size);
    if (!wasm_data) return 1;
    wasm_data_g = wasm_data;

    printf("Glint: Loading %s (%zu bytes)\n", wasm_path, wasm_size);

    // Init wasm3
    env = m3_NewEnvironment();
    runtime = m3_NewRuntime(env, STACK_SIZE, NULL);
    if (!runtime) { fprintf(stderr, "Failed to create runtime\n"); return 1; }

    M3Result result;
    result = m3_ParseModule(env, &module, wasm_data, wasm_size);
    CHECK(result, "parse module");

    result = m3_LoadModule(runtime, module);
    CHECK(result, "load module");

    // Link host functions
    m3_LinkRawFunction(module, "env", "host_random", "i()",     host_random);
    m3_LinkRawFunction(module, "env", "host_sinf",   "f(f)",    host_sinf);
    m3_LinkRawFunction(module, "env", "host_cosf",   "f(f)",    host_cosf);
    m3_LinkRawFunction(module, "env", "host_sqrtf",  "f(f)",    host_sqrtf);
    m3_LinkRawFunction(module, "env", "host_atan2f", "f(ff)",   host_atan2f);
    m3_LinkRawFunction(module, "env", "host_fabsf",  "f(f)",    host_fabsf);
    m3_LinkRawFunction(module, "env", "host_fmodf",  "f(ff)",   host_fmodf);

    // File I/O — signatures: i(pointer args become i32 in wasm3)
    m3_LinkRawFunction(module, "env", "host_file_read",  "i(**i)",  host_file_read);
    m3_LinkRawFunction(module, "env", "host_file_write", "i(**i)",  host_file_write);
    m3_LinkRawFunction(module, "env", "host_file_size",  "i(*)",    host_file_size);
    m3_LinkRawFunction(module, "env", "host_file_list",  "i(**i)",  host_file_list);

    // Find exported functions
    result = m3_FindFunction(&fn_init,   runtime, "game_init");
    CHECK(result, "find game_init");
    result = m3_FindFunction(&fn_update, runtime, "game_update");
    CHECK(result, "find game_update");
    result = m3_FindFunction(&fn_render, runtime, "game_render");
    CHECK(result, "find game_render");
    result = m3_FindFunction(&fn_get_fb, runtime, "get_framebuffer");
    CHECK(result, "find get_framebuffer");

    // Query framebuffer dimensions (optional exports)
    {
        IM3Function fn_fbw, fn_fbh;
        if (!m3_FindFunction(&fn_fbw, runtime, "get_fb_width") &&
            !m3_FindFunction(&fn_fbh, runtime, "get_fb_height")) {
            uint32_t w = 0, h = 0;
            const void *w_ret[] = { &w };
            const void *h_ret[] = { &h };
            if (!m3_Call(fn_fbw, 0, NULL) && !m3_GetResults(fn_fbw, 1, w_ret) &&
                !m3_Call(fn_fbh, 0, NULL) && !m3_GetResults(fn_fbh, 1, h_ret)) {
                if (w > 0 && w <= 4096 && h > 0 && h <= 4096) {
                    fb_width = w;
                    fb_height = h;
                    printf("Glint: Framebuffer %dx%d (from WASM)\n", fb_width, fb_height);
                }
            }
        }
    }

    // Get WASM memory
    wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
    if (!wasm_mem) { fprintf(stderr, "No WASM memory\n"); return 1; }

    // Find input_keys in WASM memory
    // We need to find the exported global 'input_keys'
    IM3Global g_input = m3_FindGlobal(module, "input_keys");
    if (g_input) {
        M3TaggedValue val;
        m3_GetGlobal(g_input, &val);
        input_keys_offset = val.value.i32;
        input_keys_ptr = wasm_mem + input_keys_offset;
    } else {
        // Fallback: try calling a function or use a known offset
        fprintf(stderr, "Warning: cannot find input_keys export, trying alternate method\n");
        // Some builds export input_keys as a function that returns the pointer
        IM3Function fn_ik;
        result = m3_FindFunction(&fn_ik, runtime, "input_keys");
        if (!result) {
            // It might be exported as a table entry or global with different linkage
            fprintf(stderr, "Warning: input_keys found as function — unusual\n");
        }
        input_keys_ptr = NULL;
        input_keys_offset = -1;
    }

    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Glint",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        fb_width * SCALE, fb_height * SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) { fprintf(stderr, "Window failed: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) { fprintf(stderr, "Renderer failed: %s\n", SDL_GetError()); return 1; }

    SDL_RenderSetLogicalSize(renderer, fb_width, fb_height);

    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        fb_width, fb_height);
    if (!texture) { fprintf(stderr, "Texture failed: %s\n", SDL_GetError()); return 1; }

    // Open first gamepad if available
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gamepad = SDL_GameControllerOpen(i);
            if (gamepad) {
                printf("Glint: Gamepad connected: %s\n", SDL_GameControllerName(gamepad));
                break;
            }
        }
    }

    // Find audio exports (optional)
    int has_audio = 0;
    if (!m3_FindFunction(&fn_audio_file_buf, runtime, "get_audio_file_buffer") &&
        !m3_FindFunction(&fn_audio_init, runtime, "audio_init") &&
        !m3_FindFunction(&fn_audio_fill, runtime, "audio_fill") &&
        !m3_FindFunction(&fn_audio_out_buf, runtime, "get_audio_out_buffer")) {
        has_audio = 1;
    }

    // Call game_init
    printf("Glint: Initializing cartridge...\n");
    result = m3_CallV(fn_init);
    CHECK(result, "game_init()");

    // Refresh memory pointer (init may have grown memory)
    wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);

    // Load XM music if available (look for music.xm next to the wasm file, or as argv[2])
    if (has_audio) {
        const char *xm_path = NULL;
        char xm_auto[512];
        if (arg_idx + 1 < argc) {
            xm_path = argv[arg_idx + 1];
        } else {
            /* Try music.xm in same directory as the wasm file */
            const char *last_slash = strrchr(wasm_path, '/');
            if (last_slash) {
                int dir_len = (int)(last_slash - wasm_path + 1);
                snprintf(xm_auto, sizeof(xm_auto), "%.*smusic.xm", dir_len, wasm_path);
            } else {
                snprintf(xm_auto, sizeof(xm_auto), "music.xm");
            }
            /* Check if it exists */
            FILE *xf = fopen(xm_auto, "rb");
            if (xf) { fclose(xf); xm_path = xm_auto; }
        }

        if (xm_path) {
            size_t xm_size;
            uint8_t *xm_data = load_file(xm_path, &xm_size);
            if (xm_data && xm_size <= 256 * 1024) {
                /* Write XM data into WASM memory */
                result = m3_CallV(fn_audio_file_buf);
                if (!result) {
                    uint32_t buf_ptr = 0;
                    m3_GetResultsV(fn_audio_file_buf, &buf_ptr);
                    wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
                    memcpy(wasm_mem + buf_ptr, xm_data, xm_size);

                    result = m3_CallV(fn_audio_init, (uint32_t)xm_size);
                    if (!result) {
                        uint32_t ret = 0;
                        m3_GetResultsV(fn_audio_init, &ret);
                        if (ret == 0) {
                            printf("Glint: Audio loaded: %s (%zu bytes)\n", xm_path, xm_size);
                            audio_active = 1;
                        } else {
                            printf("Glint: Audio init failed (bad XM?)\n");
                        }
                    }
                }
                free(xm_data);
            } else if (xm_data) {
                printf("Glint: XM file too large (max 256KB)\n");
                free(xm_data);
            }
        }
    }

    // Start SDL audio if we loaded music
    if (audio_active) {
        audio_mutex = SDL_CreateMutex();
        SDL_AudioSpec want = {0}, have;
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_callback;
        if (SDL_OpenAudio(&want, &have) == 0) {
            printf("Glint: Audio started (%dHz)\n", have.freq);
            SDL_PauseAudio(0);
        } else {
            printf("Glint: SDL audio failed: %s\n", SDL_GetError());
            audio_active = 0;
        }
    }

    printf("Glint: Running.\n");

    // Main loop
    int running = 1;
    while (running) {
        // Refresh memory pointer (may have grown) and recompute input_keys
        wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);
        if (input_keys_offset >= 0 && (uint32_t)input_keys_offset + 256 <= wasm_mem_size)
            input_keys_ptr = wasm_mem + input_keys_offset;
        else
            input_keys_ptr = NULL;

        // Clear previous frame's input
        if (input_keys_ptr) memset(input_keys_ptr, 0, 256);

        // Poll SDL events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYUP:
                    if (input_keys_ptr) {
                        int k = sdl_to_key(e.key.keysym.scancode);
                        if (k >= 0 && k < 256) input_keys_ptr[k] = 0;
                    }
                    break;
                case SDL_KEYDOWN:
                    if (input_keys_ptr) {
                        int k = sdl_to_key(e.key.keysym.scancode);
                        if (k >= 0 && k < 256) input_keys_ptr[k] = 1;
                    }
                    // F5 for hot reload
                    if (e.key.keysym.scancode == SDL_SCANCODE_F5) {
                        int r = reload_wasm();
                        if (r < 0) { running = 0; }
                    }
                    // Alt+Enter for fullscreen toggle
                    if (e.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                        (e.key.keysym.mod & KMOD_ALT)) {
                        Uint32 flags = SDL_GetWindowFlags(window);
                        SDL_SetWindowFullscreen(window,
                            (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (input_keys_ptr) {
                        // Accumulate scroll: <128 = up, >128 = down
                        int cur = (int)input_keys_ptr[KEY_MOUSE_SCROLL] - 128;
                        cur -= e.wheel.y; // SDL: positive y = scroll up
                        if (cur < -127) cur = -127;
                        if (cur > 127) cur = 127;
                        input_keys_ptr[KEY_MOUSE_SCROLL] = (uint8_t)(cur + 128);
                    }
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int ww = e.window.data1, wh = e.window.data2;
                        // Find nearest integer scale
                        int sx = (ww + fb_width / 2) / fb_width;
                        int sy = (wh + fb_height / 2) / fb_height;
                        int s = sx < sy ? sx : sy;
                        if (s < 1) s = 1;
                        int snap_w = fb_width * s, snap_h = fb_height * s;
                        if (snap_w != ww || snap_h != wh) {
                            SDL_SetWindowSize(window, snap_w, snap_h);
                        }
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (!gamepad) {
                        gamepad = SDL_GameControllerOpen(e.cdevice.which);
                        if (gamepad)
                            printf("Glint: Gamepad connected: %s\n", SDL_GameControllerName(gamepad));
                    }
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (gamepad && e.cdevice.which == SDL_JoystickInstanceID(
                            SDL_GameControllerGetJoystick(gamepad))) {
                        SDL_GameControllerClose(gamepad);
                        gamepad = NULL;
                        printf("Glint: Gamepad disconnected\n");
                    }
                    break;
            }
        }

        // Keyboard state (for held keys)
        if (input_keys_ptr) {
            const uint8_t *kb = SDL_GetKeyboardState(NULL);
            // Re-read all held keys
            for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
                if (kb[sc]) {
                    int k = sdl_to_key((SDL_Scancode)sc);
                    if (k >= 0 && k < 256) input_keys_ptr[k] = 1;
                }
            }
        }

        // Mouse state
        if (input_keys_ptr) {
            int raw_mx, raw_my;
            Uint32 mb = SDL_GetMouseState(&raw_mx, &raw_my);
            // Convert window coords to logical (framebuffer) coords
            float sx, sy;
            SDL_RenderGetScale(renderer, &sx, &sy);
            SDL_Rect viewport;
            SDL_RenderGetViewport(renderer, &viewport);
            int mx = (int)(raw_mx / sx) - viewport.x;
            int my = (int)(raw_my / sy) - viewport.y;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= fb_width) mx = fb_width - 1;
            if (my >= fb_height) my = fb_height - 1;
            input_keys_ptr[KEY_MOUSE_X_LO] = (uint8_t)(mx & 0xFF);
            input_keys_ptr[KEY_MOUSE_X_HI] = (uint8_t)((mx >> 8) & 0xFF);
            input_keys_ptr[KEY_MOUSE_Y_LO] = (uint8_t)(my & 0xFF);
            input_keys_ptr[KEY_MOUSE_Y_HI] = (uint8_t)((my >> 8) & 0xFF);
            input_keys_ptr[KEY_MOUSE_LEFT]   = (mb & SDL_BUTTON_LMASK) ? 1 : 0;
            input_keys_ptr[KEY_MOUSE_RIGHT]  = (mb & SDL_BUTTON_RMASK) ? 1 : 0;
            input_keys_ptr[KEY_MOUSE_MIDDLE] = (mb & SDL_BUTTON_MMASK) ? 1 : 0;
            input_keys_ptr[KEY_MOUSE_SCROLL] = 128; // reset each frame
        }

        // Gamepad
        update_gamepad();

        // Pump audio ring buffer from WASM
        audio_pump();

        // Call game_update + game_render
        result = m3_CallV(fn_update);
        if (result) { fprintf(stderr, "game_update error: %s\n", result); break; }

        result = m3_CallV(fn_render);
        if (result) { fprintf(stderr, "game_render error: %s\n", result); break; }

        // Get framebuffer pointer
        result = m3_CallV(fn_get_fb);
        if (result) { fprintf(stderr, "get_framebuffer error: %s\n", result); break; }

        uint32_t fb_offset = 0;
        m3_GetResultsV(fn_get_fb, &fb_offset);

        // Refresh memory (may grow)
        wasm_mem = m3_GetMemory(runtime, &wasm_mem_size, 0);

        if (fb_offset + fb_width * fb_height * 4 <= wasm_mem_size) {
            uint8_t *fb = wasm_mem + fb_offset;
            SDL_UpdateTexture(texture, NULL, fb, fb_width * 4);
        }

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    if (audio_active) {
        SDL_CloseAudio();
        SDL_DestroyMutex(audio_mutex);
    }
    if (gamepad) SDL_GameControllerClose(gamepad);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    free(wasm_data_g);

    printf("Glint: Done.\n");
    return 0;
}
