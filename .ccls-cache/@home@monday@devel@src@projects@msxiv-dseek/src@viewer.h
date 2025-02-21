#ifndef VIEWER_H
#define VIEWER_H

#include <X11/Xlib.h>
#include "config.h"

/* Initialize viewer, load image, etc. Returns 0 on success. */
int viewer_init(Display **dpy, Window *win, const char *filename, MsxivConfig *config);

/* Main loop: handle events, etc. */
void viewer_run(Display *dpy, Window win);

/* Clean up. */
void viewer_cleanup(Display *dpy);

#endif
