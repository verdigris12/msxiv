#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_FILE_NAME "config.toml"
#define CONFIG_DIR ".config/msxiv"

/*
   We parse a simple subset of TOML lines, focusing on:

   [keybinds]
   q = "delete"
   c = "convert"

   [bookmarks]
   personal = "/some/path"

   [display]
   background = "#202020"
*/

static int parse_line(MsxivConfig *config, const char *section, char *line)
{
	char *eq, *key, *val;
	size_t len;

	/* Trim leading spaces */
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	/* Skip blank or commented lines */
	if (*line == '\0' || *line == '#') {
		return 0;
	}
	/* If line starts with "[", we treat it as a section name, handled outside. */
	if (line[0] == '[') {
		return 0;
	}

	eq = strchr(line, '=');
	if (!eq) {
		return 0; /* invalid or no '=' line */
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
	/* Trim leading spaces in val */
	while (*val == ' ' || *val == '\t') {
		val++;
	}
	/* Remove quotes if present */
	if (*val == '\"') {
		val++;
		char *end_quote = strrchr(val, '\"');
		if (end_quote) {
			*end_quote = '\0';
		}
	}

	if (strcmp(section, "keybinds") == 0) {
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
		if (config->bookmark_count < MAX_BOOKMARKS) {
			snprintf(config->bookmarks[config->bookmark_count].label,
			         sizeof(config->bookmarks[config->bookmark_count].label),
			         "%s", key);
			snprintf(config->bookmarks[config->bookmark_count].directory,
			         sizeof(config->bookmarks[config->bookmark_count].directory),
			         "%s", val);
			config->bookmark_count++;
		}
	} else if (strcmp(section, "display") == 0) {
		if (strcmp(key, "background") == 0) {
			snprintf(config->bg_color, sizeof(config->bg_color), "%s", val);
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

	config->keybind_count = 0;
	config->bookmark_count = 0;
	/* default background color is black */
	snprintf(config->bg_color, sizeof(config->bg_color), "#000000");

	/* Build path to ~/.config/msxiv/config.toml */
	snprintf(path, sizeof(path), "%s/%s/%s",
	         getenv("HOME") ? getenv("HOME") : ".",
	         CONFIG_DIR,
	         CONFIG_FILE_NAME);

	if (stat(path, &st) != 0) {
		/* no config file => not an error, just no user config */
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
		if (line[0] == '[') {
			char *rb = strchr(line, ']');
			if (rb) {
				*rb = '\0';
				snprintf(current_section, sizeof(current_section),
				         "%s", line + 1);
			}
			continue;
		}
		parse_line(config, current_section, line);
	}

	fclose(fp);
	return 0;
}

