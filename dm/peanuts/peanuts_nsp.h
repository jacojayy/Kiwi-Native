// Kiwi Native, "NSP direct" stage 1: a game's RomFS read at run time straight from the player's dumps (NSP or XCI), with no imported .nup.
// peanuts/docs/NSP-DIRECT.md is the design note.
//
// The image is the index a .nup package or a DMROMFS1 image gives (dm/host/romfs_image.h: romfs_img, img_ent), built from the RomFS of the game's program
// NCA -- the update's patch RomFS over the base's when the dumps hold an update -- so the host's lookups, listings and sizes (dm/host/hle_fs.c) are the same
// code for all three. A file's entry has IMG_ENT_NSP set: a is its offset in the NCA section, b its size, and its bytes come from pn_nsp_read (the importer's
// section reader, dm/peanuts/import: decrypted, patched and decompressed as they are read; nothing is hashed, as on the import path's readers).
//
// The app does not use any of this yet: its launcher sets no dump spec and writes no <index>.nsp entry (that side is not built). The one way in on a device is
// the diagnostic switch DM_NSP_ROMFS (hle_fs.c h_fs_MountRom) with DM_NX_KEYS; with it unset the host opens what it opened before.
//
// Spec (DM_ROMFS, and what the host makes of a DM_AOC_DIR entry <index>.nsp):
//   nsp:<file>[|<file>...]              the game: its base, and its update when one is among the files (the highest version); which file is which is read from
//                                       the files (their CNMT), not from their order
//   nsp:<file>[|<file>...]|aoc/<index>  add-on content <index>: the add-on title whose ID's low 12 bits are the index (as a .nup's "aoc/<index>")
// <file> is a path, or fd:<n>/<name> for descriptor n (a dump picked with the system file picker; <name> is only for messages), as dm/host/nup_image.h's
// nup_fd_spec. A path cannot hold '|'. At most PN_NSP_FILES files.
// The keys file (prod.keys; a title.keys beside it is read too) is named by DM_NX_KEYS. Without it, with keys that do not open the dumps, with a compressed
// dump (NSZ / XCZ), a personalized ticket, an encryption the importer does not read, or dumps of another title or version than the running game, the open
// fails with the reason in why and nothing is mounted.
//
// Built into the host where DM_NSP_DIRECT is defined (the Android host: android/host/CMakeLists.txt, with the importer's three reader files). Elsewhere the
// spec is recognised and refused, so a host without the reader says so instead of taking "nsp:..." for a folder.
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../host/romfs_image.h"

#define IMG_ENT_NSP 8        // in memory only (an image over dumps): a = the file's offset in its NCA section
#define PN_NSP_FILES 4

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DM_NSP_DIRECT
// 1 when spec is a dump spec ("nsp:...")
int pn_nsp_is_spec(const char *spec);
// Opens spec as an image; NULL with errno set and the reason in why. title_id: the running game's application ID, 0 to take the dumps' own; main_build: the
// first 16 bytes of the running main module's build ID as hex (dm.h dm_main_build), NULL or "" not to check. Both are checked for the game's RomFS; an add-on
// is checked against title_id only. Close with romfs_image_close.
romfs_img *pn_nsp_open(const char *spec, uint64_t title_id, const char *main_build, char *why, size_t wn);
// Bytes [off, off + len) of file entry e (inside the file: the caller clamps) into buf. 1 ok; 0 with errno set (EFAULT when the system could not write
// buf, else EIO) and the reason in pn_nsp_last_error().
// Any number of threads may read one image at once.
int pn_nsp_read(const romfs_img *m, const img_ent *e, uint64_t off, void *buf, size_t len);
const char *pn_nsp_last_error(void);               // the calling thread's last failed read
const char *pn_nsp_describe(const romfs_img *m);   // one line for logs: "base <file> + update v<version> <file>, compressed"
#else
static inline int pn_nsp_is_spec(const char *spec) { return spec && !strncmp(spec, "nsp:", 4); }
static inline romfs_img *pn_nsp_open(const char *spec, uint64_t title_id, const char *main_build, char *why, size_t wn) {
    (void)spec; (void)title_id; (void)main_build;
    snprintf(why, wn, "this host is built without the reader of dumps (DM_NSP_DIRECT): use a .nup"); errno = ENOTSUP; return NULL;
}
static inline int pn_nsp_read(const romfs_img *m, const img_ent *e, uint64_t off, void *buf, size_t len) { (void)m; (void)e; (void)off; (void)buf; (void)len; errno = ENOTSUP; return 0; }
static inline const char *pn_nsp_last_error(void) { return "no reader of dumps in this host"; }
static inline const char *pn_nsp_describe(const romfs_img *m) { (void)m; return "none"; }
#endif

#ifdef __cplusplus
}
#endif
