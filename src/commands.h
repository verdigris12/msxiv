#ifndef COMMANDS_H
#define COMMANDS_H

#include "config.h"

int cmd_save(const char *filename);
int cmd_convert(const char *filename);
int cmd_delete(const char *filename);
int cmd_bookmark(const char *filename, const char *label, MsxivConfig *config);

/* NEW: Save current file to an explicit path */
int cmd_save_as(const char *src, const char *dest);

#endif
