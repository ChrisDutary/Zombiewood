#include "frontend/cursor.h"

#include <SDL.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace zw::cursor {
namespace {

constexpr int kResourceId = 102;  // resources/cursor.rc

struct Entry {
  int size = 0, hot_x = 0, hot_y = 0;
  const std::uint8_t* pixels = nullptr;  // size*size*4, BGRA (= SDL_PIXELFORMAT_ARGB8888 en little endian)
};

SDL_Cursor* g_cursor = nullptr;
int g_cursor_size = 0;

std::uint32_t U32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Lee el recurso incrustado. Formato (tools/make_cursor.ps1): "ZWCU", u32 version, u32 n,
// n x {u32 size, u32 hot_x, u32 hot_y, u32 offset}, pixeles.
std::vector<Entry> LoadEntries() {
  std::vector<Entry> out;
#ifdef _WIN32
  HMODULE mod = GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(kResourceId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  if (!res) return out;
  HGLOBAL h = LoadResource(mod, res);
  const auto* p = static_cast<const std::uint8_t*>(h ? LockResource(h) : nullptr);
  const DWORD len = SizeofResource(mod, res);
  if (!p || len < 12 || std::memcmp(p, "ZWCU", 4) != 0 || U32(p + 4) != 1) {
    log::Warn("cursor: recurso {} invalido; se usa el cursor de Windows", kResourceId);
    return out;
  }
  const std::uint32_t n = U32(p + 8);
  if (n == 0 || n > 16 || 12 + 16ull * n > len) return out;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint8_t* e = p + 12 + 16 * i;
    Entry en{static_cast<int>(U32(e)), static_cast<int>(U32(e + 4)), static_cast<int>(U32(e + 8)), nullptr};
    const std::uint64_t off = U32(e + 12), bytes = 4ull * en.size * en.size;
    if (en.size <= 0 || en.size > 256 || off + bytes > len) continue;
    en.pixels = p + off;
    out.push_back(en);
  }
#endif
  return out;
}

}  // namespace

bool Available() { return !LoadEntries().empty(); }

void Apply(const std::string& mode, int size) {
  if (mode != "zombie") {
    if (g_cursor) {
      SDL_SetCursor(SDL_GetDefaultCursor());
      SDL_FreeCursor(g_cursor);
      g_cursor = nullptr;
      g_cursor_size = 0;
      log::Info("cursor: cursor de Windows");
    }
    return;
  }
  const auto entries = LoadEntries();
  if (entries.empty()) {
    log::Info("cursor: el exe no trae cursor zombi incrustado; se usa el de Windows");
    return;
  }
  const Entry* best = &entries[0];
  for (const auto& e : entries)
    if (std::abs(e.size - size) < std::abs(best->size - size)) best = &e;
  if (g_cursor && g_cursor_size == best->size) return;

  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<std::uint8_t*>(best->pixels), best->size, best->size,
                                                         32, best->size * 4, SDL_PIXELFORMAT_ARGB8888);
  if (!surf) {
    log::Warn("cursor: SDL_CreateRGBSurfaceWithFormatFrom fallo: {}", SDL_GetError());
    return;
  }
  SDL_Cursor* c = SDL_CreateColorCursor(surf, best->hot_x, best->hot_y);
  SDL_FreeSurface(surf);
  if (!c) {
    log::Warn("cursor: SDL_CreateColorCursor fallo: {}", SDL_GetError());
    return;
  }
  SDL_SetCursor(c);
  if (g_cursor) SDL_FreeCursor(g_cursor);
  g_cursor = c;
  g_cursor_size = best->size;
  log::Info("cursor: zombi {}px (hotspot {},{})", best->size, best->hot_x, best->hot_y);
}

void Shutdown() {
  if (!g_cursor) return;
  SDL_SetCursor(SDL_GetDefaultCursor());
  SDL_FreeCursor(g_cursor);
  g_cursor = nullptr;
  g_cursor_size = 0;
}

}  // namespace zw::cursor
