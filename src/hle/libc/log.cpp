// liblog: __android_log_* -> log del runtime.
#include "core/log.h"
#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

void Emit(int prio, const char* tag, std::string msg) {
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  static const char kP[] = "??VDIWEFS";
  const char p = prio >= 0 && prio < 9 ? kP[prio] : '?';
  const std::string line = std::format("[logcat {}/{}] {}", p, tag ? tag : "", msg);
  if (prio >= 6) log::Error("{}", line);
  else if (prio == 5) log::Warn("{}", line);
  else if (prio <= 3) log::Debug("{}", line);
  else log::Info("{}", line);
}
int hle_log_print(int prio, const char* tag, const char* fmt, Varargs va) {
  Emit(prio, tag, FormatPrintf(fmt, va));
  return 1;
}
int hle_log_vprint(int prio, const char* tag, const char* fmt, VaList ap) {
  Emit(prio, tag, FormatPrintf(fmt, ap));
  return 1;
}
int hle_log_write(int prio, const char* tag, const char* text) {
  Emit(prio, tag, text ? text : "");
  return 1;
}
int hle_log_buf_print(int, int prio, const char* tag, const char* fmt, Varargs va) {
  Emit(prio, tag, FormatPrintf(fmt, va));
  return 1;
}
void hle_log_assert(Cpu& c, const char* cond, const char* tag, const char* fmt, Varargs va) {
  const std::string m = fmt ? FormatPrintf(fmt, va) : std::string(cond ? cond : "");
  Emit(7, tag, m);
  throw GuestFault("__android_log_assert: " + m + "\n" + debug::DumpState(c));
}

}  // namespace

void RegisterLog() {
  ZW_HLE("__android_log_print", hle_log_print);
  ZW_HLE("__android_log_vprint", hle_log_vprint);
  ZW_HLE("__android_log_write", hle_log_write);
  ZW_HLE("__android_log_buf_print", hle_log_buf_print);
  ZW_HLE("__android_log_assert", hle_log_assert);
}

}  // namespace zw::hle::libc
