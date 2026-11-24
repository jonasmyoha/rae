/* What the OS sends when files are dragged onto the window, for checks/osDrop. */
#include <SDL3/SDL.h>

static void push(Uint32 type, float x, float y, const char* data) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.drop.x = x;
    e.drop.y = y;
    e.drop.data = data;
    SDL_PushEvent(&e);
}

void rae_ext_osDropHold(float x, float y) {
    push(SDL_EVENT_DROP_BEGIN, 0, 0, NULL);
    push(SDL_EVENT_DROP_POSITION, x, y, NULL);
}

void rae_ext_osDropLetGo(float x, float y) {
    push(SDL_EVENT_DROP_POSITION, x, y, NULL);
    push(SDL_EVENT_DROP_FILE, x, y, "/tmp/one picture.png");
    push(SDL_EVENT_DROP_FILE, x, y, "/tmp/zwölf.png");
    push(SDL_EVENT_DROP_COMPLETE, x, y, NULL);
}

void rae_ext_osDropLeave(void) { push(SDL_EVENT_DROP_COMPLETE, 0, 0, NULL); }
