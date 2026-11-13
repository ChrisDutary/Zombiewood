// libm (softfp: float en r0, double en r0:r1). Todo FAST.
#include <cmath>
#include <cstring>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

#define ZW_M1(n)                                    \
  double m_##n(double x) { return std::n(x); }      \
  float m_##n##f(float x) { return std::n(x); }
#define ZW_M2(n)                                            \
  double m_##n(double x, double y) { return std::n(x, y); } \
  float m_##n##f(float x, float y) { return std::n(x, y); }
ZW_M1(sin) ZW_M1(cos) ZW_M1(tan) ZW_M1(asin) ZW_M1(acos) ZW_M1(atan) ZW_M1(sinh) ZW_M1(cosh) ZW_M1(tanh)
ZW_M1(asinh) ZW_M1(acosh) ZW_M1(atanh) ZW_M1(exp) ZW_M1(exp2) ZW_M1(expm1) ZW_M1(log) ZW_M1(log10) ZW_M1(log2)
ZW_M1(log1p) ZW_M1(logb) ZW_M1(sqrt) ZW_M1(cbrt) ZW_M1(fabs) ZW_M1(floor) ZW_M1(ceil) ZW_M1(round) ZW_M1(trunc)
ZW_M1(rint) ZW_M1(nearbyint) ZW_M1(erf) ZW_M1(erfc) ZW_M1(tgamma) ZW_M1(lgamma)
ZW_M2(pow) ZW_M2(atan2) ZW_M2(fmod) ZW_M2(remainder) ZW_M2(hypot) ZW_M2(fmin) ZW_M2(fmax) ZW_M2(fdim)
ZW_M2(copysign) ZW_M2(nextafter)
#undef ZW_M1
#undef ZW_M2

double m_fma(double a, double b, double c) { return std::fma(a, b, c); }
float m_fmaf(float a, float b, float c) { return std::fma(a, b, c); }
double m_ldexp(double x, int e) { return std::ldexp(x, e); }
float m_ldexpf(float x, int e) { return std::ldexp(x, e); }
double m_frexp(double x, GPtr<std::int32_t> e) {
  int v = 0;
  const double r = std::frexp(x, &v);
  if (e) mem::Write<std::int32_t>(e.addr, v);
  return r;
}
float m_frexpf(float x, GPtr<std::int32_t> e) {
  int v = 0;
  const float r = std::frexp(x, &v);
  if (e) mem::Write<std::int32_t>(e.addr, v);
  return r;
}
double m_modf(double x, GPtr<double> ip) {
  double i = 0;
  const double r = std::modf(x, &i);
  if (ip) mem::Write<double>(ip.addr, i);
  return r;
}
float m_modff(float x, GPtr<float> ip) {
  float i = 0;
  const float r = std::modf(x, &i);
  if (ip) mem::Write<float>(ip.addr, i);
  return r;
}
int m_ilogb(double x) { return std::ilogb(x); }
int m_ilogbf(float x) { return std::ilogb(x); }
int m_lround(double x) { return static_cast<int>(std::lround(x)); }
int m_lroundf(float x) { return static_cast<int>(std::lround(x)); }
int m_lrint(double x) { return static_cast<int>(std::lrint(x)); }
int m_lrintf(float x) { return static_cast<int>(std::lrint(x)); }
gi64 m_llround(double x) { return gi64(std::llround(x)); }
gi64 m_llroundf(float x) { return gi64(std::llround(x)); }
gi64 m_llrint(double x) { return gi64(std::llrint(x)); }
gi64 m_llrintf(float x) { return gi64(std::llrint(x)); }
double m_scalbn(double x, int n) { return std::scalbn(x, n); }
float m_scalbnf(float x, int n) { return std::scalbn(x, n); }
void m_sincos(double x, GPtr<double> s, GPtr<double> c) {
  mem::Write<double>(s.addr, std::sin(x));
  mem::Write<double>(c.addr, std::cos(x));
}
void m_sincosf(float x, GPtr<float> s, GPtr<float> c) {
  mem::Write<float>(s.addr, std::sin(x));
  mem::Write<float>(c.addr, std::cos(x));
}
double m_remquo(double x, double y, GPtr<std::int32_t> q) {
  int v = 0;
  const double r = std::remquo(x, y, &v);
  if (q) mem::Write<std::int32_t>(q.addr, v);
  return r;
}
double m_nan(const char*) { return std::nan(""); }
float m_nanf(const char*) { return std::nanf(""); }
// Clasificacion (bionic: __isnan, __isinf, __isfinite, __fpclassifyd...). FP_* de bionic:
// FP_INFINITE 0x01, FP_NAN 0x02, FP_NORMAL 0x04, FP_SUBNORMAL 0x08, FP_ZERO 0x10.
int Classify(double x) {
  switch (std::fpclassify(x)) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;
  }
}
int m_isnan(double x) { return std::isnan(x) ? 1 : 0; }
int m_isnanf(float x) { return std::isnan(x) ? 1 : 0; }
int m_isinf(double x) { return std::isinf(x) ? (x < 0 ? -1 : 1) : 0; }
int m_isinff(float x) { return std::isinf(x) ? (x < 0 ? -1 : 1) : 0; }
int m_isfinite(double x) { return std::isfinite(x) ? 1 : 0; }
int m_isfinitef(float x) { return std::isfinite(x) ? 1 : 0; }
int m_signbit(double x) { return std::signbit(x) ? 1 : 0; }
int m_signbitf(float x) { return std::signbit(x) ? 1 : 0; }
int m_fpclassify(double x) { return Classify(x); }
int m_fpclassifyf(float x) { return Classify(x); }

}  // namespace

void RegisterMath() {
#define REG(n) ZW_HLE_FAST(#n, m_##n); ZW_HLE_FAST(#n "f", m_##n##f); ZW_HLE_FAST(#n "l", m_##n);
  REG(sin) REG(cos) REG(tan) REG(asin) REG(acos) REG(atan) REG(sinh) REG(cosh) REG(tanh) REG(asinh) REG(acosh)
  REG(atanh) REG(exp) REG(exp2) REG(expm1) REG(log) REG(log10) REG(log2) REG(log1p) REG(logb) REG(sqrt) REG(cbrt)
  REG(fabs) REG(floor) REG(ceil) REG(round) REG(trunc) REG(rint) REG(nearbyint) REG(erf) REG(erfc) REG(tgamma)
  REG(lgamma) REG(pow) REG(atan2) REG(fmod) REG(remainder) REG(hypot) REG(fmin) REG(fmax) REG(fdim) REG(copysign)
  REG(nextafter) REG(fma) REG(ldexp) REG(frexp) REG(modf) REG(ilogb) REG(lround) REG(lrint) REG(llround) REG(llrint)
  REG(scalbn) REG(sincos) REG(nan)
#undef REG
  ZW_HLE_FAST("gamma", m_lgamma);
  ZW_HLE_FAST("drem", m_remainder);
  ZW_HLE_FAST("remquo", m_remquo);
  ZW_HLE_FAST("scalbln", m_scalbn);
  ZW_HLE_FAST("scalblnf", m_scalbnf);
  ZW_HLE_FAST("pow10", m_exp2);  // no se usa; mantiene el nombre resuelto
  ZW_HLE_FAST("isnan", m_isnan);
  ZW_HLE_FAST("isnanf", m_isnanf);
  ZW_HLE_FAST("__isnan", m_isnan);
  ZW_HLE_FAST("__isnanf", m_isnanf);
  ZW_HLE_FAST("isinf", m_isinf);
  ZW_HLE_FAST("isinff", m_isinff);
  ZW_HLE_FAST("__isinf", m_isinf);
  ZW_HLE_FAST("__isinff", m_isinff);
  ZW_HLE_FAST("finite", m_isfinite);
  ZW_HLE_FAST("finitef", m_isfinitef);
  ZW_HLE_FAST("__isfinite", m_isfinite);
  ZW_HLE_FAST("__isfinitef", m_isfinitef);
  ZW_HLE_FAST("__signbit", m_signbit);
  ZW_HLE_FAST("__signbitf", m_signbitf);
  ZW_HLE_FAST("__fpclassifyd", m_fpclassify);
  ZW_HLE_FAST("__fpclassifyf", m_fpclassifyf);
  ZW_HLE_FAST("__fpclassify", m_fpclassify);
}

}  // namespace zw::hle::libc
