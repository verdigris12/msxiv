#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

/* A simple copy helper for 'save', 'bookmark', 'save_as'. */
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

int cmd_save(const char *filename, char *msgbuf, size_t msgbuf_sz)
{
	/* Old approach: create a copy with "_copy" appended. */
	char dst[1024];
	snprintf(dst, sizeof(dst), "%s_copy", filename);
	if (copy_file(filename, dst) == 0) {
		snprintf(msgbuf, msgbuf_sz, "Saved copy as: %s", dst);
		return 0;
	}
	snprintf(msgbuf, msgbuf_sz, "Error saving copy of %s", filename);
	return -1;
}

int cmd_save_as(const char *src, const char *dest, char *msgbuf, size_t msgbuf_sz)
{
	char path[1024];

	/* Basic ~ expansion */
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
			snprintf(msgbuf, msgbuf_sz, "Invalid source filename: %s", src);
			return -1;
		}
		char dst[1024];
		snprintf(dst, sizeof(dst), "%s/%s", path, base);
		if (copy_file(src, dst) == 0) {
			snprintf(msgbuf, msgbuf_sz, "Saved file to: %s", dst);
			return 0;
		}
		snprintf(msgbuf, msgbuf_sz, "Error saving to directory: %s", dst);
		return -1;
	} else {
		/* It's not a directory => copy directly to 'path'. */
		if (copy_file(src, path) == 0) {
			snprintf(msgbuf, msgbuf_sz, "Saved file to: %s", path);
			return 0;
		}
		snprintf(msgbuf, msgbuf_sz, "Error saving to: %s", path);
		return -1;
	}
}

int cmd_convert(const char *filename, const char *dest,
                char *msgbuf, size_t msgbuf_sz)
{
	/* "~" expansion */
	char target[1024];
	if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
		const char *home = getenv("HOME");
		if (!home) home = ".";
		snprintf(target, sizeof(target), "%s/%s", home, dest + 2);
	} else {
		snprintf(target, sizeof(target), "%s", dest);
	}

	/* call: convert "filename" "target" */
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "convert \"%s\" \"%s\"", filename, target);

	if (system(cmd) == 0) {
		snprintf(msgbuf, msgbuf_sz, "Converted %s -> %s", filename, target);
		return 0;
	}
	snprintf(msgbuf, msgbuf_sz, "Conversion to %s failed.", target);
	return -1;
}

int cmd_delete(const char *filename, char *msgbuf, size_t msgbuf_sz)
{
	if (unlink(filename) == 0) {
		snprintf(msgbuf, msgbuf_sz, "Deleted file: %s", filename);
		return 0;
	}
	snprintf(msgbuf, msgbuf_sz, "Error deleting %s", filename);
	return -1;
}

int cmd_bookmark(const char *filename, const char *label,
                 MsxivConfig *config, char *msgbuf, size_t msgbuf_sz)
{
	char *base = basename((char *)filename);
	if (!base) {
		snprintf(msgbuf, msgbuf_sz, "Invalid filename: %s", filename);
		return -1;
	}

	int i;
	for (i = 0; i < config->bookmark_count; i++) {
		if (strcmp(config->bookmarks[i].label, label) == 0) {
			char dst[1024];
			snprintf(dst, sizeof(dst), "%s/%s",
			         config->bookmarks[i].directory, base);
			if (copy_file(filename, dst) == 0) {
				snprintf(msgbuf, msgbuf_sz, "Bookmarked to: %s", dst);
				return 0;
			}
			snprintf(msgbuf, msgbuf_sz, "Could not copy to: %s", dst);
			return -1;
		}
	}
	snprintf(msgbuf, msgbuf_sz, "Bookmark label '%s' not found in config.", label);
	return -1;
}

