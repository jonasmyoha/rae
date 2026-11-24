/* Files and text dragged onto the window from the OS (SDL's drop events), for
 * Gpu2d.readOsDrop. Collected here and read out; what a drop does is Rae's
 * (ui/DragSystem). Included by runtime_gpu2d_platform.c, which hands the
 * events over in its event loop. */

static int g_g2d_drop_over = 0;       /* something is held over the window */
static int g_g2d_drop_done = 0;       /* let go since the last poll */
static int g_g2d_drop_text = 0;       /* it is text, not files */
static float g_g2d_drop_x = 0.0f, g_g2d_drop_y = 0.0f;   /* window points */
static char** g_g2d_drop_values = NULL;
static int g_g2d_drop_n = 0, g_g2d_drop_cap = 0;

static void rae_g2d_drop_clear(void) {
    for (int i = 0; i < g_g2d_drop_n; i++) SDL_free(g_g2d_drop_values[i]);
    g_g2d_drop_n = 0;
    g_g2d_drop_text = 0;
}

static void rae_g2d_drop_add(const char* value) {
    if (!value) return;
    if (g_g2d_drop_n == g_g2d_drop_cap) {
        int cap = g_g2d_drop_cap ? g_g2d_drop_cap * 2 : 16;
        char** grown = (char**)SDL_realloc(g_g2d_drop_values, sizeof(char*) * (size_t)cap);
        if (!grown) return;
        g_g2d_drop_values = grown;
        g_g2d_drop_cap = cap;
    }
    char* copy = SDL_strdup(value);
    if (copy) g_g2d_drop_values[g_g2d_drop_n++] = copy;
}

/* A new poll: a drop is told of once. */
static void rae_g2d_drop_poll_begin(void) {
    if (g_g2d_drop_done) rae_g2d_drop_clear();
    g_g2d_drop_done = 0;
}

static void rae_g2d_drop_event(const SDL_Event* e) {
    switch (e->type) {
        case SDL_EVENT_DROP_BEGIN:
            rae_g2d_drop_clear();
            g_g2d_drop_over = 1;
            break;
        case SDL_EVENT_DROP_POSITION:
            g_g2d_drop_x = e->drop.x;
            g_g2d_drop_y = e->drop.y;
            break;
        case SDL_EVENT_DROP_TEXT:
        case SDL_EVENT_DROP_FILE:
            g_g2d_drop_text = e->type == SDL_EVENT_DROP_TEXT;
            rae_g2d_drop_add(e->drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE:
            g_g2d_drop_over = 0;
            g_g2d_drop_done = 1;
            break;
        default: break;
    }
}

rae_Bool rae_ext_Gpu2d_dropOver(void) { return g_g2d_drop_over != 0; }
rae_Bool rae_ext_Gpu2d_dropDone(void) { return g_g2d_drop_done != 0; }
rae_Bool rae_ext_Gpu2d_dropIsText(void) { return g_g2d_drop_text != 0; }
int64_t rae_ext_Gpu2d_dropCount(void) { return (int64_t)g_g2d_drop_n; }
rae_String rae_ext_Gpu2d_dropValue(int64_t index) {
    return rae_ext_rae_str_from_cstr(index >= 0 && index < g_g2d_drop_n ? g_g2d_drop_values[index] : "");
}
