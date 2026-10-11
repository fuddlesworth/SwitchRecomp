/* Force-included into gpu/gl/renderer.c when the runtime also has the Vulkan renderer (CMake, SWR_VULKAN): the OpenGL
   renderer's side of the renderer interface takes glr_ names, and renderer.c defines the real ones, calling
   whichever renderer is active. The shared variables (g_gl, g_draws, g_vi_crop...) keep their names. */
#define gpu_gl_start           glr_start
#define gpu_async_submit       glr_async_submit
#define r3d_clear_gl           glr_clear
#define r3d_draw_gl            glr_draw
#define r3d_cb_bind_gl         glr_cb_bind
#define r3d_blit_gl            glr_blit
#define gpu_gl_rt_flush        glr_rt_flush
#define gpu_gl_rt_reload       glr_rt_reload
#define gpu_gl_dispatch        glr_dispatch
#define cs_mem_need            glr_cs_mem_need
#define gpu_gl_before_syncwait glr_before_syncwait
#define gpu_gl_stream_incr     glr_stream_incr
#define gpu_gl_present_buffer  glr_present_buffer
#define gpu_gl_present_cpu     glr_present_cpu
#define gpu_gl_screenshot      glr_screenshot
#define gpu_gl_note_compute    glr_note_compute
#define gpu_gl_dump_rts        glr_dump_rts
#define gpu_gl_rec_finish      glr_rec_finish
#define gpu_gl_trace           glr_trace
#define gpu_busy_ns            glr_busy_ns
