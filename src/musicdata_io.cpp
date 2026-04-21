#include "musicdata_io.h"

#include "../third_party/cJSON.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

#include "unk_patterns.inc"

static bool is_supported_version(uint32_t v) {
  static const uint32_t kHandlers[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 80};
  for (uint32_t h : kHandlers) {
    if (h == v) return true;
  }
  return false;
}

static std::string utf16le_to_utf8(const uint8_t* data, size_t nbytes) {
  if (nbytes < 2) return {};
  size_t nwchar = nbytes / 2;
  const WCHAR* pw = reinterpret_cast<const WCHAR*>(data);
  while (nwchar > 0 && pw[nwchar - 1] == 0) --nwchar;
  if (nwchar == 0) return {};
  int out = WideCharToMultiByte(CP_UTF8, 0, pw, (int)nwchar, nullptr, 0, nullptr, nullptr);
  if (out <= 0) return {};
  std::string u8(static_cast<size_t>(out), '\0');
  WideCharToMultiByte(CP_UTF8, 0, pw, (int)nwchar, &u8[0], out, nullptr, nullptr);
  return u8;
}

static std::string cp932_to_utf8(const uint8_t* data, size_t nbytes) {
  size_t z = 0;
  while (z < nbytes && data[z] != 0) ++z;
  if (z == 0) return {};
  int wlen = MultiByteToWideChar(932, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(data), (int)z, nullptr, 0);
  if (wlen <= 0) {
    wlen = MultiByteToWideChar(932, MB_PRECOMPOSED, reinterpret_cast<LPCCH>(data), (int)z, nullptr, 0);
  }
  if (wlen <= 0) return {};
  std::wstring w(wlen, L'\0');
  MultiByteToWideChar(932, MB_PRECOMPOSED, reinterpret_cast<LPCCH>(data), (int)z, &w[0], wlen);
  int u8len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (u8len <= 1) return {};
  std::string out(u8len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], u8len, nullptr, nullptr);
  return out;
}

static std::string read_string_cp932(const uint8_t* p, size_t len) { return cp932_to_utf8(p, len); }

static std::string read_string_utf16(const uint8_t* p, size_t len) { return utf16le_to_utf8(p, len); }

struct BinReader {
  const uint8_t* base;
  size_t size;
  size_t pos = 0;

  bool require(size_t n) const { return pos + n <= size; }

  void skip(size_t n) { pos += n; }

  uint8_t u8() {
    uint8_t v = base[pos++];
    return v;
  }

  uint16_t u16le() {
    uint16_t v = static_cast<uint16_t>(base[pos] | (base[pos + 1] << 8));
    pos += 2;
    return v;
  }

  uint32_t u32le() {
    uint32_t v = static_cast<uint32_t>(base[pos] | (base[pos + 1] << 8u) | (base[pos + 2] << 16u) | (base[pos + 3] << 24u));
    pos += 4;
    return v;
  }

  int16_t i16le() {
    uint16_t u = u16le();
    return static_cast<int16_t>(u);
  }

  int32_t i32le() {
    uint32_t u = u32le();
    return static_cast<int32_t>(u);
  }

  void read_bytes(uint8_t* dst, size_t n) {
    memcpy(dst, base + pos, n);
    pos += n;
  }

  const uint8_t* ptr(size_t n) {
    const uint8_t* r = base + pos;
    pos += n;
    return r;
  }
};

struct BinWriter {
  std::vector<uint8_t> buf;

  void raw(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
  }

  void u32le(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff), static_cast<uint8_t>((v >> 16) & 0xff),
                    static_cast<uint8_t>((v >> 24) & 0xff)};
    raw(b, 4);
  }

  void u16le(uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff)};
    raw(b, 2);
  }

  void i16le(int16_t v) { u16le(static_cast<uint16_t>(v)); }

  void i32le(int32_t v) { u32le(static_cast<uint32_t>(v)); }

  void pad0(size_t n) { buf.insert(buf.end(), n, 0); }

  void write_unk(const uint8_t* p, size_t n) { raw(p, n); }
};

static void utf8_to_cp932_bytes(const std::string& utf8, size_t max_codepoints, std::vector<uint8_t>& out) {
  out.clear();
  size_t i = 0;
  size_t cp = 0;
  while (i < utf8.size() && cp < max_codepoints) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    size_t inc = 1;
    if (c >= 0xf0) inc = 4;
    else if (c >= 0xe0)
      inc = 3;
    else if (c >= 0xc0)
      inc = 2;
    if (i + inc > utf8.size()) break;
    i += inc;
    ++cp;
  }
  std::string slice = utf8.substr(0, i);
  int wlen = MultiByteToWideChar(CP_UTF8, 0, slice.c_str(), -1, nullptr, 0);
  if (wlen <= 1) return;
  std::wstring w(static_cast<size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, slice.c_str(), -1, &w[0], wlen);
  int n = WideCharToMultiByte(932, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return;
  out.resize(static_cast<size_t>(n));
  WideCharToMultiByte(932, 0, w.c_str(), (int)w.size(), reinterpret_cast<LPSTR>(out.data()), n, nullptr, nullptr);
}

static void utf8_to_utf16le_bytes(const std::string& utf8, size_t max_codepoints, std::vector<uint8_t>& out) {
  out.clear();
  size_t i = 0;
  size_t cp = 0;
  while (i < utf8.size() && cp < max_codepoints) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    size_t inc = 1;
    if (c >= 0xf0) inc = 4;
    else if (c >= 0xe0)
      inc = 3;
    else if (c >= 0xc0)
      inc = 2;
    if (i + inc > utf8.size()) break;
    i += inc;
    ++cp;
  }
  std::string slice = utf8.substr(0, i);
  int wlen = MultiByteToWideChar(CP_UTF8, 0, slice.c_str(), -1, nullptr, 0);
  if (wlen <= 1) return;
  std::wstring w(static_cast<size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, slice.c_str(), -1, &w[0], wlen);
  out.resize(w.size() * 2);
  memcpy(out.data(), w.data(), out.size());
}

static void write_string_field(BinWriter& w, const std::string& utf8, size_t field_bytes, bool utf16) {
  std::vector<uint8_t> enc;
  if (utf16) {
    utf8_to_utf16le_bytes(utf8, field_bytes, enc);
  } else {
    utf8_to_cp932_bytes(utf8, field_bytes, enc);
  }
  if (enc.size() > field_bytes) enc.resize(field_bytes);
  w.raw(enc.data(), enc.size());
  if (enc.size() < field_bytes) w.pad0(field_bytes - enc.size());
}

static cJSON* read_one_song(BinReader& br, uint32_t version) {
  std::string title, title_ascii, genre, artist, subtitle;

  if (version == 80) {
    title = read_string_cp932(br.ptr(0x80), 0x80);
    title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    genre = read_string_cp932(br.ptr(0x80), 0x80);
    artist = read_string_cp932(br.ptr(0x80), 0x80);
  } else if (version >= 32) {
    title = read_string_utf16(br.ptr(0x100), 0x100);
    title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    genre = read_string_utf16(br.ptr(0x80), 0x80);
    artist = read_string_utf16(br.ptr(0x100), 0x100);
    subtitle = read_string_utf16(br.ptr(0x100), 0x100);
  } else {
    title = read_string_cp932(br.ptr(0x40), 0x40);
    title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    genre = read_string_cp932(br.ptr(0x40), 0x40);
    artist = read_string_cp932(br.ptr(0x40), 0x40);
  }

  uint32_t texture_title = br.u32le();
  uint32_t texture_artist = br.u32le();
  uint32_t texture_genre = br.u32le();
  uint32_t texture_load = br.u32le();
  uint32_t texture_list = br.u32le();
  uint32_t texture_subtitle = 0;
  if (version >= 32 && version != 80) texture_subtitle = br.u32le();

  uint32_t font_idx = br.u32le();
  uint16_t game_version = br.u16le();

  uint16_t other_folder = 0, bemani_folder = 0, beginner_rec_folder = 0, iidx_rec_folder = 0, bemani_rec_folder = 0, splittable_diff = 0,
           unk_unused = 0;
  if (version >= 32 && version != 80) {
    other_folder = br.u16le();
    bemani_folder = br.u16le();
    beginner_rec_folder = br.u16le();
    iidx_rec_folder = br.u16le();
    bemani_rec_folder = br.u16le();
    splittable_diff = br.u16le();
    unk_unused = br.u16le();
  } else {
    other_folder = br.u16le();
    bemani_folder = br.u16le();
    splittable_diff = br.u16le();
  }

  int SPB_level = 0, SPN_level = 0, SPH_level = 0, SPA_level = 0, SPL_level = 0;
  int DPB_level = 0, DPN_level = 0, DPH_level = 0, DPA_level = 0, DPL_level = 0;

  if (version >= 27) {
    SPB_level = static_cast<int>(br.u8());
    SPN_level = static_cast<int>(br.u8());
    SPH_level = static_cast<int>(br.u8());
    SPA_level = static_cast<int>(br.u8());
    SPL_level = static_cast<int>(br.u8());
    DPB_level = static_cast<int>(br.u8());
    DPN_level = static_cast<int>(br.u8());
    DPH_level = static_cast<int>(br.u8());
    DPA_level = static_cast<int>(br.u8());
    DPL_level = static_cast<int>(br.u8());
  } else {
    SPN_level = static_cast<int>(br.u8());
    SPH_level = static_cast<int>(br.u8());
    SPA_level = static_cast<int>(br.u8());
    DPN_level = static_cast<int>(br.u8());
    DPH_level = static_cast<int>(br.u8());
    DPA_level = static_cast<int>(br.u8());
    SPB_level = static_cast<int>(br.u8());
    DPB_level = static_cast<int>(br.u8());
    SPL_level = 0;
    DPL_level = 0;
  }

  if (version == 80)
    br.skip(0x2C6);
  else if (version >= 27)
    br.skip(0x286);
  else
    br.skip(0xA0);

  uint32_t song_id = br.u32le();
  uint32_t volume = br.u32le();

  int SPB_ident = 0, SPN_ident = 0, SPH_ident = 0, SPA_ident = 0, SPL_ident = 0;
  int DPB_ident = 0, DPN_ident = 0, DPH_ident = 0, DPA_ident = 0, DPL_ident = 0;

  if (version >= 27) {
    SPB_ident = static_cast<int>(br.u8());
    SPN_ident = static_cast<int>(br.u8());
    SPH_ident = static_cast<int>(br.u8());
    SPA_ident = static_cast<int>(br.u8());
    SPL_ident = static_cast<int>(br.u8());
    DPB_ident = static_cast<int>(br.u8());
    DPN_ident = static_cast<int>(br.u8());
    DPH_ident = static_cast<int>(br.u8());
    DPA_ident = static_cast<int>(br.u8());
    DPL_ident = static_cast<int>(br.u8());
  } else {
    SPN_ident = static_cast<int>(br.u8());
    SPH_ident = static_cast<int>(br.u8());
    SPA_ident = static_cast<int>(br.u8());
    DPN_ident = static_cast<int>(br.u8());
    DPH_ident = static_cast<int>(br.u8());
    DPA_ident = static_cast<int>(br.u8());
    SPB_ident = static_cast<int>(br.u8());
    DPB_ident = static_cast<int>(br.u8());
    SPL_ident = 48;
    DPL_ident = 48;
  }

  int16_t bga_delay = br.i16le();

  if (version <= 26 || version == 80) br.skip(2);

  std::string bga_filename = read_string_cp932(br.ptr(0x20), 0x20);

  if (version == 80) br.skip(2);

  uint32_t afp_flag = br.u32le();

  int afp_n = (version >= 22) ? 10 : 9;
  cJSON* afp_arr = cJSON_CreateArray();
  for (int a = 0; a < afp_n; ++a) {
    std::string s = read_string_cp932(br.ptr(0x20), 0x20);
    cJSON_AddItemToArray(afp_arr, cJSON_CreateString(s.c_str()));
  }

  if (version >= 26) br.skip(4);

  cJSON* o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "song_id", song_id);
  cJSON_AddStringToObject(o, "title", title.c_str());
  cJSON_AddStringToObject(o, "title_ascii", title_ascii.c_str());
  cJSON_AddStringToObject(o, "genre", genre.c_str());
  cJSON_AddStringToObject(o, "artist", artist.c_str());
  cJSON_AddNumberToObject(o, "texture_title", texture_title);
  cJSON_AddNumberToObject(o, "texture_artist", texture_artist);
  cJSON_AddNumberToObject(o, "texture_genre", texture_genre);
  cJSON_AddNumberToObject(o, "texture_load", texture_load);
  cJSON_AddNumberToObject(o, "texture_list", texture_list);
  cJSON_AddNumberToObject(o, "font_idx", font_idx);
  cJSON_AddNumberToObject(o, "game_version", game_version);
  cJSON_AddNumberToObject(o, "other_folder", other_folder);
  cJSON_AddNumberToObject(o, "bemani_folder", bemani_folder);
  cJSON_AddNumberToObject(o, "splittable_diff", splittable_diff);
  cJSON_AddNumberToObject(o, "SPB_level", SPB_level);
  cJSON_AddNumberToObject(o, "SPN_level", SPN_level);
  cJSON_AddNumberToObject(o, "SPH_level", SPH_level);
  cJSON_AddNumberToObject(o, "SPA_level", SPA_level);
  cJSON_AddNumberToObject(o, "SPL_level", SPL_level);
  cJSON_AddNumberToObject(o, "DPB_level", DPB_level);
  cJSON_AddNumberToObject(o, "DPN_level", DPN_level);
  cJSON_AddNumberToObject(o, "DPH_level", DPH_level);
  cJSON_AddNumberToObject(o, "DPA_level", DPA_level);
  cJSON_AddNumberToObject(o, "DPL_level", DPL_level);
  cJSON_AddNumberToObject(o, "volume", volume);
  cJSON_AddNumberToObject(o, "SPB_ident", SPB_ident);
  cJSON_AddNumberToObject(o, "SPN_ident", SPN_ident);
  cJSON_AddNumberToObject(o, "SPH_ident", SPH_ident);
  cJSON_AddNumberToObject(o, "SPA_ident", SPA_ident);
  cJSON_AddNumberToObject(o, "SPL_ident", SPL_ident);
  cJSON_AddNumberToObject(o, "DPB_ident", DPB_ident);
  cJSON_AddNumberToObject(o, "DPN_ident", DPN_ident);
  cJSON_AddNumberToObject(o, "DPH_ident", DPH_ident);
  cJSON_AddNumberToObject(o, "DPA_ident", DPA_ident);
  cJSON_AddNumberToObject(o, "DPL_ident", DPL_ident);
  cJSON_AddStringToObject(o, "bga_filename", bga_filename.c_str());
  cJSON_AddNumberToObject(o, "bga_delay", bga_delay);
  cJSON_AddNumberToObject(o, "afp_flag", afp_flag);
  cJSON_AddItemToObject(o, "afp_data", afp_arr);

  if (version >= 32 && version != 80) {
    cJSON_AddStringToObject(o, "subtitle", subtitle.c_str());
    cJSON_AddNumberToObject(o, "texture_subtitle", texture_subtitle);
    cJSON_AddNumberToObject(o, "beginner_rec_folder", beginner_rec_folder);
    cJSON_AddNumberToObject(o, "iidx_rec_folder", iidx_rec_folder);
    cJSON_AddNumberToObject(o, "bemani_rec_folder", bemani_rec_folder);
    cJSON_AddNumberToObject(o, "unk_unused", unk_unused);
  }

  return o;
}

static int get_json_int(cJSON* o, const char* key, int def = 0) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!it || !cJSON_IsNumber(it)) return def;
  return static_cast<int>(it->valuedouble);
}

static uint32_t get_json_u32(cJSON* o, const char* key, uint32_t def = 0) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!it || !cJSON_IsNumber(it)) return def;
  return static_cast<uint32_t>(it->valuedouble);
}

static std::string get_json_str(cJSON* o, const char* key, const std::string& def = {}) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!it || !cJSON_IsString(it)) return def;
  return it->valuestring ? it->valuestring : def;
}

static void write_one_song(BinWriter& bw, uint32_t version, cJSON* song_data) {
  if (version == 80) {
    write_string_field(bw, get_json_str(song_data, "title"), 0x80, false);
    write_string_field(bw, get_json_str(song_data, "title_ascii"), 0x40, false);
    write_string_field(bw, get_json_str(song_data, "genre"), 0x80, false);
    write_string_field(bw, get_json_str(song_data, "artist"), 0x80, false);
  } else if (version >= 32) {
    write_string_field(bw, get_json_str(song_data, "title"), 0x100, true);
    write_string_field(bw, get_json_str(song_data, "title_ascii"), 0x40, false);
    write_string_field(bw, get_json_str(song_data, "genre"), 0x80, true);
    write_string_field(bw, get_json_str(song_data, "artist"), 0x100, true);
    write_string_field(bw, get_json_str(song_data, "subtitle", ""), 0x100, true);
  } else {
    write_string_field(bw, get_json_str(song_data, "title"), 0x40, false);
    write_string_field(bw, get_json_str(song_data, "title_ascii"), 0x40, false);
    write_string_field(bw, get_json_str(song_data, "genre"), 0x40, false);
    write_string_field(bw, get_json_str(song_data, "artist"), 0x40, false);
  }

  bw.u32le(get_json_u32(song_data, "texture_title"));
  bw.u32le(get_json_u32(song_data, "texture_artist"));
  bw.u32le(get_json_u32(song_data, "texture_genre"));
  bw.u32le(get_json_u32(song_data, "texture_load"));
  bw.u32le(get_json_u32(song_data, "texture_list"));
  if (version >= 32 && version != 80) bw.u32le(get_json_u32(song_data, "texture_subtitle", 0));

  bw.u32le(get_json_u32(song_data, "font_idx"));
  bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "game_version")));

  if (version >= 32 && version != 80) {
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "other_folder")));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "bemani_folder")));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "beginner_rec_folder", 0)));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "iidx_rec_folder", 0)));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "bemani_rec_folder", 0)));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "splittable_diff")));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "unk_unused", 0)));
  } else {
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "other_folder")));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "bemani_folder")));
    bw.u16le(static_cast<uint16_t>(get_json_int(song_data, "splittable_diff")));
  }

  if (version >= 27) {
    uint8_t lv[10] = {static_cast<uint8_t>(get_json_int(song_data, "SPB_level")), static_cast<uint8_t>(get_json_int(song_data, "SPN_level")),
                      static_cast<uint8_t>(get_json_int(song_data, "SPH_level")), static_cast<uint8_t>(get_json_int(song_data, "SPA_level")),
                      static_cast<uint8_t>(get_json_int(song_data, "SPL_level")), static_cast<uint8_t>(get_json_int(song_data, "DPB_level")),
                      static_cast<uint8_t>(get_json_int(song_data, "DPN_level")), static_cast<uint8_t>(get_json_int(song_data, "DPH_level")),
                      static_cast<uint8_t>(get_json_int(song_data, "DPA_level")), static_cast<uint8_t>(get_json_int(song_data, "DPL_level"))};
    bw.raw(lv, 10);
  } else {
    uint8_t lv[8] = {static_cast<uint8_t>(get_json_int(song_data, "SPN_level")), static_cast<uint8_t>(get_json_int(song_data, "SPH_level")),
                     static_cast<uint8_t>(get_json_int(song_data, "SPA_level")), static_cast<uint8_t>(get_json_int(song_data, "DPN_level")),
                     static_cast<uint8_t>(get_json_int(song_data, "DPH_level")), static_cast<uint8_t>(get_json_int(song_data, "DPA_level")),
                     static_cast<uint8_t>(get_json_int(song_data, "SPB_level")), static_cast<uint8_t>(get_json_int(song_data, "DPB_level"))};
    bw.raw(lv, 8);
  }

  if (version == 80)
    bw.write_unk(kUnk80, kUnk80_len);
  else if (version >= 32)
    bw.write_unk(kUnk32, kUnk32_len);
  else if (version >= 27)
    bw.write_unk(kUnk27, kUnk27_len);
  else
    bw.write_unk(kUnk26, kUnk26_len);

  bw.u32le(get_json_u32(song_data, "song_id"));
  bw.u32le(get_json_u32(song_data, "volume"));

  if (version >= 27) {
    uint8_t id[10] = {static_cast<uint8_t>(get_json_int(song_data, "SPB_ident")), static_cast<uint8_t>(get_json_int(song_data, "SPN_ident")),
                      static_cast<uint8_t>(get_json_int(song_data, "SPH_ident")), static_cast<uint8_t>(get_json_int(song_data, "SPA_ident")),
                      static_cast<uint8_t>(get_json_int(song_data, "SPL_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPB_ident")),
                      static_cast<uint8_t>(get_json_int(song_data, "DPN_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPH_ident")),
                      static_cast<uint8_t>(get_json_int(song_data, "DPA_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPL_ident"))};
    bw.raw(id, 10);
  } else {
    uint8_t id[8] = {static_cast<uint8_t>(get_json_int(song_data, "SPN_ident")), static_cast<uint8_t>(get_json_int(song_data, "SPH_ident")),
                     static_cast<uint8_t>(get_json_int(song_data, "SPA_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPN_ident")),
                     static_cast<uint8_t>(get_json_int(song_data, "DPH_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPA_ident")),
                     static_cast<uint8_t>(get_json_int(song_data, "SPB_ident")), static_cast<uint8_t>(get_json_int(song_data, "DPB_ident"))};
    bw.raw(id, 8);
  }

  bw.i16le(static_cast<int16_t>(get_json_int(song_data, "bga_delay")));

  if (version <= 26 || version == 80) bw.pad0(2);

  write_string_field(bw, get_json_str(song_data, "bga_filename"), 0x20, false);

  if (version == 80) bw.pad0(2);

  bw.u32le(get_json_u32(song_data, "afp_flag"));

  int afp_count = (version >= 22) ? 10 : 9;
  cJSON* afp = cJSON_GetObjectItemCaseSensitive(song_data, "afp_data");
  for (int idx = 0; idx < afp_count; ++idx) {
    std::string s;
    if (afp && cJSON_IsArray(afp)) {
      cJSON* itm = cJSON_GetArrayItem(afp, idx);
      if (itm && cJSON_IsString(itm) && itm->valuestring) s = itm->valuestring;
    }
    write_string_field(bw, s, 0x20, false);
  }

  if (version >= 26) bw.pad0(4);
}

static void writer_impl(uint32_t version, BinWriter& bw, cJSON* data_array) {
  int song_count = cJSON_GetArraySize(data_array);
  std::unordered_map<uint32_t, int> exist_ids;
  for (int i = 0; i < song_count; ++i) {
    cJSON* row = cJSON_GetArrayItem(data_array, i);
    if (!row) continue;
    uint32_t sid = get_json_u32(row, "song_id", 0xffffffffu);
    exist_ids[sid] = i;
  }

  uint32_t cur_style_entries = version * 1000u;
  uint32_t max_entries = cur_style_entries + 1000u;
  bool wide_index = (version >= 32 && version != 80);

  bw.raw("IIDX", 4);
  if (version >= 32) {
    bw.u32le(version);
    bw.u16le(static_cast<uint16_t>(song_count));
    bw.u16le(0);
    bw.u32le(max_entries);
  } else {
    bw.u32le(version);
    bw.u16le(static_cast<uint16_t>(song_count));
    bw.u16le(static_cast<uint16_t>(max_entries));
    bw.u32le(0);
  }

  int current_song = 0;
  std::vector<uint32_t> sorted_keys;
  sorted_keys.reserve(exist_ids.size());
  for (const auto& kv : exist_ids) sorted_keys.push_back(kv.first);
  std::sort(sorted_keys.begin(), sorted_keys.end());

  for (uint32_t i = 0; i < max_entries; ++i) {
    auto it = exist_ids.find(i);
    if (it != exist_ids.end()) {
      if (wide_index)
        bw.i32le(current_song++);
      else
        bw.i16le(static_cast<int16_t>(current_song++));
    } else if (i >= cur_style_entries) {
      if (wide_index)
        bw.i32le(0);
      else
        bw.i16le(0);
    } else {
      if (wide_index)
        bw.i32le(-1);
      else
        bw.i16le(static_cast<int16_t>(-1));
    }
  }

  for (uint32_t k : sorted_keys) {
    int idx = exist_ids[k];
    cJSON* song_data = cJSON_GetArrayItem(data_array, idx);
    if (song_data) write_one_song(bw, version, song_data);
  }
}

// Pretty-print cJSON to match Python 3 json.dump(..., indent=4, ensure_ascii=False).
constexpr int kJsonIndentCols = 4;

static void append_indent_units(std::string& out, int indent_units) {
  if (indent_units <= 0) return;
  out.append(static_cast<size_t>(indent_units) * static_cast<size_t>(kJsonIndentCols), ' ');
}

static void append_escaped_json_string(std::string& out, const char* input) {
  out.push_back('"');
  if (!input) {
    out.push_back('"');
    return;
  }
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
    switch (*p) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (*p < 32) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
          out += buf;
        } else {
          out.push_back(static_cast<char>(*p));
        }
        break;
    }
  }
  out.push_back('"');
}

static void append_json_number_python(std::string& out, const cJSON* item) {
  double d = item->valuedouble;
  if (!std::isfinite(d)) {
    out += "null";
    return;
  }
  constexpr double kMaxInt53 = 9007199254740992.0;
  if (d >= -kMaxInt53 && d <= kMaxInt53 && d == std::trunc(d)) {
    long long ll = static_cast<long long>(d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", ll);
    out += buf;
    return;
  }
  char buf[64];
  int n = std::snprintf(buf, sizeof(buf), "%.15g", d);
  if (n > 0 && n < (int)sizeof(buf)) {
    char* endp = nullptr;
    double test = std::strtod(buf, &endp);
    if (endp != buf && std::fabs(test - d) <= 1e-9 * (std::fabs(d) > 0 ? std::fabs(d) : 1.0)) {
      out += buf;
      return;
    }
  }
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  out += buf;
}

static void append_json_value_after_colon(std::string& out, const cJSON* val, int key_line_indent_units);

static void append_json_array_element(std::string& out, const cJSON* elem, int array_key_line_units) {
  if (cJSON_IsNumber(elem)) {
    append_json_number_python(out, elem);
    return;
  }
  if (cJSON_IsString(elem)) {
    append_escaped_json_string(out, elem->valuestring);
    return;
  }
  if (cJSON_IsNull(elem)) {
    out += "null";
    return;
  }
  if (cJSON_IsArray(elem)) {
    if (!elem->child) {
      out += "[]";
      return;
    }
    out += "[\n";
    const cJSON* x = elem->child;
    while (x) {
      append_indent_units(out, array_key_line_units + 2);
      append_json_array_element(out, x, array_key_line_units + 1);
      if (x->next)
        out += ",\n";
      else
        out += '\n';
      x = x->next;
    }
    append_indent_units(out, array_key_line_units + 1);
    out += ']';
    return;
  }
  if (cJSON_IsObject(elem)) {
    if (!elem->child) {
      out += "{}";
      return;
    }
    out += "{\n";
    const cJSON* c = elem->child;
    while (c) {
      append_indent_units(out, array_key_line_units + 2);
      append_escaped_json_string(out, c->string);
      out += ": ";
      append_json_value_after_colon(out, c, array_key_line_units + 2);
      if (c->next)
        out += ",\n";
      else
        out += '\n';
      c = c->next;
    }
    append_indent_units(out, array_key_line_units + 1);
    out += '}';
    return;
  }
}

static void append_json_value_after_colon(std::string& out, const cJSON* val, int key_line_indent_units) {
  if (cJSON_IsNumber(val)) {
    append_json_number_python(out, val);
    return;
  }
  if (cJSON_IsString(val)) {
    append_escaped_json_string(out, val->valuestring);
    return;
  }
  if (cJSON_IsNull(val)) {
    out += "null";
    return;
  }
  if (cJSON_IsArray(val)) {
    if (!val->child) {
      out += "[]";
      return;
    }
    out += "[\n";
    const cJSON* e = val->child;
    while (e) {
      append_indent_units(out, key_line_indent_units + 1);
      append_json_array_element(out, e, key_line_indent_units);
      if (e->next)
        out += ",\n";
      else
        out += '\n';
      e = e->next;
    }
    append_indent_units(out, key_line_indent_units);
    out += ']';
    return;
  }
  if (cJSON_IsObject(val)) {
    if (!val->child) {
      out += "{}";
      return;
    }
    out += "{\n";
    const cJSON* c = val->child;
    while (c) {
      append_indent_units(out, key_line_indent_units + 1);
      append_escaped_json_string(out, c->string);
      out += ": ";
      append_json_value_after_colon(out, c, key_line_indent_units + 1);
      if (c->next)
        out += ",\n";
      else
        out += '\n';
      c = c->next;
    }
    append_indent_units(out, key_line_indent_units);
    out += '}';
    return;
  }
}

static bool append_python_style_json_utf8(cJSON* root, std::string& out) {
  out.clear();
  if (!root || !cJSON_IsObject(root)) return false;
  if (!root->child) {
    out += "{}";
    return true;
  }
  out += "{\n";
  const cJSON* c = root->child;
  while (c) {
    append_indent_units(out, 1);
    append_escaped_json_string(out, c->string);
    out += ": ";
    append_json_value_after_colon(out, c, 1);
    if (c->next)
      out += ",\n";
    else
      out += '\n';
    c = c->next;
  }
  out += '}';
  return true;
}

}  // namespace

bool musicdata_extract_json(const std::vector<uint8_t>& bin, std::string& out_json_utf8, std::wstring& err) {
  err.clear();
  out_json_utf8.clear();
  if (bin.size() < 16) {
    err = L"File too small.";
    return false;
  }
  if (memcmp(bin.data(), "IIDX", 4) != 0) {
    err = L"Not an IIDX musicdata file.";
    return false;
  }
  uint32_t version = static_cast<uint32_t>(bin[4] | (bin[5] << 8u) | (bin[6] << 16u) | (bin[7] << 24u));
  if (!is_supported_version(version)) {
    err = L"Unsupported data_ver.";
    return false;
  }

  BinReader br{bin.data(), bin.size(), 8};
  uint16_t available_entries = 0;
  uint32_t total_entries = 0;
  if (version >= 32) {
    available_entries = br.u16le();
    br.u16le();
    total_entries = br.u32le();
  } else {
    available_entries = br.u16le();
    total_entries = br.u32le();
    br.u16le();
  }

  size_t entry_sz = (version >= 32 && version != 80) ? 4u : 2u;
  br.skip(static_cast<size_t>(total_entries) * entry_sz);

  cJSON* songs = cJSON_CreateArray();
  for (uint32_t i = 0; i < available_entries; ++i) {
    if (br.pos > br.size) break;
    cJSON* song = read_one_song(br, version);
    cJSON_AddItemToArray(songs, song);
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "data_ver", version);
  cJSON_AddItemToObject(root, "data", songs);

  if (!append_python_style_json_utf8(root, out_json_utf8)) {
    cJSON_Delete(root);
    err = L"JSON serialization failed.";
    return false;
  }
  cJSON_Delete(root);
  return true;
}

bool musicdata_create_bin(const std::string& json_utf8, std::vector<uint8_t>& out_bin, std::wstring& err) {
  err.clear();
  out_bin.clear();
  cJSON* root = cJSON_Parse(json_utf8.c_str());
  if (!root) {
    err = L"Invalid JSON.";
    return false;
  }
  cJSON* ver_it = cJSON_GetObjectItemCaseSensitive(root, "data_ver");
  if (!ver_it || !cJSON_IsNumber(ver_it)) {
    cJSON_Delete(root);
    err = L"Missing data_ver.";
    return false;
  }
  uint32_t version = static_cast<uint32_t>(ver_it->valuedouble);
  if (!is_supported_version(version)) {
    cJSON_Delete(root);
    err = L"Unsupported data_ver.";
    return false;
  }
  cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (!data || !cJSON_IsArray(data)) {
    cJSON_Delete(root);
    err = L"Missing data array.";
    return false;
  }

  BinWriter bw;
  writer_impl(version, bw, data);
  cJSON_Delete(root);
  out_bin = std::move(bw.buf);
  return true;
}
