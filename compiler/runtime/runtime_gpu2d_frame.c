/* gpu2d frame begin/end, screenshot readback, flush, present, and shutdown. Raw GPU frame operations stay C; frame policy can migrate later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

void rae_ext_gpu2d_beginFrame(double r, double g, double b, double a) {
    g_g2d_last_present_ok = 0;
    g_g2d_prim_count = 0;
    for (int i = 0; i < RAE_SDF_MAX_ATLAS; i++) g_g2d_text_count[i] = 0;
    g_g2d_img_cmd_count = 0;
    rae_g2d_clip_reset();
    g_g2d_img_frame_bind_n = 0;
    g_g2d_frame_buf_n = 0;
    g_g2d_frame_bind_n = 0;
    g_g2d_box_frame_buf_n = 0;
    for (int i = 0; i < RAE_SDF_MAX_ATLAS; i++) g_g2d_text_frame_buf_n[i] = 0;
    if (!g_g2d_off_view) return;
    /* Render into the persistent offscreen target (NOT the surface drawable),
     * so a frame always renders regardless of window compositing. */
    g_g2d_enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g_g2d_off_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue.r = r; ca.clearValue.g = g; ca.clearValue.b = b; ca.clearValue.a = a;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    g_g2d_pass = wgpuCommandEncoderBeginRenderPass(g_g2d_enc, &rp);
}

/* Headless verification: copy the just-rendered surface texture back to a
 * mapped buffer and save it as a BMP. Called from endFrame after submit while
 * g_g2d_frame_tex is still alive (before present/release). The surface is
 * configured with CopySrc usage (rae_g2d_configure). Gated on the env var so
 * it costs nothing in normal runs. The readback row stride must be 256-aligned
 * (WebGPU copy requirement); we unpad into a tight RGBA buffer for SDL. */
static void rae_g2d_save_screenshot(const char* path) {
    if (!path || !g_g2d_off_tex || !g_wgpu_dev) return;
    int w = g_g2d_off_w, h = g_g2d_off_h;
    if (w <= 0 || h <= 0) return;
    uint32_t bpr = (uint32_t)w * 4u;
    uint32_t padded = (bpr + 255u) & ~255u;            /* 256-byte row align */
    size_t bytes = (size_t)padded * (size_t)h;
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = bytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);
    if (!staging) return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUTexelCopyTextureInfo src; memset(&src, 0, sizeof(src));
    src.texture = g_g2d_off_tex; src.mipLevel = 0; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst; memset(&dst, 0, sizeof(dst));
    dst.buffer = staging; dst.layout.offset = 0;
    dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext; ext.width = (uint32_t)w; ext.height = (uint32_t)h; ext.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    const unsigned char* px = (const unsigned char*)wgpuBufferGetConstMappedRange(staging, 0, bytes);
    if (px) {
        unsigned char* rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4u);
        if (rgba) {
            int swap_rb = (g_g2d_fmt == WGPUTextureFormat_BGRA8Unorm ||
                           g_g2d_fmt == WGPUTextureFormat_BGRA8UnormSrgb);
            for (int y = 0; y < h; y++) {
                const unsigned char* row = px + (size_t)y * padded;
                unsigned char* orow = rgba + (size_t)y * (size_t)w * 4u;
                for (int x = 0; x < w; x++) {
                    unsigned char c0 = row[x*4+0], c1 = row[x*4+1], c2 = row[x*4+2];
                    if (swap_rb) { orow[x*4+0] = c2; orow[x*4+1] = c1; orow[x*4+2] = c0; }
                    else        { orow[x*4+0] = c0; orow[x*4+1] = c1; orow[x*4+2] = c2; }
                    orow[x*4+3] = 255;
                }
            }
            SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
            if (s) { SDL_SaveBMP(s, path); SDL_DestroySurface(s); }
            free(rgba);
        }
        wgpuBufferUnmap(staging);
    }
    wgpuBufferRelease(staging);
}

void rae_ext_gpu2d_endFrame(void) {
    if (!g_g2d_pass) return;
    rae_ext_gpu2d_flush();
    wgpuRenderPassEncoderEnd(g_g2d_pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g_g2d_enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    for (int i = 0; i < g_g2d_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_frame_binds[i]);
    g_g2d_frame_bind_n = 0;
    for (int i = 0; i < g_g2d_frame_buf_n; i++) wgpuBufferRelease(g_g2d_frame_bufs[i]);
    g_g2d_frame_buf_n = 0;
    /* Per-image bind groups are cached by draw slot + texture handle.
     * They stay alive across frames and are released at gpu2d shutdown. */
    g_g2d_img_frame_bind_n = 0;
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(g_g2d_pass); g_g2d_pass = NULL;
    wgpuCommandEncoderRelease(g_g2d_enc); g_g2d_enc = NULL;

    /* Headless screenshot reads the offscreen target — works even when the
     * surface can't vend a drawable. */
    if (g_sdl_headless_ms > 0) {
        const char* shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot) rae_g2d_save_screenshot(shot);
    }

    /* Present best-effort: copy the offscreen image into the surface drawable
     * and present. If the OS won't vend a drawable (window occluded / display
     * asleep / headless), skip — the frame already rendered + screenshotted. */
    WGPUSurfaceTexture st; memset(&st, 0, sizeof(st));
    wgpuSurfaceGetCurrentTexture(g_g2d_surface, &st);
    if (st.texture &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        WGPUCommandEncoder penc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
        WGPUTexelCopyTextureInfo cs; memset(&cs, 0, sizeof(cs)); cs.texture = g_g2d_off_tex; cs.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo cd; memset(&cd, 0, sizeof(cd)); cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
        WGPUExtent3D ext; ext.width = (uint32_t)g_sdl_w; ext.height = (uint32_t)g_sdl_h; ext.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToTexture(penc, &cs, &cd, &ext);
        WGPUCommandBuffer pcb = wgpuCommandEncoderFinish(penc, NULL);
        wgpuQueueSubmit(g_wgpu_queue, 1, &pcb);
        wgpuCommandBufferRelease(pcb); wgpuCommandEncoderRelease(penc);
        wgpuSurfacePresent(g_g2d_surface);
        g_g2d_last_present_ok = 1;
    }
    if (st.texture) wgpuTextureRelease(st.texture);
    /* wgpu-native retires deferred object destruction and presentation work
     * from device polling. Hover animation can render thousands of frames
     * without any readback/map path, so per-frame buffers/bind groups that
     * were released above otherwise sit in the backend's pending queues and
     * show up as a slow RSS climb while the UI is actively animating. */
    if (g_wgpu_dev) wgpuDevicePoll(g_wgpu_dev, false, NULL);
}

rae_Bool rae_ext_gpu2d_lastPresentOk(void) {
    return g_g2d_last_present_ok != 0;
}

void rae_ext_gpu2d_flush(void) {
    if (!g_g2d_pass) return;
    int have_text = 0;
    for (int i = 0; i < RAE_SDF_MAX_ATLAS; i++) if (g_g2d_text_count[i] > 0) have_text = 1;
    int have_img = (g_g2d_img_cmd_count > 0);
    if (g_g2d_prim_count > 0 || have_text || have_img) {
        /* rae_g2d_init_pipeline also creates the shared viewport uniform that
         * the box, image, and text bind groups reference at binding 0. */
        rae_g2d_init_pipeline();
        float xf[8]; rae_g2d_compute_xform(xf);
        wgpuQueueWriteBuffer(g_wgpu_queue, g_g2d_uniform, 0, xf, sizeof(xf));
    }
    if (g_g2d_prim_count > 0) {
        WGPUBuffer instbuf = rae_g2d_box_frame_buffer(g_g2d_prim_count);
        wgpuQueueWriteBuffer(g_wgpu_queue, instbuf, 0, g_g2d_prims,
                             (size_t)g_g2d_prim_count * G2D_PRIM_FLOATS * sizeof(float));
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_pipeline, 0);
        wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_pipeline);
        /* Draw contiguous same-clip runs. Each run sets the scissor (#144, the
         * axis-aligned bbox cull) and binds a per-run clip uniform (#118, the
         * rounded-corner SDF applied in the fragment shader). instance_index in
         * the shader includes firstInstance, so the storage buffer still
         * indexes the right primitive. */
        int bs = 0;
        while (bs < g_g2d_prim_count) {
            int clip = (g_g2d_prim_clip && bs < g_g2d_prim_clip_cap) ? g_g2d_prim_clip[bs] : 0;
            int be = bs + 1;
            while (be < g_g2d_prim_count) {
                int ec = (g_g2d_prim_clip && be < g_g2d_prim_clip_cap) ? g_g2d_prim_clip[be] : 0;
                if (ec != clip) break;
                be++;
            }
            float cu[8]; rae_g2d_fill_clip_uniform(clip, cu);
            WGPUBufferDescriptor cbd; memset(&cbd, 0, sizeof(cbd));
            cbd.size = sizeof(cu); cbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            WGPUBuffer cub = wgpuDeviceCreateBuffer(g_wgpu_dev, &cbd);
            wgpuQueueWriteBuffer(g_wgpu_queue, cub, 0, cu, sizeof(cu));
            rae_g2d_keep_frame_buf(cub);
            WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
            e[1].binding = 1; e[1].buffer = instbuf;
            e[1].size = (uint64_t)g_g2d_prim_count * G2D_PRIM_FLOATS * sizeof(float);
            e[2].binding = 2; e[2].buffer = cub; e[2].size = sizeof(cu);
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = e;
            WGPUBindGroup bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            rae_g2d_keep_frame_bind(bind);
            rae_g2d_set_scissor(clip);
            wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
            wgpuRenderPassEncoderDraw(g_g2d_pass, 6, (uint32_t)(be - bs), 0, (uint32_t)bs);
            bs = be;
        }
        wgpuBindGroupLayoutRelease(bgl);
        g_g2d_prim_count = 0;
    }
    /* Images on top of boxes, under text. */
    rae_g2d_flush_images();
    if (have_text) {
        /* One text draw per atlas that has glyphs this frame (so Roboto text
         * and the Material-icon atlas coexist). */
        rae_g2d_init_text_pipeline();
        wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_text_pipeline);
        for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
            if (g_g2d_text_count[ai] <= 0) continue;
            if (!rae_g2d_atlas_texview(ai + 1)) continue;
            WGPUBuffer instbuf = rae_g2d_text_frame_buffer(ai, g_g2d_text_count[ai]);
            wgpuQueueWriteBuffer(g_wgpu_queue, instbuf, 0, g_g2d_text_prims[ai],
                                 (size_t)g_g2d_text_count[ai] * G2D_TEXT_FLOATS * sizeof(float));
            WGPUTextureView view = rae_g2d_atlas_texview(ai + 1);
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_text_pipeline, 0);
            WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
            e[1].binding = 1; e[1].buffer = instbuf;
            e[1].size = (uint64_t)g_g2d_text_count[ai] * G2D_TEXT_FLOATS * sizeof(float);
            e[2].binding = 2; e[2].textureView = view;
            e[3].binding = 3; e[3].sampler = g_g2d_sampler;
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
            WGPUBindGroup bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            wgpuBindGroupLayoutRelease(bgl);
            rae_g2d_keep_frame_bind(bind);
            wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
            int cnt = g_g2d_text_count[ai];
            int* tclip = g_g2d_text_clip[ai];
            int tcap = g_g2d_text_clip_cap[ai];
            int ts = 0;
            while (ts < cnt) {
                int clip = (tclip && ts < tcap) ? tclip[ts] : 0;
                int te = ts + 1;
                while (te < cnt) {
                    int ec = (tclip && te < tcap) ? tclip[te] : 0;
                    if (ec != clip) break;
                    te++;
                }
                rae_g2d_set_scissor(clip);
                wgpuRenderPassEncoderDraw(g_g2d_pass, 6, (uint32_t)(te - ts), 0, (uint32_t)ts);
                ts = te;
            }
            g_g2d_text_count[ai] = 0;
        }
    }
}

void rae_ext_gpu2d_closeWindow(void) {
    if (g_g2d_off_view) { wgpuTextureViewRelease(g_g2d_off_view); g_g2d_off_view = NULL; }
    if (g_g2d_off_tex)  { wgpuTextureRelease(g_g2d_off_tex);  g_g2d_off_tex = NULL; }
    g_g2d_off_w = 0; g_g2d_off_h = 0;
    for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
        if (g_g2d_text_bind[ai]) { wgpuBindGroupRelease(g_g2d_text_bind[ai]); g_g2d_text_bind[ai] = NULL; }
        if (g_g2d_text_instbuf[ai]) { wgpuBufferRelease(g_g2d_text_instbuf[ai]); g_g2d_text_instbuf[ai] = NULL; g_g2d_text_cap[ai] = 0; }
        if (g_g2d_text_prims[ai]) { free(g_g2d_text_prims[ai]); g_g2d_text_prims[ai] = NULL; g_g2d_text_capf[ai] = 0; }
        g_g2d_text_count[ai] = 0;
    }
    if (g_g2d_text_pipeline) { wgpuRenderPipelineRelease(g_g2d_text_pipeline); g_g2d_text_pipeline = NULL; }
    if (g_g2d_sampler) { wgpuSamplerRelease(g_g2d_sampler); g_g2d_sampler = NULL; }
    for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
        if (g_g2d_atlas_view[ai]) { wgpuTextureViewRelease(g_g2d_atlas_view[ai]); g_g2d_atlas_view[ai] = NULL; }
        if (g_g2d_atlas_tex[ai]) { wgpuTextureRelease(g_g2d_atlas_tex[ai]); g_g2d_atlas_tex[ai] = NULL; }
    }
    if (g_g2d_bind) { wgpuBindGroupRelease(g_g2d_bind); g_g2d_bind = NULL; }
    if (g_g2d_instbuf) { wgpuBufferRelease(g_g2d_instbuf); g_g2d_instbuf = NULL; g_g2d_inst_cap = 0; }
    if (g_g2d_uniform) { wgpuBufferRelease(g_g2d_uniform); g_g2d_uniform = NULL; }
    if (g_g2d_pipeline) { wgpuRenderPipelineRelease(g_g2d_pipeline); g_g2d_pipeline = NULL; }
    if (g_g2d_prims) { free(g_g2d_prims); g_g2d_prims = NULL; g_g2d_prim_capf = 0; }
    g_g2d_prim_count = 0;
    for (int i = 0; i < g_g2d_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_frame_binds[i]);
    g_g2d_frame_bind_n = 0;
    if (g_g2d_frame_binds) { free(g_g2d_frame_binds); g_g2d_frame_binds = NULL; g_g2d_frame_bind_cap = 0; }
    for (int i = 0; i < g_g2d_frame_buf_n; i++) wgpuBufferRelease(g_g2d_frame_bufs[i]);
    g_g2d_frame_buf_n = 0;
    if (g_g2d_frame_bufs) { free(g_g2d_frame_bufs); g_g2d_frame_bufs = NULL; g_g2d_frame_buf_cap = 0; }
    for (int i = 0; i < g_g2d_box_frame_buf_slots; i++) {
        if (g_g2d_box_frame_bufs[i]) wgpuBufferRelease(g_g2d_box_frame_bufs[i]);
    }
    if (g_g2d_box_frame_bufs) { free(g_g2d_box_frame_bufs); g_g2d_box_frame_bufs = NULL; }
    if (g_g2d_box_frame_buf_cap) { free(g_g2d_box_frame_buf_cap); g_g2d_box_frame_buf_cap = NULL; }
    g_g2d_box_frame_buf_n = 0; g_g2d_box_frame_buf_slots = 0;
    for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
        for (int i = 0; i < g_g2d_text_frame_buf_slots[ai]; i++) {
            if (g_g2d_text_frame_bufs[ai][i]) wgpuBufferRelease(g_g2d_text_frame_bufs[ai][i]);
        }
        if (g_g2d_text_frame_bufs[ai]) { free(g_g2d_text_frame_bufs[ai]); g_g2d_text_frame_bufs[ai] = NULL; }
        if (g_g2d_text_frame_buf_cap[ai]) { free(g_g2d_text_frame_buf_cap[ai]); g_g2d_text_frame_buf_cap[ai] = NULL; }
        g_g2d_text_frame_buf_n[ai] = 0; g_g2d_text_frame_buf_slots[ai] = 0;
    }
    /* Image pipeline + textures. */
    for (int i = 0; i < g_g2d_img_frame_bind_cap; i++) {
        if (g_g2d_img_frame_binds[i]) wgpuBindGroupRelease(g_g2d_img_frame_binds[i]);
    }
    g_g2d_img_frame_bind_n = 0;
    if (g_g2d_img_frame_binds) { free(g_g2d_img_frame_binds); g_g2d_img_frame_binds = NULL; g_g2d_img_frame_bind_cap = 0; }
    if (g_g2d_img_frame_handles) { free(g_g2d_img_frame_handles); g_g2d_img_frame_handles = NULL; }
    for (int i = 0; i < g_g2d_img_ubuf_n; i++) if (g_g2d_img_ubuf[i]) wgpuBufferRelease(g_g2d_img_ubuf[i]);
    if (g_g2d_img_ubuf) { free(g_g2d_img_ubuf); g_g2d_img_ubuf = NULL; g_g2d_img_ubuf_n = 0; }
    for (int i = 0; i < g_g2d_img_n; i++) {
        if (g_g2d_img_view[i]) { wgpuTextureViewRelease(g_g2d_img_view[i]); g_g2d_img_view[i] = NULL; }
        if (g_g2d_img_tex[i]) { wgpuTextureRelease(g_g2d_img_tex[i]); g_g2d_img_tex[i] = NULL; }
    }
    g_g2d_img_n = 0;
    g_g2d_img_key_n = 0;
    if (g_g2d_img_cmds) { free(g_g2d_img_cmds); g_g2d_img_cmds = NULL; g_g2d_img_cmd_cap = 0; }
    g_g2d_img_cmd_count = 0;
    if (g_g2d_img_pipeline) { wgpuRenderPipelineRelease(g_g2d_img_pipeline); g_g2d_img_pipeline = NULL; }
    if (g_g2d_surface) { wgpuSurfaceRelease(g_g2d_surface); g_g2d_surface = NULL; }
    for (int i = 0; i < 7; i++) {
        if (g_g2d_cursors[i]) { SDL_DestroyCursor(g_g2d_cursors[i]); g_g2d_cursors[i] = NULL; }
    }
    g_g2d_cursor_kind = -1;
    if (g_g2d_metal_view) { SDL_Metal_DestroyView(g_g2d_metal_view); g_g2d_metal_view = NULL; }
    if (g_sdl_win) { SDL_DestroyWindow(g_sdl_win); g_sdl_win = NULL; }
}
