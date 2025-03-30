#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <search.h>
#include <MagickWand/MagickWand.h>

#include "viewer.h"
#include "config.h"

/* Custom comparator for tsearch/tfind.
   Keys are char* so we compare the strings directly. */
static int cmpstr(const void *a, const void *b) {
    const char *s1 = a;
    const char *s2 = b;
    return strcmp(s1, s2);
}

/* No‑op free callback for tdestroy.
   Since validFiles owns the duplicated strings, we don't want tdestroy to free them. */
static void noop_free(void *node) {
    /* do nothing */
}

int main(int argc, char **argv)
{
    /* Initialize Xlib for multi-threading */
    if (!XInitThreads()) {
        fprintf(stderr, "Failed to initialize Xlib threads.\n");
        return 1;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image1> [image2 ...]\n", argv[0]);
        return 1;
    }

    /* Initialize ImageMagick library */
    MagickWandGenesis();

    /* Allocate an array to hold valid, unique image paths */
    char **validFiles = malloc(sizeof(char *) * (argc - 1));
    if (!validFiles) {
        fprintf(stderr, "Allocation failed.\n");
        MagickWandTerminus();
        return 1;
    }
    int validCount = 0;

    /* Create a binary search tree root for duplicate checking.
       The tree is used solely for duplicate detection. */
    void *dup_tree = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        printf("%d %s\n", i, argv[i]);
        char *copy = strdup(argv[i]);
        if (!copy) {
            fprintf(stderr, "Out of memory duplicating filename: %s\n", argv[i]);
            continue;
        }
        /* Use tfind with our custom comparator */
        ENTRY *found = tfind(copy, &dup_tree, cmpstr);
        if (found) {
            /* Duplicate found; free our duplicate copy */
            free(copy);
        } else {
            tsearch(copy, &dup_tree, cmpstr);
            validFiles[validCount] = copy;
            validCount++;
        }
    }

    /* If no valid files, we cannot proceed */
    if (validCount == 0) {
        fprintf(stderr, "No valid files to open.\n");
        free(validFiles);
        tdestroy(dup_tree, noop_free);
        MagickWandTerminus();
        return 1;
    }

    /* Check each file with ImageMagick (using Ping) */
    for (i = 0; i < validCount; i++) {
        printf("Testing %s...", validFiles[i]);
        MagickWand *testWand = NewMagickWand();
        if (MagickPingImage(testWand, validFiles[i]) == MagickFalse) {
            fprintf(stderr, "Failed.\nWarning: ignoring invalid file: %s\n", validFiles[i]);
            free(validFiles[i]);
            /* Shift remaining pointers down */
            for (int j = i; j < validCount - 1; j++) {
                validFiles[j] = validFiles[j + 1];
            }
            validCount--;
            i--; /* re-check new file at this index */
        } else {
            printf("OK.\n");
        }
        DestroyMagickWand(testWand);
    }

    /* Clean up the duplicate tree without freeing the keys */
    tdestroy(dup_tree, noop_free);

    /* Load user config (keybinds, bookmarks, etc.) */
    MsxivConfig config;
    if (load_config(&config) < 0) {
        fprintf(stderr, "Warning: could not load config.\n");
    }

    /* Build ViewerData from validFiles */
    ViewerData vdata;
    vdata.fileCount = validCount;
    vdata.files = validFiles;
    vdata.currentIndex = 0;

    /* Initialize viewer */
    Display *dpy = NULL;
    Window win = 0;
    if (viewer_init(&dpy, &win, &vdata, &config) != 0) {
        fprintf(stderr, "Viewer initialization failed.\n");
        for (i = 0; i < validCount; i++) {
            free(validFiles[i]);
        }
        free(validFiles);
        MagickWandTerminus();
        return 1;
    }

    /* Run main event loop */
    viewer_run(dpy, win, &vdata);

    /* Cleanup viewer */
    viewer_cleanup(dpy);

    /* Free allocated file list */
    for (i = 0; i < validCount; i++) {
        free(validFiles[i]);
    }
    free(validFiles);

    /* Terminate ImageMagick */
    MagickWandTerminus();

    return 0;
}

