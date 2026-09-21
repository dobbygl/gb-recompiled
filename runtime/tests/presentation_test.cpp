#include "gb_presentation.h"
#include "gbrt.h"
#include "platform_sdl.h"
#include <SDL.h>
#include <SDL_opengles2.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::string calls;
bool predict = false, cover = false;
int consumed = 0, loaded = 0;
uint8_t external = 255;
std::vector<uint8_t> captured;
void require(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (calls=%s)\n", why, calls.c_str());
        std::exit(1);
    }
}
void attributes() {
    calls += 'A';
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
}
bool begin(GBContext *, bool, const uint32_t *) {
    calls += 'B';
    return predict;
}
bool frame(GBContext *, int w, int h, bool) {
    calls += 'F';
    if (cover || predict) {
        glViewport(0, 0, w, h);
        glClearColor(.8f, .1f, .4f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (predict && !cover) {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glActiveTexture(GL_TEXTURE1);
    }
    return cover;
}
bool event(const SDL_Event *e, bool) {
    if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_F2) {
        calls += 'E';
        ++consumed;
        return true;
    }
    return false;
}
void capture(int w, int h, bool) {
    calls += 'C';
    captured.resize(size_t(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, captured.data());
    require(glGetError() == GL_NO_ERROR, "capture runs with a current GL context");
}
void shutdown() {
    calls += 'S';
    require(SDL_GL_GetCurrentContext() != nullptr, "shutdown before GL destruction");
}
void state_loaded(GBContext *) { ++loaded; }
void poll(GBContext *, bool) { gb_platform_set_external_dpad(external); }

} // namespace
int main(int argc, char **argv) {
    SDL_SetMainReady();
    GBConfig config{};
    config.model = GB_MODEL_DMG;
    config.speed_percent = 100;
    GBContext *ctx = gb_context_create(&config);
    require(ctx != nullptr, "context allocation");
    // Procedural cartridge, with RAM but no battery or commercial header data.
    std::array<uint8_t, 32768> rom{};
    rom[0x147] = 2;
    rom[0x149] = 2;
    require(gb_context_load_rom(ctx, rom.data(), rom.size()), "procedural cartridge load");
    require(ctx->eram_size == 8192, "cartridge RAM participates in render invariants");
    std::array<uint32_t, 160 * 144> lcd{};
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++)
            lcd[y * 160 + x] = 0xff000000u | uint32_t(x * 255 / 159) << 16 |
                               uint32_t(y * 255 / 143) << 8 | uint32_t((x ^ y) & 255);
    // Observe the ordinary LCD before swap. EGL may discard the back buffer
    // after swap, so a post-swap read is not a valid baseline on ANGLE.
    GBPresentationHooks observer{};
    observer.api_version = GB_PRESENTATION_API_VERSION;
    observer.struct_size = sizeof(observer);
    observer.before_swap = capture;
    require(gb_platform_set_presentation(&observer), "register capture-only observer");
    require(gb_platform_init(2), "SDL initialization");
    gb_platform_register_context(ctx);
    SDL_SetWindowSize(SDL_GL_GetCurrentWindow(), 480, 320);
    gb_platform_poll_events(ctx);
    gb_platform_render_frame(lcd.data());
    auto baseline = captured;
    require(!baseline.empty(), "ordinary LCD surface");
    if (argc == 2) {
        FILE *out = std::fopen(argv[1], "wb");
        require(out, "surface export");
        std::fwrite(baseline.data(), 1, baseline.size(), out);
        std::fclose(out);
    }
    size_t varied = 0;
    for (size_t i = 4; i < baseline.size(); i += 4)
        if (std::memcmp(baseline.data() + i, baseline.data(), 3))
            ++varied;
    require(varied > baseline.size() / 16,
            "ordinary LCD is visibly rendered, not an empty comparison");
    gb_platform_shutdown();
    GBPresentationHooks hooks{};
    hooks.api_version = GB_PRESENTATION_API_VERSION;
    hooks.struct_size = sizeof(hooks);
    hooks.gl_attributes = attributes;
    hooks.begin_frame = begin;
    hooks.frame = frame;
    hooks.event = event;
    hooks.before_swap = capture;
    hooks.shutdown = shutdown;
    hooks.input_poll = poll;
    hooks.state_loaded = state_loaded;
    require(gb_platform_set_presentation(&hooks), "register matching version");
    auto bad = hooks;
    ++bad.api_version;
    require(!gb_platform_set_presentation(&bad), "wrong version rejected");
    bad = hooks;
    --bad.struct_size;
    require(!gb_platform_set_presentation(&bad), "wrong size rejected");
    calls.clear();
    require(gb_platform_init(2), "SDL extension initialization");
    gb_platform_register_context(ctx);
    require(calls == "A", "attributes precede any drawing");
    SDL_SetWindowSize(SDL_GL_GetCurrentWindow(), 480, 320);
    gb_platform_poll_events(ctx);
    auto render_checked = [&](bool host_only = false) {
        std::vector<uint8_t> wram(ctx->wram, ctx->wram + 32768), vram(ctx->vram, ctx->vram + 16384);
        std::vector<uint8_t> eram(ctx->eram, ctx->eram + ctx->eram_size);
        const uint32_t *fb = gb_get_framebuffer(ctx);
        std::vector<uint32_t> guest(fb, fb + 160 * 144);
        auto original_lcd = lcd;
        if (host_only)
            gb_platform_present_framebuffer(lcd.data());
        else
            gb_platform_render_frame(lcd.data());
        require(!std::memcmp(wram.data(), ctx->wram, wram.size()) &&
                    !std::memcmp(vram.data(), ctx->vram, vram.size()) &&
                    !std::memcmp(eram.data(), ctx->eram, eram.size()) &&
                    !std::memcmp(guest.data(), gb_get_framebuffer(ctx),
                                 guest.size() * sizeof(uint32_t)) &&
                    lcd == original_lcd,
                "every presentation preserves WRAM, VRAM, cartridge RAM and both framebuffers");
    };
    calls.clear();
    render_checked();
    require(calls == "BFC", "begin, draw, capture order");
    require(captured == baseline, "non-covering extension keeps every original LCD pixel");
    predict = true;
    cover = false;
    calls.clear();
    render_checked();
    require(calls == "BFC" && captured == baseline,
            "failed cover restores original LCD on the same frame");
    cover = true;
    calls.clear();
    render_checked(true);
    require(calls == "BFC" && captured != baseline,
            "host-only presentation receives all hooks and covers LCD");
    SDL_Event key{};
    key.type = SDL_KEYDOWN;
    key.key.keysym.scancode = SDL_SCANCODE_F2;
    key.key.keysym.sym = SDLK_F2;
    require(SDL_PushEvent(&key) == 1 && gb_platform_poll_events(ctx), "event polling");
    require(consumed == 1, "presentation consumes event once");
    external = 0xfe;
    require(gb_platform_poll_events(ctx) && g_joypad_dpad == 0xfe, "external right input");
    gb_platform_set_input_script("c0:U:100000");
    require(gb_platform_poll_events(ctx) && g_joypad_dpad == 0xfa,
            "independent script and external channels");
    external = 255;
    gb_platform_set_input_script(nullptr);
    require(gb_platform_poll_events(ctx) && g_joypad_dpad == 255, "both channels release");
    key.key.keysym.scancode = SDL_SCANCODE_W;
    key.key.keysym.sym = SDLK_w;
    require(SDL_PushEvent(&key) == 1 && gb_platform_poll_events(ctx), "manual key press");
    require((g_joypad_dpad & 4) == 0, "W binding presses up");
    const SDL_Scancode release[] = {SDL_SCANCODE_W};
    gb_platform_release_keys(release, 1);
    require(gb_platform_poll_events(ctx) && g_joypad_dpad == 255,
            "mode change releases held key bindings");
    gb_platform_set_input_record_file("external.input");
    ctx->cycles = 100;
    external = 0xfe;
    require(gb_platform_poll_events(ctx), "record external press");
    ctx->cycles = 200;
    external = 255;
    require(gb_platform_poll_events(ctx), "record external release");
    gb_platform_set_input_record_file(nullptr);
    FILE *recorded = std::fopen("external.input", "rb");
    require(recorded, "external recording file");
    char text[512]{};
    std::fread(text, 1, sizeof(text) - 1, recorded);
    std::fclose(recorded);
    require(std::strstr(text, "c100:R:100"),
            "recording includes external channel with original cycle anchors");
    auto press = [&](SDL_Scancode scancode, SDL_Keycode symbol) {
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.keysym.scancode = scancode;
        e.key.keysym.sym = symbol;
        require(SDL_PushEvent(&e) == 1 && gb_platform_poll_events(ctx), "savestate shortcut down");
        e.type = SDL_KEYUP;
        require(SDL_PushEvent(&e) == 1 && gb_platform_poll_events(ctx), "savestate shortcut up");
    };
    std::remove("game.state1");
    press(SDL_SCANCODE_F8, SDLK_F8);
    require(loaded == 0, "failed load does not notify presentation");
    ctx->wram[0] = 0x31;
    press(SDL_SCANCODE_F5, SDLK_F5);
    ctx->wram[0] = 0x62;
    press(SDL_SCANCODE_F8, SDLK_F8);
    require(loaded == 1 && ctx->wram[0] == 0x31,
            "successful frontend load notifies after restoring state");
    calls.clear();
    gb_platform_shutdown();
    require(calls == "S", "shutdown callback runs once before destroying GL");
    require(gb_platform_set_presentation(nullptr), "unregister after shutdown");
    require(calls == "S", "unregister does not duplicate shutdown");
    gb_context_destroy(ctx);
    std::puts("PASS: presentation lifecycle, version, fallback pixels, events and independent "
              "input channels");
}
