#include "vm_raylib.h"
#include <raylib.h>
#include <rlgl.h>
#if defined(__APPLE__)
/* Apple deprecated OpenGL in 10.14; symbols still work via Metal
 * under the hood. Silence the deprecation warnings. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>

/* GLFW wait-events bindings. raylib bundles GLFW statically inside
 * libraylib.a; the dynamic libraylib.dylib does NOT export these
 * symbols. The compiler/Makefile links libraylib.a directly so these
 * symbols resolve in the rae driver binary. Any external launcher
 * embedding the VM must do the same — see the matching comment in
 * compiler/runtime/rae_runtime.c. */
extern void glfwWaitEventsTimeout(double timeout);
extern void glfwWaitEvents(void);
extern void glfwPostEmptyEvent(void);

/* Window-close callback waker — see matching comment in
 * compiler/runtime/rae_runtime.c. Must do BOTH jobs raylib's default
 * does PLUS post an empty event: set the should-close flag AND wake
 * the wait. */
typedef struct GLFWwindow GLFWwindow;
typedef void (*GLFWwindowclosefun)(GLFWwindow*);
typedef void (*GLFWmousebuttonfun)(GLFWwindow*, int, int, int);
extern GLFWwindow* glfwGetCurrentContext(void);
extern void glfwSetWindowCloseCallback(GLFWwindow* w, GLFWwindowclosefun cb);
extern void glfwSetWindowShouldClose(GLFWwindow* w, int value);
extern GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb);
#define VM_GLFW_PRESS 1
#define VM_GLFW_RELEASE 0
#define VM_GLFW_MOUSE_BUTTON_LEFT 0

/* macOS click-drop workaround. See matching comment in
 * compiler/runtime/rae_runtime.c. raylib's edge detection collapses
 * press+release pairs that happen between two polls; chaining a GLFW
 * callback below raylib's lets us see every transition. */
static int g_vm_mouse_press_pending = 0;
static int g_vm_mouse_release_pending = 0;
static GLFWmousebuttonfun g_vm_prev_mouse_button_cb = NULL;
static int g_vm_mouse_button_hook_installed = 0;

static void vm_glfw_mouse_button_chain_cb(GLFWwindow* w, int button, int action, int mods) {
    if (g_vm_prev_mouse_button_cb) g_vm_prev_mouse_button_cb(w, button, action, mods);
    if (button == VM_GLFW_MOUSE_BUTTON_LEFT) {
        if (action == VM_GLFW_PRESS) g_vm_mouse_press_pending = 1;
        else if (action == VM_GLFW_RELEASE) g_vm_mouse_release_pending = 1;
    }
}

static void vm_glfw_close_waker(GLFWwindow* w) {
    /* macOS GLFW quirk: red-X click invokes this callback but the
     * wait-events main loop doesn't reliably wake from it. Just
     * exit() — see matching comment in rae_runtime.c. */
    fprintf(stderr, "[close-waker] fired (w=%p) — exiting\n", (void*)w);
    fflush(stderr);
    glfwSetWindowShouldClose(w, 1);
    exit(0);
}

static bool native_getScreenWidth(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetScreenWidth());
    return true;
}

static bool native_getScreenHeight(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetScreenHeight());
    return true;
}

static bool native_getRenderWidth(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetRenderWidth());
    return true;
}

static bool native_getRenderHeight(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetRenderHeight());
    return true;
}

static bool native_initWindow(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) {
        fprintf(stderr, "error: initWindow expects 3 args, got %zu\n", count);
        return false;
    }
    int w = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int h = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    const char* title = "Rae Window";
    if (args[2].type == VAL_STRING && args[2].as.string_value.chars) {
        title = args[2].as.string_value.chars;
    } else {
        fprintf(stderr, "error: initWindow expects string for title, got type %d\n", args[2].type);
    }
    InitWindow(w, h, title);
    out->has_value = false;
    return true;
}

static bool native_setConfigFlags(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: setConfigFlags expects 1 arg, got %zu\n", count);
        return false;
    }
    int flags = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    SetConfigFlags((unsigned int)flags);
    out->has_value = false;
    return true;
}

static bool native_windowShouldClose(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: windowShouldClose expects 0 args, got %zu\n", count);
        return false;
    }
    out->has_value = true;
    out->value = value_int(WindowShouldClose() ? 1 : 0);
    return true;
}

static bool native_closeWindow(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: closeWindow expects 0 args, got %zu\n", count);
        return false;
    }
    CloseWindow();
    out->has_value = false;
    return true;
}

static bool native_beginDrawing(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: beginDrawing expects 0 args, got %zu\n", count);
        return false;
    }
    BeginDrawing();
    out->has_value = false;
    return true;
}

static bool native_endDrawing(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: endDrawing expects 0 args, got %zu\n", count);
        return false;
    }
    EndDrawing();
    out->has_value = false;
    return true;
}

static bool native_clearBackground(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
  (void)vm; (void)data;
  if (count != 4) {
      fprintf(stderr, "error: clearBackground expects 4 args, got %zu\n", count);
      return false;
  }
  unsigned char r = (args[0].type == VAL_FLOAT) ? (unsigned char)args[0].as.float_value : (unsigned char)args[0].as.int_value;
  unsigned char g = (args[1].type == VAL_FLOAT) ? (unsigned char)args[1].as.float_value : (unsigned char)args[1].as.int_value;
  unsigned char b = (args[2].type == VAL_FLOAT) ? (unsigned char)args[2].as.float_value : (unsigned char)args[2].as.int_value;
  unsigned char a = (args[3].type == VAL_FLOAT) ? (unsigned char)args[3].as.float_value : (unsigned char)args[3].as.int_value;
  ClearBackground((Color){r, g, b, a});
  out->has_value = false;
  return true;
}

static bool native_loadTexture(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1 || args[0].type != VAL_STRING) {
        fprintf(stderr, "error: loadTexture expects 1 string arg, got %zu\n", count);
        return false;
    }
    Texture t = LoadTexture(args[0].as.string_value.chars);
    out->has_value = true;
    out->value = value_object(5, "Texture");
    out->value.as.object_value.fields[0] = value_int((int64_t)t.id);
    out->value.as.object_value.fields[1] = value_int((int64_t)t.width);
    out->value.as.object_value.fields[2] = value_int((int64_t)t.height);
    out->value.as.object_value.fields[3] = value_int((int64_t)t.mipmaps);
    out->value.as.object_value.fields[4] = value_int((int64_t)t.format);
    return true;
}

static bool native_loadCircleCroppedTexture(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1 || args[0].type != VAL_STRING) {
        fprintf(stderr, "error: loadCircleCroppedTexture expects 1 string arg, got %zu\n", count);
        return false;
    }
    Image img = LoadImage(args[0].as.string_value.chars);
    Texture t = {0};
    if (img.data != NULL) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        float cx = (float)img.width / 2.0f;
        float cy = (float)img.height / 2.0f;
        float r = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
        float r2 = r * r;
        unsigned char* data2 = (unsigned char*)img.data;
        for (int y = 0; y < img.height; y++) {
            for (int x = 0; x < img.width; x++) {
                float dx = (float)x + 0.5f - cx;
                float dy = (float)y + 0.5f - cy;
                float d2 = dx * dx + dy * dy;
                if (d2 > r2) {
                    data2[(y * img.width + x) * 4 + 3] = 0;
                }
            }
        }
        t = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    out->has_value = true;
    out->value = value_object(5, "Texture");
    out->value.as.object_value.fields[0] = value_int((int64_t)t.id);
    out->value.as.object_value.fields[1] = value_int((int64_t)t.width);
    out->value.as.object_value.fields[2] = value_int((int64_t)t.height);
    out->value.as.object_value.fields[3] = value_int((int64_t)t.mipmaps);
    out->value.as.object_value.fields[4] = value_int((int64_t)t.format);
    return true;
}

static bool native_loadRoundedCroppedTexture(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2 || args[0].type != VAL_STRING) {
        fprintf(stderr, "error: loadRoundedCroppedTexture expects (string, float), got %zu args\n", count);
        return false;
    }
    double radiusArg = (args[1].type == VAL_FLOAT) ? args[1].as.float_value : (double)args[1].as.int_value;
    Image img = LoadImage(args[0].as.string_value.chars);
    Texture t = {0};
    if (img.data != NULL) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        /* Same thumbnail downscale as the C runtime — see the
         * `rae_ext_loadRoundedCroppedTexture` comment for the
         * display-pixel intent of `radius`. */
        const int thumbMax = 64;
        int origMaxSide = (img.width > img.height) ? img.width : img.height;
        if (origMaxSide > thumbMax) {
            float k = (float)thumbMax / (float)origMaxSide;
            int newW = (int)((float)img.width * k);
            int newH = (int)((float)img.height * k);
            if (newW < 1) newW = 1;
            if (newH < 1) newH = 1;
            ImageResize(&img, newW, newH);
        }
        float r = (float)radiusArg;
        if (r < 0.0f) r = 0.0f;
        float maxR = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
        if (r > maxR) r = maxR;
        float r2 = r * r;
        float w1 = (float)img.width - 1.0f;
        float h1 = (float)img.height - 1.0f;
        unsigned char* data2 = (unsigned char*)img.data;
        for (int y = 0; y < img.height; y++) {
            for (int x = 0; x < img.width; x++) {
                float fx = (float)x;
                float fy = (float)y;
                int inCorner = 0;
                float cx = 0.0f, cy = 0.0f;
                if (fx < r && fy < r) { inCorner = 1; cx = r; cy = r; }
                else if (fx > w1 - r && fy < r) { inCorner = 1; cx = w1 - r; cy = r; }
                else if (fx < r && fy > h1 - r) { inCorner = 1; cx = r; cy = h1 - r; }
                else if (fx > w1 - r && fy > h1 - r) { inCorner = 1; cx = w1 - r; cy = h1 - r; }
                if (inCorner) {
                    float dx = fx - cx;
                    float dy = fy - cy;
                    if (dx * dx + dy * dy > r2) {
                        data2[(y * img.width + x) * 4 + 3] = 0;
                    }
                }
            }
        }
        t = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    out->has_value = true;
    out->value = value_object(5, "Texture");
    out->value.as.object_value.fields[0] = value_int((int64_t)t.id);
    out->value.as.object_value.fields[1] = value_int((int64_t)t.width);
    out->value.as.object_value.fields[2] = value_int((int64_t)t.height);
    out->value.as.object_value.fields[3] = value_int((int64_t)t.mipmaps);
    out->value.as.object_value.fields[4] = value_int((int64_t)t.format);
    return true;
}

static bool native_raylibSetLogLevel(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: raylibSetLogLevel expects 1 int arg, got %zu\n", count);
        return false;
    }
    SetTraceLogLevel((int)args[0].as.int_value);
    out->has_value = false;
    return true;
}

static bool native_unloadTexture(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 5) {
        fprintf(stderr, "error: unloadTexture expects 5 args (flattened Texture), got %zu\n", count);
        return false;
    }
    Texture t = {
        .id = (unsigned int)args[0].as.int_value,
        .width = (int)args[1].as.int_value,
        .height = (int)args[2].as.int_value,
        .mipmaps = (int)args[3].as.int_value,
        .format = (int)args[4].as.int_value
    };
    UnloadTexture(t);
    out->has_value = false;
    return true;
}

/* See `rae_ext_roundedSpriteBegin` / `rae_ext_roundedSpriteEnd` in
 * compiler/runtime/rae_runtime.c for the design notes. This is the
 * VM-side mirror — one global shader, lazy-loaded on first use. */
static Shader g_vm_rounded_shader = {0};
static int g_vm_rounded_loc_size = -1;
static int g_vm_rounded_loc_radius = -1;
static int g_vm_rounded_loaded = 0;
static void vm_rounded_shader_ensure(void) {
    if (g_vm_rounded_loaded) return;
    const char* fs =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "out vec4 finalColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "uniform vec2 uSize;\n"
        "uniform float uRadius;\n"
        "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
        "  vec2 q = abs(p) - b + vec2(r);\n"
        "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
        "}\n"
        "void main() {\n"
        "  vec4 texel = texture(texture0, fragTexCoord);\n"
        "  vec4 c = texel * colDiffuse * fragColor;\n"
        "  vec2 p = (fragTexCoord - vec2(0.5)) * uSize;\n"
        "  float d = sdRoundRect(p, uSize * 0.5, uRadius);\n"
        "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
        "  c.a *= aa;\n"
        "  finalColor = c;\n"
        "}\n";
    g_vm_rounded_shader = LoadShaderFromMemory(NULL, fs);
    g_vm_rounded_loc_size = GetShaderLocation(g_vm_rounded_shader, "uSize");
    g_vm_rounded_loc_radius = GetShaderLocation(g_vm_rounded_shader, "uRadius");
    g_vm_rounded_loaded = 1;
}

static bool native_roundedSpriteBegin(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) {
        fprintf(stderr, "error: roundedSpriteBegin expects (width, height, radius), got %zu\n", count);
        return false;
    }
    vm_rounded_shader_ensure();
    double w = (args[0].type == VAL_FLOAT) ? args[0].as.float_value : (double)args[0].as.int_value;
    double h = (args[1].type == VAL_FLOAT) ? args[1].as.float_value : (double)args[1].as.int_value;
    double r = (args[2].type == VAL_FLOAT) ? args[2].as.float_value : (double)args[2].as.int_value;
    float size[2] = { (float)w, (float)h };
    float fr = (float)r;
    SetShaderValue(g_vm_rounded_shader, g_vm_rounded_loc_size, size, SHADER_UNIFORM_VEC2);
    SetShaderValue(g_vm_rounded_shader, g_vm_rounded_loc_radius, &fr, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(g_vm_rounded_shader);
    out->has_value = false;
    return true;
}

static bool native_roundedSpriteEnd(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    EndShaderMode();
    out->has_value = false;
    return true;
}

/* VM mirror of `rae_ext_drawGradientRect`. See rae_runtime.c for the
 * design notes. */
static Shader g_vm_gradient_shader = {0};
static int g_vm_grad_loc_size = -1;
static int g_vm_grad_loc_radius = -1;
static int g_vm_grad_loc_from = -1;
static int g_vm_grad_loc_to = -1;
static int g_vm_grad_loc_angle = -1;
static int g_vm_gradient_loaded = 0;
static void vm_gradient_shader_ensure(void) {
    if (g_vm_gradient_loaded) return;
    const char* fs =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "out vec4 finalColor;\n"
        "uniform vec2 uSize;\n"
        "uniform float uRadius;\n"
        "uniform vec4 uFrom;\n"
        "uniform vec4 uTo;\n"
        "uniform float uAngleRad;\n"
        "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
        "  vec2 q = abs(p) - b + vec2(r);\n"
        "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
        "}\n"
        "void main() {\n"
        "  vec2 dir = vec2(cos(uAngleRad), sin(uAngleRad));\n"
        "  vec2 p = fragTexCoord - vec2(0.5);\n"
        "  float t = clamp(dot(p, dir) + 0.5, 0.0, 1.0);\n"
        "  vec4 c = mix(uFrom, uTo, t);\n"
        "  vec2 pp = (fragTexCoord - vec2(0.5)) * uSize;\n"
        "  float d = sdRoundRect(pp, uSize * 0.5, uRadius);\n"
        "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
        "  c.a *= aa * fragColor.a;\n"
        "  finalColor = c;\n"
        "}\n";
    g_vm_gradient_shader = LoadShaderFromMemory(NULL, fs);
    g_vm_grad_loc_size = GetShaderLocation(g_vm_gradient_shader, "uSize");
    g_vm_grad_loc_radius = GetShaderLocation(g_vm_gradient_shader, "uRadius");
    g_vm_grad_loc_from = GetShaderLocation(g_vm_gradient_shader, "uFrom");
    g_vm_grad_loc_to = GetShaderLocation(g_vm_gradient_shader, "uTo");
    g_vm_grad_loc_angle = GetShaderLocation(g_vm_gradient_shader, "uAngleRad");
    g_vm_gradient_loaded = 1;
}

static double vm_arg_num(const Value* a) {
    return (a->type == VAL_FLOAT) ? a->as.float_value : (double)a->as.int_value;
}

static bool native_drawGradientRect(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    /* Args: x, y, w, h, radius, r1,g1,b1,a1, r2,g2,b2,a2, angleDeg → 14. */
    if (count != 14) {
        fprintf(stderr, "error: drawGradientRect expects 14 args (x,y,w,h,radius,from(4),to(4),angleDeg), got %zu\n", count);
        return false;
    }
    vm_gradient_shader_ensure();
    double x = vm_arg_num(&args[0]);
    double y = vm_arg_num(&args[1]);
    double w = vm_arg_num(&args[2]);
    double h = vm_arg_num(&args[3]);
    double radius = vm_arg_num(&args[4]);
    float size[2] = { (float)w, (float)h };
    float fr = (float)radius;
    float from[4] = {
        (float)args[5].as.int_value / 255.0f,
        (float)args[6].as.int_value / 255.0f,
        (float)args[7].as.int_value / 255.0f,
        (float)args[8].as.int_value / 255.0f
    };
    float to[4] = {
        (float)args[9].as.int_value / 255.0f,
        (float)args[10].as.int_value / 255.0f,
        (float)args[11].as.int_value / 255.0f,
        (float)args[12].as.int_value / 255.0f
    };
    float angleRad = (float)(vm_arg_num(&args[13]) * 3.14159265358979 / 180.0);
    SetShaderValue(g_vm_gradient_shader, g_vm_grad_loc_size, size, SHADER_UNIFORM_VEC2);
    SetShaderValue(g_vm_gradient_shader, g_vm_grad_loc_radius, &fr, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_vm_gradient_shader, g_vm_grad_loc_from, from, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_vm_gradient_shader, g_vm_grad_loc_to, to, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_vm_gradient_shader, g_vm_grad_loc_angle, &angleRad, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(g_vm_gradient_shader);
    /* Explicit UV quad — see rae_runtime.c for the rationale. */
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
      rlColor4ub(255, 255, 255, 255);
      rlNormal3f(0.0f, 0.0f, 1.0f);
      rlTexCoord2f(0.0f, 0.0f); rlVertex2f((float)x,       (float)y);
      rlTexCoord2f(0.0f, 1.0f); rlVertex2f((float)x,       (float)(y + h));
      rlTexCoord2f(1.0f, 1.0f); rlVertex2f((float)(x + w), (float)(y + h));
      rlTexCoord2f(1.0f, 0.0f); rlVertex2f((float)(x + w), (float)y);
    rlEnd();
    rlSetTexture(0);
    EndShaderMode();
    out->has_value = false;
    return true;
}

static bool native_captureAndBlurRegion(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 5) {
        fprintf(stderr, "error: captureAndBlurRegion expects 5 args (x,y,w,h,blurSize), got %zu\n", count);
        return false;
    }
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.float_value : (double)args[0].as.int_value;
    double y = (args[1].type == VAL_FLOAT) ? args[1].as.float_value : (double)args[1].as.int_value;
    double w = (args[2].type == VAL_FLOAT) ? args[2].as.float_value : (double)args[2].as.int_value;
    double h = (args[3].type == VAL_FLOAT) ? args[3].as.float_value : (double)args[3].as.int_value;
    int blurSize = (args[4].type == VAL_FLOAT) ? (int)args[4].as.float_value : (int)args[4].as.int_value;
    rlDrawRenderBatchActive();
    glFinish();
    Image full = LoadImageFromScreen();
    Vector2 scale = GetWindowScaleDPI();
    Rectangle crop = { (float)x * scale.x, (float)y * scale.y, (float)w * scale.x, (float)h * scale.y };
    Image region = ImageFromImage(full, crop);
    UnloadImage(full);
    ImageBlurGaussian(&region, blurSize);
    Texture t = LoadTextureFromImage(region);
    UnloadImage(region);
    out->has_value = true;
    out->value = value_object(5, "Texture");
    out->value.as.object_value.fields[0] = value_int((int64_t)t.id);
    out->value.as.object_value.fields[1] = value_int((int64_t)t.width);
    out->value.as.object_value.fields[2] = value_int((int64_t)t.height);
    out->value.as.object_value.fields[3] = value_int((int64_t)t.mipmaps);
    out->value.as.object_value.fields[4] = value_int((int64_t)t.format);
    return true;
}

static bool native_captureAndBlurBackdrop(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: captureAndBlurBackdrop expects 1 arg, got %zu\n", count);
        return false;
    }
    int blurSize = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    /* Flush pending raylib draws + GPU sync before reading the back
     * buffer — otherwise LoadImageFromScreen returns the clear color
     * on macOS+Metal. See matching comment in rae_runtime.c. */
    rlDrawRenderBatchActive();
    glFinish();
    Image img = LoadImageFromScreen();
    ImageBlurGaussian(&img, blurSize);
    Texture t = LoadTextureFromImage(img);
    UnloadImage(img);
    out->has_value = true;
    out->value = value_object(5, "Texture");
    out->value.as.object_value.fields[0] = value_int((int64_t)t.id);
    out->value.as.object_value.fields[1] = value_int((int64_t)t.width);
    out->value.as.object_value.fields[2] = value_int((int64_t)t.height);
    out->value.as.object_value.fields[3] = value_int((int64_t)t.mipmaps);
    out->value.as.object_value.fields[4] = value_int((int64_t)t.format);
    return true;
}

static bool native_drawTexture(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 11) {
        fprintf(stderr, "error: drawTexture expects 11 args (Texture(5) + x(1) + y(1) + Color(4)), got %zu\n", count);
        return false;
    }
    Texture t = {
        .id = (unsigned int)args[0].as.int_value,
        .width = (int)args[1].as.int_value,
        .height = (int)args[2].as.int_value,
        .mipmaps = (int)args[3].as.int_value,
        .format = (int)args[4].as.int_value
    };
    int x = (args[5].type == VAL_FLOAT) ? (int)args[5].as.float_value : (int)args[5].as.int_value;
    int y = (args[6].type == VAL_FLOAT) ? (int)args[6].as.float_value : (int)args[6].as.int_value;
    unsigned char r = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    unsigned char g = (args[8].type == VAL_FLOAT) ? (unsigned char)args[8].as.float_value : (unsigned char)args[8].as.int_value;
    unsigned char b = (args[9].type == VAL_FLOAT) ? (unsigned char)args[9].as.float_value : (unsigned char)args[9].as.int_value;
    unsigned char a = (args[10].type == VAL_FLOAT) ? (unsigned char)args[10].as.float_value : (unsigned char)args[10].as.int_value;
    DrawTexture(t, (float)x, (float)y, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawTextureEx(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 13) {
        fprintf(stderr, "error: drawTextureEx expects 13 args (Texture(5) + pos(2) + rotation + scale + Color(4)), got %zu\n", count);
        return false;
    }
    Texture t = {
        .id = (unsigned int)args[0].as.int_value,
        .width = (int)args[1].as.int_value,
        .height = (int)args[2].as.int_value,
        .mipmaps = (int)args[3].as.int_value,
        .format = (int)args[4].as.int_value
    };
    float px = (args[5].type == VAL_FLOAT) ? (float)args[5].as.float_value : (float)args[5].as.int_value;
    float py = (args[6].type == VAL_FLOAT) ? (float)args[6].as.float_value : (float)args[6].as.int_value;
    float rotation = (args[7].type == VAL_FLOAT) ? (float)args[7].as.float_value : (float)args[7].as.int_value;
    float scale = (args[8].type == VAL_FLOAT) ? (float)args[8].as.float_value : (float)args[8].as.int_value;
    unsigned char r = (args[9].type == VAL_FLOAT) ? (unsigned char)args[9].as.float_value : (unsigned char)args[9].as.int_value;
    unsigned char g = (args[10].type == VAL_FLOAT) ? (unsigned char)args[10].as.float_value : (unsigned char)args[10].as.int_value;
    unsigned char b = (args[11].type == VAL_FLOAT) ? (unsigned char)args[11].as.float_value : (unsigned char)args[11].as.int_value;
    unsigned char a = (args[12].type == VAL_FLOAT) ? (unsigned char)args[12].as.float_value : (unsigned char)args[12].as.int_value;
    DrawTextureEx(t, (Vector2){px, py}, rotation, scale, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawRectangle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) {
        fprintf(stderr, "error: drawRectangle expects 8 args, got %zu\n", count);
        return false;
    }
    int x = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int y = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    int w = (args[2].type == VAL_FLOAT) ? (int)args[2].as.float_value : (int)args[2].as.int_value;
    int h = (args[3].type == VAL_FLOAT) ? (int)args[3].as.float_value : (int)args[3].as.int_value;
    unsigned char r = (args[4].type == VAL_FLOAT) ? (unsigned char)args[4].as.float_value : (unsigned char)args[4].as.int_value;
    unsigned char g = (args[5].type == VAL_FLOAT) ? (unsigned char)args[5].as.float_value : (unsigned char)args[5].as.int_value;
    unsigned char b = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    unsigned char a = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    DrawRectangle(x, y, w, h, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawRectangleLines(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) {
        fprintf(stderr, "error: drawRectangleLines expects 8 args, got %zu\n", count);
        return false;
    }
    int x = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int y = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    int w = (args[2].type == VAL_FLOAT) ? (int)args[2].as.float_value : (int)args[2].as.int_value;
    int h = (args[3].type == VAL_FLOAT) ? (int)args[3].as.float_value : (int)args[3].as.int_value;
    unsigned char r = (args[4].type == VAL_FLOAT) ? (unsigned char)args[4].as.float_value : (unsigned char)args[4].as.int_value;
    unsigned char g = (args[5].type == VAL_FLOAT) ? (unsigned char)args[5].as.float_value : (unsigned char)args[5].as.int_value;
    unsigned char b = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    unsigned char a = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    DrawRectangleLines(x, y, w, h, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawRectangleRounded(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 10) {
        fprintf(stderr, "error: drawRectangleRounded expects 10 args (x, y, w, h, roundness, segments, Color(4)), got %zu\n", count);
        return false;
    }
    float x = (args[0].type == VAL_FLOAT) ? (float)args[0].as.float_value : (float)args[0].as.int_value;
    float y = (args[1].type == VAL_FLOAT) ? (float)args[1].as.float_value : (float)args[1].as.int_value;
    float w = (args[2].type == VAL_FLOAT) ? (float)args[2].as.float_value : (float)args[2].as.int_value;
    float h = (args[3].type == VAL_FLOAT) ? (float)args[3].as.float_value : (float)args[3].as.int_value;
    float roundness = (args[4].type == VAL_FLOAT) ? (float)args[4].as.float_value : (float)args[4].as.int_value;
    int segments = (args[5].type == VAL_FLOAT) ? (int)args[5].as.float_value : (int)args[5].as.int_value;
    unsigned char r = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    unsigned char g = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    unsigned char b = (args[8].type == VAL_FLOAT) ? (unsigned char)args[8].as.float_value : (unsigned char)args[8].as.int_value;
    unsigned char a = (args[9].type == VAL_FLOAT) ? (unsigned char)args[9].as.float_value : (unsigned char)args[9].as.int_value;
    Rectangle rec = {x, y, w, h};
    DrawRectangleRounded(rec, roundness, segments, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_measureText(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2) {
        fprintf(stderr, "error: measureText expects 2 args (text, fontSize), got %zu\n", count);
        return false;
    }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "error: measureText expects text as string\n");
        return false;
    }
    const char* text = args[0].as.string_value.chars;
    int fontSize = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    out->has_value = true;
    out->value = value_int(MeasureText(text, fontSize));
    return true;
}

static bool native_drawCircle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 7) {
        fprintf(stderr, "error: drawCircle expects 7 args, got %zu\n", count);
        return false;
    }
    int x = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int y = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    float rad = (args[2].type == VAL_FLOAT) ? (float)args[2].as.float_value : (float)args[2].as.int_value;
    unsigned char r = (args[3].type == VAL_FLOAT) ? (unsigned char)args[3].as.float_value : (unsigned char)args[3].as.int_value;
    unsigned char g = (args[4].type == VAL_FLOAT) ? (unsigned char)args[4].as.float_value : (unsigned char)args[4].as.int_value;
    unsigned char b = (args[5].type == VAL_FLOAT) ? (unsigned char)args[5].as.float_value : (unsigned char)args[5].as.int_value;
    unsigned char a = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    DrawCircle(x, y, rad, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawCircleGradient(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 11) {
        fprintf(stderr, "error: drawCircleGradient expects 11 args (x, y, radius, Color1(4), Color2(4)), got %zu\n", count);
        return false;
    }
    int x = (int)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    int y = (int)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float rad = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    unsigned char r1 = (unsigned char)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    unsigned char g1 = (unsigned char)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    unsigned char b1 = (unsigned char)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char a1 = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char r2 = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char g2 = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char b2 = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    unsigned char a2 = (unsigned char)((args[10].type == VAL_FLOAT) ? args[10].as.float_value : args[10].as.int_value);
    DrawCircleGradient(x, y, rad, (Color){r1, g1, b1, a1}, (Color){r2, g2, b2, a2});
    out->has_value = false;
    return true;
}

static bool native_drawRectangleGradientV(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 12) {
        fprintf(stderr, "error: drawRectangleGradientV expects 12 args, got %zu\n", count);
        return false;
    }
    int x = (int)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    int y = (int)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    int w = (int)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    int h = (int)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    unsigned char r1 = (unsigned char)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    unsigned char g1 = (unsigned char)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char b1 = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char a1 = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char r2 = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char g2 = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    unsigned char b2 = (unsigned char)((args[10].type == VAL_FLOAT) ? args[10].as.float_value : args[10].as.int_value);
    unsigned char a2 = (unsigned char)((args[11].type == VAL_FLOAT) ? args[11].as.float_value : args[11].as.int_value);
    DrawRectangleGradientV(x, y, w, h, (Color){r1, g1, b1, a1}, (Color){r2, g2, b2, a2});
    out->has_value = false;
    return true;
}

static bool native_drawRectangleGradientH(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 12) {
        fprintf(stderr, "error: drawRectangleGradientH expects 12 args, got %zu\n", count);
        return false;
    }
    int x = (int)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    int y = (int)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    int w = (int)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    int h = (int)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    unsigned char r1 = (unsigned char)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    unsigned char g1 = (unsigned char)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char b1 = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char a1 = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char r2 = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char g2 = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    unsigned char b2 = (unsigned char)((args[10].type == VAL_FLOAT) ? args[10].as.float_value : args[10].as.int_value);
    unsigned char a2 = (unsigned char)((args[11].type == VAL_FLOAT) ? args[11].as.float_value : args[11].as.int_value);
    DrawRectangleGradientH(x, y, w, h, (Color){r1, g1, b1, a1}, (Color){r2, g2, b2, a2});
    out->has_value = false;
    return true;
}

static bool native_drawText(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) {
        fprintf(stderr, "error: drawText expects 8 args, got %zu\n", count);
        return false;
    }
    
    const char* text = "???";
    if (args[0].type == VAL_STRING) {
        text = args[0].as.string_value.chars;
    } else {
        fprintf(stderr, "error: drawText expects string, got type %d\n", args[0].type);
    }

    int x = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    int y = (args[2].type == VAL_FLOAT) ? (int)args[2].as.float_value : (int)args[2].as.int_value;
    int size = (args[3].type == VAL_FLOAT) ? (int)args[3].as.float_value : (int)args[3].as.int_value;
    unsigned char r = (args[4].type == VAL_FLOAT) ? (unsigned char)args[4].as.float_value : (unsigned char)args[4].as.int_value;
    unsigned char g = (args[5].type == VAL_FLOAT) ? (unsigned char)args[5].as.float_value : (unsigned char)args[5].as.int_value;
    unsigned char b = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    unsigned char a = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    DrawText(text, x, y, size, (Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_setTargetFPS(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: setTargetFPS expects 1 arg, got %zu\n", count);
        return false;
    }
    int fps = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    SetTargetFPS(fps);
    out->has_value = false;
    return true;
}

static bool native_waitEventsTimeout(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: waitEventsTimeout expects 1 arg, got %zu\n", count);
        return false;
    }
    double seconds = (args[0].type == VAL_FLOAT) ? args[0].as.float_value : (double)args[0].as.int_value;
    glfwWaitEventsTimeout(seconds);
    out->has_value = false;
    return true;
}

static bool native_waitEvents(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: waitEvents expects 0 args, got %zu\n", count);
        return false;
    }
    glfwWaitEvents();
    out->has_value = false;
    return true;
}

static bool native_postEmptyEvent(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: postEmptyEvent expects 0 args, got %zu\n", count);
        return false;
    }
    glfwPostEmptyEvent();
    out->has_value = false;
    return true;
}

extern void rae_ext_disableAppNap(void);

static bool native_disableAppNap(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: disableAppNap expects 0 args, got %zu\n", count);
        return false;
    }
    rae_ext_disableAppNap();
    out->has_value = false;
    return true;
}

static bool native_installWindowCloseWaker(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: installWindowCloseWaker expects 0 args, got %zu\n", count);
        return false;
    }
    GLFWwindow* w = glfwGetCurrentContext();
    if (w) {
        glfwSetWindowCloseCallback(w, vm_glfw_close_waker);
        fprintf(stderr, "[close-waker] installed for window %p\n", (void*)w);
    } else {
        fprintf(stderr, "[close-waker] FAILED: no current GLFW context\n");
    }
    fflush(stderr);
    out->has_value = false;
    return true;
}

static bool native_installMouseButtonHook(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: installMouseButtonHook expects 0 args, got %zu\n", count);
        return false;
    }
    if (g_vm_mouse_button_hook_installed) {
        out->has_value = false;
        return true;
    }
    GLFWwindow* w = glfwGetCurrentContext();
    if (!w) {
        fprintf(stderr, "[mouse-hook] FAILED: no current GLFW context\n");
        fflush(stderr);
        out->has_value = false;
        return true;
    }
    g_vm_prev_mouse_button_cb = glfwSetMouseButtonCallback(w, vm_glfw_mouse_button_chain_cb);
    g_vm_mouse_button_hook_installed = 1;
    fprintf(stderr, "[mouse-hook] installed (prev cb=%p)\n", (void*)g_vm_prev_mouse_button_cb);
    fflush(stderr);
    out->has_value = false;
    return true;
}

static bool native_mouseHookDrainPressed(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    int v = g_vm_mouse_press_pending;
    g_vm_mouse_press_pending = 0;
    out->has_value = true;
    out->value = value_int(v != 0 ? 1 : 0);
    return true;
}

static bool native_mouseHookDrainReleased(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    int v = g_vm_mouse_release_pending;
    g_vm_mouse_release_pending = 0;
    out->has_value = true;
    out->value = value_int(v != 0 ? 1 : 0);
    return true;
}

static bool native_isKeyDown(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: isKeyDown expects 1 arg, got %zu\n", count);
        return false;
    }
    int key = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsKeyDown(key) ? 1 : 0);
    return true;
}

static bool native_isKeyPressed(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: isKeyPressed expects 1 arg, got %zu\n", count);
        return false;
    }
    int key = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsKeyPressed(key) ? 1 : 0);
    return true;
}

static bool native_getMouseX(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetMouseX());
    return true;
}

static bool native_getMouseY(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetMouseY());
    return true;
}

static bool native_getMouseWheelMove(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_float((double)GetMouseWheelMove());
    return true;
}

static bool native_isMouseButtonDown(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) { fprintf(stderr, "error: isMouseButtonDown expects 1 arg, got %zu\n", count); return false; }
    int b = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsMouseButtonDown(b) ? 1 : 0);
    return true;
}

static bool native_isMouseButtonPressed(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) { fprintf(stderr, "error: isMouseButtonPressed expects 1 arg, got %zu\n", count); return false; }
    int b = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsMouseButtonPressed(b) ? 1 : 0);
    return true;
}

static bool native_isMouseButtonReleased(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) { fprintf(stderr, "error: isMouseButtonReleased expects 1 arg, got %zu\n", count); return false; }
    int b = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsMouseButtonReleased(b) ? 1 : 0);
    return true;
}

static bool native_setMouseScale(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2) { fprintf(stderr, "error: setMouseScale expects 2 args, got %zu\n", count); return false; }
    float sx = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float sy = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    SetMouseScale(sx, sy);
    out->has_value = false;
    return true;
}

static bool native_drawCube(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 10) {
        fprintf(stderr, "error: drawCube expects 10 args, got %zu\n", count);
        return false;
    }
    float px = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float py = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float pz = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    float w = (float)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    float h = (float)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    float l = (float)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char cr = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char cg = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char cb = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char ca = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    DrawCube((Vector3){px, py, pz}, w, h, l, (Color){cr, cg, cb, ca});
    out->has_value = false;
    return true;
}

static bool native_drawCubeWires(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 10) {
        fprintf(stderr, "error: drawCubeWires expects 10 args, got %zu\n", count);
        return false;
    }
    float px = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float py = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float pz = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    float w = (float)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    float h = (float)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    float l = (float)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char cr = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char cg = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char cb = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char ca = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    DrawCubeWires((Vector3){px, py, pz}, w, h, l, (Color){cr, cg, cb, ca});
    out->has_value = false;
    return true;
}

static bool native_drawSphere(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) {
        fprintf(stderr, "error: drawSphere expects 8 args (Vector3 flattened + radius + Color flattened), got %zu\n", count);
        return false;
    }
    float px = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float py = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float pz = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    float rad = (float)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    unsigned char cr = (unsigned char)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    unsigned char cg = (unsigned char)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    unsigned char cb = (unsigned char)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char ca = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    DrawSphere((Vector3){px, py, pz}, rad, (Color){cr, cg, cb, ca});
    out->has_value = false;
    return true;
}

static bool native_drawCylinder(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 11) {
        fprintf(stderr, "error: drawCylinder expects 11 args (Vector3 flattened + radTop + radBot + height + slices + Color flattened), got %zu\n", count);
        return false;
    }
    float px = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float py = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float pz = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    float radTop = (float)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    float radBot = (float)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    float h = (float)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    int slices = (int)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    unsigned char cr = (unsigned char)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    unsigned char cg = (unsigned char)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    unsigned char cb = (unsigned char)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    unsigned char ca = (unsigned char)((args[10].type == VAL_FLOAT) ? args[10].as.float_value : args[10].as.int_value);
    DrawCylinder((Vector3){px, py, pz}, radTop, radBot, h, slices, (Color){cr, cg, cb, ca});
    out->has_value = false;
    return true;
}

static bool native_getTime(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    if (count != 0) {
        fprintf(stderr, "error: getTime expects 0 args, got %zu\n", count);
        return false;
    }
    out->has_value = true;
    out->value = value_float(GetTime());
    return true;
}

static bool native_colorFromHSV(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) {
        fprintf(stderr, "error: colorFromHSV expects 3 args, got %zu\n", count);
        return false;
    }
    float h = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float s = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    float v = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    Color c = ColorFromHSV(h, s, v);
    out->has_value = true;
    out->value = value_object(4, "Color");
    out->value.as.object_value.fields[0] = value_int(c.r);
    out->value.as.object_value.fields[1] = value_int(c.g);
    out->value.as.object_value.fields[2] = value_int(c.b);
    out->value.as.object_value.fields[3] = value_int(c.a);
    return true;
}

static bool native_drawGrid(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2) {
        fprintf(stderr, "error: drawGrid expects 2 args, got %zu\n", count);
        return false;
    }
    int slices = (int)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    float spacing = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    DrawGrid(slices, spacing);
    out->has_value = false;
    return true;
}

static bool native_beginMode3D(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 11) {
        fprintf(stderr, "error: beginMode3D expects 11 args, got %zu\n", count);
        return false;
    }
    Camera3D cam = {0};
    cam.position.x = (float)((args[0].type == VAL_FLOAT) ? args[0].as.float_value : args[0].as.int_value);
    cam.position.y = (float)((args[1].type == VAL_FLOAT) ? args[1].as.float_value : args[1].as.int_value);
    cam.position.z = (float)((args[2].type == VAL_FLOAT) ? args[2].as.float_value : args[2].as.int_value);
    cam.target.x = (float)((args[3].type == VAL_FLOAT) ? args[3].as.float_value : args[3].as.int_value);
    cam.target.y = (float)((args[4].type == VAL_FLOAT) ? args[4].as.float_value : args[4].as.int_value);
    cam.target.z = (float)((args[5].type == VAL_FLOAT) ? args[5].as.float_value : args[5].as.int_value);
    cam.up.x = (float)((args[6].type == VAL_FLOAT) ? args[6].as.float_value : args[6].as.int_value);
    cam.up.y = (float)((args[7].type == VAL_FLOAT) ? args[7].as.float_value : args[7].as.int_value);
    cam.up.z = (float)((args[8].type == VAL_FLOAT) ? args[8].as.float_value : args[8].as.int_value);
    cam.fovy = (float)((args[9].type == VAL_FLOAT) ? args[9].as.float_value : args[9].as.int_value);
    cam.projection = (int)((args[10].type == VAL_FLOAT) ? args[10].as.float_value : args[10].as.int_value);
    BeginMode3D(cam);
    out->has_value = false;
    return true;
}

static bool native_endMode3D(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) {
        fprintf(stderr, "error: endMode3D expects 0 args, got %zu\n", count);
        return false;
    }
    EndMode3D();
    out->has_value = false;
    return true;
}

/* Font slot natives for the VM. We don't share storage with the compiled
 * runtime — the runtime's raylib bits are #ifdef'd out of bin/rae's
 * rae_runtime.o — so this module owns its own font slots. The slot indices
 * + codepoint coverage match the runtime side so user code is portable. */
#define VM_FONT_SLOTS 8
static Font g_vm_fonts[VM_FONT_SLOTS];
static int g_vm_font_loaded[VM_FONT_SLOTS];

static const int g_vm_font_codepoints[] = {
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    0x00B7, 0x2013, 0x2014, 0x2192, 0x2190, 0x2191, 0x2193, 0x2026,
    /* Material Icons Outlined codepoints. See `lib/ui/icon_codepoints.rae`. */
    0xe145, 0xe5c4, 0xe5cb, 0xe5cc, 0xe5cd, 0xf090, 0xe01d, 0xe5ce,
    0xe5cf, 0xe87d, 0xe87e, 0xe88a, 0xe02e, 0xe02f, 0xe030, 0xe5d2,
    0xeae1, 0xe034, 0xe037, 0xe03b, 0xe065, 0xe05f, 0xe03d, 0xe040,
    0xe8b6, 0xe043, 0xe044, 0xe045, 0xe047, 0xe80e, 0xe7fd,
    0xe429, /* tune */
    0xe9ba  /* logout */
};
#define VM_FONT_CODEPOINT_COUNT ((int)(sizeof(g_vm_font_codepoints)/sizeof(g_vm_font_codepoints[0])))

static bool native_loadFontInto(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) {
        fprintf(stderr, "error: loadFontInto expects 3 args (slot, path, fontSize), got %zu\n", count);
        return false;
    }
    int64_t slot = (args[0].type == VAL_FLOAT) ? (int64_t)args[0].as.float_value : args[0].as.int_value;
    if (args[1].type != VAL_STRING) {
        fprintf(stderr, "error: loadFontInto expects path as string\n");
        return false;
    }
    const char* path = args[1].as.string_value.chars;
    int fontSize = (args[2].type == VAL_FLOAT) ? (int)args[2].as.float_value : (int)args[2].as.int_value;
    if (slot < 0 || slot >= VM_FONT_SLOTS) {
        out->has_value = false;
        return true;
    }
    if (g_vm_font_loaded[slot]) {
        UnloadFont(g_vm_fonts[slot]);
        g_vm_font_loaded[slot] = 0;
    }
    /* High-res atlas + bilinear filter — same strategy as the compiled
     * runtime; see rae_runtime.c::rae_ext_loadFontInto for the why. */
    int atlasSize = fontSize * 2;
    if (atlasSize < 64) atlasSize = 64;
    g_vm_fonts[slot] = LoadFontEx(path, atlasSize, (int*)g_vm_font_codepoints, VM_FONT_CODEPOINT_COUNT);
    if (g_vm_fonts[slot].texture.id != 0) {
        SetTextureFilter(g_vm_fonts[slot].texture, TEXTURE_FILTER_BILINEAR);
        g_vm_font_loaded[slot] = 1;
    }
    out->has_value = false;
    return true;
}

static bool native_unloadFontSlot(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: unloadFontSlot expects 1 arg, got %zu\n", count);
        return false;
    }
    int64_t slot = (args[0].type == VAL_FLOAT) ? (int64_t)args[0].as.float_value : args[0].as.int_value;
    if (slot >= 0 && slot < VM_FONT_SLOTS && g_vm_font_loaded[slot]) {
        UnloadFont(g_vm_fonts[slot]);
        g_vm_font_loaded[slot] = 0;
    }
    out->has_value = false;
    return true;
}

static bool native_isFontSlotLoaded(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: isFontSlotLoaded expects 1 arg, got %zu\n", count);
        return false;
    }
    int64_t slot = (args[0].type == VAL_FLOAT) ? (int64_t)args[0].as.float_value : args[0].as.int_value;
    int loaded = (slot >= 0 && slot < VM_FONT_SLOTS && g_vm_font_loaded[slot]) ? 1 : 0;
    out->has_value = true;
    out->value = value_int(loaded);
    return true;
}

static bool native_drawTextWithFont(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 10) {
        fprintf(stderr, "error: drawTextWithFont expects 10 args (slot, text, x, y, fontSize, spacing, r, g, b, a), got %zu\n", count);
        return false;
    }
    int64_t slot = (args[0].type == VAL_FLOAT) ? (int64_t)args[0].as.float_value : args[0].as.int_value;
    if (args[1].type != VAL_STRING) {
        fprintf(stderr, "error: drawTextWithFont expects text as string\n");
        return false;
    }
    const char* text = args[1].as.string_value.chars;
    float x = (args[2].type == VAL_FLOAT) ? (float)args[2].as.float_value : (float)args[2].as.int_value;
    float y = (args[3].type == VAL_FLOAT) ? (float)args[3].as.float_value : (float)args[3].as.int_value;
    float fontSize = (args[4].type == VAL_FLOAT) ? (float)args[4].as.float_value : (float)args[4].as.int_value;
    float spacing = (args[5].type == VAL_FLOAT) ? (float)args[5].as.float_value : (float)args[5].as.int_value;
    unsigned char r = (args[6].type == VAL_FLOAT) ? (unsigned char)args[6].as.float_value : (unsigned char)args[6].as.int_value;
    unsigned char g = (args[7].type == VAL_FLOAT) ? (unsigned char)args[7].as.float_value : (unsigned char)args[7].as.int_value;
    unsigned char b = (args[8].type == VAL_FLOAT) ? (unsigned char)args[8].as.float_value : (unsigned char)args[8].as.int_value;
    unsigned char a = (args[9].type == VAL_FLOAT) ? (unsigned char)args[9].as.float_value : (unsigned char)args[9].as.int_value;
    Color col = {r, g, b, a};
    if (slot >= 0 && slot < VM_FONT_SLOTS && g_vm_font_loaded[slot]) {
        DrawTextEx(g_vm_fonts[slot], text, (Vector2){x, y}, fontSize, spacing, col);
    } else {
        DrawText(text, (int)x, (int)y, (int)fontSize, col);
    }
    out->has_value = false;
    return true;
}

/* Slot-aware width measurement; companion to native_drawTextWithFont.
 * MeasureText (the default-font measurer) returns the wrong width for
 * codepoints not present in the default font (e.g. Material Icons),
 * which puts those glyphs off-center inside their containers. */
static bool native_measureTextWithFont(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 4) {
        fprintf(stderr, "error: measureTextWithFont expects 4 args (slot, text, fontSize, spacing), got %zu\n", count);
        return false;
    }
    int64_t slot = (args[0].type == VAL_FLOAT) ? (int64_t)args[0].as.float_value : args[0].as.int_value;
    if (args[1].type != VAL_STRING) {
        fprintf(stderr, "error: measureTextWithFont expects text as string\n");
        return false;
    }
    const char* text = args[1].as.string_value.chars;
    float fontSize = (args[2].type == VAL_FLOAT) ? (float)args[2].as.float_value : (float)args[2].as.int_value;
    float spacing = (args[3].type == VAL_FLOAT) ? (float)args[3].as.float_value : (float)args[3].as.int_value;
    int64_t width = 0;
    if (slot >= 0 && slot < VM_FONT_SLOTS && g_vm_font_loaded[slot]) {
        Vector2 sz = MeasureTextEx(g_vm_fonts[slot], text, fontSize, spacing);
        width = (int64_t)sz.x;
    } else {
        width = (int64_t)MeasureText(text, (int)fontSize);
    }
    out->has_value = true;
    out->value = value_int(width);
    return true;
}

static bool native_takeScreenshot(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: takeScreenshot expects 1 arg (fileName), got %zu\n", count);
        return false;
    }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "error: takeScreenshot expects fileName as string\n");
        return false;
    }
    const char* fileName = (const char*)args[0].as.string_value.chars;
    TakeScreenshot(fileName);
    out->has_value = false;
    return true;
}

/* Window/monitor natives — used by examples that want to pick a window
 * size relative to the user's display. */
static bool native_getCurrentMonitor(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args; (void)count;
    out->has_value = true;
    out->value = value_int(GetCurrentMonitor());
    return true;
}

static bool native_getMonitorWidth(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: getMonitorWidth expects 1 arg, got %zu\n", count);
        return false;
    }
    int monitor = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(GetMonitorWidth(monitor));
    return true;
}

static bool native_getMonitorHeight(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) {
        fprintf(stderr, "error: getMonitorHeight expects 1 arg, got %zu\n", count);
        return false;
    }
    int monitor = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(GetMonitorHeight(monitor));
    return true;
}

static bool native_setWindowSize(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2) {
        fprintf(stderr, "error: setWindowSize expects 2 args, got %zu\n", count);
        return false;
    }
    int w = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int h = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    SetWindowSize(w, h);
    out->has_value = false;
    return true;
}

static bool native_setWindowPosition(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 2) {
        fprintf(stderr, "error: setWindowPosition expects 2 args, got %zu\n", count);
        return false;
    }
    int x = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    int y = (args[1].type == VAL_FLOAT) ? (int)args[1].as.float_value : (int)args[1].as.int_value;
    SetWindowPosition(x, y);
    out->has_value = false;
    return true;
}

static bool native_getWindowPositionX(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)args; (void)count; (void)data;
    Vector2 p = GetWindowPosition();
    out->has_value = true;
    out->value = (Value){.type = VAL_INT, .as.int_value = (int64_t)p.x};
    return true;
}

static bool native_getWindowPositionY(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)args; (void)count; (void)data;
    Vector2 p = GetWindowPosition();
    out->has_value = true;
    out->value = (Value){.type = VAL_INT, .as.int_value = (int64_t)p.y};
    return true;
}

bool vm_registry_register_raylib(VmRegistry* registry) {
    bool ok = true;
    ok &= vm_registry_register_native(registry, "initWindow", native_initWindow, NULL);
    ok &= vm_registry_register_native(registry, "setConfigFlags", native_setConfigFlags, NULL);
    ok &= vm_registry_register_native(registry, "windowShouldClose", native_windowShouldClose, NULL);
    ok &= vm_registry_register_native(registry, "closeWindow", native_closeWindow, NULL);
    ok &= vm_registry_register_native(registry, "beginDrawing", native_beginDrawing, NULL);
    ok &= vm_registry_register_native(registry, "endDrawing", native_endDrawing, NULL);
    ok &= vm_registry_register_native(registry, "clearBackground", native_clearBackground, NULL);
    ok &= vm_registry_register_native(registry, "loadTexture", native_loadTexture, NULL);
    ok &= vm_registry_register_native(registry, "loadCircleCroppedTexture", native_loadCircleCroppedTexture, NULL);
    ok &= vm_registry_register_native(registry, "loadRoundedCroppedTexture", native_loadRoundedCroppedTexture, NULL);
    ok &= vm_registry_register_native(registry, "unloadTexture", native_unloadTexture, NULL);
    ok &= vm_registry_register_native(registry, "raylibSetLogLevel", native_raylibSetLogLevel, NULL);
    ok &= vm_registry_register_native(registry, "roundedSpriteBegin", native_roundedSpriteBegin, NULL);
    ok &= vm_registry_register_native(registry, "roundedSpriteEnd", native_roundedSpriteEnd, NULL);
    ok &= vm_registry_register_native(registry, "drawGradientRect", native_drawGradientRect, NULL);
    ok &= vm_registry_register_native(registry, "captureAndBlurBackdrop", native_captureAndBlurBackdrop, NULL);
    ok &= vm_registry_register_native(registry, "captureAndBlurRegion", native_captureAndBlurRegion, NULL);
    ok &= vm_registry_register_native(registry, "drawTexture", native_drawTexture, NULL);
    ok &= vm_registry_register_native(registry, "drawTextureEx", native_drawTextureEx, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangle", native_drawRectangle, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangleLines", native_drawRectangleLines, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangleRounded", native_drawRectangleRounded, NULL);
    ok &= vm_registry_register_native(registry, "measureText", native_measureText, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangleGradientV", native_drawRectangleGradientV, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangleGradientH", native_drawRectangleGradientH, NULL);
    ok &= vm_registry_register_native(registry, "drawCircle", native_drawCircle, NULL);
    ok &= vm_registry_register_native(registry, "drawCircleGradient", native_drawCircleGradient, NULL);
    ok &= vm_registry_register_native(registry, "drawText", native_drawText, NULL);
    ok &= vm_registry_register_native(registry, "drawCube", native_drawCube, NULL);
    ok &= vm_registry_register_native(registry, "drawCubeWires", native_drawCubeWires, NULL);
    ok &= vm_registry_register_native(registry, "drawSphere", native_drawSphere, NULL);
    ok &= vm_registry_register_native(registry, "drawCylinder", native_drawCylinder, NULL);
    ok &= vm_registry_register_native(registry, "drawGrid", native_drawGrid, NULL);
    ok &= vm_registry_register_native(registry, "beginMode3D", native_beginMode3D, NULL);
    ok &= vm_registry_register_native(registry, "endMode3D", native_endMode3D, NULL);
    ok &= vm_registry_register_native(registry, "setTargetFPS", native_setTargetFPS, NULL);
    ok &= vm_registry_register_native(registry, "waitEventsTimeout", native_waitEventsTimeout, NULL);
    ok &= vm_registry_register_native(registry, "waitEvents", native_waitEvents, NULL);
    ok &= vm_registry_register_native(registry, "postEmptyEvent", native_postEmptyEvent, NULL);
    ok &= vm_registry_register_native(registry, "installWindowCloseWaker", native_installWindowCloseWaker, NULL);
    ok &= vm_registry_register_native(registry, "installMouseButtonHook", native_installMouseButtonHook, NULL);
    ok &= vm_registry_register_native(registry, "mouseHookDrainPressed", native_mouseHookDrainPressed, NULL);
    ok &= vm_registry_register_native(registry, "mouseHookDrainReleased", native_mouseHookDrainReleased, NULL);
    ok &= vm_registry_register_native(registry, "disableAppNap", native_disableAppNap, NULL);
    ok &= vm_registry_register_native(registry, "isKeyDown", native_isKeyDown, NULL);
    ok &= vm_registry_register_native(registry, "isKeyPressed", native_isKeyPressed, NULL);
    ok &= vm_registry_register_native(registry, "getMouseX", native_getMouseX, NULL);
    ok &= vm_registry_register_native(registry, "getMouseY", native_getMouseY, NULL);
    ok &= vm_registry_register_native(registry, "getMouseWheelMove", native_getMouseWheelMove, NULL);
    ok &= vm_registry_register_native(registry, "isMouseButtonDown", native_isMouseButtonDown, NULL);
    ok &= vm_registry_register_native(registry, "isMouseButtonPressed", native_isMouseButtonPressed, NULL);
    ok &= vm_registry_register_native(registry, "isMouseButtonReleased", native_isMouseButtonReleased, NULL);
    ok &= vm_registry_register_native(registry, "setMouseScale", native_setMouseScale, NULL);
    ok &= vm_registry_register_native(registry, "getTime", native_getTime, NULL);
    ok &= vm_registry_register_native(registry, "getScreenWidth", native_getScreenWidth, NULL);
    ok &= vm_registry_register_native(registry, "getScreenHeight", native_getScreenHeight, NULL);
    ok &= vm_registry_register_native(registry, "getRenderWidth", native_getRenderWidth, NULL);
    ok &= vm_registry_register_native(registry, "getRenderHeight", native_getRenderHeight, NULL);
    ok &= vm_registry_register_native(registry, "colorFromHSV", native_colorFromHSV, NULL);
    ok &= vm_registry_register_native(registry, "takeScreenshot", native_takeScreenshot, NULL);
    ok &= vm_registry_register_native(registry, "loadFontInto", native_loadFontInto, NULL);
    ok &= vm_registry_register_native(registry, "unloadFontSlot", native_unloadFontSlot, NULL);
    ok &= vm_registry_register_native(registry, "isFontSlotLoaded", native_isFontSlotLoaded, NULL);
    ok &= vm_registry_register_native(registry, "drawTextWithFont", native_drawTextWithFont, NULL);
    ok &= vm_registry_register_native(registry, "measureTextWithFont", native_measureTextWithFont, NULL);
    ok &= vm_registry_register_native(registry, "getCurrentMonitor", native_getCurrentMonitor, NULL);
    ok &= vm_registry_register_native(registry, "getMonitorWidth", native_getMonitorWidth, NULL);
    ok &= vm_registry_register_native(registry, "getMonitorHeight", native_getMonitorHeight, NULL);
    ok &= vm_registry_register_native(registry, "setWindowSize", native_setWindowSize, NULL);
    ok &= vm_registry_register_native(registry, "setWindowPosition", native_setWindowPosition, NULL);
    ok &= vm_registry_register_native(registry, "getWindowPositionX", native_getWindowPositionX, NULL);
    ok &= vm_registry_register_native(registry, "getWindowPositionY", native_getWindowPositionY, NULL);
    return ok;
}
