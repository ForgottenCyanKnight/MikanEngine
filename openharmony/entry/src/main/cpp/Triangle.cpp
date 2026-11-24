#include <SDL3/SDL.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include <GLES2/gl2.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace {
constexpr char vertexShader[] = R"(
attribute vec2 position;
attribute vec3 color;
uniform vec2 aspectScale;
varying mediump vec3 vertexColor;
void main()
{
    gl_Position = vec4(position * aspectScale, 0.0, 1.0);
    vertexColor = color;
}
)";

constexpr char fragmentShader[] = R"(
precision mediump float;
varying mediump vec3 vertexColor;
void main()
{
    gl_FragColor = vec4(vertexColor, 1.0);
}
)";

struct Vertex {
    GLfloat position[2];
    GLfloat color[3];
};

constexpr Vertex vertices[] = {
    {{ 0.000f,  0.700f}, {1.00f, 0.26f, 0.22f}},
    {{-0.606f, -0.350f}, {0.28f, 0.89f, 0.65f}},
    {{ 0.606f, -0.350f}, {0.35f, 0.48f, 1.00f}},
};

constexpr char fpsGlyphs[] = "0123456789FPS-";
constexpr Uint16 fpsGlyphRows[] = {
    0b111'101'101'101'111, // 0
    0b010'110'010'010'111, // 1
    0b111'001'111'100'111, // 2
    0b111'001'111'001'111, // 3
    0b101'101'111'001'001, // 4
    0b111'100'111'001'111, // 5
    0b111'100'111'101'111, // 6
    0b111'001'010'010'010, // 7
    0b111'101'111'101'111, // 8
    0b111'101'111'001'111, // 9
    0b111'100'110'100'100, // F
    0b111'101'111'100'100, // P
    0b111'100'111'001'111, // S
    0b000'000'111'000'000, // -
};

struct FpsOverlay {
    GLuint buffer = 0;
    GLsizei vertexCount = 0;
    int width = 0;
    int height = 0;
    int value = -1;
    bool needsUpload = true;
    Uint64 sampleStart = 0;
    Uint64 presentedFrames = 0;
    std::atomic_bool resetSample{true};
};

struct TriangleApp {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    GLuint program = 0;
    GLuint vertexBuffer = 0;
    GLint aspectScale = -1;
    FpsOverlay fps;
    std::atomic_bool inBackground{false};
    bool firstFramePresented = false;
};

TriangleApp app;

void BindVertexBuffer(GLuint buffer)
{
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, color)));
}

void UploadFpsText(FpsOverlay& fps, int width, int height)
{
    char text[16] = "FPS --";
    if (fps.value >= 0) SDL_snprintf(text, sizeof(text), "FPS %d", fps.value);

    const int cell = std::clamp(std::min(width, height) / 165, 2, 12);
    const int top = 3 * cell;
    int left = 3 * cell;
    const auto vertex = [width, height](int x, int y) {
        return Vertex{{2.0f * x / width - 1.0f, 1.0f - 2.0f * y / height},
                      {0.90f, 0.94f, 1.0f}};
    };

    std::vector<Vertex> geometry;
    geometry.reserve(SDL_strlen(text) * 3 * 5 * 6);
    for (const char* character = text; *character; ++character, left += 4 * cell) {
        const char* glyph = SDL_strchr(fpsGlyphs, *character);
        if (!glyph) continue;
        const Uint16 bits = fpsGlyphRows[glyph - fpsGlyphs];
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (!(bits & (1u << (14 - row * 3 - column)))) continue;
                const int x = left + column * cell;
                const int y = top + row * cell;
                const Vertex topLeft = vertex(x, y);
                const Vertex topRight = vertex(x + cell, y);
                const Vertex bottomLeft = vertex(x, y + cell);
                const Vertex bottomRight = vertex(x + cell, y + cell);
                geometry.insert(geometry.end(), {topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight});
            }
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, fps.buffer);
    glBufferData(GL_ARRAY_BUFFER, geometry.size() * sizeof(Vertex), geometry.data(), GL_DYNAMIC_DRAW);
    fps.vertexCount = static_cast<GLsizei>(geometry.size());
    fps.width = width;
    fps.height = height;
    fps.needsUpload = false;
}

void RecordPresentedFrame(FpsOverlay& fps)
{
    const Uint64 now = SDL_GetTicksNS();
    if (fps.resetSample.exchange(false)) {
        fps.sampleStart = now;
        fps.presentedFrames = 0;
        return;
    }

    ++fps.presentedFrames;
    const Uint64 elapsed = now - fps.sampleStart;
    if (elapsed < SDL_NS_PER_SECOND) return;

    const int value = static_cast<int>((fps.presentedFrames * SDL_NS_PER_SECOND + elapsed / 2) / elapsed);
    if (fps.value != value) {
        fps.value = value;
        fps.needsUpload = true;
    }
    fps.sampleStart = now;
    fps.presentedFrames = 0;
}

GLuint CompileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES shader allocation failed");
        return 0;
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    char log[512]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES shader compilation failed: %s", log);
    glDeleteShader(shader);
    return 0;
}

bool InitializeTriangle(TriangleApp& triangle)
{
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexShader);
    if (!vertex) return false;

    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentShader);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }

    triangle.program = glCreateProgram();
    glAttachShader(triangle.program, vertex);
    glAttachShader(triangle.program, fragment);
    glBindAttribLocation(triangle.program, 0, "position");
    glBindAttribLocation(triangle.program, 1, "color");
    glLinkProgram(triangle.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(triangle.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512]{};
        glGetProgramInfoLog(triangle.program, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES program link failed: %s", log);
        return false;
    }

    triangle.aspectScale = glGetUniformLocation(triangle.program, "aspectScale");

    // The HarmonyOS emulator needs a GPU buffer for vertex attributes.
    glGenBuffers(1, &triangle.vertexBuffer);
    glGenBuffers(1, &triangle.fps.buffer);
    glBindBuffer(GL_ARRAY_BUFFER, triangle.vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUseProgram(triangle.program);
    glClearColor(15.0f / 255.0f, 21.0f / 255.0f, 35.0f / 255.0f, 1.0f);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES setup failed: 0x%x", error);
        return false;
    }
    return triangle.vertexBuffer != 0 && triangle.fps.buffer != 0 && triangle.aspectScale >= 0;
}
}

SDL_AppResult SDL_AppInit(void** state, int, char**)
{
    *state = &app;
    if (!SDL_Init(SDL_INIT_VIDEO) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL video setup failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    app.window = SDL_CreateWindow("MikanEngine", 800, 600, SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    app.context = SDL_GL_CreateContext(app.window);
    if (!app.context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES context creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    return InitializeTriangle(app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event)
{
    auto& triangle = *static_cast<TriangleApp*>(state);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        triangle.inBackground = true;
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        triangle.fps.resetSample = true;
        triangle.inBackground = false;
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* state)
{
    auto& triangle = *static_cast<TriangleApp*>(state);
    if (triangle.inBackground) return SDL_APP_CONTINUE;

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(triangle.window, &width, &height) || width <= 0 || height <= 0) {
        return SDL_APP_CONTINUE;
    }

    const float edge = static_cast<float>(std::min(width, height));
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform2f(triangle.aspectScale, edge / static_cast<float>(width), edge / static_cast<float>(height));
    BindVertexBuffer(triangle.vertexBuffer);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    auto& fps = triangle.fps;
    if (fps.needsUpload || fps.width != width || fps.height != height) {
        UploadFpsText(fps, width, height);
    }
    glUniform2f(triangle.aspectScale, 1.0f, 1.0f);
    BindVertexBuffer(fps.buffer);
    glDrawArrays(GL_TRIANGLES, 0, fps.vertexCount);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL ES draw failed: 0x%x", error);
        return SDL_APP_FAILURE;
    }
    if (!SDL_GL_SwapWindow(triangle.window)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL presentation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    RecordPresentedFrame(fps);

    if (!triangle.firstFramePresented) {
        SDL_Log("MikanTriangle: first frame presented (OpenGL ES, %dx%d, %s)",
                width, height, SDL_GetRevision());
        triangle.firstFramePresented = true;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* state, SDL_AppResult)
{
    if (!state) return;
    auto& triangle = *static_cast<TriangleApp*>(state);
    if (triangle.context) {
        if (triangle.fps.buffer) glDeleteBuffers(1, &triangle.fps.buffer);
        if (triangle.vertexBuffer) glDeleteBuffers(1, &triangle.vertexBuffer);
        if (triangle.program) glDeleteProgram(triangle.program);
        SDL_GL_DestroyContext(triangle.context);
    }
    if (triangle.window) SDL_DestroyWindow(triangle.window);
}
