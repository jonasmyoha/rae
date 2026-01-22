#include "vm_raylib.h"
#include <raylib.h>
#include <stdio.h>

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
    out->value = value_object(4);
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
    ok &= vm_registry_register_native(registry, "drawCube", native_drawCube, NULL);
    ok &= vm_registry_register_native(registry, "drawCubeWires", native_drawCubeWires, NULL);
    ok &= vm_registry_register_native(registry, "drawSphere", native_drawSphere, NULL);
    ok &= vm_registry_register_native(registry, "drawCylinder", native_drawCylinder, NULL);
    ok &= vm_registry_register_native(registry, "drawGrid", native_drawGrid, NULL);
    ok &= vm_registry_register_native(registry, "beginMode3D", native_beginMode3D, NULL);
    ok &= vm_registry_register_native(registry, "endMode3D", native_endMode3D, NULL);
    ok &= vm_registry_register_native(registry, "setTargetFPS", native_setTargetFPS, NULL);
    ok &= vm_registry_register_native(registry, "isKeyDown", native_isKeyDown, NULL);
    ok &= vm_registry_register_native(registry, "getTime", native_getTime, NULL);
    ok &= vm_registry_register_native(registry, "colorFromHSV", native_colorFromHSV, NULL);
    return ok;
}
