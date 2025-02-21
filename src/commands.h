#ifndef COMMANDS_H
#define COMMANDS_H

#include "config.h"

/* Command actions */
int cmd_save(const char *filename);
int cmd_convert(const char *filename);
int cmd_delete(const char *filename);
int cmd_bookmark(const char *filename, const char *label, MsxivConfig *config);

#endif

