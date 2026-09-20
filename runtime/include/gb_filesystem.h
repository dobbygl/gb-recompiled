#ifndef GB_FILESYSTEM_H
#define GB_FILESYSTEM_H

/* Small C interface for the runtime's host directory operations.
 * Names use the same native narrow encoding as fopen(). Iteration excludes
 * dot entries; each name remains valid until the next read or close. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBDirectory GBDirectory;
GBDirectory* gb_directory_open(const char* path);
const char* gb_directory_next(GBDirectory* directory);
void gb_directory_close(GBDirectory* directory);
/* Returns 0 for a created directory or an existing directory; -1 on failure.
 * A regular file at the requested path is a failure, never success.
 * Failure sets errno for the asset loader diagnostics. */
int gb_make_directory(const char* path);

#ifdef __cplusplus
}
#endif
#endif
