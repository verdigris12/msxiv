#ifndef COMMANDS_H
#define COMMANDS_H

#include "config.h"
#include <stddef.h>

/* Each command now takes an extra char* buffer to store status messages
 * for the caller to display in the viewer's command bar.
 * Return 0 on success, -1 on error. */
int cmd_save(const char *filename, char *msgbuf, size_t msgbuf_sz);
int cmd_save_as(const char *src, const char *dest, char *msgbuf, size_t msgbuf_sz);
int cmd_convert(const char *filename, const char *dest,
                char *msgbuf, size_t msgbuf_sz);
int cmd_delete(const char *filename, char *msgbuf, size_t msgbuf_sz);
int cmd_bookmark(const char *filename, const char *label,
                 MsxivConfig *config, char *msgbuf, size_t msgbuf_sz);

#endif

