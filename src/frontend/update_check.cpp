#include "frontend/update_check.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace zw::update {
namespace {

std::mutex g_mu;
std::string g_tag, g_url;
bool g_newer = false;

// "v0.2.10" -> {0,2,10}
std::array<int, 3> Parse(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  std::array<int, 3> r{0, 0, 0};
  std::sscanf(v.c_str(), "%d.%d.%d", &r[0], &r[1], &r[2]);
  return r;
}

void Publish(const std::string& tag, const std::string& url) {
  const bool newer = !tag.empty() && Parse(tag) > Parse(ZW_VERSION);
  std::lock_guard lk(g_mu);
  g_tag = tag;
  g_url = url;
  g_newer = newer;
  if (newer) log::Info("actualizacion: hay una version nueva {} ({})", tag, url);
}

#ifdef _WIN32
std::string HttpGet(const wchar_t* host, const wchar_t* path) {
  std::string body;
  const std::wstring ua = L"Zombiewood-port/" + std::wstring(ZW_VERSION, ZW_VERSION + std::strlen(ZW_VERSION));
  HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return body;
  WinHttpSetTimeouts(s, 5000, 5000, 5000, 8000);
  if (HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
    if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)) {
      const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
      if (WinHttpSendRequest(r, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          DWORD avail = 0;
          while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
            if (body.size() > (4u << 20)) break;  // tope de seguridad
          }
        } else {
          log::Info("actualizacion: GitHub respondio {}", status);
        }
      }
      WinHttpCloseHandle(r);
    }
    WinHttpCloseHandle(c);
  }
  WinHttpCloseHandle(s);
  return body;
}
#else
std::string HttpGet(const wchar_t*, const wchar_t*) { return {}; }  // Linux: pendiente (sin aviso)
#endif

void Run(std::filesystem::path state_file) {
  using namespace std::chrono;
  const long long now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  nlohmann::json st;
  if (std::ifstream f(state_file); f) st = nlohmann::json::parse(f, nullptr, false);
  if (st.is_object() && now - st.value("last_check", 0LL) < 24 * 3600) {  // cache de un dia
    Publish(st.value("tag", std::string()), st.value("url", std::string()));
    return;
  }
  const std::string body = HttpGet(L"api.github.com", L"/repos/ChrisDutary/Zombiewood/releases?per_page=10");
  if (body.empty()) return;
  const auto rel = nlohmann::json::parse(body, nullptr, false);
  if (!rel.is_array()) return;
  std::string tag, url;
  for (const auto& r : rel) {  // la API las devuelve de mas nueva a mas antigua
    const std::string t = r.value("tag_name", std::string());
    if (r.value("draft", false) || t.find("-test") != std::string::npos || t.empty()) continue;
    tag = t;
    url = r.value("html_url", std::string());
    break;
  }
  Publish(tag, url);
  std::ofstream(state_file) << nlohmann::json{{"last_check", now}, {"tag", tag}, {"url", url}}.dump(2);
}

}  // namespace

void Start(const std::filesystem::path& home) {
  std::thread([f = home / "update_check.json"] {
    try {
      Run(f);
    } catch (...) {
      log::Warn("actualizacion: la consulta fallo");
    }
  }).detach();
}

bool Available(std::string& tag, std::string& url) {
  std::lock_guard lk(g_mu);
  tag = g_tag;
  url = g_url;
  return g_newer;
}

}  // namespace zw::update
