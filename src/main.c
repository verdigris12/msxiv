#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <MagickWand/MagickWand.h>

#include "viewer.h"
#include "config.h"

/* Simple helper to see if a path is already in our array of unique files */
static int is_duplicate(const char *path, char **files, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		if (strcmp(path, files[i]) == 0)
			return 1;
	}
	return 0;
}

/* Check if the file can be opened by ImageMagick */
static int can_open_image(const char *path)
{
	MagickWand *testWand = NewMagickWand();
	if (MagickReadImage(testWand, path) == MagickFalse) {
		DestroyMagickWand(testWand);
		return 0;
	}
	DestroyMagickWand(testWand);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <image1> [image2 ...]\n", argv[0]);
		return 1;
	}

	/* Initialize ImageMagick library */
	MagickWandGenesis();

	/* Prepare dynamic list for deduped, valid image paths */
	char **validFiles = (char **)malloc(sizeof(char *) * (argc - 1));
	if (!validFiles) {
		fprintf(stderr, "Allocation failed.\n");
		MagickWandTerminus();
		return 1;
	}
	int validCount = 0;

	/* Collect only unique and valid files */
	int i;
	for (i = 1; i < argc; i++) {
		if (!is_duplicate(argv[i], validFiles, validCount)) {
			if (can_open_image(argv[i])) {
				validFiles[validCount] = strdup(argv[i]);
				if (!validFiles[validCount]) {
					fprintf(stderr, "Out of memory.\n");
					MagickWandTerminus();
					return 1;
				}
				validCount++;
			} else {
				fprintf(stderr, "Warning: ignoring invalid file: %s\n", argv[i]);
			}
		}
	}

	/* If no valid files, we cannot proceed */
	if (validCount == 0) {
		fprintf(stderr, "No valid files to open.\n");
		free(validFiles);
		MagickWandTerminus();
		return 1;
	}

	/* Load user config (keybinds, bookmarks, etc.) */
	MsxivConfig config;
	if (load_config(&config) < 0) {
		fprintf(stderr, "Warning: could not load config.\n");
	}

	/* Build ViewerData from validFiles. */
	ViewerData vdata;
	vdata.fileCount = validCount;
	vdata.files = validFiles;
	vdata.currentIndex = 0;

	/* Initialize viewer */
	Display *dpy = NULL;
	Window win = 0;
	if (viewer_init(&dpy, &win, &vdata, &config) != 0) {
		fprintf(stderr, "Viewer initialization failed.\n");
		MagickWandTerminus();
		return 1;
	}

	/* Run main event loop */
	viewer_run(dpy, win, &vdata);

	/* Cleanup viewer */
	viewer_cleanup(dpy);

	/* Free allocated file list */
	int j;
	for (j = 0; j < validCount; j++) {
		free(validFiles[j]);
	}
	free(validFiles);

	/* Terminate ImageMagick */
	MagickWandTerminus();

	return 0;
}

