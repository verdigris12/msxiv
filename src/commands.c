#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h> /* for basename() */

static int copy_file(const char *src, const char *dst)
{
	/* Very naive copy implementation */
	FILE *fin, *fout;
	char buf[8192];
	size_t n;

	fin = fopen(src, "rb");
	if (!fin) {
		return -1;
	}
	fout = fopen(dst, "wb");
	if (!fout) {
		fclose(fin);
		return -1;
	}

	while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
		fwrite(buf, 1, n, fout);
	}

	fclose(fin);
	fclose(fout);
	return 0;
}

int cmd_save(const char *filename)
{
	/* Save a copy in the same directory with a suffix "_copy". */
	char dst[1024];
	char *base = basename((char *)filename);
	if (!base) {
		return -1;
	}
	snprintf(dst, sizeof(dst), "%s_copy", filename);
	if (copy_file(filename, dst) == 0) {
		fprintf(stderr, "Saved copy: %s\n", dst);
		return 0;
	}
	return -1;
}

int cmd_convert(const char *filename)
{
	/* Prompt user for a desired format, then use ImageMagick's "convert" from system(3). */
	char format[128] = {0};
	char cmd[2048];
	fprintf(stderr, "Enter target format (e.g. png, jpg): ");
	if (!fgets(format, sizeof(format), stdin)) {
		return -1;
	}
	/* Strip newline */
	format[strcspn(format, "\n")] = '\0';

	snprintf(cmd, sizeof(cmd), "convert \"%s\" \"%s.%s\"", filename, filename, format);
	if (system(cmd) == 0) {
		fprintf(stderr, "Converted %s to %s.%s\n", filename, filename, format);
		return 0;
	}
	return -1;
}

int cmd_delete(const char *filename)
{
	if (unlink(filename) == 0) {
		fprintf(stderr, "Deleted file: %s\n", filename);
		return 0;
	}
	return -1;
}

int cmd_bookmark(const char *filename, const char *label, MsxivConfig *config)
{
	int i;
	char *base = basename((char *)filename);
	char dst[1024] = {0};

	for (i = 0; i < config->bookmark_count; i++) {
		if (strcmp(config->bookmarks[i].label, label) == 0) {
			snprintf(dst, sizeof(dst), "%s/%s", config->bookmarks[i].directory, base);
			if (copy_file(filename, dst) == 0) {
				fprintf(stderr, "Bookmarked file to %s\n", dst);
				return 0;
			} else {
				fprintf(stderr, "Failed to copy file to %s\n", dst);
				return -1;
			}
		}
	}

	fprintf(stderr, "Bookmark label '%s' not found in config.\n", label);
	return -1;
}
