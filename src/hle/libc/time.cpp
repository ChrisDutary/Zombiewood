// Tiempo con tipos de 32 bits de bionic: time_t/clock_t = int32, timeval/timespec = 8 bytes,
// struct tm = 44 bytes (9 int + long tm_gmtoff + const char* tm_zone). Sueno: usleep/sleep/nanosleep.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

struct GTm {
  std::int32_t sec, min, hour, mday, mon, year, wday, yday, isdst, gmtoff;
  std::uint32_t zone;
};
static_assert(sizeof(GTm) == 44);

std::int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::int64_t MonoNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

GTm FromHost(const std::tm& t, std::int32_t gmtoff, gaddr zone) {
  return GTm{t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst, gmtoff, zone};
}
std::tm ToHost(const GTm& g) {
  std::tm t{};
  t.tm_sec = g.sec;
  t.tm_min = g.min;
  t.tm_hour = g.hour;
  t.tm_mday = g.mday;
  t.tm_mon = g.mon;
  t.tm_year = g.year;
  t.tm_wday = g.wday;
  t.tm_yday = g.yday;
  t.tm_isdst = g.isdst;
  return t;
}

bool GmTime(std::int64_t t, GTm& out) {
  std::tm h{};
  const __time64_t tt = t;
  if (_gmtime64_s(&h, &tt)) return false;
  out = FromHost(h, 0, InternString("GMT"));
  return true;
}
bool LocalTime(std::int64_t t, GTm& out) {
  std::tm h{};
  const __time64_t tt = t;
  if (_localtime64_s(&h, &tt)) return false;
  std::tm copy = h;
  const std::int64_t as_utc = _mkgmtime64(&copy);
  const auto off = static_cast<std::int32_t>(as_utc - t);
  std::string zone = "GMT";
  if (off) zone = std::format("GMT{}{:02}:{:02}", off < 0 ? '-' : '+', std::abs(off) / 3600, (std::abs(off) / 60) % 60);
  out = FromHost(h, off, InternString(zone));
  return true;
}

// --- time / reloj --------------------------------------------------------------------------------
int hle_time(GPtr<std::int32_t> out) {
  const auto t = static_cast<std::int32_t>(NowUs() / 1000000);
  if (out) *out = t;
  return t;
}
int hle_gettimeofday(GPtr<std::int32_t> tv, GPtr<std::int32_t> tz) {
  const auto us = NowUs();
  if (tv) {
    tv[0] = static_cast<std::int32_t>(us / 1000000);
    tv[1] = static_cast<std::int32_t>(us % 1000000);
  }
  if (tz) {
    tz[0] = 0;
    tz[1] = 0;
  }
  return 0;
}
int hle_clock_gettime(int id, GPtr<std::int32_t> ts) {
  if (!ts) {
    SetErrno(err::FAULT);
    return -1;
  }
  std::int64_t ns;
  if (id == 0 /*REALTIME*/) ns = NowUs() * 1000;
  else if (id == 2 || id == 3) {  // PROCESS/THREAD_CPUTIME_ID: aproximacion con el monotono
    static const std::int64_t t0 = MonoNs();
    ns = MonoNs() - t0;
  } else {
    ns = MonoNs();  // MONOTONIC, MONOTONIC_RAW, BOOTTIME...
  }
  ts[0] = static_cast<std::int32_t>(ns / 1000000000);
  ts[1] = static_cast<std::int32_t>(ns % 1000000000);
  return 0;
}
int hle_clock_getres(int, GPtr<std::int32_t> ts) {
  if (ts) {
    ts[0] = 0;
    ts[1] = 1;
  }
  return 0;
}
int hle_clock() {
  static const std::int64_t t0 = MonoNs();
  return static_cast<int>((MonoNs() - t0) / 1000);  // CLOCKS_PER_SEC = 1 000 000
}
double hle_difftime(int a, int b) { return static_cast<double>(a) - static_cast<double>(b); }

// --- conversiones --------------------------------------------------------------------------------
gaddr TmBuf() { return ThreadBuffer(&ThreadState::tm_buf, sizeof(GTm)); }
gaddr hle_gmtime_r(GPtr<std::int32_t> t, gaddr out) {
  GTm g;
  if (!t || !GmTime(*t, g)) {
    SetErrno(err::INVAL);
    return 0;
  }
  mem::Write<GTm>(out, g);
  return out;
}
gaddr hle_gmtime(GPtr<std::int32_t> t) { return hle_gmtime_r(t, TmBuf()); }
gaddr hle_localtime_r(GPtr<std::int32_t> t, gaddr out) {
  GTm g;
  if (!t || !LocalTime(*t, g)) {
    SetErrno(err::INVAL);
    return 0;
  }
  mem::Write<GTm>(out, g);
  return out;
}
gaddr hle_localtime(GPtr<std::int32_t> t) { return hle_localtime_r(t, TmBuf()); }
int hle_mktime(gaddr tmp) {
  GTm g = mem::Read<GTm>(tmp);
  std::tm h = ToHost(g);
  const __time64_t r = _mktime64(&h);
  if (r == -1) return -1;
  GTm out;
  LocalTime(r, out);
  mem::Write<GTm>(tmp, out);
  return static_cast<int>(r);
}
int hle_timegm(gaddr tmp) {
  GTm g = mem::Read<GTm>(tmp);
  std::tm h = ToHost(g);
  const __time64_t r = _mkgmtime64(&h);
  if (r == -1) return -1;
  GTm out;
  GmTime(r, out);
  mem::Write<GTm>(tmp, out);
  return static_cast<int>(r);
}

const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kMonths[] = {"January", "February", "March", "April", "May", "June",
                         "July", "August", "September", "October", "November", "December"};
std::string Ab(const char* s) { return std::string(s, 3); }

int IsoWeek(const GTm& t, int& iso_year) {
  // Algoritmo ISO 8601 (como glibc).
  const int wday = (t.wday + 6) % 7;  // lunes = 0
  int year = t.year + 1900;
  int yday = t.yday;
  auto is_leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
  int week = (yday - wday + 10) / 7;
  if (week < 1) {
    --year;
    const int prev_days = is_leap(year) ? 366 : 365;
    week = (yday + prev_days - wday + 10) / 7;
  } else if (week == 53) {
    const int days = is_leap(year) ? 366 : 365;
    if (yday - wday + 3 >= days) {
      week = 1;
      ++year;
    }
  }
  iso_year = year;
  return week;
}

}  // namespace

std::string Strftime(const char* fmt, gaddr tmp) {
  const GTm t = mem::Read<GTm>(tmp);
  std::string out;
  auto day = [&](bool full) {
    const int w = (t.wday % 7 + 7) % 7;
    return full ? std::string(kDays[w]) : Ab(kDays[w]);
  };
  auto mon = [&](bool full) {
    const int m = (t.mon % 12 + 12) % 12;
    return full ? std::string(kMonths[m]) : Ab(kMonths[m]);
  };
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      out += *p;
      continue;
    }
    ++p;
    while (*p == 'E' || *p == 'O' || *p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') ++p;
    if (!*p) break;
    switch (*p) {
      case 'a': out += day(false); break;
      case 'A': out += day(true); break;
      case 'b': case 'h': out += mon(false); break;
      case 'B': out += mon(true); break;
      case 'c': out += std::format("{} {} {:2} {:02}:{:02}:{:02} {}", day(false), mon(false), t.mday, t.hour, t.min, t.sec, t.year + 1900); break;
      case 'C': out += std::format("{:02}", (t.year + 1900) / 100); break;
      case 'd': out += std::format("{:02}", t.mday); break;
      case 'D': out += std::format("{:02}/{:02}/{:02}", t.mon + 1, t.mday, (t.year + 1900) % 100); break;
      case 'e': out += std::format("{:2}", t.mday); break;
      case 'F': out += std::format("{}-{:02}-{:02}", t.year + 1900, t.mon + 1, t.mday); break;
      case 'g': case 'G': case 'V': {
        int iy = 0;
        const int wk = IsoWeek(t, iy);
        if (*p == 'V') out += std::format("{:02}", wk);
        else if (*p == 'G') out += std::to_string(iy);
        else out += std::format("{:02}", iy % 100);
        break;
      }
      case 'H': out += std::format("{:02}", t.hour); break;
      case 'I': out += std::format("{:02}", t.hour % 12 ? t.hour % 12 : 12); break;
      case 'j': out += std::format("{:03}", t.yday + 1); break;
      case 'k': out += std::format("{:2}", t.hour); break;
      case 'l': out += std::format("{:2}", t.hour % 12 ? t.hour % 12 : 12); break;
      case 'm': out += std::format("{:02}", t.mon + 1); break;
      case 'M': out += std::format("{:02}", t.min); break;
      case 'n': out += '\n'; break;
      case 'p': out += t.hour < 12 ? "AM" : "PM"; break;
      case 'P': out += t.hour < 12 ? "am" : "pm"; break;
      case 'r': out += std::format("{:02}:{:02}:{:02} {}", t.hour % 12 ? t.hour % 12 : 12, t.min, t.sec, t.hour < 12 ? "AM" : "PM"); break;
      case 'R': out += std::format("{:02}:{:02}", t.hour, t.min); break;
      case 's': {
        std::tm h = ToHost(t);
        out += std::to_string(static_cast<long long>(_mktime64(&h)));
        break;
      }
      case 'S': out += std::format("{:02}", t.sec); break;
      case 't': out += '\t'; break;
      case 'T': out += std::format("{:02}:{:02}:{:02}", t.hour, t.min, t.sec); break;
      case 'u': out += std::to_string(t.wday == 0 ? 7 : t.wday); break;
      case 'U': out += std::format("{:02}", (t.yday + 7 - t.wday) / 7); break;
      case 'w': out += std::to_string(t.wday); break;
      case 'W': out += std::format("{:02}", (t.yday + 7 - (t.wday + 6) % 7) / 7); break;
      case 'x': out += std::format("{:02}/{:02}/{:02}", t.mon + 1, t.mday, (t.year + 1900) % 100); break;
      case 'X': out += std::format("{:02}:{:02}:{:02}", t.hour, t.min, t.sec); break;
      case 'y': out += std::format("{:02}", (t.year + 1900) % 100); break;
      case 'Y': out += std::to_string(t.year + 1900); break;
      case 'z': {
        const int o = t.gmtoff;
        out += std::format("{}{:02}{:02}", o < 0 ? '-' : '+', std::abs(o) / 3600, (std::abs(o) / 60) % 60);
        break;
      }
      case 'Z': out += t.zone && GuestMemory::IsMapped(t.zone) ? mem::ReadCStr(t.zone, 64) : "GMT"; break;
      case '%': out += '%'; break;
      default: out += '%'; out += *p; break;
    }
  }
  return out;
}

namespace {

gsize hle_strftime(GPtr<char> dst, gsize n, const char* fmt, gaddr tmp) {
  const std::string s = Strftime(fmt, tmp);
  if (s.size() + 1 > n) return 0;
  std::memcpy(dst.get(), s.c_str(), s.size() + 1);
  return static_cast<gsize>(s.size());
}

// strptime: subconjunto usado para fechas HTTP (%a, %d %b %Y %H:%M:%S GMT, etc.).
const char* MatchName(const char* s, const char* const* names, int count, int& idx) {
  for (int i = 0; i < count; ++i) {
    const std::size_t full = std::strlen(names[i]);
    if (!_strnicmp(s, names[i], full)) {
      idx = i;
      return s + full;
    }
    if (!_strnicmp(s, names[i], 3)) {
      idx = i;
      return s + 3;
    }
  }
  return nullptr;
}
const char* Num(const char* s, int maxd, int& v) {
  if (*s < '0' || *s > '9') return nullptr;
  v = 0;
  for (int i = 0; i < maxd && *s >= '0' && *s <= '9'; ++i) v = v * 10 + (*s++ - '0');
  return s;
}
gaddr hle_strptime(GPtr<char> sp, const char* fmt, gaddr tmp) {
  GTm t = mem::Read<GTm>(tmp);
  const char* s = sp.get();
  for (const char* f = fmt; *f && s; ++f) {
    if (*f == ' ' || (*f >= '\t' && *f <= '\r')) {
      while (*s == ' ' || (*s >= '\t' && *s <= '\r')) ++s;
      continue;
    }
    if (*f != '%') {
      if (*s != *f) return 0;
      ++s;
      continue;
    }
    ++f;
    while (*f == 'E' || *f == 'O') ++f;
    int v = 0;
    switch (*f) {
      case 'a': case 'A': s = MatchName(s, kDays, 7, t.wday); break;
      case 'b': case 'B': case 'h': s = MatchName(s, kMonths, 12, t.mon); break;
      case 'd': case 'e': while (*s == ' ') ++s; s = Num(s, 2, t.mday); break;
      case 'H': case 'k': while (*s == ' ') ++s; s = Num(s, 2, t.hour); break;
      case 'I': case 'l': while (*s == ' ') ++s; s = Num(s, 2, t.hour); break;
      case 'M': s = Num(s, 2, t.min); break;
      case 'S': s = Num(s, 2, t.sec); break;
      case 'm': if ((s = Num(s, 2, v))) t.mon = v - 1; break;
      case 'j': if ((s = Num(s, 3, v))) t.yday = v - 1; break;
      case 'y': if ((s = Num(s, 2, v))) t.year = v < 69 ? v + 100 : v; break;
      case 'Y': if ((s = Num(s, 4, v))) t.year = v - 1900; break;
      case 'p': if (!_strnicmp(s, "PM", 2)) { if (t.hour < 12) t.hour += 12; s += 2; } else if (!_strnicmp(s, "AM", 2)) { if (t.hour == 12) t.hour = 0; s += 2; } else s = nullptr; break;
      case 'Z': while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) ++s; break;
      case 'z': if (*s == '+' || *s == '-') ++s; while (*s >= '0' && *s <= '9') ++s; break;
      case 'T': if ((s = Num(s, 2, t.hour)) && *s == ':' && (s = Num(s + 1, 2, t.min)) && *s == ':') s = Num(s + 1, 2, t.sec); else s = nullptr; break;
      case 'n': case 't': while (*s == ' ' || (*s >= '\t' && *s <= '\r')) ++s; break;
      case '%': if (*s == '%') ++s; else s = nullptr; break;
      default: s = nullptr; break;
    }
  }
  if (!s) return 0;
  mem::Write<GTm>(tmp, t);
  return h2g(s);
}

gaddr hle_asctime_r(gaddr tmp, GPtr<char> buf) {
  const std::string s = Strftime("%a %b %e %H:%M:%S %Y\n", tmp);
  mem::WriteCStr(buf.addr, s, 26);
  return buf.addr;
}
gaddr hle_asctime(gaddr tmp) { return hle_asctime_r(tmp, GPtr<char>(ThreadBuffer(&ThreadState::text_buf, 256))); }
gaddr hle_ctime(GPtr<std::int32_t> t) {
  const gaddr tm = hle_localtime(t);
  return tm ? hle_asctime(tm) : 0;
}
void hle_tzset() {}

// --- sueno ------------------------------------------------------------------------------------
int hle_usleep(std::uint32_t us) {
  if (us) std::this_thread::sleep_for(std::chrono::microseconds(us));
  else std::this_thread::yield();
  return 0;
}
std::uint32_t hle_sleep(std::uint32_t s) {
  std::this_thread::sleep_for(std::chrono::seconds(s));
  return 0;
}
int hle_nanosleep(GPtr<std::int32_t> req, GPtr<std::int32_t> rem) {
  if (!req || req[1] < 0 || req[1] >= 1000000000) {
    SetErrno(err::INVAL);
    return -1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(req[0]) + std::chrono::nanoseconds(req[1]));
  if (rem) {
    rem[0] = 0;
    rem[1] = 0;
  }
  return 0;
}
int hle_sched_yield() {
  std::this_thread::yield();
  return 0;
}

}  // namespace

void RegisterTime() {
  ZW_HLE_FAST("time", hle_time);
  ZW_HLE_FAST("gettimeofday", hle_gettimeofday);
  ZW_HLE_FAST("clock_gettime", hle_clock_gettime);
  ZW_HLE_FAST("clock_getres", hle_clock_getres);
  ZW_HLE_FAST("clock", hle_clock);
  ZW_HLE_FAST("difftime", hle_difftime);
  ZW_HLE("gmtime", hle_gmtime);
  ZW_HLE("gmtime_r", hle_gmtime_r);
  ZW_HLE("localtime", hle_localtime);
  ZW_HLE("localtime_r", hle_localtime_r);
  ZW_HLE("mktime", hle_mktime);
  ZW_HLE("timegm", hle_timegm);
  ZW_HLE("timelocal", hle_mktime);
  ZW_HLE("strftime", hle_strftime);
  ZW_HLE("strptime", hle_strptime);
  ZW_HLE("asctime", hle_asctime);
  ZW_HLE("asctime_r", hle_asctime_r);
  ZW_HLE("ctime", hle_ctime);
  ZW_HLE("tzset", hle_tzset);
  ZW_HLE("usleep", hle_usleep);
  ZW_HLE("sleep", hle_sleep);
  ZW_HLE("nanosleep", hle_nanosleep);
  ZW_HLE("sched_yield", hle_sched_yield);
}

}  // namespace zw::hle::libc
