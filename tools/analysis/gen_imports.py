#!/usr/bin/env python3
"""Generate docs/imports.json: one entry per imported symbol of libZombieWood.so with
{name, lib, category, kind, signature, used, calls, stub_ok, note}.

`used`/`calls` come from import_xrefs.py output (direct BL/BLX to the PLT stub, or data relocations).
Signatures are the bionic (Android 4.x, ILP32, softfp AAPCS) prototypes; they are maintained by hand below.

Usage: python gen_imports.py <import_xrefs.json> <out imports.json>
"""
import sys, json

# category -> (lib, [ "name|signature|stub_ok(0/1)|note" ... ])
T = {}


def add(cat, lib, rows):
    T.setdefault(cat, []).extend((lib, r) for r in rows.strip().split('\n'))


add('data', 'libc.so', r'''
__sF|FILE __sF[3]  /* stdin, stdout, stderr; sizeof(FILE)=84 on bionic 32-bit */|0|R_ARM_GLOB_DAT; guest takes &__sF[1] (=+84) and &__sF[2] (=+168). Must be guest memory with a recognisable FILE layout
__stack_chk_guard|uintptr_t __stack_chk_guard|0|R_ARM_GLOB_DAT; guest reads *GOT -> value. Any constant works, must not change after load
_ctype_|const char *_ctype_  /* points to table[1+256]; use (_ctype_+1)[c] */|0|R_ARM_GLOB_DAT; bionic bits: _U=0x01 _L=0x02 _N=0x04 _S=0x08 _P=0x10 _C=0x20 _X=0x40 _B=0x80. Inlined isalpha()/isdigit() from <ctype.h> macros read it
_tolower_tab_|const short *_tolower_tab_  /* points to short[1+256]; (_tolower_tab_+1)[c] */|0|R_ARM_GLOB_DAT
_toupper_tab_|const short *_toupper_tab_  /* points to short[1+256] */|0|R_ARM_GLOB_DAT
''')

add('libc-runtime', 'libc.so', r'''
__assert2|void __assert2(const char *file, int line, const char *func, const char *expr) __noreturn|0|5.5k call sites (glitch/boost asserts compiled in). Log and abort the guest
__cxa_atexit|int __cxa_atexit(void (*fn)(void *), void *arg, void *dso_handle)|1|Record or ignore (process never unloads the .so). Return 0
__cxa_finalize|void __cxa_finalize(void *dso_handle)|1|Only from .fini_array stub; no-op
__errno|int *__errno(void)|0|Per-guest-thread int in guest memory
__gnu_Unwind_Find_exidx|_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *pcount)|0|Needed for C++ exceptions (unwinder is statically linked). Return guest address of .ARM.exidx (0xb1c678 + base) and *pcount = 0x2f6a0/8 = 24276 when pc is inside the module, else 0
__stack_chk_fail|void __stack_chk_fail(void) __noreturn|0|Abort with diagnostics
abort|void abort(void) __noreturn|0|
exit|void exit(int status) __noreturn|0|Called by Java-facing Exit paths; terminate host process cleanly
getenv|char *getenv(const char *name)|1|Only OpenSSL/curl/Lua/libjpeg(JPEGMEM). Return NULL
system|int system(const char *cmd)|1|Only Lua os.execute. Return -1
sysconf|long sysconf(int name)|1|1 site: GetNumberOfCores() -> _SC_NPROCESSORS_ONLN (0x61 on bionic). Return host core count (or 2)
getpid|pid_t getpid(void)|1|OpenSSL RAND seeding. Any constant
getuid|uid_t getuid(void)|1|OpenSSL/curl
geteuid|uid_t geteuid(void)|1|
getgid|gid_t getgid(void)|1|
getegid|gid_t getegid(void)|1|
getpwuid|struct passwd *getpwuid(uid_t uid)|1|curl netrc home lookup. Return NULL
raise|int raise(int sig)|1|OpenSSL UI only
sigaction|int sigaction(int sig, const struct sigaction *act, struct sigaction *old)|1|OpenSSL UI only. Return 0
bsd_signal|sighandler_t bsd_signal(int sig, sighandler_t h)|1|OpenSSL UI only. Return SIG_DFL(0)
setjmp|int setjmp(jmp_buf env)  /* jmp_buf = long[64] = 256 bytes on bionic ARM */|0|Used by libpng, libjpeg, Lua, FreeType, GLU tess. Must be implemented on the guest CPU state (save r4-r11,sp,lr + d8-d15,fpscr), not as host setjmp
longjmp|void longjmp(jmp_buf env, int val) __noreturn|0|See setjmp
qsort|void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))|0|Callback into guest code: run comparator via host->guest call (or run a guest-side implementation)
srand48|void srand48(long seed)|0|
lrand48|long lrand48(void)|0|47 call sites: gameplay RNG. Implement exact 48-bit LCG for determinism
''')

add('libc-memory', 'libc.so', r'''
malloc|void *malloc(size_t n)|0|Guest heap. Also referenced by address (R_ARM_ABS32 x3 + GLOB_DAT): function pointers stored in allocator tables (zlib/curl/OpenSSL) -> the import must have a guest-callable address
free|void free(void *p)|0|Same: address taken (ABS32 x3 + GLOB_DAT)
calloc|void *calloc(size_t n, size_t sz)|0|Only by address (ABS32 x1 + GLOB_DAT; curl/OpenSSL callbacks)
realloc|void *realloc(void *p, size_t n)|0|Address taken (ABS32 x2 + GLOB_DAT)
memchr|void *memchr(const void *s, int c, size_t n)|0|
memcmp|int memcmp(const void *a, const void *b, size_t n)|0|
memcpy|void *memcpy(void *d, const void *s, size_t n)|0|3.5k call sites; hot. Implement natively on host pointers
memmove|void *memmove(void *d, const void *s, size_t n)|0|
memset|void *memset(void *d, int c, size_t n)|0|
''')

add('libc-string', 'libc.so', r'''
atoi|int atoi(const char *s)|0|
atol|long atol(const char *s)|0|
strcasecmp|int strcasecmp(const char *a, const char *b)|0|
strcat|char *strcat(char *d, const char *s)|0|
strchr|char *strchr(const char *s, int c)|0|
strcmp|int strcmp(const char *a, const char *b)|0|Address also taken (GLOB_DAT) as comparator
strcoll|int strcoll(const char *a, const char *b)|0|= strcmp (C locale)
strcpy|char *strcpy(char *d, const char *s)|0|
strcspn|size_t strcspn(const char *s, const char *reject)|0|
strdup|char *strdup(const char *s)|0|Must allocate from the guest malloc heap (freed by guest free). Address taken (ABS32 + GLOB_DAT)
strerror|char *strerror(int e)|0|Return pointer into guest memory
strerror_r|int strerror_r(int e, char *buf, size_t n)|0|bionic = POSIX variant (returns int)
strlcat|size_t strlcat(char *d, const char *s, size_t n)|0|
strlen|size_t strlen(const char *s)|0|
strncasecmp|int strncasecmp(const char *a, const char *b, size_t n)|0|
strncat|char *strncat(char *d, const char *s, size_t n)|0|
strncmp|int strncmp(const char *a, const char *b, size_t n)|0|
strncpy|char *strncpy(char *d, const char *s, size_t n)|0|
strpbrk|char *strpbrk(const char *s, const char *accept)|0|
strrchr|char *strrchr(const char *s, int c)|0|
strstr|char *strstr(const char *h, const char *n)|0|
strtod|double strtod(const char *s, char **end)|0|Returns double in r0:r1 (softfp)
strtok|char *strtok(char *s, const char *delim)|0|Global state (bionic of that era: not thread-safe)
strtok_r|char *strtok_r(char *s, const char *delim, char **save)|0|
strtol|long strtol(const char *s, char **end, int base)|0|
strtoll|long long strtoll(const char *s, char **end, int base)|0|64-bit return in r0:r1
strtoul|unsigned long strtoul(const char *s, char **end, int base)|0|
strxfrm|size_t strxfrm(char *d, const char *s, size_t n)|0|C locale: strlcpy semantics
tolower|int tolower(int c)|0|
toupper|int toupper(int c)|0|
isalnum|int isalnum(int c)|0|
isalpha|int isalpha(int c)|0|
iscntrl|int iscntrl(int c)|0|
islower|int islower(int c)|0|
ispunct|int ispunct(int c)|0|
isspace|int isspace(int c)|0|
isupper|int isupper(int c)|0|
isxdigit|int isxdigit(int c)|0|
setlocale|char *setlocale(int category, const char *locale)|1|Return guest pointer to "C" (libstdc++ locale init compares the result with "C"/"POSIX")
''')

add('libc-wchar', 'libc.so', r'''
btowc|wint_t btowc(int c)|0|wchar_t is 32-bit (Tag_ABI_PCS_wchar_t=4). Used by statically linked libstdc++ locale facets
wctob|int wctob(wint_t c)|0|
mbrtowc|size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)|0|bionic 4.x: byte-wise (no real multibyte): *pwc=(unsigned char)*s, returns 0/1
wcrtomb|size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)|0|bionic 4.x: *s=(char)wc, returns 1
iswctype|int iswctype(wint_t wc, wctype_t t)|0|
wctype|wctype_t wctype(const char *name)|0|bionic enum order: alnum=1, alpha, blank, cntrl, digit, graph, lower, print, punct, space, upper, xdigit
towlower|wint_t towlower(wint_t c)|0|
towupper|wint_t towupper(wint_t c)|0|
wcscmp|int wcscmp(const wchar_t *a, const wchar_t *b)|0|
wcscoll|int wcscoll(const wchar_t *a, const wchar_t *b)|0|
wcscpy|wchar_t *wcscpy(wchar_t *d, const wchar_t *s)|0|
wcsftime|size_t wcsftime(wchar_t *d, size_t n, const wchar_t *fmt, const struct tm *tm)|1|libstdc++ time_put<wchar_t> only
wcslen|size_t wcslen(const wchar_t *s)|0|190 call sites (glitch stringw, gameswf)
wcsxfrm|size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)|0|
wmemchr|wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)|0|
wmemcmp|int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)|0|
wmemcpy|wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)|0|
wmemmove|wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n)|0|
wmemset|wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)|0|
swprintf|int swprintf(wchar_t *d, size_t n, const wchar_t *fmt, ...)|0|2 call sites; 32-bit wchar_t format parsing, varargs on guest stack
getwc|wint_t getwc(FILE *f)|1|libstdc++ stdio_sync_filebuf<wchar_t> only
putwc|wint_t putwc(wchar_t c, FILE *f)|1|idem
ungetwc|wint_t ungetwc(wint_t c, FILE *f)|1|idem
''')

add('libc-stdio', 'libc.so', r'''
fopen|FILE *fopen(const char *path, const char *mode)|0|Return guest FILE* = real 84-byte bionic FILE in guest memory: guest code reads _flags (short @12; __SEOF=0x20, __SERR=0x40 via inlined ferror/feof macros) and _file (short @14, inlined fileno). Path translation layer goes here
fdopen|FILE *fdopen(int fd, const char *mode)|1|curl only
freopen|FILE *freopen(const char *path, const char *mode, FILE *f)|1|Lua io only
fclose|int fclose(FILE *f)|0|
fread|size_t fread(void *p, size_t sz, size_t n, FILE *f)|0|Address taken (GLOB_DAT): used as callback (curl/zlib ioapi)
fwrite|size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)|0|Address taken (GLOB_DAT). f may be &__sF[1]/&__sF[2]
fseek|int fseek(FILE *f, long off, int whence)|0|
ftell|long ftell(FILE *f)|0|
rewind|void rewind(FILE *f)|0|
feof|int feof(FILE *f)|0|Real function here (not the macro)
fflush|int fflush(FILE *f)|0|f may be NULL
fgets|char *fgets(char *s, int n, FILE *f)|0|
fputc|int fputc(int c, FILE *f)|0|Address taken (GLOB_DAT)
fputs|int fputs(const char *s, FILE *f)|0|
getc|int getc(FILE *f)|0|
putc|int putc(int c, FILE *f)|0|
putchar|int putchar(int c)|1|lua_stackDump debug only
puts|int puts(const char *s)|1|Debug prints -> log
ungetc|int ungetc(int c, FILE *f)|0|Lua/libstdc++
clearerr|void clearerr(FILE *f)|1|AFile wrapper clearerr(AFILE*)
setvbuf|int setvbuf(FILE *f, char *buf, int mode, size_t n)|1|Return 0
tmpfile|FILE *tmpfile(void)|1|Lua io only. Return NULL
tmpnam|char *tmpnam(char *s)|1|Lua os only
perror|void perror(const char *s)|1|
remove|int remove(const char *path)|0|Save files / trackers
rename|int rename(const char *from, const char *to)|0|Save swap (player.dat <-> playerbckp.dat)
printf|int printf(const char *fmt, ...)|1|-> log. Varargs: AAPCS (double/long long 8-byte aligned in r2:r3 or stack)
fprintf|int fprintf(FILE *f, const char *fmt, ...)|0|
vfprintf|int vfprintf(FILE *f, const char *fmt, va_list ap)|0|va_list on ARM EABI = struct { void *__ap; } passed by value (one word)
snprintf|int snprintf(char *d, size_t n, const char *fmt, ...)|0|
sprintf|int sprintf(char *d, const char *fmt, ...)|0|1037 call sites
vsnprintf|int vsnprintf(char *d, size_t n, const char *fmt, va_list ap)|0|
vsprintf|int vsprintf(char *d, const char *fmt, va_list ap)|0|
sscanf|int sscanf(const char *s, const char *fmt, ...)|0|57 call sites (config/xml parsing): needs full scanf incl. %f %[..] %n
fscanf|int fscanf(FILE *f, const char *fmt, ...)|1|Lua io only
''')

add('libc-fs', 'libc.so', r'''
open|int open(const char *path, int flags, ... /* mode_t */)|1|Only OpenSSL RAND_poll(/dev/urandom) and curl. Flags are Linux ARM values (O_CREAT=0x40, O_TRUNC=0x200, O_NONBLOCK=0x800)
close|int close(int fd)|0|Mostly sockets
read|ssize_t read(int fd, void *buf, size_t n)|1|
write|ssize_t write(int fd, const void *buf, size_t n)|1|
writev|ssize_t writev(int fd, const struct iovec *iov, int cnt)|1|curl
pread|ssize_t pread(int fd, void *buf, size_t n, off_t off)|0|glf::FileStreamImpl::ReadWithOffset: fd comes from the inlined fileno() macro = *(short*)((char*)FILE + 14). The HLE FILE must expose a valid fd at offset 14 (thread-safe positioned read used by async loading)
lseek|off_t lseek(int fd, off_t off, int whence)|1|off_t is 32-bit
fsync|int fsync(int fd)|1|glf::FileStreamImpl::Close: fsync(FILE->_file @+14) then fclose. Return 0
fcntl|int fcntl(int fd, int cmd, ...)|1|Sockets (O_NONBLOCK). Return 0
ioctl|int ioctl(int fd, int req, ...)|1|Sockets (FIONBIO/SIOCGIFCONF). Return -1
fstat|int fstat(int fd, struct stat *st)|0|bionic ARM struct stat = 104 bytes: st_dev u64@0, pad[4]@8, __st_ino u32@12, st_mode u32@16, st_nlink u32@20, st_uid@24, st_gid@28, st_rdev u64@32, pad[4]@40, st_size s64@48, st_blksize u32@56, st_blocks u64@64, st_atime@72 (+nsec@76), st_mtime@80, st_ctime@88, st_ino u64@96
stat|int stat(const char *path, struct stat *st)|0|Same layout. S_IFDIR=0x4000 S_IFREG=0x8000. Used for file-exists / size checks (17 call sites)
chdir|int chdir(const char *path)|1|glf::Fs::Impl::SetCwd. Keep a virtual cwd
getcwd|char *getcwd(char *buf, size_t n)|1|glf::Fs::Impl::GetCwd. Return virtual cwd (e.g. "/")
chmod|int chmod(const char *path, mode_t mode)|1|Return 0
mkdir|int mkdir(const char *path, mode_t mode)|0|glf::Fs::MkDir (save dir creation)
unlink|int unlink(const char *path)|0|
opendir|DIR *opendir(const char *path)|0|glf::fs::DirHandle::FindFirst
readdir|struct dirent *readdir(DIR *d)|0|bionic dirent: d_ino u64@0, d_off s64@8, d_reclen u16@16, d_type u8@18, d_name[256]@19. DT_DIR=4 DT_REG=8
closedir|int closedir(DIR *d)|0|
''')

add('libc-time', 'libc.so', r'''
time|time_t time(time_t *t)|0|time_t = 32-bit long
clock|clock_t clock(void)|0|CLOCKS_PER_SEC = 1000000
clock_gettime|int clock_gettime(clockid_t id, struct timespec *ts)|0|timespec {long tv_sec; long tv_nsec} (8 bytes). CLOCK_REALTIME=0, CLOCK_MONOTONIC=1
gettimeofday|int gettimeofday(struct timeval *tv, struct timezone *tz)|0|timeval {long, long}; main frame timer
difftime|double difftime(time_t a, time_t b)|0|
gmtime|struct tm *gmtime(const time_t *t)|0|struct tm = 44 bytes (9 ints + long tm_gmtoff + const char *tm_zone); result in guest static storage
gmtime_r|struct tm *gmtime_r(const time_t *t, struct tm *out)|0|
localtime|struct tm *localtime(const time_t *t)|0|
localtime_r|struct tm *localtime_r(const time_t *t, struct tm *out)|0|
mktime|time_t mktime(struct tm *tm)|0|
strftime|size_t strftime(char *d, size_t n, const char *fmt, const struct tm *tm)|0|
strptime|char *strptime(const char *s, const char *fmt, struct tm *tm)|1|HTTP date parsing only
sleep|unsigned sleep(unsigned sec)|0|Application::SetupDevice calls sleep()
usleep|int usleep(useconds_t us)|0|glf::Thread::Sleep
sched_yield|int sched_yield(void)|0|
''')

add('libc-pthread', 'libc.so', r'''
pthread_create|int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)|0|pthread_t = long (4 bytes). 4 call sites: glf::Thread::Start, vox::DriverAndroid::_InitAT (dead on OpenSL path), glwebtools::Thread::Start, CTransportManagerImpl::StartReceiverThread
pthread_join|int pthread_join(pthread_t t, void **ret)|0|
pthread_self|pthread_t pthread_self(void)|0|Must be unique non-zero per guest thread
pthread_equal|int pthread_equal(pthread_t a, pthread_t b)|0|
pthread_once|int pthread_once(pthread_once_t *once, void (*init)(void))|0|pthread_once_t = volatile int, initialiser 0. Guest callback
pthread_attr_init|int pthread_attr_init(pthread_attr_t *a)|0|pthread_attr_t = 24 bytes {u32 flags; void *stack_base; size_t stack_size; size_t guard_size; int sched_policy; int sched_priority}
pthread_attr_destroy|int pthread_attr_destroy(pthread_attr_t *a)|1|
pthread_attr_setdetachstate|int pthread_attr_setdetachstate(pthread_attr_t *a, int state)|0|PTHREAD_CREATE_DETACHED = 1
pthread_attr_setstacksize|int pthread_attr_setstacksize(pthread_attr_t *a, size_t sz)|0|Honor (guest stack allocation)
pthread_mutex_init|int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)|0|pthread_mutex_t = { int volatile value } (4 bytes!). Do NOT store host objects inline; keep a side table keyed by guest address (also handle zero-initialised static mutexes = PTHREAD_MUTEX_INITIALIZER 0; recursive initialiser 0x4000, errorcheck 0x8000)
pthread_mutex_destroy|int pthread_mutex_destroy(pthread_mutex_t *m)|0|
pthread_mutex_lock|int pthread_mutex_lock(pthread_mutex_t *m)|0|
pthread_mutex_trylock|int pthread_mutex_trylock(pthread_mutex_t *m)|0|Returns EBUSY=16
pthread_mutex_unlock|int pthread_mutex_unlock(pthread_mutex_t *m)|0|
pthread_mutexattr_init|int pthread_mutexattr_init(pthread_mutexattr_t *a)|0|pthread_mutexattr_t = long
pthread_mutexattr_destroy|int pthread_mutexattr_destroy(pthread_mutexattr_t *a)|1|
pthread_mutexattr_settype|int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)|0|PTHREAD_MUTEX_NORMAL=0 RECURSIVE=1 ERRORCHECK=2. glf::Mutex uses RECURSIVE
pthread_cond_init|int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)|0|pthread_cond_t = { int volatile value } (4 bytes). Side table as for mutexes
pthread_cond_destroy|int pthread_cond_destroy(pthread_cond_t *c)|0|
pthread_cond_signal|int pthread_cond_signal(pthread_cond_t *c)|0|
pthread_cond_broadcast|int pthread_cond_broadcast(pthread_cond_t *c)|0|
pthread_cond_wait|int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)|0|
pthread_cond_timedwait|int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)|0|Absolute CLOCK_REALTIME; returns ETIMEDOUT=110
pthread_key_create|int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))|0|pthread_key_t = int. 3 sites: AndroidOS_GetEnv (dtor detaches thread), glf::ThreadMgr::Init, __cxa_get_globals (C++ EH per-thread state!)
pthread_key_delete|int pthread_key_delete(pthread_key_t key)|1|
pthread_getspecific|void *pthread_getspecific(pthread_key_t key)|0|79 call sites; hot (glf TLS, EH globals)
pthread_setspecific|int pthread_setspecific(pthread_key_t key, const void *v)|0|
''')

add('libc-net', 'libc.so', r'''
socket|int socket(int domain, int type, int proto)|1|Return -1 with errno=EACCES(13) -> every network path fails as "no connection"
connect|int connect(int fd, const struct sockaddr *a, socklen_t n)|1|-1
bind|int bind(int fd, const struct sockaddr *a, socklen_t n)|1|-1
listen|int listen(int fd, int backlog)|1|-1
accept|int accept(int fd, struct sockaddr *a, socklen_t *n)|1|-1
send|ssize_t send(int fd, const void *buf, size_t n, int flags)|1|-1
sendto|ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t tolen)|1|-1
recv|ssize_t recv(int fd, void *buf, size_t n, int flags)|1|-1
recvfrom|ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen)|1|-1
shutdown|int shutdown(int fd, int how)|1|0
select|int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)|1|18 sites (curl/glwebtools/CSocket). With no fds: sleep for tv and return 0. fd_set = 1024 bits (128 bytes)
poll|int poll(struct pollfd *fds, nfds_t n, int timeout)|1|curl: with n==0 sleep(timeout) and return 0
setsockopt|int setsockopt(int fd, int level, int opt, const void *val, socklen_t len)|1|-1
getsockopt|int getsockopt(int fd, int level, int opt, void *val, socklen_t *len)|1|-1
getsockname|int getsockname(int fd, struct sockaddr *a, socklen_t *n)|1|-1
getpeername|int getpeername(int fd, struct sockaddr *a, socklen_t *n)|1|-1
gethostbyname|struct hostent *gethostbyname(const char *name)|1|NULL (h_errno not imported)
gethostbyaddr|struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)|1|NULL
gethostname|int gethostname(char *name, size_t len)|1|Write "localhost", return 0
getaddrinfo|int getaddrinfo(const char *node, const char *svc, const struct addrinfo *hints, struct addrinfo **res)|1|Return EAI_NONAME (8 on bionic)
freeaddrinfo|void freeaddrinfo(struct addrinfo *ai)|1|no-op
inet_addr|in_addr_t inet_addr(const char *cp)|1|Pure function; implement
inet_ntoa|char *inet_ntoa(struct in_addr in)|1|Pure function; guest static buffer
''')

add('libm', 'libm.so', r'''
acos|double acos(double)|0|softfp: double args in r0:r1 (r2:r3), result r0:r1
acosf|float acosf(float)|0|softfp: float in r0, result r0
asin|double asin(double)|0|
asinf|float asinf(float)|0|
atan|double atan(double)|0|
atan2|double atan2(double y, double x)|0|
atan2f|float atan2f(float y, float x)|0|
atanf|float atanf(float)|0|
ceil|double ceil(double)|0|
ceilf|float ceilf(float)|0|
cos|double cos(double)|0|
cosf|float cosf(float)|0|
cosh|double cosh(double)|0|
exp|double exp(double)|0|
expf|float expf(float)|0|
floor|double floor(double)|0|
floorf|float floorf(float)|0|
fmod|double fmod(double, double)|0|
fmodf|float fmodf(float, float)|0|
frexpf|float frexpf(float, int *exp)|0|
ldexp|double ldexp(double, int)|0|double in r0:r1, int in r2
ldexpf|float ldexpf(float, int)|0|
log|double log(double)|0|
log10|double log10(double)|0|
logf|float logf(float)|0|
modff|float modff(float, float *iptr)|0|
pow|double pow(double, double)|0|
powf|float powf(float, float)|0|
sin|double sin(double)|0|
sinf|float sinf(float)|0|
sinh|double sinh(double)|0|
tan|double tan(double)|0|
tanf|float tanf(float)|0|
tanh|double tanh(double)|0|
''')

add('libstdc++', 'libstdc++.so', r'''
__cxa_guard_acquire|int __cxa_guard_acquire(__guard *g)  /* ARM EABI: 32-bit guard, bit 0 = initialised */|0|530 sites. Must be thread-safe & recursive-aware: returns 1 if caller must run the initialiser
__cxa_guard_release|void __cxa_guard_release(__guard *g)|0|Sets bit 0
__cxa_guard_abort|void __cxa_guard_abort(__guard *g)|0|
__cxa_pure_virtual|void __cxa_pure_virtual(void) __noreturn|0|6457 R_ARM_ABS32 relocations in vtables: needs a guest-callable address (a trap thunk)
''')

add('liblog', 'liblog.so', r'''
__android_log_print|int __android_log_print(int prio, const char *tag, const char *fmt, ...)|1|4 sites. Route to host log
__android_log_vprint|int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)|1|glwebtools::Console::_Print
''')

add('opensles', 'libOpenSLES.so', r'''
slCreateEngine|SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const SLEngineOption *opts, SLuint32 numItf, const SLInterfaceID *ids, const SLboolean *req)|0|1 site: vox::DriverAndroid::_InitOSL. Must write a guest pointer to a guest vtable pointer (SLObjectItf = const struct SLObjectItf_ * const *)
SL_IID_ENGINE|const SLInterfaceID SL_IID_ENGINE  /* pointer to 16-byte IID struct */|0|GLOB_DAT; guest reads the pointer value and passes it to GetInterface -> compare by pointer identity
SL_IID_BUFFERQUEUE|const SLInterfaceID SL_IID_BUFFERQUEUE|0|idem
SL_IID_PLAY|const SLInterfaceID SL_IID_PLAY|0|idem
''')

GL = r'''
glActiveTexture|void glActiveTexture(GLenum texture)
glAttachShader|void glAttachShader(GLuint program, GLuint shader)
glBindBuffer|void glBindBuffer(GLenum target, GLuint buffer)
glBindFramebuffer|void glBindFramebuffer(GLenum target, GLuint framebuffer)
glBindRenderbuffer|void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
glBindTexture|void glBindTexture(GLenum target, GLuint texture)
glBlendColor|void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
glBlendEquation|void glBlendEquation(GLenum mode)
glBlendFunc|void glBlendFunc(GLenum sfactor, GLenum dfactor)
glBufferData|void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
glBufferSubData|void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
glCheckFramebufferStatus|GLenum glCheckFramebufferStatus(GLenum target)
glClear|void glClear(GLbitfield mask)
glClearColor|void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
glClearDepthf|void glClearDepthf(GLclampf depth)
glClearStencil|void glClearStencil(GLint s)
glColorMask|void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
glCompileShader|void glCompileShader(GLuint shader)
glCompressedTexImage2D|void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data)
glCompressedTexImage3DOES|void glCompressedTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const GLvoid *data)
glCompressedTexSubImage2D|void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid *data)
glCompressedTexSubImage3DOES|void glCompressedTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const GLvoid *data)
glCopyTexSubImage2D|void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
glCreateProgram|GLuint glCreateProgram(void)
glCreateShader|GLuint glCreateShader(GLenum type)
glCullFace|void glCullFace(GLenum mode)
glDeleteBuffers|void glDeleteBuffers(GLsizei n, const GLuint *buffers)
glDeleteFramebuffers|void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
glDeleteProgram|void glDeleteProgram(GLuint program)
glDeleteRenderbuffers|void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers)
glDeleteShader|void glDeleteShader(GLuint shader)
glDeleteTextures|void glDeleteTextures(GLsizei n, const GLuint *textures)
glDepthFunc|void glDepthFunc(GLenum func)
glDepthMask|void glDepthMask(GLboolean flag)
glDepthRangef|void glDepthRangef(GLclampf zNear, GLclampf zFar)
glDisable|void glDisable(GLenum cap)
glDisableVertexAttribArray|void glDisableVertexAttribArray(GLuint index)
glDrawArrays|void glDrawArrays(GLenum mode, GLint first, GLsizei count)
glDrawElements|void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
glEnable|void glEnable(GLenum cap)
glEnableVertexAttribArray|void glEnableVertexAttribArray(GLuint index)
glFinish|void glFinish(void)
glFlush|void glFlush(void)
glFramebufferRenderbuffer|void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
glFramebufferTexture2D|void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
glFrontFace|void glFrontFace(GLenum mode)
glGenBuffers|void glGenBuffers(GLsizei n, GLuint *buffers)
glGenFramebuffers|void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
glGenRenderbuffers|void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers)
glGenTextures|void glGenTextures(GLsizei n, GLuint *textures)
glGenerateMipmap|void glGenerateMipmap(GLenum target)
glGetActiveAttrib|void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
glGetActiveUniform|void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
glGetAttribLocation|GLint glGetAttribLocation(GLuint program, const GLchar *name)
glGetError|GLenum glGetError(void)
glGetFloatv|void glGetFloatv(GLenum pname, GLfloat *params)
glGetIntegerv|void glGetIntegerv(GLenum pname, GLint *params)
glGetProgramInfoLog|void glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei *length, GLchar *infolog)
glGetProgramiv|void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
glGetShaderInfoLog|void glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *infolog)
glGetShaderSource|void glGetShaderSource(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source)
glGetShaderiv|void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
glGetString|const GLubyte *glGetString(GLenum name)
glGetUniformLocation|GLint glGetUniformLocation(GLuint program, const GLchar *name)
glLineWidth|void glLineWidth(GLfloat width)
glLinkProgram|void glLinkProgram(GLuint program)
glMapBufferOES|void *glMapBufferOES(GLenum target, GLenum access)
glPixelStorei|void glPixelStorei(GLenum pname, GLint param)
glPolygonOffset|void glPolygonOffset(GLfloat factor, GLfloat units)
glReadPixels|void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
glRenderbufferStorage|void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
glSampleCoverage|void glSampleCoverage(GLclampf value, GLboolean invert)
glScissor|void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
glShaderSource|void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
glStencilFunc|void glStencilFunc(GLenum func, GLint ref, GLuint mask)
glStencilMask|void glStencilMask(GLuint mask)
glStencilOp|void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
glTexImage2D|void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
glTexImage3DOES|void glTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
glTexParameterf|void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
glTexParameteri|void glTexParameteri(GLenum target, GLenum pname, GLint param)
glTexSubImage2D|void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
glTexSubImage3DOES|void glTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels)
glUniform1f|void glUniform1f(GLint location, GLfloat x)
glUniform1fv|void glUniform1fv(GLint location, GLsizei count, const GLfloat *v)
glUniform1i|void glUniform1i(GLint location, GLint x)
glUniform1iv|void glUniform1iv(GLint location, GLsizei count, const GLint *v)
glUniform2fv|void glUniform2fv(GLint location, GLsizei count, const GLfloat *v)
glUniform2iv|void glUniform2iv(GLint location, GLsizei count, const GLint *v)
glUniform3fv|void glUniform3fv(GLint location, GLsizei count, const GLfloat *v)
glUniform3iv|void glUniform3iv(GLint location, GLsizei count, const GLint *v)
glUniform4fv|void glUniform4fv(GLint location, GLsizei count, const GLfloat *v)
glUniform4iv|void glUniform4iv(GLint location, GLsizei count, const GLint *v)
glUniformMatrix4fv|void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
glUnmapBufferOES|GLboolean glUnmapBufferOES(GLenum target)
glUseProgram|void glUseProgram(GLuint program)
glVertexAttrib4f|void glVertexAttrib4f(GLuint indx, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
glVertexAttribPointer|void glVertexAttribPointer(GLuint indx, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *ptr)
glViewport|void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
'''
GL_NOTES = {
    'glGetString': 'Result must be a guest pointer (copy host string into guest memory once per name)',
    'glMapBufferOES': 'Returns guest pointer: needs a guest-side shadow buffer copied back on glUnmapBufferOES (GL_WRITE_ONLY_OES only)',
    'glUnmapBufferOES': 'Flush shadow buffer (see glMapBufferOES)',
    'glVertexAttribPointer': 'ptr is a guest pointer when no VBO is bound (client-side arrays): translate guest->host; data must stay valid until draw',
    'glDrawElements': 'indices is a guest pointer when no ELEMENT_ARRAY_BUFFER is bound',
    'glShaderSource': 'string is guest char** (array of guest pointers -> translate each); length may be NULL. Reached by tail call through a linker veneer from CGLSLShaderCode::createShader',
    'glCompressedTexImage2D': 'Only ETC1 (0x8D64) expected from shipped data; PVRTC only for 19 legacy textures (see ANALYSIS section 7)',
    'glCompressedTexImage3DOES': 'Reachable only for 3D textures; no 3D texture in shipped data. Stub: set GL_INVALID_OPERATION',
    'glCompressedTexSubImage3DOES': 'idem',
    'glTexImage3DOES': 'idem (GL_OES_texture_3D must not be advertised)',
    'glTexSubImage3DOES': 'idem',
    'glGetShaderSource': 'debug path',
    'glReadPixels': 'screenshot/debugger only',
    'glClearDepthf': 'float arg in r0 (softfp)',
    'glVertexAttrib4f': 'floats in r1-r3 + stack (softfp)',
    'glUniform1f': 'float in r1 (softfp)',
}
STUB_GL = {'glCompressedTexImage3DOES', 'glCompressedTexSubImage3DOES', 'glTexImage3DOES', 'glTexSubImage3DOES', 'glGetShaderSource'}
for row in GL.strip().split('\n'):
    n, sig = row.split('|')
    T.setdefault('gles2', []).append(('libGLESv2.so', '%s|%s|%d|%s' % (n, sig, 1 if n in STUB_GL else 0, GL_NOTES.get(n, ''))))


def main(xrefs_path, out_path):
    xr = {d['name']: d for d in json.load(open(xrefs_path))}
    out = []
    seen = set()
    for cat, rows in T.items():
        for lib, row in rows:
            name, sig, stub, note = (row.split('|') + ['', '', ''])[:4]
            x = xr.get(name)
            if x is None:
                print('WARNING: %s not an import of this binary' % name, file=sys.stderr)
                continue
            seen.add(name)
            out.append({
                'name': name, 'lib': lib, 'category': cat,
                'kind': 'object' if x['type'] == 'STT_OBJECT' else 'function',
                'signature': sig.strip(),
                'used': bool(x['used']),
                'calls': x['calls'],
                'data_relocs': x['abs32'] + x['glob_dat'],
                'stub_ok': bool(int(stub or 0)),
                'note': note.strip(),
            })
    missing = sorted(set(xr) - seen)
    if missing:
        print('WARNING: imports without a signature entry: %s' % missing, file=sys.stderr)
    out.sort(key=lambda d: (d['lib'], d['category'], d['name']))
    json.dump(out, open(out_path, 'w', encoding='utf-8'), indent=1)
    print('%d entries (%d missing)' % (len(out), len(missing)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
