/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "steps.h"
#include <stdio.h>
#include <string.h>

void gui_steps_reset(GuiSteps* steps) { memset(steps, 0, sizeof *steps); }

static GuiStep* current(GuiSteps* steps) { return steps->count ? &steps->step[steps->count - 1] : NULL; }
static void begin(GuiSteps* steps, const char* label, const char* detail, size_t detail_length, uint64_t now) {
    GuiStep * step;
    GuiStep* last = current(steps);
    if (last && last->state == GUI_STEP_ACTIVE) last->state = GUI_STEP_DONE;
    if (steps->count == GUI_STEPS_MAX)
        memmove(steps->step, steps->step + 1, sizeof steps->step - sizeof *steps->step);
    else
        steps->count++;
    step = current(steps);
    memset(step, 0, sizeof *step);
    snprintf(step->label, sizeof step->label, "%s", label);
    if (detail_length >= sizeof step->detail) detail_length = sizeof step->detail - 1;
    memcpy(step->detail, detail, detail_length);
    step->shown_ms = now;
    step->serial = ++steps->serial;
}
static void note(GuiSteps* steps, const char* s, uint64_t now) {
    GuiNote * n;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s || !steps->count) return;
    if (steps->notes == GUI_NOTES_MAX)
        memmove(steps->note, steps->note + 1, sizeof steps->note - sizeof *steps->note);
    else
        steps->notes++;
    n = &steps->note[steps->notes - 1];
    snprintf(n->text, sizeof n->text, "%s", s);
    n->shown_ms = now;
    n->step = steps->step[steps->count - 1].serial;
}
static int prefix(const char* line, const char* text) { return !strncmp(line, text, strlen(text)); }
static void line(GuiSteps* steps, const char* s, uint64_t now) {
    GuiStep* last = current(steps);
    uint32_t before = steps->serial;
    unsigned value, total;
    const char* block = strstr(s, "compiled block ");
    if (block && last && !strcmp(last->label, "RECOMPILE") &&
        sscanf(block, "compiled block %u/%u", &value, &total) == 2) {
        last->value = value;
        last->total = total;
    } else if (prefix(s, "Unpacking "))
        begin(steps, "UNPACK", s + 10, strlen(s + 10), now);
    else if (prefix(s, "Extracting "))
        begin(steps, "EXTRACT", s + 11, strlen(s + 11), now);
    else if (prefix(s, "Building hactool"))
        begin(steps, "TOOLS", "hactool", 7, now);
    else if (prefix(s, "Recompiling ")) {
        const char* name = s + 12;
        const char* end = strstr(name, " (");
        begin(steps, "RECOMPILE", name, end ? (size_t)(end - name) : strlen(name), now);
    } else if (prefix(s, "Built ")) {
        begin(steps, "BUILT", s + 6, strlen(s + 6), now);
        current(steps)->state = GUI_STEP_DONE;
    } else if (prefix(s, "ScarletNX - "))
        begin(steps, "LAUNCH", s + 12, strlen(s + 12), now);
    else if (prefix(s, "Ready "))
        begin(steps, "READY", s + 6, strlen(s + 6), now);
    else if (prefix(s, "vulkan: ") && !steps->gpu && strstr(s, ", Vulkan ")) {
        /* the first device line: "vulkan: <device> [(<driver>)], Vulkan <version>, ..."; a later "(" is not the
           device's */
        const char* device = s + 8;
        const char* end = strstr(device, ", Vulkan ");
        const char* paren = strstr(device, " (");
        if (paren && paren < end) end = paren;
        begin(steps, "GPU", device, (size_t)(end - device), now);
        steps->gpu = 1;
    }
    if (steps->serial == before) note(steps, s, now); /* not a step of its own: a line of the current one */
}
void gui_steps_feed(GuiSteps* steps, const char* data, size_t size, uint64_t now_ms) {
    size_t i;
    for (i = 0; i < size; ++i) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            steps->line[steps->length] = 0;
            if (steps->length) line(steps, steps->line, now_ms);
            steps->length = 0;
        } else if (steps->length + 1 < sizeof steps->line)
            steps->line[steps->length++] = c;
    }
}
void gui_steps_finish(GuiSteps* steps, int ok) {
    GuiStep * last;
    if (steps->length) gui_steps_feed(steps, "\n", 1, current(steps) ? current(steps)->shown_ms : 0);
    last = current(steps);
    if (last && last->state == GUI_STEP_ACTIVE)
        last->state = ok ? GUI_STEP_DONE : GUI_STEP_FAILED;
    else if (last && !ok)
        last->state = GUI_STEP_FAILED;
}
int gui_steps_percent(const GuiSteps* steps) {
    int i;
    int percent = -1;
    for (i = 0; i < steps->count; ++i) {
        const GuiStep* s = &steps->step[i];
        if (!strcmp(s->label, "RECOMPILE"))
            percent = s->state == GUI_STEP_DONE ? 100 : s->total ? (int)(s->value * 100ull / s->total) : 0;
        else if (!strcmp(s->label, "BUILT") || !strcmp(s->label, "LAUNCH") || !strcmp(s->label, "READY"))
            percent = 100;
    }
    return percent;
}
int gui_step_notes(const GuiSteps* steps, const GuiStep* step, const GuiNote** out, int max) {
    int i;
    int count = 0;
    for (i = steps->notes - 1; i >= 0 && count < max; --i)
        if (steps->note[i].step == step->serial) out[count++] = &steps->note[i];
    for (i = 0; i < count / 2; ++i) {
        const GuiNote* swap = out[i];
        out[i] = out[count - 1 - i];
        out[count - 1 - i] = swap;
    }
    return count;
}
