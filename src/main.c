#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <search.h>
#include <MagickWand/MagickWand.h>
#include <X11/Xlib.h>

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

/* Check MIME type using the `file` command.
   Returns 1 if the file's MIME type starts with "image/", 0 otherwise. */
static int check_mime(const char *filename) {
    char command[1024];
    snprintf(command, sizeof(command), "file --mime-type -b \"%s\"", filename);

    FILE *fp = popen(command, "r");
    if (!fp) {
        fprintf(stderr, "Failed to run file command on %s\n", filename);
        return 0;
    }

    char mime[256];
    if (fgets(mime, sizeof(mime), fp) == NULL) {
        fprintf(stderr, "Failed to read MIME type for %s\n", filename);
        pclose(fp);
        return 0;
    }
    pclose(fp);

    // Remove trailing newline if present.
    mime[strcspn(mime, "\n")] = '\0';

    if (strncmp(mime, "image/", 6) != 0) {
        fprintf(stderr, "File %s excluded: MIME type '%s' is not an image.\n", filename, mime);
        return 0;
    }
    return 1;
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

    /* Create a binary search tree for duplicate checking. */
    void *dup_tree = NULL;
    for (int i = 1; i < argc; i++) {
        char *copy = strdup(argv[i]);
        if (!copy) {
            fprintf(stderr, "Out of memory duplicating filename: %s\n", argv[i]);
            continue;
        }
        ENTRY *found = tfind(copy, &dup_tree, cmpstr);
        if (found) {
            free(copy);
        } else {
            tsearch(copy, &dup_tree, cmpstr);
            validFiles[validCount] = copy;
            validCount++;
        }
    }

    if (validCount == 0) {
        fprintf(stderr, "No valid files to open.\n");
        free(validFiles);
        tdestroy(dup_tree, noop_free);
        MagickWandTerminus();
        return 1;
    }

    /* Check each file using `file` for MIME and ImageMagick for ping */
    for (int i = 0; i < validCount; i++) {
        int validImage = 1;

        /* Check MIME type using the file command */
        if (!check_mime(validFiles[i])) {
            validImage = 0;
        }

        /* If MIME is valid, check if the image can be pinged */
        if (validImage) {
            MagickWand *testWand = NewMagickWand();
            if (MagickPingImage(testWand, validFiles[i]) == MagickFalse) {
                fprintf(stderr, "File %s excluded: failed to ping.\n", validFiles[i]);
                validImage = 0;
            }
            DestroyMagickWand(testWand);
        }

        /* Exclude file if any check fails */
        if (!validImage) {
            free(validFiles[i]);
            /* Shift remaining pointers down */
            for (int j = i; j < validCount - 1; j++) {
                validFiles[j] = validFiles[j + 1];
            }
            validCount--;
            i--; /* Re-check the new file at this index */
        } else {
        }
    }

    /* Clean up the duplicate tree without freeing the keys */
    tdestroy(dup_tree, noop_free);

    if (validCount == 0) {
        fprintf(stderr, "No valid image files after checking MIME and ping.\n");
        free(validFiles);
        MagickWandTerminus();
        return 1;
    }

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
        for (int i = 0; i < validCount; i++) {
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
    for (int i = 0; i < validCount; i++) {
        free(validFiles[i]);
    }
    free(validFiles);

    /* Terminate ImageMagick */
    MagickWandTerminus();

    return 0;
}
