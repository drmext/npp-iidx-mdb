#include "musicdata_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

struct SongScratch {
  std::string title, title_ascii, genre, artist, subtitle;
  std::string bga_filename;
  std::string afp_entry;
};

struct SongData {
  uint32_t song_id = 0xffffffffu;

  std::string title;
  std::string title_ascii;
  std::string genre;
  std::string artist;
  std::string subtitle;

  uint32_t texture_title = 0;
  uint32_t texture_artist = 0;
  uint32_t texture_genre = 0;
  uint32_t texture_load = 0;
  uint32_t texture_list = 0;
  uint32_t texture_subtitle = 0;

  uint32_t font_idx = 0;
  int game_version = 0;
  int other_folder = 0;
  int bemani_folder = 0;
  int beginner_rec_folder = 0;
  int iidx_rec_folder = 0;
  int bemani_rec_folder = 0;
  int splittable_diff = 0;
  int unk_unused = 0;

  int SPB_level = 0, SPN_level = 0, SPH_level = 0, SPA_level = 0, SPL_level = 0;
  int DPB_level = 0, DPN_level = 0, DPH_level = 0, DPA_level = 0, DPL_level = 0;

  uint32_t volume = 0;
  int SPB_ident = 0, SPN_ident = 0, SPH_ident = 0, SPA_ident = 0, SPL_ident = 0;
  int DPB_ident = 0, DPN_ident = 0, DPH_ident = 0, DPA_ident = 0, DPL_ident = 0;

  int bga_delay = 0;
  std::string bga_filename;

  uint32_t afp_flag = 0;
  // JSON contains 9 or 10 elements depending on version; store up to 10.
  std::array<std::string, 10> afp_data{};
  int afp_data_count = 0;
};

static void write_one_song_typed(BinWriter& bw, uint32_t version, const SongData& s) {
  if (version == 80) {
    write_string_field(bw, s.title, 0x80, false);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x80, false);
    write_string_field(bw, s.artist, 0x80, false);
  } else if (version >= 32) {
    write_string_field(bw, s.title, 0x100, true);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x80, true);
    write_string_field(bw, s.artist, 0x100, true);
    write_string_field(bw, s.subtitle, 0x100, true);
  } else {
    write_string_field(bw, s.title, 0x40, false);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x40, false);
    write_string_field(bw, s.artist, 0x40, false);
  }

  bw.u32le(s.texture_title);
  bw.u32le(s.texture_artist);
  bw.u32le(s.texture_genre);
  bw.u32le(s.texture_load);
  bw.u32le(s.texture_list);
  if (version >= 32 && version != 80) bw.u32le(s.texture_subtitle);

  bw.u32le(s.font_idx);
  bw.u16le(static_cast<uint16_t>(s.game_version));

  if (version >= 32 && version != 80) {
    bw.u16le(static_cast<uint16_t>(s.other_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_folder));
    bw.u16le(static_cast<uint16_t>(s.beginner_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.iidx_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.splittable_diff));
    bw.u16le(static_cast<uint16_t>(s.unk_unused));
  } else {
    bw.u16le(static_cast<uint16_t>(s.other_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_folder));
    bw.u16le(static_cast<uint16_t>(s.splittable_diff));
  }

  if (version >= 27) {
    uint8_t lv[10] = {static_cast<uint8_t>(s.SPB_level), static_cast<uint8_t>(s.SPN_level), static_cast<uint8_t>(s.SPH_level),
                      static_cast<uint8_t>(s.SPA_level), static_cast<uint8_t>(s.SPL_level), static_cast<uint8_t>(s.DPB_level),
                      static_cast<uint8_t>(s.DPN_level), static_cast<uint8_t>(s.DPH_level), static_cast<uint8_t>(s.DPA_level),
                      static_cast<uint8_t>(s.DPL_level)};
    bw.raw(lv, 10);
  } else {
    uint8_t lv[8] = {static_cast<uint8_t>(s.SPN_level), static_cast<uint8_t>(s.SPH_level), static_cast<uint8_t>(s.SPA_level),
                     static_cast<uint8_t>(s.DPN_level), static_cast<uint8_t>(s.DPH_level), static_cast<uint8_t>(s.DPA_level),
                     static_cast<uint8_t>(s.SPB_level), static_cast<uint8_t>(s.DPB_level)};
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

  bw.u32le(s.song_id);
  bw.u32le(s.volume);

  if (version >= 27) {
    uint8_t id[10] = {static_cast<uint8_t>(s.SPB_ident), static_cast<uint8_t>(s.SPN_ident), static_cast<uint8_t>(s.SPH_ident),
                      static_cast<uint8_t>(s.SPA_ident), static_cast<uint8_t>(s.SPL_ident), static_cast<uint8_t>(s.DPB_ident),
                      static_cast<uint8_t>(s.DPN_ident), static_cast<uint8_t>(s.DPH_ident), static_cast<uint8_t>(s.DPA_ident),
                      static_cast<uint8_t>(s.DPL_ident)};
    bw.raw(id, 10);
  } else {
    uint8_t id[8] = {static_cast<uint8_t>(s.SPN_ident), static_cast<uint8_t>(s.SPH_ident), static_cast<uint8_t>(s.SPA_ident),
                     static_cast<uint8_t>(s.DPN_ident), static_cast<uint8_t>(s.DPH_ident), static_cast<uint8_t>(s.DPA_ident),
                     static_cast<uint8_t>(s.SPB_ident), static_cast<uint8_t>(s.DPB_ident)};
    bw.raw(id, 8);
  }

  bw.i16le(static_cast<int16_t>(s.bga_delay));

  if (version <= 26 || version == 80) bw.pad0(2);

  write_string_field(bw, s.bga_filename, 0x20, false);

  if (version == 80) bw.pad0(2);

  bw.u32le(s.afp_flag);

  const int afp_count = (version >= 22) ? 10 : 9;
  for (int idx = 0; idx < afp_count; ++idx) {
    const std::string& x = (idx < s.afp_data_count) ? s.afp_data[static_cast<size_t>(idx)] : std::string{};
    write_string_field(bw, x, 0x20, false);
  }

  if (version >= 26) bw.pad0(4);
}


static void writer_impl_typed(uint32_t version, BinWriter& bw, const std::vector<SongData>& songs) {
  const int song_count = static_cast<int>(songs.size());
  std::unordered_map<uint32_t, int> exist_ids;
  exist_ids.reserve(songs.size());
  for (int i = 0; i < song_count; ++i) {
    exist_ids[songs[static_cast<size_t>(i)].song_id] = i;
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
    if (idx < 0 || idx >= song_count) continue;
    write_one_song_typed(bw, version, songs[static_cast<size_t>(idx)]);
  }
}

// Pretty-print JSON to match Python 3 json.dump(..., indent=4, ensure_ascii=False).
constexpr int kJsonIndentCols = 4;

static void append_indent_units(std::string& out, int indent_units) {
  if (indent_units <= 0) return;
  out.append(static_cast<size_t>(indent_units) * static_cast<size_t>(kJsonIndentCols), ' ');
}

static void append_json_int(std::string& out, long long v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", v);
  out.append(buf);
}

static void append_json_u32(std::string& out, uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
  out.append(buf);
}

static void append_json_i16(std::string& out, int16_t v) { append_json_int(out, static_cast<long long>(v)); }
static void append_json_i32(std::string& out, int32_t v) { append_json_int(out, static_cast<long long>(v)); }
static void append_json_u16(std::string& out, uint16_t v) { append_json_int(out, static_cast<long long>(v)); }

static void append_escaped_json_string(std::string& out, const char* input) {
  out.push_back('"');
  if (!input) {
    out.push_back('"');
    return;
  }
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
    switch (*p) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (*p < 32) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
          out.append(buf);
        } else {
          out.push_back(static_cast<char>(*p));
        }
        break;
    }
  }
  out.push_back('"');
}

static void append_afp_data_from_bin_python(std::string& out, BinReader& br, uint32_t version, SongScratch& scratch, int key_line_indent_units) {
  const int afp_n = (version >= 22) ? 10 : 9;
  out += "[\n";
  for (int i = 0; i < afp_n; ++i) {
    scratch.afp_entry = read_string_cp932(br.ptr(0x20), 0x20);
    append_indent_units(out, key_line_indent_units + 1);
    append_escaped_json_string(out, scratch.afp_entry.c_str());
    if (i + 1 < afp_n)
      out += ",\n";
    else
      out += '\n';
  }
  append_indent_units(out, key_line_indent_units);
  out += ']';
}

static void append_one_song_from_bin_python(std::string& out, BinReader& br, uint32_t version, SongScratch& scratch, int array_key_line_units) {
  // Formatting matches the plugin's Python-style pretty JSON (indent=4, ensure_ascii=False).
  scratch.subtitle.clear();

  if (version == 80) {
    scratch.title = read_string_cp932(br.ptr(0x80), 0x80);
    scratch.title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    scratch.genre = read_string_cp932(br.ptr(0x80), 0x80);
    scratch.artist = read_string_cp932(br.ptr(0x80), 0x80);
  } else if (version >= 32) {
    scratch.title = read_string_utf16(br.ptr(0x100), 0x100);
    scratch.title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    scratch.genre = read_string_utf16(br.ptr(0x80), 0x80);
    scratch.artist = read_string_utf16(br.ptr(0x100), 0x100);
    scratch.subtitle = read_string_utf16(br.ptr(0x100), 0x100);
  } else {
    scratch.title = read_string_cp932(br.ptr(0x40), 0x40);
    scratch.title_ascii = read_string_cp932(br.ptr(0x40), 0x40);
    scratch.genre = read_string_cp932(br.ptr(0x40), 0x40);
    scratch.artist = read_string_cp932(br.ptr(0x40), 0x40);
  }

  const uint32_t texture_title = br.u32le();
  const uint32_t texture_artist = br.u32le();
  const uint32_t texture_genre = br.u32le();
  const uint32_t texture_load = br.u32le();
  const uint32_t texture_list = br.u32le();
  uint32_t texture_subtitle = 0;
  if (version >= 32 && version != 80) texture_subtitle = br.u32le();

  const uint32_t font_idx = br.u32le();
  const uint16_t game_version = br.u16le();

  uint16_t other_folder = 0, bemani_folder = 0, beginner_rec_folder = 0, iidx_rec_folder = 0, bemani_rec_folder = 0, splittable_diff = 0, unk_unused = 0;
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

  const uint32_t song_id = br.u32le();
  const uint32_t volume = br.u32le();

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

  const int16_t bga_delay = br.i16le();
  if (version <= 26 || version == 80) br.skip(2);

  scratch.bga_filename = read_string_cp932(br.ptr(0x20), 0x20);
  if (version == 80) br.skip(2);

  const uint32_t afp_flag = br.u32le();

  out += "{\n";
  auto key = [&](const char* k) {
    append_indent_units(out, array_key_line_units + 2);
    append_escaped_json_string(out, k);
    out += ": ";
  };
  auto comma_nl = [&](bool more) {
    if (more)
      out += ",\n";
    else
      out += '\n';
  };

  key("song_id");
  append_json_u32(out, song_id);
  comma_nl(true);
  key("title");
  append_escaped_json_string(out, scratch.title.c_str());
  comma_nl(true);
  key("title_ascii");
  append_escaped_json_string(out, scratch.title_ascii.c_str());
  comma_nl(true);
  key("genre");
  append_escaped_json_string(out, scratch.genre.c_str());
  comma_nl(true);
  key("artist");
  append_escaped_json_string(out, scratch.artist.c_str());
  comma_nl(true);
  key("texture_title");
  append_json_u32(out, texture_title);
  comma_nl(true);
  key("texture_artist");
  append_json_u32(out, texture_artist);
  comma_nl(true);
  key("texture_genre");
  append_json_u32(out, texture_genre);
  comma_nl(true);
  key("texture_load");
  append_json_u32(out, texture_load);
  comma_nl(true);
  key("texture_list");
  append_json_u32(out, texture_list);
  comma_nl(true);
  key("font_idx");
  append_json_u32(out, font_idx);
  comma_nl(true);
  key("game_version");
  append_json_u16(out, game_version);
  comma_nl(true);
  key("other_folder");
  append_json_u16(out, other_folder);
  comma_nl(true);
  key("bemani_folder");
  append_json_u16(out, bemani_folder);
  comma_nl(true);
  key("splittable_diff");
  append_json_u16(out, splittable_diff);
  comma_nl(true);
  key("SPB_level");
  append_json_i32(out, SPB_level);
  comma_nl(true);
  key("SPN_level");
  append_json_i32(out, SPN_level);
  comma_nl(true);
  key("SPH_level");
  append_json_i32(out, SPH_level);
  comma_nl(true);
  key("SPA_level");
  append_json_i32(out, SPA_level);
  comma_nl(true);
  key("SPL_level");
  append_json_i32(out, SPL_level);
  comma_nl(true);
  key("DPB_level");
  append_json_i32(out, DPB_level);
  comma_nl(true);
  key("DPN_level");
  append_json_i32(out, DPN_level);
  comma_nl(true);
  key("DPH_level");
  append_json_i32(out, DPH_level);
  comma_nl(true);
  key("DPA_level");
  append_json_i32(out, DPA_level);
  comma_nl(true);
  key("DPL_level");
  append_json_i32(out, DPL_level);
  comma_nl(true);
  key("volume");
  append_json_u32(out, volume);
  comma_nl(true);
  key("SPB_ident");
  append_json_i32(out, SPB_ident);
  comma_nl(true);
  key("SPN_ident");
  append_json_i32(out, SPN_ident);
  comma_nl(true);
  key("SPH_ident");
  append_json_i32(out, SPH_ident);
  comma_nl(true);
  key("SPA_ident");
  append_json_i32(out, SPA_ident);
  comma_nl(true);
  key("SPL_ident");
  append_json_i32(out, SPL_ident);
  comma_nl(true);
  key("DPB_ident");
  append_json_i32(out, DPB_ident);
  comma_nl(true);
  key("DPN_ident");
  append_json_i32(out, DPN_ident);
  comma_nl(true);
  key("DPH_ident");
  append_json_i32(out, DPH_ident);
  comma_nl(true);
  key("DPA_ident");
  append_json_i32(out, DPA_ident);
  comma_nl(true);
  key("DPL_ident");
  append_json_i32(out, DPL_ident);
  comma_nl(true);
  key("bga_filename");
  append_escaped_json_string(out, scratch.bga_filename.c_str());
  comma_nl(true);
  key("bga_delay");
  append_json_i16(out, bga_delay);
  comma_nl(true);
  key("afp_flag");
  append_json_u32(out, afp_flag);
  comma_nl(true);
  key("afp_data");
  append_afp_data_from_bin_python(out, br, version, scratch, array_key_line_units + 2);

  if (version >= 32 && version != 80) {
    comma_nl(true);
    key("subtitle");
    append_escaped_json_string(out, scratch.subtitle.c_str());
    comma_nl(true);
    key("texture_subtitle");
    append_json_u32(out, texture_subtitle);
    comma_nl(true);
    key("beginner_rec_folder");
    append_json_u16(out, beginner_rec_folder);
    comma_nl(true);
    key("iidx_rec_folder");
    append_json_u16(out, iidx_rec_folder);
    comma_nl(true);
    key("bemani_rec_folder");
    append_json_u16(out, bemani_rec_folder);
    comma_nl(true);
    key("unk_unused");
    append_json_u16(out, unk_unused);
    comma_nl(false);
  } else {
    comma_nl(false);
  }

  append_indent_units(out, array_key_line_units + 1);
  out += '}';

  if (version >= 26) br.skip(4);
}

// -------- Streaming JSON parser for save path (schema-tailored) --------

namespace {

static inline bool json_is_ws(unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

struct JsonReader {
  const char* p = nullptr;
  const char* end = nullptr;

  explicit JsonReader(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

  bool eof() const { return p >= end; }

  void skip_ws() {
    while (!eof() && json_is_ws(static_cast<unsigned char>(*p))) ++p;
  }

  bool consume(char c) {
    skip_ws();
    if (!eof() && *p == c) {
      ++p;
      return true;
    }
    return false;
  }

  bool peek(char c) {
    skip_ws();
    return (!eof() && *p == c);
  }

  bool expect(char c, std::wstring& err) {
    if (consume(c)) return true;
    err = L"Invalid JSON (expected MusicDataPlugin schema): missing expected token.";
    return false;
  }

  static void append_utf8_codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  bool read_hex4(uint32_t& out, std::wstring& err) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
      if (eof()) {
        err = L"Invalid JSON: truncated \\u escape.";
        return false;
      }
      char c = *p++;
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<uint32_t>(10 + (c - 'a'));
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<uint32_t>(10 + (c - 'A'));
      else {
        err = L"Invalid JSON: bad hex in \\u escape.";
        return false;
      }
    }
    return true;
  }

  bool read_string(std::string& out, std::wstring& err) {
    skip_ws();
    if (eof() || *p != '"') {
      err = L"Invalid JSON (expected MusicDataPlugin schema): expected string.";
      return false;
    }
    ++p;
    out.clear();
    while (!eof()) {
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c == '\\') {
        if (eof()) {
          err = L"Invalid JSON: truncated escape.";
          return false;
        }
        char e = *p++;
        switch (e) {
          case '"':
          case '\\':
          case '/':
            out.push_back(e);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            uint32_t cp = 0;
            if (!read_hex4(cp, err)) return false;
            // Surrogates not expected in plugin output; treat as code point.
            append_utf8_codepoint(out, cp);
            break;
          }
          default:
            err = L"Invalid JSON: unknown escape.";
            return false;
        }
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
    err = L"Invalid JSON: unterminated string.";
    return false;
  }

  bool read_null(std::wstring& err) {
    skip_ws();
    if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
      p += 4;
      return true;
    }
    err = L"Invalid JSON: expected null.";
    return false;
  }

  bool read_number(double& out, std::wstring& err) {
    skip_ws();
    if (eof()) {
      err = L"Invalid JSON: expected number.";
      return false;
    }
    const char* start = p;
    // Basic JSON number chars: [-+0-9.eE]
    if (*p == '-' || *p == '+') ++p;
    while (!eof()) {
      char c = *p;
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++p;
        continue;
      }
      break;
    }
    if (p == start) {
      err = L"Invalid JSON: expected number.";
      return false;
    }
    char* endp = nullptr;
    out = std::strtod(start, &endp);
    if (!endp || endp != p) {
      err = L"Invalid JSON: bad number.";
      return false;
    }
    return true;
  }

  bool skip_value(std::wstring& err) {
    skip_ws();
    if (eof()) {
      err = L"Invalid JSON: unexpected end.";
      return false;
    }
    char c = *p;
    if (c == '"') {
      std::string tmp;
      return read_string(tmp, err);
    }
    if (c == '{') {
      ++p;
      skip_ws();
      if (consume('}')) return true;
      while (true) {
        std::string k;
        if (!read_string(k, err)) return false;
        if (!expect(':', err)) return false;
        if (!skip_value(err)) return false;
        if (consume('}')) return true;
        if (!expect(',', err)) return false;
      }
    }
    if (c == '[') {
      ++p;
      skip_ws();
      if (consume(']')) return true;
      while (true) {
        if (!skip_value(err)) return false;
        if (consume(']')) return true;
        if (!expect(',', err)) return false;
      }
    }
    if (c == 'n') return read_null(err);
    // number
    double d = 0;
    return read_number(d, err);
  }
};

static int json_to_int(double d) {
  if (!std::isfinite(d)) return 0;
  if (d > static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
  if (d < static_cast<double>(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
  return static_cast<int>(d);
}

static uint32_t json_to_u32(double d) {
  if (!std::isfinite(d) || d < 0) return 0;
  if (d > 4294967295.0) return 0xffffffffu;
  return static_cast<uint32_t>(d);
}

static bool parse_song_object(JsonReader& jr, uint32_t version, SongData& out, std::wstring& err) {
  out = SongData{};
  out.afp_data_count = 0;

  if (!jr.expect('{', err)) return false;
  if (jr.consume('}')) return true;
  while (true) {
    std::string key;
    if (!jr.read_string(key, err)) return false;
    if (!jr.expect(':', err)) return false;

    if (key == "song_id") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.song_id = json_to_u32(d);
    } else if (key == "title") {
      if (!jr.read_string(out.title, err)) return false;
    } else if (key == "title_ascii") {
      if (!jr.read_string(out.title_ascii, err)) return false;
    } else if (key == "genre") {
      if (!jr.read_string(out.genre, err)) return false;
    } else if (key == "artist") {
      if (!jr.read_string(out.artist, err)) return false;
    } else if (key == "subtitle") {
      if (!jr.read_string(out.subtitle, err)) return false;
    } else if (key == "texture_title") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_title = json_to_u32(d);
    } else if (key == "texture_artist") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_artist = json_to_u32(d);
    } else if (key == "texture_genre") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_genre = json_to_u32(d);
    } else if (key == "texture_load") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_load = json_to_u32(d);
    } else if (key == "texture_list") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_list = json_to_u32(d);
    } else if (key == "texture_subtitle") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.texture_subtitle = json_to_u32(d);
    } else if (key == "font_idx") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.font_idx = json_to_u32(d);
    } else if (key == "game_version") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.game_version = json_to_int(d);
    } else if (key == "other_folder") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.other_folder = json_to_int(d);
    } else if (key == "bemani_folder") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.bemani_folder = json_to_int(d);
    } else if (key == "beginner_rec_folder") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.beginner_rec_folder = json_to_int(d);
    } else if (key == "iidx_rec_folder") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.iidx_rec_folder = json_to_int(d);
    } else if (key == "bemani_rec_folder") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.bemani_rec_folder = json_to_int(d);
    } else if (key == "splittable_diff") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.splittable_diff = json_to_int(d);
    } else if (key == "unk_unused") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.unk_unused = json_to_int(d);
    } else if (key == "SPB_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPB_level = json_to_int(d);
    } else if (key == "SPN_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPN_level = json_to_int(d);
    } else if (key == "SPH_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPH_level = json_to_int(d);
    } else if (key == "SPA_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPA_level = json_to_int(d);
    } else if (key == "SPL_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPL_level = json_to_int(d);
    } else if (key == "DPB_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPB_level = json_to_int(d);
    } else if (key == "DPN_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPN_level = json_to_int(d);
    } else if (key == "DPH_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPH_level = json_to_int(d);
    } else if (key == "DPA_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPA_level = json_to_int(d);
    } else if (key == "DPL_level") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPL_level = json_to_int(d);
    } else if (key == "volume") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.volume = json_to_u32(d);
    } else if (key == "SPB_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPB_ident = json_to_int(d);
    } else if (key == "SPN_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPN_ident = json_to_int(d);
    } else if (key == "SPH_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPH_ident = json_to_int(d);
    } else if (key == "SPA_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPA_ident = json_to_int(d);
    } else if (key == "SPL_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.SPL_ident = json_to_int(d);
    } else if (key == "DPB_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPB_ident = json_to_int(d);
    } else if (key == "DPN_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPN_ident = json_to_int(d);
    } else if (key == "DPH_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPH_ident = json_to_int(d);
    } else if (key == "DPA_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPA_ident = json_to_int(d);
    } else if (key == "DPL_ident") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.DPL_ident = json_to_int(d);
    } else if (key == "bga_filename") {
      if (!jr.read_string(out.bga_filename, err)) return false;
    } else if (key == "bga_delay") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.bga_delay = json_to_int(d);
    } else if (key == "afp_flag") {
      double d = 0;
      if (!jr.read_number(d, err)) return false;
      out.afp_flag = json_to_u32(d);
    } else if (key == "afp_data") {
      if (!jr.expect('[', err)) return false;
      int idx = 0;
      if (!jr.consume(']')) {
        while (true) {
          jr.skip_ws();
          if (jr.peek('n')) {
            if (!jr.read_null(err)) return false;
            if (idx < 10) out.afp_data[static_cast<size_t>(idx)].clear();
          } else {
            std::string s;
            if (!jr.read_string(s, err)) return false;
            if (idx < 10) out.afp_data[static_cast<size_t>(idx)] = std::move(s);
          }
          ++idx;
          if (jr.consume(']')) break;
          if (!jr.expect(',', err)) return false;
        }
      }
      out.afp_data_count = std::min(idx, 10);
    } else {
      // Unknown key: skip its value.
      if (!jr.skip_value(err)) return false;
    }

    if (jr.consume('}')) break;
    if (!jr.expect(',', err)) return false;
  }

  // Ensure afp_data_count matches expected schema if present.
  if (out.afp_data_count == 0) out.afp_data_count = 0;
  (void)version;
  return true;
}

static bool parse_musicdata_root(const std::string& json_utf8, uint32_t& out_version, std::vector<SongData>& out_songs, std::wstring& err) {
  JsonReader jr(json_utf8);
  out_songs.clear();
  out_version = 0;

  if (!jr.expect('{', err)) return false;
  bool have_ver = false;
  bool have_data = false;
  if (!jr.consume('}')) {
    while (true) {
      std::string key;
      if (!jr.read_string(key, err)) return false;
      if (!jr.expect(':', err)) return false;

      if (key == "data_ver") {
        double d = 0;
        if (!jr.read_number(d, err)) return false;
        out_version = json_to_u32(d);
        have_ver = true;
      } else if (key == "data") {
        if (!jr.expect('[', err)) return false;
        have_data = true;
        if (!jr.consume(']')) {
          while (true) {
            SongData song;
            if (!parse_song_object(jr, out_version, song, err)) return false;
            out_songs.emplace_back(std::move(song));
            if (jr.consume(']')) break;
            if (!jr.expect(',', err)) return false;
          }
        }
      } else {
        if (!jr.skip_value(err)) return false;
      }

      if (jr.consume('}')) break;
      if (!jr.expect(',', err)) return false;
    }
  }

  if (!have_ver) {
    err = L"Missing data_ver.";
    return false;
  }
  if (!have_data) {
    err = L"Missing data array.";
    return false;
  }

  jr.skip_ws();
  if (!jr.eof()) {
    err = L"Invalid JSON: trailing content.";
    return false;
  }
  return true;
}

}  // namespace

}  // namespace

bool musicdata_extract_json(const std::vector<uint8_t>& bin, std::string& out_json_utf8, std::wstring& err) {
  err.clear();
  // Avoid repeated reallocations during pretty-print serialization.
  // Typical output size is close to input size for music_*.bin, so this is a good lower bound.
  out_json_utf8.clear();
  if (out_json_utf8.capacity() < bin.size() + (bin.size() / 4)) out_json_utf8.reserve(bin.size() + (bin.size() / 4));
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

  // Streaming JSON writer (skips DOM construction) while preserving the exact
  // indentation, commas, and key order of the legacy extract output.
  out_json_utf8 += "{\n";
  append_indent_units(out_json_utf8, 1);
  append_escaped_json_string(out_json_utf8, "data_ver");
  out_json_utf8 += ": ";
  append_json_u32(out_json_utf8, version);
  out_json_utf8 += ",\n";
  append_indent_units(out_json_utf8, 1);
  append_escaped_json_string(out_json_utf8, "data");
  out_json_utf8 += ": ";

  if (available_entries == 0) {
    out_json_utf8 += "[]\n";
    out_json_utf8 += '}';
    return true;
  }

  out_json_utf8 += "[\n";
  SongScratch scratch{};
  for (uint32_t i = 0; i < available_entries; ++i) {
    if (br.pos > br.size) break;
    append_indent_units(out_json_utf8, 2);
    append_one_song_from_bin_python(out_json_utf8, br, version, scratch, 1);
    if (i + 1 < available_entries)
      out_json_utf8 += ",\n";
    else
      out_json_utf8 += '\n';
  }
  append_indent_units(out_json_utf8, 1);
  out_json_utf8 += "]\n";
  out_json_utf8 += '}';
  return true;
}

bool musicdata_create_bin(const std::string& json_utf8, std::vector<uint8_t>& out_bin, std::wstring& err) {
  err.clear();
  out_bin.clear();
  uint32_t version = 0;
  std::vector<SongData> songs;
  if (!parse_musicdata_root(json_utf8, version, songs, err)) {
    if (err.empty()) err = L"Invalid JSON (expected MusicDataPlugin schema).";
    return false;
  }
  if (!is_supported_version(version)) {
    err = L"Unsupported data_ver.";
    return false;
  }
  BinWriter bw;
  writer_impl_typed(version, bw, songs);
  out_bin = std::move(bw.buf);
  return true;
}
