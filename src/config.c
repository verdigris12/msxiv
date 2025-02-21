#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_FILE_NAME "config.toml"
#define CONFIG_DIR ".config/msxiv"

/* A very naive parser for a simple subset of TOML,
 * focusing on lines like:
 *
 * [keybinds]
 * q = "command"
 * c = "convert"
 *
 * [bookmarks]
 * personal = "/path/to/personal"
 */
static int parse_line(MsxivConfig *config, const char *section, char *line)
{
	char *eq, *key, *val;
	size_t len;

	/* Trim leading spaces */
	while (*line == ' ' || *line == '\t') {
		line++;
	}

	/* Skip empty or commented lines */
	if (*line == '\0' || *line == '#') {
		return 0;
	}

	/* For lines like [keybinds], we change the current section. */
	if (line[0] == '[') {
		return 0; /* handled externally */
	}

	eq = strchr(line, '=');
	if (!eq) {
		return 0; /* invalid line */
	}

	*eq = '\0';
	key = line;
	val = eq + 1;

	/* Trim trailing spaces in key */
	len = strlen(key);
	while (len > 0 && (key[len - 1] == ' ' || key[len - 1] == '\t')) {
		key[len - 1] = '\0';
		len--;
	}

	/* Also trim leading spaces in val */
	while (*val == ' ' || *val == '\t') {
		val++;
	}

	/* Remove potential quotes around the value */
	if (*val == '\"') {
		val++;
		char *end_quote = strrchr(val, '\"');
		if (end_quote) {
			*end_quote = '\0';
		}
	}

	if (strcmp(section, "keybinds") == 0) {
		/* We'll store up to MAX_KEY_BINDS. */
		if (config->keybind_count < MAX_KEY_BINDS) {
			snprintf(config->keybinds[config->keybind_count].key,
			         sizeof(config->keybinds[config->keybind_count].key),
			         "%s", key);

			snprintf(config->keybinds[config->keybind_count].action,
			         sizeof(config->keybinds[config->keybind_count].action),
			         "%s", val);

			config->keybind_count++;
		}
	} else if (strcmp(section, "bookmarks") == 0) {
		/* We'll store up to MAX_BOOKMARKS. */
		if (config->bookmark_count < MAX_BOOKMARKS) {
			snprintf(config->bookmarks[config->bookmark_count].label,
			         sizeof(config->bookmarks[config->bookmark_count].label),
			         "%s", key);

			snprintf(config->bookmarks[config->bookmark_count].directory,
			         sizeof(config->bookmarks[config->bookmark_count].directory),
			         "%s", val);

			config->bookmark_count++;
		}
	}

	return 0;
}

int load_config(MsxivConfig *config)
{
	char path[1024];
	FILE *fp;
	char line[1024];
	char current_section[64];
	struct stat st;
	int i;

	config->keybind_count = 0;
	config->bookmark_count = 0;

	/* Construct path: ~/.config/msxiv/config.toml */
	snprintf(path, sizeof(path), "%s/%s/%s",
	         getenv("HOME") ? getenv("HOME") : ".",
	         CONFIG_DIR, CONFIG_FILE_NAME);

	if (stat(path, &st) != 0) {
		/* No config file found, not an error. We'll just have no config. */
		return 0;
	}

	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}

	memset(current_section, 0, sizeof(current_section));

	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n') {
			line[len - 1] = '\0';
		}

		/* Check if it's a section */
		if (line[0] == '[') {
			char *rbracket = strchr(line, ']');
			if (rbracket) {
				*rbracket = '\0';
				snprintf(current_section, sizeof(current_section), "%s", line + 1);
			}
			continue;
		}

		parse_line(config, current_section, line);
	}

	fclose(fp);

	/* Debug prints (if needed)
	for (i = 0; i < config->keybind_count; i++) {
		printf("KeyBind: key=%s action=%s\n",
		       config->keybinds[i].key,
		       config->keybinds[i].action);
	}
	for (i = 0; i < config->bookmark_count; i++) {
		printf("Bookmark: label=%s directory=%s\n",
		       config->bookmarks[i].label,
		       config->bookmarks[i].directory);
	}
	*/

	return 0;
}
