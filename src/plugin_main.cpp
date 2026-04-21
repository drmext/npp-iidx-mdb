#include "musicdata_io.h"
#include "npp_defs.h"

#include <shlwapi.h>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "Shlwapi.lib")

#define SCI_SETTEXT 2181
#define SCI_GETLENGTH 2006
#define SCI_GETTEXT 2182

static const wchar_t kPluginName[] = L"MusicDataPlugin";

static NppData g_npp;
static HMODULE g_hModule = nullptr;
static std::unordered_set<UINT_PTR> g_musicBinBuffers;
static bool g_inPluginNotify = false;

static bool restore_last_write_time(const wchar_t* path, const FILETIME& ft) {
  HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
  return ok != FALSE;
}

static bool path_is_music_bin(const wchar_t* path) {
  if (!path || !path[0]) return false;
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* fname = slash ? slash + 1 : path;
  return PathMatchSpecW(fname, L"music_*.bin") != FALSE;
}

static bool read_file_bytes(const wchar_t* path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (1LL << 31)) {
    CloseHandle(h);
    return false;
  }
  out.resize(static_cast<size_t>(sz.QuadPart));
  DWORD rd = 0;
  BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &rd, nullptr);
  CloseHandle(h);
  return ok && rd == out.size();
}

static bool write_file_bytes(const wchar_t* path, const std::vector<uint8_t>& data) {
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wr = 0;
  BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr);
  CloseHandle(h);
  return ok && wr == data.size();
}

static bool get_buffer_full_path(UINT_PTR bufferID, std::wstring& out) {
  HWND npp = g_npp._nppHandle;
  int len = static_cast<int>(SendMessageW(npp, NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferID), 0));
  if (len <= 0) return false;
  out.assign(static_cast<size_t>(len) + 1, L'\0');
  int got = static_cast<int>(
      SendMessageW(npp, NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferID), reinterpret_cast<LPARAM>(out.data())));
  if (got <= 0) return false;
  out.resize(static_cast<size_t>(got));
  return true;
}

static bool activate_buffer(UINT_PTR bufferID) {
  HWND npp = g_npp._nppHandle;
  int pos = static_cast<int>(SendMessage(npp, NPPM_GETPOSFROMBUFFERID, static_cast<WPARAM>(bufferID), MAIN_VIEW));
  if (pos < 0) pos = static_cast<int>(SendMessage(npp, NPPM_GETPOSFROMBUFFERID, static_cast<WPARAM>(bufferID), SUB_VIEW));
  if (pos < 0) return false;
  unsigned up = static_cast<unsigned>(pos);
  int view = static_cast<int>(up >> 30);
  int index = static_cast<int>(up & 0x3FFFFFFFu);
  return SendMessage(npp, NPPM_ACTIVATEDOC, static_cast<WPARAM>(view), static_cast<LPARAM>(index)) == TRUE;
}

static HWND current_scintilla() {
  int which = 0;
  SendMessage(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
  return which ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

static bool get_utf8_buffer_text(std::string& out) {
  HWND sci = current_scintilla();
  LRESULT doc_len = SendMessage(sci, SCI_GETLENGTH, 0, 0);
  if (doc_len < 0) return false;
  size_t n = static_cast<size_t>(doc_len);
  if (n == 0) {
    out.clear();
    return true;
  }
  out.resize(n + 1, '\0');
  SendMessage(sci, SCI_GETTEXT, static_cast<WPARAM>(n + 1), reinterpret_cast<LPARAM>(&out[0]));
  out.resize(n);
  return true;
}

static void set_buffer_utf8_text(UINT_PTR bufferID, const std::string& utf8) {
  activate_buffer(bufferID);
  HWND sci = current_scintilla();
  const LRESULT change_history_flags = SendMessage(sci, SCI_GETCHANGEHISTORY, 0, 0);
  SendMessage(sci, SCI_SETCHANGEHISTORY, static_cast<WPARAM>(SC_CHANGE_HISTORY_DISABLED), 0);
  SendMessage(sci, SCI_SETUNDOCOLLECTION, 0, 0);
  SendMessage(sci, SCI_SETCODEPAGE, static_cast<WPARAM>(SC_CP_UTF8), 0);
  SendMessage(sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(utf8.c_str()));
  SendMessage(g_npp._nppHandle, NPPM_SETBUFFERLANGTYPE, static_cast<WPARAM>(bufferID), static_cast<LPARAM>(NPP_L_JSON));
  sci = current_scintilla();
  SendMessage(sci, SCI_SETUNDOCOLLECTION, static_cast<WPARAM>(TRUE), 0);
  SendMessage(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
  SendMessage(sci, SCI_SETSAVEPOINT, 0, 0);
  SendMessage(sci, SCI_SETCHANGEHISTORY, static_cast<WPARAM>(change_history_flags), 0);
}

static void show_err(const wchar_t* msg) { MessageBoxW(g_npp._nppHandle, msg, kPluginName, MB_OK | MB_ICONWARNING); }

// PluginsManager requires _nbFuncItem > 0 (see Notepad++ PluginsManager.cpp).
static void cmd_about() {
  MessageBoxW(
      g_npp._nppHandle,
      L"Opens files matching music_*.bin as JSON and writes binary on save.\n"
      L"Tab may still show .bin; lexer is JSON.\n"
      L"Unknown regions match musicdata_tool.py fixed padding.\n"
      L"After save, the file's last-write time may match the JSON save (avoids reload prompts).",
      kPluginName,
      MB_OK | MB_ICONINFORMATION);
}

static FuncItem s_funcItems[] = {
    {L"About MusicDataPlugin...", cmd_about, 0, false, nullptr},
};

extern "C" __declspec(dllexport) void setInfo(NppData data) { g_npp = data; }

extern "C" __declspec(dllexport) const wchar_t* getName() { return kPluginName; }

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nb) {
  *nb = static_cast<int>(sizeof(s_funcItems) / sizeof(s_funcItems[0]));
  return s_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notify) {
  if (!notify || g_inPluginNotify) return;
  const int code = notify->nmhdr.code;
  const UINT_PTR bufferID = notify->nmhdr.idFrom;

  if (code == NPPN_FILEOPENED) {
    std::wstring path;
    if (!get_buffer_full_path(bufferID, path)) return;
    if (!path_is_music_bin(path.c_str())) return;

    std::vector<uint8_t> raw;
    if (!read_file_bytes(path.c_str(), raw)) return;
    if (raw.size() < 4 || memcmp(raw.data(), "IIDX", 4) != 0) return;

    std::string json;
    std::wstring err;
    if (!musicdata_extract_json(raw, json, err)) {
      show_err(err.c_str());
      return;
    }

    g_inPluginNotify = true;
    set_buffer_utf8_text(bufferID, json);
    g_inPluginNotify = false;
    g_musicBinBuffers.insert(bufferID);
    return;
  }

  if (code == NPPN_FILECLOSED) {
    g_musicBinBuffers.erase(bufferID);
    return;
  }

  if (code == NPPN_FILESAVED) {
    if (g_musicBinBuffers.count(bufferID) == 0) return;
    activate_buffer(bufferID);
    std::string text;
    if (!get_utf8_buffer_text(text)) {
      show_err(L"Could not read editor text for save.");
      return;
    }
    std::vector<uint8_t> bin;
    std::wstring err;
    if (!musicdata_create_bin(text, bin, err)) {
      show_err(err.c_str());
      return;
    }
    std::wstring path;
    if (!get_buffer_full_path(bufferID, path)) {
      show_err(L"Could not resolve file path after save.");
      return;
    }
    bool have_ts = false;
    FILETIME ft_before{};
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
      ft_before = fad.ftLastWriteTime;
      have_ts = true;
    }
    if (!write_file_bytes(path.c_str(), bin)) {
      show_err(L"Failed to write musicdata .bin after save.");
      return;
    }
    if (have_ts && !restore_last_write_time(path.c_str(), ft_before)) {
      // Non-fatal: user may still see a reload prompt.
    }
  }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) { return FALSE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) g_hModule = hModule;
  return TRUE;
}
