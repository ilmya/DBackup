#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <cwctype>

namespace repository_path_safety {
inline std::wstring utf8ToWide(const std::string &value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count)) return {};
    return result;
}
inline bool isHex(const std::string &value, size_t expected) {
    if (value.size() != expected) return false;
    for (unsigned char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool validSnapshotId(const std::string &id) {
    return id.size() == 30 && id[13] == '-' && std::all_of(id.begin(), id.begin() + 13, [](char c) { return c >= '0' && c <= '9'; }) && isHex(id.substr(14), 16);
}
inline bool safeRelativePath(const std::string &path) {
    const std::wstring value = utf8ToWide(path);
    if (value.empty() || value.front() == L'/' || value.front() == L'\\' || value.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find_first_of(L"/\\", start);
        const std::wstring part = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (wchar_t c : part) if (c < 32 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') return false;
        std::wstring stem = part.substr(0, part.find(L'.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' && stem[3] <= L'9')) return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}
inline std::wstring absolutePath(const std::wstring &path) {
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > 32767) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    const DWORD count = GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
    if (!count || count >= needed) return {};
    result.resize(count); std::replace(result.begin(), result.end(), L'/', L'\\'); return result;
}
inline bool isWithinOrEqual(const std::wstring &candidate, const std::wstring &base) {
    auto normalize = [](std::wstring value) { std::replace(value.begin(), value.end(), L'/', L'\\'); while (value.size() > 3 && value.back() == L'\\') value.pop_back(); return value; };
    const std::wstring child = normalize(candidate), parent = normalize(base);
    if (child.size() < parent.size() || CompareStringOrdinal(child.data(), static_cast<int>(parent.size()), parent.data(), static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL) return false;
    return child.size() == parent.size() || child[parent.size()] == L'\\';
}
inline bool ensureSafeDirectories(const std::wstring &requested) {
    std::wstring full = absolutePath(requested);
    if (full.empty() || full.rfind(L"\\\\?\\", 0) == 0 || full.rfind(L"\\\\.\\", 0) == 0) return false;
    size_t offset = 0; std::wstring current;
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') { current = full.substr(0, 3); offset = 3; }
    else if (full.rfind(L"\\\\", 0) == 0) { const size_t server = full.find(L'\\', 2); if (server == std::wstring::npos) return false; const size_t share = full.find(L'\\', server + 1); if (share == std::wstring::npos) { current = full; offset = full.size(); } else { current = full.substr(0, share + 1); offset = share + 1; } }
    else return false;
    auto validate = [](const std::wstring &path) { DWORD attributes = GetFileAttributesW(path.c_str()); if (attributes == INVALID_FILE_ATTRIBUTES) { if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false; attributes = GetFileAttributesW(path.c_str()); } return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT); };
    if (!validate(current)) return false;
    while (offset < full.size()) { while (offset < full.size() && full[offset] == L'\\') ++offset; if (offset >= full.size()) break; const size_t end = full.find(L'\\', offset); current += full.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset); if (!validate(current)) return false; current += L'\\'; if (end == std::wstring::npos) break; offset = end + 1; }
    return true;
}
}  // namespace repository_path_safety
