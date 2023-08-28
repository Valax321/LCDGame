#include "lcd.h"

#define SCREEN_WIDTH 60
#define SCREEN_HEIGHT 40

static SDL_Window* g_Window = NULL;
static SDL_Renderer* g_Renderer = NULL;
static SDL_Texture* g_Framebuffer = NULL;
static uint32_t g_FramebufferPixelFormat;
static SDL_PixelFormat* g_FramebufferPixelFormatter = NULL;
static bool g_ShouldQuit = false;
static uint64_t g_TimerTicks = 0;
static double g_CurrentTime = 0;
static double g_DeltaTime = 0;
static ivec2 g_PixelPos = { SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };
static bool g_KeyboardState[SDL_NUM_SCANCODES];
static bool g_PrevKeyboardState[SDL_NUM_SCANCODES];

static bool IsKeyDown(SDL_Scancode scancode) {
    return g_KeyboardState[scancode];
}

static bool WasKeyDown(SDL_Scancode scancode) {
    return g_PrevKeyboardState[scancode];
}

static bool WasKeyPressed(SDL_Scancode scancode) {
    return IsKeyDown(scancode) && !WasKeyDown(scancode);
}

static bool WasKeyReleased(SDL_Scancode scancode) {
    return !IsKeyDown(scancode) && WasKeyDown(scancode);
}

// -- Color --

// ARGB format
#define LOCOLOR 0xff405010
#define HICOLOR 0xffd0d058

// Allows for looking up a brightness value against the ARGB color of a pixel
static uint32_t g_ColorInterpTable[256];

#define Uint8ToDouble(v) ((double)(((v) & 0xff)) / 255.0)
#define DoubleToUint8(v) ((uint8_t)(255.0 * SDL_clamp((v), 0.0, 1.0)))
#define Lerp(a, b, f) (a * (1.0 - f) + (b * f))

inline static void FillColor32(double* c, uint32_t v) {
    c[0] = Uint8ToDouble((v >> 16) & 0xff);
    c[1] = Uint8ToDouble((v >> 8) & 0xff);
    c[2] = Uint8ToDouble(v & 0xff);
    c[3] = Uint8ToDouble((v >> 24) & 0xff);
}

inline static uint32_t PackColor32(double* c) {
    const uint8_t r = DoubleToUint8(c[0]);
    const uint8_t g = DoubleToUint8(c[1]);
    const uint8_t b = DoubleToUint8(c[2]);
    const uint8_t a = DoubleToUint8(c[3]);

    return (uint32_t)b + ((uint32_t)g << 8) + ((uint32_t)r << 16) + ((uint32_t)a << 24);
}

static uint32_t ARGBLerp(uint32_t a, uint32_t b, double v) {
    double af[4];
    double bf[4];
    double rf[4];

    FillColor32(af, a);
    FillColor32(bf, b);

    for (int i = 0; i < 4; ++i) {
        rf[i] = Lerp(af[i], bf[i], v);
    }

    return PackColor32(rf);
}

static void BuildColorInterpTable(void) {
    for (int i = 0; i < 256; ++i) {
        const double v = 1 - pow((double)i / 255.0, 2.0);
        g_ColorInterpTable[i] = ARGBLerp(LOCOLOR, HICOLOR, v);
    }
}

// -- LCD Panel --

static uint8_t g_LCDPanel[SCREEN_WIDTH * SCREEN_HEIGHT];
const static size_t g_LCDPanelSize = sizeof(g_LCDPanel) / sizeof(g_LCDPanel[0]);

#define GetLCDOffset(x, y) ((size_t)(((y) * SCREEN_WIDTH) + (x)))

#define LCD_DECAY_RATE 10

// -- Functions --

static void DispatchMainLoop(void);

int main(int argc, char* argv[]) {

    BuildColorInterpTable();

    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        fprintf(stderr, "Failed to init SDL: %s\n", SDL_GetError());
        return 1;
    }

    g_Window = SDL_CreateWindow("LCD", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH * 5, SCREEN_HEIGHT * 5,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );
    if (g_Window == NULL) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(g_Window, SCREEN_WIDTH, SCREEN_HEIGHT);

    g_Renderer = SDL_CreateRenderer(g_Window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (g_Renderer == NULL) {
        fprintf(stderr, "Failed to create renderer: %s\n", SDL_GetError());
        return 1;
    }

    g_Framebuffer = SDL_CreateTexture(g_Renderer, 
        SDL_PIXELFORMAT_ARGB32, 
        SDL_TEXTUREACCESS_STREAMING, 
        SCREEN_WIDTH, SCREEN_HEIGHT
    );
    if (g_Framebuffer == NULL) {
        fprintf(stderr, "Failed to create framebuffer: %s\n", SDL_GetError());
        return 1;
    }
    // Crashes
    //SDL_SetTextureScaleMode(g_Framebuffer, SDL_ScaleModeNearest);

    SDL_QueryTexture(g_Framebuffer, &g_FramebufferPixelFormat, NULL, NULL, NULL);
    fprintf(stdout, "Framebuffer format: %s\n", SDL_GetPixelFormatName((uint32_t)g_FramebufferPixelFormat));
    g_FramebufferPixelFormatter = SDL_AllocFormat(g_FramebufferPixelFormat);

    DispatchMainLoop();

    SDL_FreeFormat(g_FramebufferPixelFormatter);
    SDL_DestroyTexture(g_Framebuffer);
    g_Framebuffer = NULL;
    
    SDL_DestroyRenderer(g_Renderer);
    g_Renderer = NULL;

    SDL_DestroyWindow(g_Window);
    g_Window = NULL;

    SDL_Quit();
    return 0;
}

static void ProcessEvents(void) {
    SDL_Event ev;
    
    // Copy current keyboard state into previous keyboard state
    memcpy(g_PrevKeyboardState, g_KeyboardState, sizeof(g_KeyboardState));
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                g_ShouldQuit = true;
                break;
            case SDL_KEYDOWN:
            {
                SDL_Scancode scanCode = ev.key.keysym.scancode;
                g_KeyboardState[scanCode] = true;
            }
            break;
            case SDL_KEYUP:
            {
                SDL_Scancode scanCode = ev.key.keysym.scancode;
                g_KeyboardState[scanCode] = false;
            }
            break;
        }
    }
}

static void UpdateTimers(void) {
    const uint64_t c = SDL_GetPerformanceCounter();
    const uint64_t p = g_TimerTicks;
    g_TimerTicks = c;

    g_DeltaTime = ((double)c - (double)p) / (double)SDL_GetPerformanceFrequency();
    g_CurrentTime += g_DeltaTime;
}

static void LCDUpdate(void) {
    // Apply ghosting effect
    for (int i = 0; i < g_LCDPanelSize; i++) {
        const double v = fmax(0.0, (Uint8ToDouble(g_LCDPanel[i])) - (LCD_DECAY_RATE * g_DeltaTime));
        g_LCDPanel[i] = DoubleToUint8(v);
    }

    // Update drawn pixel pos
    int xDir = 0;
    int yDir = 0;
    if (IsKeyDown(SDL_SCANCODE_DOWN)) yDir++;
    if (IsKeyDown(SDL_SCANCODE_UP)) yDir--;
    if (IsKeyDown(SDL_SCANCODE_RIGHT)) xDir++;
    if (IsKeyDown(SDL_SCANCODE_LEFT)) xDir--;

    g_PixelPos.x = SDL_clamp(g_PixelPos.x + xDir, 0, SCREEN_WIDTH);
    g_PixelPos.y = SDL_clamp(g_PixelPos.y + yDir, 0, SCREEN_HEIGHT);

    // Plot a single dark pixel
    g_LCDPanel[GetLCDOffset(g_PixelPos.x, g_PixelPos.y)] = 255;
}

static void UpdateDisplayFramebuffer(void) {
    uint32_t* pixelData;
    int pitch;
    SDL_LockTexture(g_Framebuffer, NULL, (void**)(&pixelData), &pitch);
    SDL_assert((SCREEN_WIDTH * sizeof(uint32_t)) == pitch);
    {
        for (int pixel = 0; pixel < g_LCDPanelSize; ++pixel) {
            const uint8_t lcdValue = g_LCDPanel[pixel];
            const uint32_t argbValue = g_ColorInterpTable[lcdValue];
            pixelData[pixel] = SDL_MapRGBA(g_FramebufferPixelFormatter, 
            (argbValue >> 16) & 0xff, (argbValue >> 8) & 0xff, argbValue & 0xff, (argbValue >> 24) & 0xff);
        }
    }
    SDL_UnlockTexture(g_Framebuffer);
}

static void DrawFramebuffer(void) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(g_Renderer, &windowWidth, &windowHeight);

    int32_t scale = (int32_t)floorf(SDL_min(
        (float)windowWidth / SCREEN_WIDTH,
        (float)windowHeight / SCREEN_HEIGHT
    ));

    if (scale < 1)
        scale = 1;

    const int32_t scaledWidth = SCREEN_WIDTH * scale;
    const int32_t scaledHeight = SCREEN_HEIGHT * scale;
    const int32_t wDiff = windowWidth - scaledWidth;
    const int32_t hDiff = windowHeight - scaledHeight;

    const SDL_Rect dst = {
        wDiff / 2,
        hDiff / 2,
        scaledWidth,
        scaledHeight
    };

    SDL_SetRenderDrawBlendMode(g_Renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(g_Renderer, NULL);

    SDL_RenderClear(g_Renderer);
    SDL_RenderCopy(g_Renderer, g_Framebuffer, NULL, &dst);
}

static void DispatchMainLoop(void) {
    g_CurrentTime = SDL_GetPerformanceCounter();

    while (!g_ShouldQuit) {
        ProcessEvents();
        UpdateTimers();
        LCDUpdate();
        UpdateDisplayFramebuffer();
        DrawFramebuffer();
        SDL_RenderPresent(g_Renderer);
    }
}