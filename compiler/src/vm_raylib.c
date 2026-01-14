#include "vm_raylib.h"
#include <raylib.h>
#include <stdio.h>

static bool native_initWindow(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) return false;
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

static bool native_windowShouldClose(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) return false;
    out->has_value = true;
    out->value = value_int(WindowShouldClose() ? 1 : 0);
    return true;
}

static bool native_closeWindow(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) return false;
    CloseWindow();
    out->has_value = false;
    return true;
}

static bool native_beginDrawing(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) return false;
    BeginDrawing();
    out->has_value = false;
    return true;
}

static bool native_endDrawing(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data; (void)args;
    if (count != 0) return false;
    EndDrawing();
    out->has_value = false;
    return true;
}

static bool native_clearBackground(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 4) return false;
    unsigned char r = (args[0].type == VAL_FLOAT) ? (unsigned char)args[0].as.float_value : (unsigned char)args[0].as.int_value;
    unsigned char g = (args[1].type == VAL_FLOAT) ? (unsigned char)args[1].as.float_value : (unsigned char)args[1].as.int_value;
    unsigned char b = (args[2].type == VAL_FLOAT) ? (unsigned char)args[2].as.float_value : (unsigned char)args[2].as.int_value;
    unsigned char a = (args[3].type == VAL_FLOAT) ? (unsigned char)args[3].as.float_value : (unsigned char)args[3].as.int_value;
    ClearBackground((Color){r, g, b, a});
    out->has_value = false;
    return true;
}

static bool native_drawRectangle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) return false;
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

static bool native_drawCircle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 7) return false;
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

static bool native_drawText(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) return false;
    
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
    if (count != 1) return false;
    int fps = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    SetTargetFPS(fps);
    out->has_value = false;
    return true;
}

static bool native_isKeyDown(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) return false;
    int key = (args[0].type == VAL_FLOAT) ? (int)args[0].as.float_value : (int)args[0].as.int_value;
    out->has_value = true;
    out->value = value_int(IsKeyDown(key) ? 1 : 0);
    return true;
}

bool vm_registry_register_raylib(VmRegistry* registry) {
    bool ok = true;
    ok &= vm_registry_register_native(registry, "initWindow", native_initWindow, NULL);
    ok &= vm_registry_register_native(registry, "windowShouldClose", native_windowShouldClose, NULL);
    ok &= vm_registry_register_native(registry, "closeWindow", native_closeWindow, NULL);
    ok &= vm_registry_register_native(registry, "beginDrawing", native_beginDrawing, NULL);
    ok &= vm_registry_register_native(registry, "endDrawing", native_endDrawing, NULL);
    ok &= vm_registry_register_native(registry, "clearBackground", native_clearBackground, NULL);
    ok &= vm_registry_register_native(registry, "drawRectangle", native_drawRectangle, NULL);
    ok &= vm_registry_register_native(registry, "drawCircle", native_drawCircle, NULL);
    ok &= vm_registry_register_native(registry, "drawText", native_drawText, NULL);
    ok &= vm_registry_register_native(registry, "setTargetFPS", native_setTargetFPS, NULL);
    ok &= vm_registry_register_native(registry, "isKeyDown", native_isKeyDown, NULL);
    return ok;
}
