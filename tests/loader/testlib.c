// Libreria guest de prueba para tests/loader (T3). gen_blobs.py la compila dos veces:
//   libzwtest.so  (-DZW_LIB_A): exporta funciones/datos e importa de la HLE.
//   libzwtest2.so (-DZW_LIB_B): importa de libzwtest.so (resolucion cruzada).
#define EXPORT __attribute__((visibility("default"), noinline, used))

#ifdef ZW_LIB_A
extern int host_add(int, int);                 // import de funcion (JUMP_SLOT)
extern double host_scale(double, float);       // softfp
extern int host_var(const char*, ...);         // variadica
extern int host_data;                          // import de dato (GLOB_DAT)
extern void* malloc(unsigned);

EXPORT int exported_value = 1234;              // dato exportado
static int ctor_count;
static int (*fn_table[2])(int, int) = {host_add, host_add};  // ABS32 contra import
static const char* msg = "hola desde libzwtest";             // RELATIVE

__attribute__((constructor)) static void ctor_a(void) { ctor_count += 1; }
__attribute__((constructor)) static void ctor_b(void) { ctor_count += 10; exported_value += host_data; }

EXPORT int get_ctor_count(void) { return ctor_count; }
EXPORT int call_add(int a, int b) { return host_add(a, b) + fn_table[1](a, b); }
EXPORT double call_scale(double a, float b) { return host_scale(a, b); }
EXPORT int call_var(int x) { return host_var(msg, x, 1.5, (long long)x << 33); }
EXPORT const char* get_msg(void) { return msg; }
EXPORT int read_host_data(void) { return host_data; }
EXPORT int* alloc_int(int v) { int* p = (int*)malloc(4); *p = v; return p; }
__attribute__((target("arm"))) EXPORT int arm_mul(int a, int b) { return a * b; }
EXPORT int JNI_OnLoad(void* vm, void* reserved) { return vm && !reserved ? 0x10004 : -1; }
#endif

#ifdef ZW_LIB_B
extern int call_add(int, int);     // definido en libzwtest.so
extern int arm_mul(int, int);
extern int exported_value;
EXPORT int cross_call(int a, int b) { return call_add(a, b) + arm_mul(a, b) + exported_value; }
#endif
