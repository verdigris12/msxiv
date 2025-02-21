#ifndef VIEWER_H
#define VIEWER_H

#include <X11/Xlib.h>
#include "config.h"

/* Keep track of multiple files so we can move forward/back. */
typedef struct {
	int fileCount;
	char **files;     /* array of file paths */
	int currentIndex; /* which file we are currently displaying */
} ViewerData;

/* Initialize the viewer: open display, create window, load first image, etc. */
int viewer_init(Display **dpy, Window *win, ViewerData *vdata, MsxivConfig *config);

/* Run the main event loop, handling key presses (space/backspace, etc.) 
 * and command line. Returns when window closed or user quits. */
void viewer_run(Display *dpy, Window win, ViewerData *vdata);

/* Cleanup: destroy wand, close display. */
void viewer_cleanup(Display *dpy);

#endif
