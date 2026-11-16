/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_VIEW_H
#define CX231XX_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct live_view;

enum live_view_kind { VIEW_PREVIEW, VIEW_CAMERA, VIEW_COUNT };

/** Starts the selected output helper. Camera uses the installed app's signed publisher. */
struct live_view *live_view_open(enum live_view_kind kind);
/** Offers one full woven YUYV frame without waiting for the child. Accepts a NULL view. */
void live_view_offer(struct live_view *view, const uint8_t *frame);
/** Check whether the child is still available. Accepts NULL. */
bool live_view_running(struct live_view *view);
/** Reports a pipe or child failure without consuming the child's wait status. Accepts NULL. */
bool live_view_failed(struct live_view *view);
/** Stops the worker and reaps the player or encoder. Accepts NULL. */
void live_view_close(struct live_view *view);

#endif
