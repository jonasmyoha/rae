#include "vm_raylib.h"
#include <raylib.h>
#include <stdio.h>

static bool native_initWindow(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 3) return false;
    InitWindow((int)args[0].as.int_value, (int)args[1].as.int_value, args[2].as.string_value.chars);
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
    ClearBackground((Color){(unsigned char)args[0].as.int_value, (unsigned char)args[1].as.int_value, (unsigned char)args[2].as.int_value, (unsigned char)args[3].as.int_value});
    out->has_value = false;
    return true;
}

static bool native_drawRectangle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) return false;
    DrawRectangle((int)args[0].as.int_value, (int)args[1].as.int_value, (int)args[2].as.int_value, (int)args[3].as.int_value,
                  (Color){(unsigned char)args[4].as.int_value, (unsigned char)args[5].as.int_value, (unsigned char)args[6].as.int_value, (unsigned char)args[7].as.int_value});
    out->has_value = false;
    return true;
}

static bool native_drawCircle(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 7) return false;
    DrawCircle((int)args[0].as.int_value, (int)args[1].as.int_value, (float)args[2].as.int_value,
               (Color){(unsigned char)args[3].as.int_value, (unsigned char)args[4].as.int_value, (unsigned char)args[5].as.int_value, (unsigned char)args[6].as.int_value});
    out->has_value = false;
    return true;
}

static bool native_drawText(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 8) return false;
    DrawText(args[0].as.string_value.chars, (int)args[1].as.int_value, (int)args[2].as.int_value, (int)args[3].as.int_value,
             (Color){(unsigned char)args[4].as.int_value, (unsigned char)args[5].as.int_value, (unsigned char)args[6].as.int_value, (unsigned char)args[7].as.int_value});
    out->has_value = false;
    return true;
}

static bool native_setTargetFPS(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) return false;
    SetTargetFPS((int)args[0].as.int_value);
    out->has_value = false;
    return true;
}

static bool native_isKeyDown(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) return false;
    out->has_value = true;
    out->value = value_int(IsKeyDown((int)args[0].as.int_value) ? 1 : 0);
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
