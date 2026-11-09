#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <SDL.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#define STREAM_FILE "sdmc:/switch/lampa-nx/stream.url"
#define DEMO_URL "https://storage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4"

static void *get_proc(void *ctx, const char *name) {
    (void)ctx;
    return SDL_GL_GetProcAddress(name);
}

static int read_url(char *out, size_t capacity) {
    FILE *file = fopen(STREAM_FILE, "r");
    if (!file) return 0;
    int success = fgets(out, (int)capacity, file) != NULL;
    fclose(file);
    if (!success) return 0;
    out[strcspn(out, "\r\n")] = 0;
    return strncmp(out, "https://", 8) == 0 || strncmp(out, "http://", 7) == 0;
}

static void send_command(mpv_handle *player, const char *one, const char *two) {
    const char *args[] = {one, two, NULL};
    mpv_command_async(player, 0, args);
}

static void seek(mpv_handle *player, const char *seconds) {
    const char *args[] = {"seek", seconds, "relative", NULL};
    mpv_command_async(player, 0, args);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char url[2048];
    if (!read_url(url, sizeof(url))) snprintf(url, sizeof(url), "%s", DEMO_URL);

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window *window = SDL_CreateWindow("Lampa Native", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) { SDL_Quit(); return 2; }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) { SDL_DestroyWindow(window); SDL_Quit(); return 3; }

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    }

    mpv_handle *player = mpv_create();
    if (!player) goto cleanup_gl;
    mpv_set_option_string(player, "vo", "libmpv");
    mpv_set_option_string(player, "hwdec", "auto-safe");
    mpv_set_option_string(player, "terminal", "no");
    mpv_set_option_string(player, "osd-level", "1");
    if (mpv_initialize(player) < 0) goto cleanup_player;

    mpv_opengl_init_params gl_init = { .get_proc_address = get_proc, .get_proc_address_ctx = NULL };
    mpv_render_param init[] = {
        { MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
        { MPV_RENDER_PARAM_INVALID, NULL }
    };
    mpv_render_context *render = NULL;
    if (mpv_render_context_create(&render, player, init) < 0) goto cleanup_player;

    send_command(player, "loadfile", url);
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_A: send_command(player, "cycle", "pause"); break;
                    case SDL_CONTROLLER_BUTTON_B: send_command(player, "stop", NULL); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: seek(player, "-10"); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: seek(player, "10"); break;
                    case SDL_CONTROLLER_BUTTON_Y: send_command(player, "show-text", "${time-pos} / ${duration}"); break;
                    case SDL_CONTROLLER_BUTTON_START: running = 0; break;
                    default: break;
                }
            }
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_SPACE: send_command(player, "cycle", "pause"); break;
                    case SDLK_LEFT: seek(player, "-10"); break;
                    case SDLK_RIGHT: seek(player, "10"); break;
                    case SDLK_ESCAPE: running = 0; break;
                    default: break;
                }
            }
        }
        // Drain events so EOF/errors do not accumulate in the libmpv queue.
        while (mpv_wait_event(player, 0)->event_id != MPV_EVENT_NONE) {}

        int width, height;
        SDL_GetWindowSize(window, &width, &height);
        mpv_opengl_fbo fbo = { .fbo = 0, .w = width, .h = height, .internal_format = 0 };
        int flip = 1;
        mpv_render_param frame[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
            { MPV_RENDER_PARAM_FLIP_Y, &flip },
            { MPV_RENDER_PARAM_INVALID, NULL }
        };
        mpv_render_context_render(render, frame);
        SDL_GL_SwapWindow(window);
        SDL_Delay(16);
    }
    mpv_render_context_free(render);
cleanup_player:
    mpv_terminate_destroy(player);
cleanup_gl:
    if (pad) SDL_GameControllerClose(pad);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
