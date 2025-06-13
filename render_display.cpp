#include "render_display.h"
#include <SDL2/SDL.h>
#include <iostream>
#include <thread>
#include <chrono>

const int WINDOW_WIDTH = 640;
const int WINDOW_HEIGHT = 480;

void sdl_display_loop(DualCameraCapture& capture, std::atomic<bool>& should_exit) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
        std::cerr << "Could not initialize SDL: " << SDL_GetError() << std::endl;
        should_exit = true;
        return;
    }

    SDL_Window* window = SDL_CreateWindow("Dual Camera View",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        2 * WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        should_exit = true;
        return;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* textures[2] = { nullptr, nullptr };

    for (int i = 0; i < 2; ++i) {
        textures[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);
    }

    while (!should_exit) {
        // 先处理 SDL 事件（不依赖是否获取到帧）
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT ||
                (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                should_exit = true;
                break;
            }
        }
        if (should_exit) break;

        // 再尝试获取帧
        std::vector<AVFrame*> frames = capture.get_sync_rgb_frames();
        if (frames.size() == 2) {
            for (int i = 0; i < 2; ++i) {
                SDL_UpdateTexture(textures[i], nullptr, frames[i]->data[0], frames[i]->linesize[0]);
            }

            SDL_RenderClear(renderer);
            SDL_Rect rects[2] = {
                { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT },
                { WINDOW_WIDTH, 0, WINDOW_WIDTH, WINDOW_HEIGHT }
            };

            for (int i = 0; i < 2; ++i) {
                SDL_RenderCopy(renderer, textures[i], nullptr, &rects[i]);
                av_frame_free(&frames[i]);
            }

            SDL_RenderPresent(renderer);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }


    for (int i = 0; i < 2; ++i) {
        if (textures[i]) SDL_DestroyTexture(textures[i]);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void start_rendering(DualCameraCapture& capture, std::atomic<bool>& should_exit) {
    std::thread(sdl_display_loop, std::ref(capture), std::ref(should_exit)).detach();
}
