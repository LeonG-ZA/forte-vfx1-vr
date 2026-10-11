#ifndef VFXAPP_FETCH_H
#define VFXAPP_FETCH_H
#include <stddef.h>
/* Where http_get_text keeps its temporary file. */
void fetch_set_temp_dir(const char *dir);
/* GET an http:// URL into dest (via dest.part). 1 on success; err holds the
 * reason, or the byte count on success. Blocking. */
int http_download(const char *url, const char *dest, char *err, size_t err_size);
/* GET an http:// URL into a NUL-terminated buffer, truncated to size - 1. */
int http_get_text(const char *url, char *out, size_t size, char *err, size_t err_size);
#endif
