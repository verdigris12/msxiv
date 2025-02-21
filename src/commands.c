#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

/* A simple copy helper, used by 'save' & 'bookmark' & 'save_as'. */
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
	/* Just saves a copy in the same dir with "_copy". */
	char dst[1024];
	snprintf(dst, sizeof(dst), "%s_copy", filename);
	if (copy_file(filename, dst) == 0) {
		fprintf(stderr, "Saved copy as: %s\n", dst);
		return 0;
	}
	fprintf(stderr, "Error saving %s\n", dst);
	return -1;
}

int cmd_save_as(const char *src, const char *dest)
{
	/*
	 * If dest starts with "~/", we expand "~" to $HOME.
	 * Otherwise, we copy as is.
	 */
	char path[1024];
	if (dest[0] == '~' && dest[1] == '/') {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		snprintf(path, sizeof(path), "%s/%s", home, dest + 2);
	} else {
		snprintf(path, sizeof(path), "%s", dest);
	}

	if (copy_file(src, path) == 0) {
		fprintf(stderr, "Saved file to: %s\n", path);
		return 0;
	}
	fprintf(stderr, "Error saving to: %s\n", path);
	return -1;
}

int cmd_convert(const char *filename)
{
	char format[128] = {0};
	fprintf(stderr, "Enter target format (e.g. png): ");
	if (!fgets(format, sizeof(format), stdin)) {
		return -1;
	}
	format[strcspn(format, "\n")] = '\0'; /* strip newline */
	if (strlen(format) == 0) {
		fprintf(stderr, "No format given.\n");
		return -1;
	}

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "convert \"%s\" \"%s.%s\"", filename, filename, format);
	if (system(cmd) == 0) {
		fprintf(stderr, "Converted %s -> %s.%s\n", filename, filename, format);
		return 0;
	}
	fprintf(stderr, "Conversion failed.\n");
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
