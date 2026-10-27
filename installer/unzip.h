/* unzip.h -- a small zip reader for Setup.exe (stored and deflated entries).
 *
 * Setup used Windows' tar.exe to unpack zips, but Wine/Proton (Steam Deck)
 * has no tar.exe. This needs no C runtime and no OS calls: the caller hands
 * in the whole zip and gets each entry back through a callback. */
#ifndef ORTA_UNZIP_H
#define ORTA_UNZIP_H

/* Called for each entry. name is UTF-8 with '/' separators and has already
 * been checked: not absolute, no "..". Return 0 to stop with an error. */
typedef int (*unz_put_fn)(void *ctx, const char *name, int is_dir,
                          const unsigned char *data, unsigned long size);
typedef void *(*unz_alloc_fn)(unsigned long n);
typedef void (*unz_free_fn)(void *p);

enum {
    UNZ_OK = 0, UNZ_NOT_ZIP, UNZ_BAD_ENTRY, UNZ_UNSUPPORTED, UNZ_BAD_DATA,
    UNZ_CRC, UNZ_NO_MEMORY, UNZ_WRITE, UNZ_BAD_NAME
};

int unz_extract(const unsigned char *zip, unsigned long size, unz_put_fn put, void *ctx,
                unz_alloc_fn alloc, unz_free_fn release);

#endif
