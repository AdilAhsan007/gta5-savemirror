/* GTA V Save Mirror - offline, read-only save snapshotter for GoldHEN.
 *
 * Runs as a GoldHEN ELF payload (PayLoader LaunchPad / AutoRun), NOT as a
 * plugin inside the game. It only ever READS the console's save files and
 * WRITES new, uniquely named folders under <USB>/SaveMirror/.
 *
 * Built against the ps4-payload-dev SDK (GPLv3+), so this file is GPLv3+.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HOST_TEST
#include "host_stubs.h"
#else
#include <ps4/kernel.h>
#endif

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */
#define TITLE_ID          "CUSA00419"
#define POLL_SECS         15      /* how often we look at the save        */
#ifndef STABLE_IDLE_SECS
#define STABLE_IDLE_SECS 30      /* unchanged this long -> safe (game off)*/
#endif
#ifndef STABLE_LIVE_SECS
#define STABLE_LIVE_SECS 90      /* unchanged this long -> ok (game on)   */
#endif
#ifndef KEEP_RECENT
#define KEEP_RECENT 40      /* newest snapshots always kept          */
#endif
#define KEEP_DAILY_DAYS   90      /* plus first snapshot of each day       */
#ifndef MIN_FREE_MB
#define MIN_FREE_MB 2048    /* never fill the drive past this        */
#endif
#define MAX_USB           8
#define COPY_CHUNK        (512*1024)
#define MAX_FILES         64

#ifndef ROOT                        /* only overridden by the host test */
#define ROOT              ""
#endif
#ifndef POLL_OVERRIDE
#define POLL_OVERRIDE     POLL_SECS
#endif
#define USER_HOME         ROOT "/user/home"
#define SANDBOX_DIR       ROOT "/mnt/sandbox/" TITLE_ID "_000"
#define SANDBOX_SAVE      SANDBOX_DIR "/savedata0"
#define STATE_DIR         ROOT "/data/savemirror"
#define USB_FMT           ROOT "/mnt/usb%d"
#define LOCK_FILE         STATE_DIR "/instance.lock"
#define MIRROR_DIR        "SaveMirror"
#define MARKER_FILE       "ENABLE_SAVEMIRROR.txt"
#define STOP_FILE         "STOP.txt"
#define QUIET_FILE        "QUIET.txt"

/* ------------------------------------------------------------------ */
/* Notifications and logging                                           */
/* ------------------------------------------------------------------ */
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
#ifdef HOST_TEST
static int sceKernelSendNotificationRequest(int a, notify_request_t *r,
                                            size_t n, int b) {
  (void)a; (void)n; (void)b;
  printf("[NOTIFY] %s\n", r->message);
  fflush(stdout);
  return 0;
}
#else
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif

static char g_usb[192];          /* e.g. "/mnt/usb1", empty if none */
static int  g_quiet;

static void notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;
  if(g_quiet) return;
  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static void logmsg(const char *fmt, ...) {
  char path[256], line[1024], ts[32];
  time_t now = time(NULL);
  struct tm tm;
  va_list ap;
  FILE *f;

  localtime_r(&now, &tm);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  if((f = fopen(STATE_DIR "/savemirror.log", "a"))) {
    fprintf(f, "%s %s\n", ts, line);
    fclose(f);
  }
  if(g_usb[0]) {
    snprintf(path, sizeof path, "%s/%s/savemirror.log", g_usb, MIRROR_DIR);
    if((f = fopen(path, "a"))) {
      fprintf(f, "%s %s\n", ts, line);
      fclose(f);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static uint32_t crc_table[256];
static void crc_init(void) {
  for(uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for(int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *b, size_t n) {
  crc = ~crc;
  while(n--) crc = crc_table[(crc ^ *b++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}
static void mkdir_p(const char *p) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", p);
  for(char *s = tmp + 1; *s; s++) {
    if(*s == '/') { *s = 0; mkdir(tmp, 0777); *s = '/'; }
  }
  mkdir(tmp, 0777);
}

/* Remove a directory tree that THIS program created (snapshots only). */
static void rm_tree(const char *p) {
  DIR *d = opendir(p);
  struct dirent *e;
  char c[512];
  if(!d) { unlink(p); return; }
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(c, sizeof c, "%s/%s", p, e->d_name);
    if(is_dir(c)) rm_tree(c); else unlink(c);
  }
  closedir(d);
  rmdir(p);
}

/* ------------------------------------------------------------------ */
/* Privileges: GoldHEN's loader normally gives us root + real FS root. */
/* Only escalate ourselves if we cannot already see /user/home.        */
/* ------------------------------------------------------------------ */
static void ensure_privileges(void) {
  pid_t pid = getpid();
  if(access(USER_HOME, R_OK) == 0) return;
  kernel_set_ucred_uid(pid, 0);
  kernel_set_ucred_ruid(pid, 0);
  kernel_set_ucred_svuid(pid, 0);
  kernel_set_ucred_rgid(pid, 0);
  kernel_set_ucred_svgid(pid, 0);
  kernel_set_ucred_prison(pid, KERNEL_ADDRESS_PRISON0);
  intptr_t root = kernel_getlong(KERNEL_ADDRESS_ROOTVNODE);
  kernel_set_proc_rootdir(pid, root);
  kernel_set_proc_jaildir(pid, root);
}

/* ------------------------------------------------------------------ */
/* Finding the USB drive: the one whose SaveMirror/ folder contains    */
/* ENABLE_SAVEMIRROR.txt. Re-checked every cycle (usb0/usb1 may swap). */
/* ------------------------------------------------------------------ */
static int find_usb(void) {
  char p[256];
  for(int i = 0; i < MAX_USB; i++) {
    snprintf(p, sizeof p, USB_FMT "/%s/%s", i, MIRROR_DIR, MARKER_FILE);
    if(exists(p)) {
      snprintf(g_usb, sizeof g_usb, USB_FMT, i);
      return 1;
    }
  }
  g_usb[0] = 0;
  return 0;
}

static long usb_free_mb(void) {
  struct statfs sf;
  if(statfs(g_usb, &sf) != 0) return -1;
  return (long)(((uint64_t)sf.f_bavail * sf.f_bsize) / (1024 * 1024));
}

static int flag_on_usb(const char *name) {
  char p[192];
  snprintf(p, sizeof p, "%s/%s/%s", g_usb, MIRROR_DIR, name);
  return exists(p);
}

/* ------------------------------------------------------------------ */
/* A "signature" of a save folder: names + sizes + mtimes.             */
/* ------------------------------------------------------------------ */
typedef struct {
  char     name[256];
  off_t    size;
  int64_t  mtime_s;
  long     mtime_ns;
} fileinfo_t;

typedef struct {
  int        n;
  fileinfo_t f[MAX_FILES];
  uint32_t   hash;      /* hash over all fields above */
} savesig_t;

static int cmp_fi(const void *a, const void *b) {
  return strcmp(((const fileinfo_t*)a)->name, ((const fileinfo_t*)b)->name);
}

/* Flat folder (raw encrypted saves live flat in .../savedata/CUSAxxxxx). */
static int read_sig_flat(const char *dir, savesig_t *s) {
  DIR *d = opendir(dir);
  struct dirent *e;
  struct stat st;
  char p[512];
  memset(s, 0, sizeof *s);
  if(!d) return -1;
  while((e = readdir(d)) && s->n < MAX_FILES) {
    if(e->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    if(stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    fileinfo_t *fi = &s->f[s->n++];
    snprintf(fi->name, sizeof fi->name, "%s", e->d_name);
    fi->size = st.st_size;
    fi->mtime_s = st.st_mtim.tv_sec;
    fi->mtime_ns = st.st_mtim.tv_nsec;
  }
  closedir(d);
  qsort(s->f, s->n, sizeof s->f[0], cmp_fi);
  s->hash = crc_update(0, (const uint8_t*)s->f, sizeof(fileinfo_t) * s->n);
  return s->n > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Copy one file, CRC while copying, then re-read the copy and compare. */
/* ------------------------------------------------------------------ */
static int copy_verify(const char *src, const char *dst, off_t expect,
                       uint32_t *crc_out) {
  static uint8_t *buf = NULL;
  int in = -1, out = -1, rc = -1;
  uint32_t crc_src = 0, crc_dst = 0;
  off_t total = 0;
  ssize_t n;

  if(!buf && !(buf = malloc(COPY_CHUNK))) return -1;
  if((in = open(src, O_RDONLY)) < 0) goto done;
  if((out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) goto done;

  while((n = read(in, buf, COPY_CHUNK)) > 0) {
    crc_src = crc_update(crc_src, buf, n);
    if(write(out, buf, n) != n) goto done;
    total += n;
    usleep(2000);               /* be gentle with the disk during gameplay */
  }
  if(n < 0 || total != expect) goto done;
  fsync(out);
  close(out); out = -1;
  close(in);  in = -1;

  if((in = open(dst, O_RDONLY)) < 0) goto done;
  total = 0;
  while((n = read(in, buf, COPY_CHUNK)) > 0) {
    crc_dst = crc_update(crc_dst, buf, n);
    total += n;
  }
  if(n < 0 || total != expect || crc_dst != crc_src) goto done;
  *crc_out = crc_src;
  rc = 0;
done:
  if(in >= 0) close(in);
  if(out >= 0) close(out);
  return rc;
}

/* Recursive copy for the optional decrypted (mounted) view. */
static int copy_tree(const char *src, const char *dst, FILE *manifest,
                     const char *rel) {
  DIR *d = opendir(src);
  struct dirent *e;
  struct stat st;
  char s[512], t[512], r[512];
  uint32_t crc;
  int rc = 0;
  if(!d) return -1;
  mkdir_p(dst);
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(s, sizeof s, "%s/%s", src, e->d_name);
    snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
    snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
    if(stat(s, &st) != 0) { rc = -1; continue; }
    if(S_ISDIR(st.st_mode)) {
      if(copy_tree(s, t, manifest, r)) rc = -1;
    } else if(S_ISREG(st.st_mode)) {
      if(copy_verify(s, t, st.st_size, &crc)) rc = -1;
      else fprintf(manifest, "decrypted/%s  size=%lld  crc32=%08x\n",
                   r, (long long)st.st_size, crc);
    }
  }
  closedir(d);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Snapshot + retention                                                */
/* ------------------------------------------------------------------ */
static int game_running(void) { return is_dir(SANDBOX_DIR); }

static int take_snapshot(const char *user, const char *srcdir, const savesig_t *sig,
                         int live) {
  char base[256], part[320], fin[320], p[512], q[512], ts[32];
  time_t now = time(NULL);
  struct tm tm;
  FILE *mf;
  savesig_t after;
  int ok = 1;

  localtime_r(&now, &tm);
  strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tm);
  snprintf(base, sizeof base, "%s/%s/%s/%s", g_usb, MIRROR_DIR, TITLE_ID, user);
  mkdir_p(base);
  snprintf(part, sizeof part, "%s/%s_%s.partial", base, ts, live ? "live" : "idle");
  snprintf(fin,  sizeof fin,  "%s/%s_%s",         base, ts, live ? "live" : "idle");
  if(exists(fin) || exists(part)) return -1;   /* never overwrite anything */
  snprintf(p, sizeof p, "%s/raw", part);
  mkdir_p(p);

  snprintf(p, sizeof p, "%s/MANIFEST.txt", part);
  if(!(mf = fopen(p, "w"))) { rm_tree(part); return -1; }
  fprintf(mf, "GTA V Save Mirror snapshot\ntitle=%s user=%s mode=%s\n"
              "source=%s\n\n", TITLE_ID, user, live ? "live(game running)"
              : "idle(game closed)", srcdir);

  for(int i = 0; i < sig->n && ok; i++) {
    uint32_t crc;
    snprintf(p, sizeof p, "%s/%s", srcdir, sig->f[i].name);
    snprintf(q, sizeof q, "%s/raw/%s", part, sig->f[i].name);
    if(copy_verify(p, q, sig->f[i].size, &crc)) ok = 0;
    else fprintf(mf, "raw/%s  size=%lld  crc32=%08x\n", sig->f[i].name,
                 (long long)sig->f[i].size, crc);
  }

  /* The save must not have changed while we were copying it. */
  if(ok && (read_sig_flat(srcdir, &after) || after.hash != sig->hash)) {
    logmsg("user %s: save changed during copy, discarding attempt", user);
    ok = 0;
  }

  /* Optional, best effort: decrypted files from the game's mounted save. */
  if(ok && live && is_dir(SANDBOX_SAVE)) {
    snprintf(p, sizeof p, "%s/decrypted", part);
    if(copy_tree(SANDBOX_SAVE, p, mf, ""))
      fprintf(mf, "\nNOTE: decrypted copy incomplete - ignore that folder\n");
  }

  fclose(mf);
  if(!ok) { rm_tree(part); return -1; }
  if(rename(part, fin) != 0) { rm_tree(part); return -1; }
  logmsg("user %s: snapshot OK -> %s (%d files)", user, fin, sig->n);
  return 0;
}

static int cmp_str_desc(const void *a, const void *b) {
  return strcmp(*(char * const *)b, *(char * const *)a);
}

/* Keep the newest KEEP_RECENT snapshots, plus the newest snapshot of each
 * of the last KEEP_DAILY_DAYS days that have snapshots. Only folders whose
 * names this program generates are ever considered. */
static void prune(const char *user) {
  char base[256], p[512];
  char *names[2048];
  int n = 0;
  DIR *d;
  struct dirent *e;

  snprintf(base, sizeof base, "%s/%s/%s/%s", g_usb, MIRROR_DIR, TITLE_ID, user);
  if(!(d = opendir(base))) return;
  while((e = readdir(d)) && n < 2048) {
    size_t L = strlen(e->d_name);
    if(L < 24 || e->d_name[4] != '-' || e->d_name[10] != '_') continue;
    if(strstr(e->d_name, ".partial")) {        /* leftover from power loss */
      snprintf(p, sizeof p, "%s/%s", base, e->d_name);
      rm_tree(p);
      continue;
    }
    names[n++] = strdup(e->d_name);
  }
  closedir(d);
  qsort(names, n, sizeof names[0], cmp_str_desc);   /* newest first */

  char lastday[11] = "";
  int days_kept = 0;
  for(int i = 0; i < n; i++) {
    int keep = i < KEEP_RECENT;
    if(strncmp(names[i], lastday, 10) != 0) {        /* newest of its day */
      memcpy(lastday, names[i], 10);
      lastday[10] = 0;
      if(++days_kept <= KEEP_DAILY_DAYS) keep = 1;
    }
    if(!keep) {
      snprintf(p, sizeof p, "%s/%s", base, names[i]);
      rm_tree(p);
      logmsg("user %s: pruned old snapshot %s", user, names[i]);
    }
    free(names[i]);
  }
}

/* ------------------------------------------------------------------ */
/* Per-user state (kept in memory + last backed-up hash on /data).     */
/* ------------------------------------------------------------------ */
typedef struct {
  char     user[16];
  uint32_t seen_hash;      /* last observed signature            */
  time_t   seen_since;     /* when it was first observed         */
  uint32_t saved_hash;     /* signature of last good snapshot    */
} ustate_t;

static ustate_t g_users[16];
static int g_nusers;

static ustate_t *get_user(const char *u) {
  for(int i = 0; i < g_nusers; i++)
    if(!strcmp(g_users[i].user, u)) return &g_users[i];
  if(g_nusers >= 16) return NULL;
  ustate_t *s = &g_users[g_nusers++];
  memset(s, 0, sizeof *s);
  snprintf(s->user, sizeof s->user, "%s", u);
  char p[128];
  snprintf(p, sizeof p, STATE_DIR "/last_%s.txt", u);
  FILE *f = fopen(p, "r");
  if(f) { if(fscanf(f, "%x", &s->saved_hash) != 1) s->saved_hash = 0; fclose(f); }
  return s;
}

static void store_saved(ustate_t *s) {
  char p[128];
  snprintf(p, sizeof p, STATE_DIR "/last_%s.txt", s->user);
  FILE *f = fopen(p, "w");
  if(f) { fprintf(f, "%08x\n", s->saved_hash); fclose(f); }
}

static void cycle(void) {
  DIR *d;
  struct dirent *e;
  char dir[256];
  savesig_t sig;
  int live = game_running();
  time_t now = time(NULL);

  if(!(d = opendir(USER_HOME))) return;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.' || strlen(e->d_name) != 8) continue;
    snprintf(dir, sizeof dir, USER_HOME "/%s/savedata/" TITLE_ID, e->d_name);
    if(!is_dir(dir) || read_sig_flat(dir, &sig)) continue;

    ustate_t *s = get_user(e->d_name);
    if(!s) continue;
    if(sig.hash != s->seen_hash) {           /* something is being written */
      s->seen_hash = sig.hash;
      s->seen_since = now;
      continue;
    }
    if(sig.hash == s->saved_hash) continue;  /* already backed up */
    if(now - s->seen_since < (live ? STABLE_LIVE_SECS : STABLE_IDLE_SECS))
      continue;                              /* not quiet for long enough */

    long fmb = usb_free_mb();
    if(fmb >= 0 && fmb < MIN_FREE_MB) {
      logmsg("USB has only %ld MB free - backup skipped", fmb);
      notify("GTA V Save Mirror: USB drive nearly full, backup skipped");
      continue;
    }
    if(take_snapshot(s->user, dir, &sig, live) == 0) {
      s->saved_hash = sig.hash;
      store_saved(s);
      prune(s->user);
      notify("GTA V save backed up%s", live ? "" : " (game closed)");
    } else {
      s->seen_since = now;                   /* wait a full window, retry */
    }
  }
  closedir(d);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  crc_init();
  ensure_privileges();
  setpriority(PRIO_PROCESS, 0, 20);
  mkdir_p(STATE_DIR);

  /* Single instance: a second copy exits immediately. */
  int lk = open(LOCK_FILE, O_RDWR | O_CREAT, 0666);
  if(lk < 0 || flock(lk, LOCK_EX | LOCK_NB) != 0) return 0;

  int warned = 0;
  if(find_usb()) {
    g_quiet = flag_on_usb(QUIET_FILE);
    logmsg("started (pid %d), backing up to %s", getpid(), g_usb);
    notify("GTA V Save Mirror running -> %s", g_usb);
  } else {
    logmsg("started (pid %d), no USB with " MIRROR_DIR "/" MARKER_FILE, getpid());
  }

  for(;;) {
    if(!find_usb()) {
      if(!warned) {
        notify("GTA V Save Mirror: backup drive not found");
        logmsg("backup drive not found, waiting");
        warned = 1;
      }
    } else {
      warned = 0;
      g_quiet = flag_on_usb(QUIET_FILE);
      if(flag_on_usb(STOP_FILE)) {
        logmsg("STOP.txt found - exiting");
        notify("GTA V Save Mirror stopped (STOP.txt)");
        return 0;
      }
      cycle();
    }
    sleep(POLL_OVERRIDE);
  }
  return 0;
}
