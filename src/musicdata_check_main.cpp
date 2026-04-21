// Console helper to compare C++ codec with musicdata_tool.py (see plugin/tools/roundtrip_compare.py).

#include "musicdata_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static bool read_all(const wchar_t* path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  auto sz = f.tellg();
  if (sz <= 0) return false;
  f.seekg(0);
  out.resize(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(out.data()), sz);
  return static_cast<bool>(f);
}

static bool write_all(const wchar_t* path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

static bool write_text(const wchar_t* path, const std::string& utf8) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  return static_cast<bool>(f);
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::fwprintf(stderr,
                  L"Usage:\n"
                  L"  MusicDataCheck extract <input.bin> <out.json>\n"
                  L"  MusicDataCheck create <input.json> <out.bin>\n");
    return 1;
  }
  std::wstring cmd = argv[1];
  if (cmd == L"extract") {
    std::vector<uint8_t> raw;
    if (!read_all(argv[2], raw)) {
      std::fwprintf(stderr, L"Failed to read input bin.\n");
      return 2;
    }
    std::string json;
    std::wstring err;
    if (!musicdata_extract_json(raw, json, err)) {
      std::fwprintf(stderr, L"extract: %ls\n", err.c_str());
      return 3;
    }
    if (!write_text(argv[3], json)) {
      std::fwprintf(stderr, L"Failed to write json.\n");
      return 4;
    }
    return 0;
  }
  if (cmd == L"create") {
    std::vector<uint8_t> raw;
    if (!read_all(argv[2], raw)) {
      std::fwprintf(stderr, L"Failed to read input json.\n");
      return 2;
    }
    std::string json(reinterpret_cast<const char*>(raw.data()), raw.size());
    std::vector<uint8_t> bin;
    std::wstring err;
    if (!musicdata_create_bin(json, bin, err)) {
      std::fwprintf(stderr, L"create: %ls\n", err.c_str());
      return 3;
    }
    if (!write_all(argv[3], bin)) {
      std::fwprintf(stderr, L"Failed to write bin.\n");
      return 4;
    }
    return 0;
  }
  std::fwprintf(stderr, L"Unknown command.\n");
  return 1;
}
