#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viewer.h"
#include "config.h"

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <image1> [image2 ...]\n", argv[0]);
		return 1;
	}

	/* Load config from ~/.config/msxiv/config.toml if exists. */
	MsxivConfig config;
	if (load_config(&config) < 0) {
		fprintf(stderr, "Warning: could not load config.\n");
	}

	/* Prepare file list from argv[1..] */
	int fileCount = argc - 1;
	char **files = &argv[1];

	/* Create a struct for viewer data. */
	ViewerData vdata;
	vdata.fileCount = fileCount;
	vdata.files = files;
	vdata.currentIndex = 0;

	/* Initialize viewer. */
	Display *dpy = NULL;
	Window win = 0;
	if (viewer_init(&dpy, &win, &vdata, &config) != 0) {
		return 1;
	}

	/* Enter event loop (blocks until user quits or closes window). */
	viewer_run(dpy, win, &vdata);

	/* Cleanup. */
	viewer_cleanup(dpy);
	return 0;
}

