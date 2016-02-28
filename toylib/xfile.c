/* xwrap.c - wrappers around existing library functions.
 *
 * Functions with the x prefix are wrappers that either succeed or kill the
 * program with an error message, but never return failure. They usually have
 * the same arguments and return value as the function they wrap.
 *
 * Copyright 2006 Rob Landley <rob@landley.net>
 */

#include "toys.h"
#include "xfuncs.h"


// Die unless we can allocate enough space to sprintf() into.
char *xmprintf(char *format, ...)
{
  va_list va, va2;
  int len;
  char *ret;

  va_start(va, format);
  va_copy(va2, va);

  // How long is it?
  len = vsnprintf(0, 0, format, va);
  len++;
  va_end(va);

  // Allocate and do the sprintf()
  ret = xmalloc(len);
  vsnprintf(ret, len, format, va2);
  va_end(va2);

  return ret;
}

void xprintf(char *format, ...)
{
  va_list va;
  va_start(va, format);

  vprintf(format, va);
  va_end(va);
  if (fflush(stdout) || ferror(stdout)) perror_exit("write");
}

void xputs(char *s)
{
  if (EOF == puts(s) || fflush(stdout) || ferror(stdout)) perror_exit("write");
}

void xputc(char c)
{
  if (EOF == fputc(c, stdout) || fflush(stdout) || ferror(stdout))
    perror_exit("write");
}

void xflush(void)
{
  if (fflush(stdout) || ferror(stdout)) perror_exit("write");;
}

void xaccess(char *path, int flags)
{
  if (access(path, flags)) perror_exit("Can't access '%s'", path);
}

// Die unless we can delete a file.  (File must exist to be deleted.)
void xunlink(char *path)
{
  if (unlink(path)) perror_exit("unlink '%s'", path);
}

// Die unless we can open/create a file, returning file descriptor.
int xcreate(char *path, int flags, int mode)
{
  int fd = open(path, flags^O_CLOEXEC, mode);
  if (fd == -1) perror_exit("%s", path);
  return fd;
}

// Die unless we can open a file, returning file descriptor.
int xopen(char *path, int flags)
{
  return xcreate(path, flags, 0);
}

void xclose(int fd)
{
  if (close(fd)) perror_exit("xclose");
}

int xdup(int fd)
{
  if (fd != -1) {
    fd = dup(fd);
    if (fd == -1) perror_exit("xdup");
  }
  return fd;
}

FILE *xfdopen(int fd, char *mode)
{
  FILE *f = fdopen(fd, mode);

  if (!f) perror_exit("xfdopen");

  return f;
}

// Die unless we can open/create a file, returning FILE *.
FILE *xfopen(char *path, char *mode)
{
  FILE *f = fopen(path, mode);
  if (!f) perror_exit("No file %s", path);
  return f;
}

// Die if there's an error other than EOF.
size_t xread(int fd, void *buf, size_t len)
{
  ssize_t ret = read(fd, buf, len);
  if (ret < 0) perror_exit("xread");

  return ret;
}

void xreadall(int fd, void *buf, size_t len)
{
  if (len != readall(fd, buf, len)) perror_exit("xreadall");
}

// There's no xwriteall(), just txwrite().  When we read, there may or may not
// be more data waiting.  When we write, there is data and it had better go
// somewhere.

void txwrite(int fd, void *buf, size_t len)
{
  if (len != writeall(fd, buf, len)) perror_exit("txwrite");
}

// Die if lseek fails, probably due to being called on a pipe.

off_t xlseek(int fd, off_t offset, int whence)
{
  offset = lseek(fd, offset, whence);
  if (offset<0) perror_exit("lseek");

  return offset;
}

char *xgetcwd(void)
{
  char *buf = getcwd(NULL, 0);
  if (!buf) perror_exit("xgetcwd");

  return buf;
}

void xstat(char *path, struct stat *st)
{
  if(stat(path, st)) perror_exit("Can't stat %s", path);
}

// Cannonicalize path, even to file with one or more missing components at end.
// if exact, require last path component to exist
char *xabspath(char *path, int exact)
{
  struct string_list *todo, *done = 0;
  int try = 9999, dirfd = open("/", 0);;
  char buf[4096], *ret;

  // If this isn't an absolute path, start with cwd.
  if (*path != '/') {
    char *temp = xgetcwd();

    splitpath(path, splitpath(temp, &todo));
    free(temp);
  } else splitpath(path, &todo);

  // Iterate through path components
  while (todo) {
    struct string_list *new = llist_pop(&todo), **tail;
    ssize_t len;

    if (!try--) {
      errno = ELOOP;
      goto error;
    }

    // Removable path componenents.
    if (!strcmp(new->str, ".") || !strcmp(new->str, "..")) {
      int x = new->str[1];

      free(new);
      if (x) {
        if (done) free(llist_pop(&done));
        len = 0;
      } else continue;

    // Is this a symlink?
    } else len=xreadlinkat(dirfd, new->str, buf, 4096);

    if (len>4095) goto error;
    if (len<1) {
      int fd;
      char *s = "..";

      // For .. just move dirfd
      if (len) {
        // Not a symlink: add to linked list, move dirfd, fail if error
        if ((exact || todo) && errno != EINVAL) goto error;
        new->next = done;
        done = new;
        if (errno == EINVAL && !todo) break;
        s = new->str;
      }
      fd = xopenat(dirfd, s, 0);
      if (fd == -1 && (exact || todo || errno != ENOENT)) goto error;
      close(dirfd);
      dirfd = fd;
      continue;
    }

    // If this symlink is to an absolute path, discard existing resolved path
    buf[len] = 0;
    if (*buf == '/') {
      llist_traverse(done, free);
      done=0;
      close(dirfd);
      dirfd = open("/", 0);
    }
    free(new);

    // prepend components of new path. Note symlink to "/" will leave new NULL
    tail = splitpath(buf, &new);

    // symlink to "/" will return null and leave tail alone
    if (new) {
      *tail = todo;
      todo = new;
    }
  }
  close(dirfd);

  // At this point done has the path, in reverse order. Reverse list while
  // calculating buffer length.

  try = 2;
  while (done) {
    struct string_list *temp = llist_pop(&done);;

    if (todo) try++;
    try += strlen(temp->str);
    temp->next = todo;
    todo = temp;
  }

  // Assemble return buffer

  ret = xmalloc(try);
  *ret = '/';
  ret [try = 1] = 0;
  while (todo) {
    if (try>1) ret[try++] = '/';
    try = stpcpy(ret+try, todo->str) - ret;
    free(llist_pop(&todo));
  }

  return ret;

error:
  close(dirfd);
  llist_traverse(todo, free);
  llist_traverse(done, free);

  return NULL;
}

void xchdir(char *path)
{
  if (chdir(path)) error_exit("chdir '%s'", path);
}

// This can return null (meaning file not found).  It just won't return null
// for memory allocation reasons.
char *xreadlink(char *name)
{
  int len, size = 0;
  char *buf = 0;

  // Grow by 64 byte chunks until it's big enough.
  for(;;) {
    size +=64;
    buf = xrealloc(buf, size);
    len = readlink(name, buf, size);

    if (len<0) {
      free(buf);
      return 0;
    }
    if (len<size) {
      buf[len]=0;
      return buf;
    }
  }
}

char *xreadfile(char *name, char *buf, off_t len)
{
  if (!(buf = readfile(name, buf, len))) perror_exit("Bad '%s'", name);

  return buf;
}

int xioctl(int fd, int request, void *data)
{
  int rc;

  errno = 0;
  rc = ioctl(fd, request, data);
  if (rc == -1 && errno) perror_exit("ioctl %x", request);

  return rc;
}

// Open a /var/run/NAME.pid file, dying if we can't write it or if it currently
// exists and is this executable.
void xpidfile(char *name)
{
  char pidfile[256], spid[32];
  int i, fd;
  pid_t pid;

  sprintf(pidfile, "/var/run/%s.pid", name);
  // Try three times to open the sucker.
  for (i=0; i<3; i++) {
    fd = open(pidfile, O_CREAT|O_EXCL|O_WRONLY, 0644);
    if (fd != -1) break;

    // If it already existed, read it.  Loop for race condition.
    fd = open(pidfile, O_RDONLY);
    if (fd == -1) continue;

    // Is the old program still there?
    spid[xread(fd, spid, sizeof(spid)-1)] = 0;
    close(fd);
    pid = atoi(spid);
    if (pid < 1 || (kill(pid, 0) && errno == ESRCH)) unlink(pidfile);

    // An else with more sanity checking might be nice here.
  }

  if (i == 3) error_exit("xpidfile %s", name);

  txwrite(fd, spid, sprintf(spid, "%ld\n", (long)getpid()));
  close(fd);
}

// Copy the rest of in to out and close both files.

void xsendfile(int in, int out)
{
  long len;

  if (in<0) return;
  for (;;) {
    len = xread(in, libbuf, sizeof(libbuf));
    if (len<1) break;
    txwrite(out, libbuf, len);
  }
}

// parse fractional seconds with optional s/m/h/d suffix
long xparsetime(char *arg, long units, long *fraction)
{
  double d;
  long l;

  if (CFG_TOYBOX_FLOAT) d = strtod(arg, &arg);
  else l = strtoul(arg, &arg, 10);

  // Parse suffix
  if (*arg) {
    int ismhd[]={1,60,3600,86400}, i = stridx("smhd", *arg);

    if (i == -1) error_exit("Unknown suffix '%c'", *arg);
    if (CFG_TOYBOX_FLOAT) d *= ismhd[i];
    else l *= ismhd[i];
  }

  if (CFG_TOYBOX_FLOAT) {
    l = (long)d;
    if (fraction) *fraction = units*(d-l);
  } else if (fraction) *fraction = 0;

  return l;
}
