// Sockets / DNS: sin red. Todo falla limpio para que el juego vaya a modo offline.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

int Fail(int e) {
  SetErrno(e);
  return -1;
}
int hle_socket(int, int, int) { return Fail(err::ACCES); }
int hle_connect(int, gaddr, int) { return Fail(err::NETUNREACH); }
int hle_bind(int, gaddr, int) { return Fail(err::BADF); }
int hle_listen(int, int) { return Fail(err::BADF); }
int hle_accept(int, gaddr, gaddr) { return Fail(err::BADF); }
int hle_send(int, gaddr, gsize, int) { return Fail(err::BADF); }
int hle_sendto(int, gaddr, gsize, int, gaddr, int) { return Fail(err::BADF); }
int hle_recv(int, gaddr, gsize, int) { return Fail(err::BADF); }
int hle_recvfrom(int, gaddr, gsize, int, gaddr, gaddr) { return Fail(err::BADF); }
int hle_sockopt(int, int, int, gaddr, gaddr) { return Fail(err::BADF); }
int hle_sockname(int, gaddr, gaddr) { return Fail(err::BADF); }
int hle_shutdown(int, int) { return 0; }
int hle_getaddrinfo(const char*, const char*, gaddr, GPtr<std::uint32_t> res) {
  if (res) *res = 0;
  return 8;  // EAI_NONAME de bionic
}
void hle_freeaddrinfo(gaddr) {}
gaddr hle_gai_strerror(int) { return InternString("Name or service not known"); }
gaddr hle_gethostbyname(const char*) { return 0; }
gaddr hle_gethostbyaddr(gaddr, int, int) { return 0; }
int hle_gethostname(GPtr<char> name, gsize len) {
  mem::WriteCStr(name.addr, "localhost", len);
  return 0;
}
std::uint32_t hle_inet_addr(const char* cp) {
  unsigned a, b, c, d;
  char tail;
  if (!cp || std::sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0xFFFFFFFFu;  // INADDR_NONE
  return a | (b << 8) | (c << 16) | (d << 24);  // orden de red en memoria little-endian
}
int hle_inet_aton(const char* cp, GPtr<std::uint32_t> out) {
  const std::uint32_t v = hle_inet_addr(cp);
  if (v == 0xFFFFFFFFu && (!cp || std::strcmp(cp, "255.255.255.255"))) return 0;
  if (out) *out = v;
  return 1;
}
gaddr hle_inet_ntoa(std::uint32_t in) {
  const gaddr buf = ThreadBuffer(&ThreadState::text_buf, 256);
  mem::WriteCStr(buf, std::format("{}.{}.{}.{}", in & 0xFF, (in >> 8) & 0xFF, (in >> 16) & 0xFF, in >> 24), 256);
  return buf;
}
gaddr hle_inet_ntop(int af, GPtr<std::uint8_t> src, GPtr<char> dst, gsize n) {
  if (af != 2) return 0;
  mem::WriteCStr(dst.addr, std::format("{}.{}.{}.{}", src[0], src[1], src[2], src[3]), n);
  return dst.addr;
}
int hle_inet_pton(int af, const char* s, GPtr<std::uint32_t> dst) { return af == 2 ? hle_inet_aton(s, dst) : -1; }
std::uint32_t hle_htonl(std::uint32_t v) { return _byteswap_ulong(v); }
std::uint32_t hle_htons(std::uint32_t v) { return _byteswap_ushort(static_cast<std::uint16_t>(v)); }
// select/poll sin sockets: solo sirven de temporizador.
void SleepMs(std::int64_t ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(ms, 5000)));
}
int hle_select(int n, gaddr r, gaddr w, gaddr e, GPtr<std::int32_t> tv) {
  bool any = false;
  for (gaddr set : {r, w, e})
    if (set)
      for (int i = 0; i < (n + 31) / 32 && i < 32; ++i) {
        if (mem::R32(set + 4 * i)) any = true;
        mem::W32(set + 4 * i, 0);
      }
  if (!tv) {
    if (any) return Fail(err::BADF);
    SleepMs(1000);
    return 0;
  }
  SleepMs(static_cast<std::int64_t>(tv[0]) * 1000 + tv[1] / 1000);
  return 0;
}
int hle_poll(gaddr fds, std::uint32_t n, int timeout) {
  for (std::uint32_t i = 0; i < n; ++i) mem::W16(fds + 8 * i + 6, 0x20);  // POLLNVAL
  if (n) return static_cast<int>(n);
  SleepMs(timeout < 0 ? 1000 : timeout);
  return 0;
}
int hle_socketpair(int, int, int, gaddr) { return Fail(err::OPNOTSUPP); }
int hle_pipe(gaddr) { return Fail(err::NFILE); }

}  // namespace

void RegisterNet() {
  ZW_HLE("socket", hle_socket);
  ZW_HLE("connect", hle_connect);
  ZW_HLE("bind", hle_bind);
  ZW_HLE("listen", hle_listen);
  ZW_HLE("accept", hle_accept);
  ZW_HLE("send", hle_send);
  ZW_HLE("sendto", hle_sendto);
  ZW_HLE("recv", hle_recv);
  ZW_HLE("recvfrom", hle_recvfrom);
  ZW_HLE("setsockopt", hle_sockopt);
  ZW_HLE("getsockopt", hle_sockopt);
  ZW_HLE("getsockname", hle_sockname);
  ZW_HLE("getpeername", hle_sockname);
  ZW_HLE("shutdown", hle_shutdown);
  ZW_HLE("getaddrinfo", hle_getaddrinfo);
  ZW_HLE("freeaddrinfo", hle_freeaddrinfo);
  ZW_HLE("gai_strerror", hle_gai_strerror);
  ZW_HLE("gethostbyname", hle_gethostbyname);
  ZW_HLE("gethostbyaddr", hle_gethostbyaddr);
  ZW_HLE("gethostname", hle_gethostname);
  ZW_HLE_FAST("inet_addr", hle_inet_addr);
  ZW_HLE_FAST("inet_aton", hle_inet_aton);
  ZW_HLE("inet_ntoa", hle_inet_ntoa);
  ZW_HLE_FAST("inet_ntop", hle_inet_ntop);
  ZW_HLE_FAST("inet_pton", hle_inet_pton);
  ZW_HLE_FAST("htonl", hle_htonl);
  ZW_HLE_FAST("ntohl", hle_htonl);
  ZW_HLE_FAST("htons", hle_htons);
  ZW_HLE_FAST("ntohs", hle_htons);
  ZW_HLE("select", hle_select);
  ZW_HLE("poll", hle_poll);
  ZW_HLE("socketpair", hle_socketpair);
  ZW_HLE("pipe", hle_pipe);
}

}  // namespace zw::hle::libc
