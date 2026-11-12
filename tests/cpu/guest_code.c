// Codigo guest de prueba para tests/cpu (T3). Se compila con el clang de VS a ARMv7 Thumb-2
// softfp (ver gen_blobs.py) y se incrusta como bytes en guest_blob.inc. Sin relocaciones:
// todo lo externo llega por parametros.
typedef int (*fn_mixed)(int, float, double, long long, int, float, double);
typedef int (*fn_var)(const char*, ...);
typedef double (*fn_dd)(double, double);
typedef float (*fn_ff)(float);
typedef long long (*fn_ll)(long long, int);
typedef int (*fn_cb)(int (*)(int, int), int);
typedef int (*fn_i)(int);

#define EXPORT __attribute__((noinline, used))

// int/float/double/long long mezclados: ejercita pares alineados y la pila.
EXPORT int t_mixed(fn_mixed f) { return f(7, 2.5f, 3.25, 0x1122334455667788LL, 9, -1.5f, 1e10); }

// variadica: int, double, long long, puntero, char promocionado.
EXPORT int t_variadic(fn_var f, const char* fmt, const char* s) { return f(fmt, 42, 2.75, 0x0102030405060708LL, s, (char)'z'); }

EXPORT double t_ret_double(fn_dd f, double a, double b) { return f(a, b) * 2.0; }
EXPORT float t_ret_float(fn_ff f, float a) { return f(a) + 1.0f; }
EXPORT long long t_ret_ll(fn_ll f, long long a) { return f(a, 3) + 1; }

// host -> guest -> host -> guest -> host
static int guest_twice(int a, int b) { return a * 2 + b; }
EXPORT int t_callback(fn_cb host, int x) { return host(guest_twice, x) + 1; }

// Funcion en modo ARM (interworking Thumb <-> ARM).
__attribute__((target("arm"))) EXPORT int t_arm_add(int a, int b, fn_i f) { return f(a) + b; }
typedef int (*fn_arm)(int, int, fn_i);
EXPORT int t_interwork(int a, int b, fn_i f, fn_arm arm_fn) { return arm_fn(a, b, f) * 3; }

// Argumentos host -> guest por pila (softfp).
EXPORT double t_sum8(int a, double b, float c, long long d, int e, double f) { return a + b + c + (int)(d >> 32) + (int)d + e + f; }

// LDREX/STREX: contador atomico compartido por dos hilos.
EXPORT int t_atomic_loop(int* counter, int n) {
  for (int i = 0; i < n; ++i) __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
  return n;
}
// Spinlock con compare-exchange + contador no atomico protegido por el.
EXPORT int t_spin_loop(int* lock, int* counter, int n) {
  for (int i = 0; i < n; ++i) {
    int exp;
    do { exp = 0; } while (!__atomic_compare_exchange_n(lock, &exp, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    *counter = *counter + 1;
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
  }
  return n;
}

// VFP: producto escalar (vmul/vadd/vcvt) y division.
EXPORT float t_dot(const float* a, const float* b, int n) {
  float s = 0;
  for (int i = 0; i < n; ++i) s += a[i] * b[i];
  return s / (float)n;
}

// setjmp/longjmp a traves de un callback host.
typedef int (*fn_setjmp)(int*);
typedef void (*fn_longjmp)(int*, int);
typedef int (*fn_call2)(void (*)(int*, fn_longjmp), int*, fn_longjmp);
static void thrower(int* buf, fn_longjmp lj) { lj(buf, 77); }
EXPORT int t_setjmp(fn_setjmp sj, fn_longjmp lj, fn_call2 host_call, int* buf) {
  volatile int stage = 1;
  int r = sj(buf);
  if (r == 0) {
    stage = 2;
    host_call(thrower, buf, lj);  // host -> guest thrower -> longjmp (cruza call_guest)
    return -1;                    // no se alcanza
  }
  return r + stage * 1000;        // 77 + 2000
}

// Fallo deliberado: escritura en NULL.
EXPORT int t_null_write(int v) { *(volatile int*)8 = v; return 1; }

// TLS por CP15 (TPIDRURO).
__attribute__((target("arm"))) EXPORT unsigned t_read_tls(void) {
  unsigned v;
  __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
