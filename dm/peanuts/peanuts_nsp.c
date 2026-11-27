// Kiwi Native: a RomFS image over the player's dumps. See peanuts_nsp.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peanuts_nsp.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include "import/nximport.h"
#include "import/nx_internal.h"

typedef struct pn_nsp {
    NxContainer c[PN_NSP_FILES]; int nc;
    NxTitle *ts[PN_NSP_FILES]; int nts[PN_NSP_FILES];
    NxNca base, upd; int has_base, has_upd;   // at fixed addresses: a patch section points at its base's NCA
    NxNca *top; int sec;                      // the NCA section the image's files are read from
    char what[320];
} pn_nsp;

// One open at a time: the importer's keys are one table for the process (nx_keys), read while the NCAs are opened and not after
static pthread_mutex_t g_open = PTHREAD_MUTEX_INITIALIZER;
static char g_keys_path[1024];

static void nsp_free(void *p) {
    pn_nsp *a = p; if (!a) return;
    if (a->has_upd) nx_nca_close(&a->upd);    // before its base: the cache's entries of both go with their tables
    if (a->has_base) nx_nca_close(&a->base);
    for (int i = 0; i < a->nc; i++) { free(a->ts[i]); nx_container_close(&a->c[i]); }
    free(a);
}

int pn_nsp_is_spec(const char *spec) { return spec && !strncmp(spec, "nsp:", 4); }
const char *pn_nsp_last_error(void) { return nximport_last_error(); }
const char *pn_nsp_describe(const romfs_img *m) { return m && m->aux_free == nsp_free ? ((const pn_nsp *)m->aux)->what : "none"; }

static const char *leaf(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

// "fd:<n>/<name>": 1 and *fd; 0 for a path
static int fd_part(const char *file, int *fd) {
    if (strncmp(file, "fd:", 3) || file[3] < '0' || file[3] > '9') return 0;
    char *end; long n = strtol(file + 3, &end, 10);
    if ((*end != '/' && *end) || n < 0 || n > INT_MAX) return 0;
    *fd = (int)n; return 1;
}

static int keys_ready(char *why, size_t wn) {
    const char *kp = getenv("DM_NX_KEYS");
    if (!kp || !*kp) { snprintf(why, wn, "DM_NX_KEYS is not set: it names the keys file (prod.keys) a dump is read with"); return -1; }
    if (nx_keys.loaded && !strcmp(g_keys_path, kp)) return 0;
    g_keys_path[0] = 0;
    if (nximport_keys(kp)) { snprintf(why, wn, "the keys file (DM_NX_KEYS): %s", nximport_last_error()); return -1; }
    snprintf(g_keys_path, sizeof g_keys_path, "%s", kp);
    return 0;
}

static int open_nca(NxNca *n, const NxTitle *t, const char *id, const char *role, char *why, size_t wn) {
    char nm[48]; snprintf(nm, sizeof nm, "%s.nca", id);
    const NxEntry *e = id[0] ? nx_container_find(t->c, nm) : NULL;
    if (!e) { snprintf(why, wn, "%s: the %s NCA %s is not in the file", leaf(t->c->path), role, id[0] ? nm : "(none in its title metadata)"); return -1; }
    if (nx_nca_open(n, t->c, e)) { snprintf(why, wn, "%s: %s", leaf(t->c->path), nximport_last_error()); return -1; }
    if (!n->key_ok) {
        if (n->has_rights && t->c->personal)
            snprintf(why, wn, "%s: the %s NCA's ticket is personalized (its title key is wrapped for one console), which is not read", leaf(t->c->path), role);
        else snprintf(why, wn, "%s: the %s NCA cannot be decrypted: %s", leaf(t->c->path), role, n->key_why);
        nx_nca_close(n); return -1;
    }
    return 0;
}

// The encryption of a section the image reads: none (1), AES-CTR (3), and a patch's AES-CTR-EX (4) once its base is set
static int crypt_ok(const NxNca *n, int s, int patch_ok, const char *role, char *why, size_t wn) {
    int t = n->sec[s].crypt_type;
    if (t == 1 || t == 3 || (t == 4 && patch_ok)) return 1;
    snprintf(why, wn, "the %s's RomFS section uses encryption type %d (%s), which is not read", role, t,
             t == 2 ? "AES-XTS" : t == 4 ? "a patch of another title's RomFS" : t == 5 || t == 6 ? "AES-CTR without layer hashes" : "unknown");
    return 0;
}

// The first 16 bytes of the build ID of the ExeFS's main, as 32 hex digits. 0 ok; 1 the NCA has no ExeFS; -1 it cannot be read (why)
static int main_build_of(NxNca *n, char out[33], char *why, size_t wn) {
    int ex = nx_nca_find_section(n, NX_FS_PFS0); if (ex < 0) return 1;
    NxNode *root = nx_pfs0_tree(n, ex); uint8_t h[0x60]; int rc = -1;
    if (!root) { snprintf(why, wn, "the program's ExeFS cannot be read (wrong keys?): %s", nximport_last_error()); return -1; }
    NxNode *f = nx_node_find(root, "main");
    if (!f || f->is_dir || f->size < sizeof h) snprintf(why, wn, "the program's ExeFS has no main");
    else if (nx_node_read(f, 0, h, sizeof h)) snprintf(why, wn, "the program's main cannot be read: %s", nximport_last_error());
    else if (memcmp(h, "NSO0", 4)) snprintf(why, wn, "the program's main is not an NSO (wrong keys?)");
    else { for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", h[0x40 + i]); rc = 0; }
    nx_node_free(root); return rc;
}

static int name_ok(const char *nm) {
    size_t l = strlen(nm);
    return l && l <= 0x300 && !memchr(nm, '/', l) && !(l == 1 && nm[0] == '.') && !(l == 2 && nm[0] == '.' && nm[1] == '.');
}
static int count_tree(const NxNode *d, uint64_t *n, uint64_t *names, uint64_t *dirs) {
    ++*dirs;
    for (int i = 0; i < d->nkids; i++) {
        const NxNode *k = d->kids[i];
        if (!name_ok(k->name)) return -1;
        ++*n; *names += strlen(k->name);
        if (k->is_dir && count_tree(k, n, names, dirs)) return -1;
    }
    return 0;
}
// The index of a tree in the shape romfs_image_open and nup_image_open give: entry 0 is the root, each directory's children are one run of entries in
// name order (the trees keep them sorted by bytes, as a .nup's are), directories visited breadth first
static romfs_img *index_of(const NxNode *root, char *why, size_t wn) {
    uint64_t n = 1, names = 0, dirs = 0; romfs_img *m = NULL; const NxNode **queue = NULL; uint64_t *ent_of = NULL;
    if (count_tree(root, &n, &names, &dirs)) { snprintf(why, wn, "the RomFS has a name that is not a plain file name"); errno = EINVAL; return NULL; }
    if (n > UINT32_MAX || names > UINT32_MAX) { snprintf(why, wn, "the RomFS has too many entries"); errno = EINVAL; return NULL; }
    size_t isz = (size_t)n * sizeof(img_ent) + (size_t)names;
    if (!(m = calloc(1, sizeof *m)) || !(m->e = malloc(isz)) || !(queue = malloc(sizeof *queue * (size_t)dirs)) || !(ent_of = malloc(sizeof *ent_of * (size_t)dirs))) {
        if (m) free(m->e); free(m); free(queue); free(ent_of); snprintf(why, wn, "out of memory for the index"); errno = ENOMEM; return NULL;
    }
    m->fd = -1; m->n = n; m->names = (const char *)m->e + n * sizeof(img_ent);
    char *np = (char *)m->names; uint64_t at = 0, k = 1, qh = 0, qt = 0;
    memset(&m->e[0], 0, sizeof m->e[0]); queue[qt] = root; ent_of[qt++] = 0;
    while (qh < qt) {
        const NxNode *d = queue[qh]; uint64_t me = ent_of[qh++];
        m->e[me].a = k; m->e[me].b = (uint64_t)d->nkids;
        for (int j = 0; j < d->nkids; j++, k++) {
            const NxNode *c = d->kids[j]; size_t nl = strlen(c->name); img_ent *x = &m->e[k];
            memset(x, 0, sizeof *x); x->name = (uint32_t)at; x->nlen = (uint16_t)nl; memcpy(np + at, c->name, nl); at += nl;
            if (c->is_dir) { queue[qt] = c; ent_of[qt++] = k; }
            else { x->type = 1; x->a = c->off; x->b = c->size; x->flags = IMG_ENT_NSP; }
        }
    }
    free(queue); free(ent_of);
    return m;
}

romfs_img *pn_nsp_open(const char *spec, uint64_t title_id, const char *main_build, char *why, size_t wn) {
    pn_nsp *a = NULL; romfs_img *m = NULL; NxNode *tree = NULL; int e = EINVAL, aoc = -1; char parts[1400];
#define BAD(...) do { snprintf(why, wn, __VA_ARGS__); goto bad; } while (0)
    pthread_mutex_lock(&g_open);
    if (!pn_nsp_is_spec(spec) || strlen(spec + 4) >= sizeof parts) BAD("not a dump spec (nsp:<file>[|<file>...])");
    if (keys_ready(why, wn)) { e = ENOKEY; goto bad; }
    if (!(a = calloc(1, sizeof *a))) { e = ENOMEM; BAD("out of memory"); }
    for (int i = 0; i < PN_NSP_FILES; i++) a->c[i].fd = -1;
    strcpy(parts, spec + 4);
    for (char *p = parts, *next; p; p = next) {
        if ((next = strchr(p, '|'))) *next++ = 0;
        if (!strncmp(p, "aoc/", 4) && p[4] && strspn(p + 4, "0123456789") == strlen(p + 4) && !next) {
            if (strlen(p + 4) > 4) BAD("no add-on content %s in the dump", p + 4);   // an index is the low 12 bits of a title ID; a longer number is not left to atoi, where one that does not fit an int could come out negative and read as "no add-on asked for"
            aoc = atoi(p + 4); break;
        }
        if (!*p) BAD("an empty file name in the dump spec");
        if (a->nc == PN_NSP_FILES) BAD("more than %d files in the dump spec", PN_NSP_FILES);
        int fd = -1, isfd = fd_part(p, &fd); NxContainer *c = &a->c[a->nc];
        if (isfd && fcntl(fd, F_GETFD) < 0) { e = EBADF; BAD("%s: descriptor %d is not open", p, fd); }
        if (nx_container_open_rt(c, p, isfd ? fd : -1)) { c->fd = -1; e = errno == ENOENT ? ENOENT : EINVAL; BAD("%s", nximport_last_error()); }
        a->nc++;
        for (int k = 0; k < c->n; k++) { size_t l = strlen(c->e[k].name); if (l > 4 && !strcasecmp(c->e[k].name + l - 4, ".ncz"))
            BAD("%s is a compressed dump (NSZ / XCZ: it holds %s), which is not read where it is: import it to a .nup, or use the NSP / XCI", leaf(p), c->e[k].name); }
        if (nx_container_titles(c, &a->ts[a->nc - 1], &a->nts[a->nc - 1])) BAD("%s: %s", leaf(p), nximport_last_error());
    }
    if (!a->nc) BAD("no file in the dump spec");

    if (aoc >= 0) {   // add-on content: the data NCA's RomFS of the add-on title with that index
        const NxTitle *t = NULL, *other = NULL;
        for (int i = 0; i < a->nc; i++) for (int k = 0; k < a->nts[i]; k++) { const NxTitle *x = &a->ts[i][k];
            if (x->kind != 2 || (int)(x->title_id & 0xfff) != aoc) continue;
            if (title_id && x->app_id && x->app_id != title_id) other = x; else if (!t) t = x; }
        if (!t && other) BAD("add-on content %d in the dump is of application %016llx, the running game is %016llx", aoc, (unsigned long long)other->app_id, (unsigned long long)title_id);
        if (!t) BAD("no add-on content %d in the dump", aoc);
        if (open_nca(&a->base, t, t->data, "add-on data", why, wn)) goto bad;
        a->has_base = 1; a->top = &a->base; a->sec = nx_nca_find_section(a->top, NX_FS_ROMFS);
        if (a->sec < 0) BAD("add-on %016llx has no RomFS", (unsigned long long)t->title_id);
        if (!crypt_ok(a->top, a->sec, 0, "add-on", why, wn)) goto bad;
        snprintf(a->what, sizeof a->what, "add-on %016llx v%u of %.160s", (unsigned long long)t->title_id, t->version, leaf(t->c->path));
    } else {          // the game: the update's RomFS (a patch over the base's, or whole), else the base's
        const NxTitle *base = NULL, *upd = NULL, *odd = NULL;
        for (int i = 0; i < a->nc; i++) for (int k = 0; k < a->nts[i]; k++) { const NxTitle *x = &a->ts[i][k];
            if (x->kind == 0) { if (title_id && x->title_id != title_id) odd = x; else if (!base) base = x; } }
        uint64_t app = base ? base->title_id : title_id;
        for (int i = 0; i < a->nc; i++) for (int k = 0; k < a->nts[i]; k++) { const NxTitle *x = &a->ts[i][k];
            if (x->kind == 1) { if (app && x->app_id != app) odd = x; else if (!upd || x->version > upd->version) upd = x; } }
        if (odd) BAD("%s holds the %s of title %016llx, the %s is %016llx", leaf(odd->c->path), odd->kind ? "update" : "base", (unsigned long long)(odd->kind ? odd->app_id : odd->title_id),
                     title_id ? "running game" : "base", (unsigned long long)app);
        if (!base && !upd) BAD("no base or update of a game in the dump%s", a->nc > 1 ? "s" : "");
        int brom = -1, uro = -1; const char *role = "base";
        if (base) { if (open_nca(&a->base, base, base->program, "base program", why, wn)) goto bad; a->has_base = 1; brom = nx_nca_find_section(&a->base, NX_FS_ROMFS); }
        if (upd) { if (open_nca(&a->upd, upd, upd->program, "update program", why, wn)) goto bad; a->has_upd = 1; uro = nx_nca_find_section(&a->upd, NX_FS_ROMFS); }
        if (uro >= 0) {
            a->top = &a->upd; a->sec = uro; role = "update";
            if (a->upd.sec[uro].crypt_type == 4) {
                if (brom < 0) BAD("the update v%u patches the base game's RomFS: the base dump must be among the files", upd->version);
                if (!crypt_ok(&a->base, brom, 0, "base", why, wn)) goto bad;
                if (nx_nca_set_base(&a->upd, uro, &a->base, brom)) BAD("the update's patch tables: %s", nximport_last_error());
            }
        } else if (brom >= 0) {
            a->top = &a->base; a->sec = brom;
            if (a->base.sec[brom].sparse) BAD("the base dump's RomFS is sparse (it holds only what its update reads from it): the update dump must be among the files");
        } else BAD("the game has no RomFS");
        if (!crypt_ok(a->top, a->sec, 1, role, why, wn)) goto bad;
        if (main_build && *main_build) {   // the version: the dumps' main against the module the game runs
            char got[33] = ""; int r = a->has_upd ? main_build_of(&a->upd, got, why, wn) : 1;
            if (r > 0 && a->has_base) r = main_build_of(&a->base, got, why, wn);
            if (r < 0) goto bad;
            if (r > 0) BAD("the dumps hold no ExeFS to check the game's version against");
            if (strncasecmp(got, main_build, strlen(main_build)))
                BAD("the dumps are of another version than the running game: their main is build %s%s, the game's is %s (use the base and update the game was built from)",
                    got, upd ? "" : " (no update among the files)", main_build);
        }
        int p = base ? snprintf(a->what, sizeof a->what, "base %.120s", leaf(base->c->path)) : 0;
        if (upd && p < (int)sizeof a->what) snprintf(a->what + p, sizeof a->what - (size_t)p, "%supdate v%u %.120s", base ? " + " : "", upd->version, leaf(upd->c->path));
    }
    if (nx_sec_share(a->top, a->sec)) BAD("the RomFS section's compression table: %s", nximport_last_error());
    if (a->top->sec[a->sec].compressed && strlen(a->what) + 32 < sizeof a->what) strcat(a->what, ", compressed (LZ4)");
    if (!(tree = nx_romfs_tree(a->top, a->sec))) BAD("the RomFS cannot be read (wrong keys?): %s", nximport_last_error());
    if (!(m = index_of(tree, why, wn))) { e = errno; goto bad; }
    nx_node_free(tree);
    m->aux = a; m->aux_free = nsp_free;
    pthread_mutex_unlock(&g_open);
    return m;
bad:
    nx_node_free(tree); nsp_free(a);
    pthread_mutex_unlock(&g_open);
    errno = e; return NULL;
#undef BAD
}

int pn_nsp_read(const romfs_img *m, const img_ent *e, uint64_t off, void *buf, size_t len) {
    const pn_nsp *a = m->aux;
    if (!len) return 1;
    errno = 0;
    if (nx_sec_read_tc(a->top, a->sec, e->a + off, buf, len, NULL)) { if (errno != EFAULT) errno = EIO; return 0; }   // EFAULT: buf is not writable (the host prepares it and reads again)
    return 1;
}
