#ifndef CONFIG_H
#define CONFIG_H

#include <X11/keysym.h>

#define MAX_KEY_BINDS 128
#define MAX_BOOKMARKS 64
#define MAX_LABEL_LEN 64
#define MAX_PATH_LEN 1024

typedef struct {
	char key[32];
	char action[256];
} KeyBind;

typedef struct {
	char label[MAX_LABEL_LEN];
	char directory[MAX_PATH_LEN];
} BookmarkEntry;

typedef struct {
	int keybind_count;
	KeyBind keybinds[MAX_KEY_BINDS];

	int bookmark_count;
	BookmarkEntry bookmarks[MAX_BOOKMARKS];

	/* NEW: background color field, e.g. "#000000", "white", etc. */
	char bg_color[32];
} MsxivConfig;

/* Parse the TOML config file at ~/.config/msxiv/config.toml.
 * Returns 0 on success, -1 on failure. */
int load_config(MsxivConfig *config);

#endif
