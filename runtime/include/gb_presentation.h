#ifndef GB_PRESENTATION_H
#define GB_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef GB_HAS_SDL2
#include <SDL_scancode.h>
#else
/* Allows runtime-only builds without SDL development headers. */
typedef int SDL_Scancode;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;
typedef union SDL_Event SDL_Event;

#define GB_PRESENTATION_API_VERSION 1

/* SDL/OpenGL presentation extension. All callbacks run on the frontend thread.
 * Treat the guest context and framebuffer as read-only presentation input.
 * Null callbacks retain the ordinary frontend behavior. Initialize the whole
 * structure to zero, then set api_version and struct_size before registering.
 * Registration copies this structure; callback code must outlive registration.
 */
typedef struct GBPresentationHooks {
    uint32_t api_version;
    size_t struct_size;
    /* After default GL attributes, before creating the context. */
    void (*gl_attributes)(void);
    /* Before the ordinary LCD upload. Return true only when frame() will
     * cover the drawable. This permits avoiding a redundant texture upload.
     * Called for guest frames AND host-only presentations (paused/loading). */
    bool (*begin_frame)(GBContext* ctx, bool menu_open, const uint32_t* framebuffer);
    /* After ImGui::NewFrame and before the runtime settings UI. Return true
     * when the drawable is covered. If a predicted cover fails, the runtime
     * restores the ordinary LCD image in the same frame. */
    bool (*frame)(GBContext* ctx, int width, int height, bool menu_open);
    /* Before game bindings. Return true to consume this event. */
    bool (*event)(const SDL_Event* event, bool menu_open);
    /* After ImGui draw submission, immediately before SDL_GL_SwapWindow.
     * May compose a final transition and/or capture the completed drawable. */
    void (*before_swap)(int width, int height, bool menu_open);
    /* GL is still current. Release all extension-owned GL resources. */
    void (*shutdown)(void);
    /* Before combining manual, scripted and external joypad channels. */
    void (*input_poll)(GBContext* ctx, bool menu_open);
    /* Only after a successful explicit savestate load in the SDL frontend. */
    void (*state_loaded)(GBContext* ctx);
} GBPresentationHooks;

/* Register before gb_platform_init. Replacement/unregistration with a live
 * context shuts down the previous extension first. NULL unregisters. A version
 * or size mismatch is rejected without changing the current registration. */
bool gb_platform_set_presentation(const GBPresentationHooks* hooks);
/* Independent active-low d-pad channel, ANDed with manual/scripted input and
 * included in recordings. 0xff releases it. No direct guest-memory writes. */
void gb_platform_set_external_dpad(uint8_t mask);
/* Release pressed bindings for these scancodes when changing control modes.
 * Does not alter scripts, controller bindings, or other keyboard actions. */
void gb_platform_release_keys(const SDL_Scancode* scancodes, size_t count);

#ifdef __cplusplus
}
#endif
#endif
