#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viewer.h"
#include "config.h"

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <image file>\n", argv[0]);
		return 1;
	}

	Display *dpy = NULL;
	Window win = 0;
	MsxivConfig config;
	if (load_config(&config) < 0) {
		fprintf(stderr, "Warning: Could not load config.\n");
	}

	if (viewer_init(&dpy, &win, argv[1], &config) != 0) {
		return 1;
	}

	viewer_run(dpy, win);
	viewer_cleanup(dpy);

	return 0;
}

