/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what a window backend (platform/backend/) provides: the window, input and presenting */
#ifndef SNX_BACKEND_H
#define SNX_BACKEND_H
#include "abi/cpu.h"

void video_init(const char* title);
void video_present_rgba(const u32* px, int w, int h, int stride); /* final RGBA8 image */
void video_poll(void);
void backend_ini(const char* k, const char* v);
void frontend_frame(void);
/* the window's size in pixels, for the Vulkan renderer's swapchain: the SDL3 backend only (the headless build has no
   Vulkan renderer) */
void backend_window_size(int* w, int* h);
/* fatal's error box, on every platform despite the name (a no-op where the backend has none), from any thread */
void win_fatal_box(const char* msg);

/* the Vulkan renderer's window side, the SDL3 backend only. The handles as vulkan_core.h defines them on 64-bit
   hosts (C allows the same typedef twice), so including this needs no Vulkan header */
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
void* backend_vk_loader(void); /* vkGetInstanceProcAddr */
const char* const* backend_vk_extensions(u32* n);
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface);

#endif
