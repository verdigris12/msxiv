#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

/* A simple copy helper for 'save' & 'bookmark' & 'save_as'. */
static int copy_file(const char *src, const char *dst)
{
	FILE *fin = fopen(src, "rb");
	if (!fin) {
		return -1;
	}
	FILE *fout = fopen(dst, "wb");
	if (!fout) {
		fclose(fin);
		return -1;
	}
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
		fwrite(buf, 1, n, fout);
	}
	fclose(fin);
	fclose(fout);
	return 0;
}

int cmd_save(const char *filename)
{
	/* The old approach: save a copy with "_copy" appended. */
	char dst[1024];
	snprintf(dst, sizeof(dst), "%s_copy", filename);
	if (copy_file(filename, dst) == 0) {
		fprintf(stderr, "Saved copy as: %s\n", dst);
		return 0;
	}
	fprintf(stderr, "Error saving %s\n", dst);
	return -1;
}

/*
 * If 'dest' is a directory, we append the old basename to it.
 * Otherwise, we treat 'dest' as a complete path to copy to.
 */
int cmd_save_as(const char *src, const char *dest)
{
	char path[1024];

	/* Basic ~ expansion, if you want it: 
	 * (If done earlier or in viewer, skip this.)
	 */
	if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		snprintf(path, sizeof(path), "%s/%s", home, dest + 2);
	} else {
		snprintf(path, sizeof(path), "%s", dest);
	}

	/* Check if 'path' is a directory */
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		/* It's a directory => append old basename */
		char *base = basename((char *)src);
		if (!base) {
			fprintf(stderr, "Invalid source filename.\n");
			return -1;
		}
		char dst[1024];
		snprintf(dst, sizeof(dst), "%s/%s", path, base);
		if (copy_file(src, dst) == 0) {
			fprintf(stderr, "Saved file to: %s\n", dst);
			return 0;
		}
		fprintf(stderr, "Error saving to directory: %s\n", dst);
		return -1;
	} else {
		/* It's not a directory => copy directly to 'path'. */
		if (copy_file(src, path) == 0) {
			fprintf(stderr, "Saved file to: %s\n", path);
			return 0;
		}
		fprintf(stderr, "Error saving to: %s\n", path);
		return -1;
	}
}

int cmd_convert(const char *filename, const char *dest)
{
	/*
	 * We'll do basic "~" expansion if 'dest' starts with "~/".
	 */
	char target[1024];
	if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		snprintf(target, sizeof(target), "%s/%s", home, dest + 2);
	} else {
		snprintf(target, sizeof(target), "%s", dest);
	}

	/*
	 * Then just call: convert "filename" "target"
	 */
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "convert \"%s\" \"%s\"", filename, target);

	if (system(cmd) == 0) {
		fprintf(stderr, "Converted %s -> %s\n", filename, target);
		return 0;
	}
	fprintf(stderr, "Conversion to %s failed.\n", target);
	return -1;
}
int cmd_delete(const char *filename)
{
	if (unlink(filename) == 0) {
		fprintf(stderr, "Deleted file: %s\n", filename);
		return 0;
	}
	fprintf(stderr, "Error deleting %s\n", filename);
	return -1;
}

int cmd_bookmark(const char *filename, const char *label, MsxivConfig *config)
{
	char *base = basename((char *)filename);
	if (!base) {
		fprintf(stderr, "Invalid filename: %s\n", filename);
		return -1;
	}

	for (int i = 0; i < config->bookmark_count; i++) {
		if (strcmp(config->bookmarks[i].label, label) == 0) {
			char dst[1024];
			snprintf(dst, sizeof(dst), "%s/%s", config->bookmarks[i].directory, base);
			if (copy_file(filename, dst) == 0) {
				fprintf(stderr, "Bookmarked to: %s\n", dst);
				return 0;
			}
			fprintf(stderr, "Could not copy to: %s\n", dst);
			return -1;
		}
	}
	fprintf(stderr, "Bookmark label '%s' not found in config.\n", label);
	return -1;
}

