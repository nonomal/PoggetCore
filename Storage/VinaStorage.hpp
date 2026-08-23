// vina_storage.hpp
#ifndef VINA_STORAGE_HPP
#define VINA_STORAGE_HPP

#include "VinaBuilder.hpp" 
#include "vui.parser.hpp" 
#include "../tsl/ordered_map.h"
#include <algorithm>
#include <charconv>
#include <codecvt>
#include <variant>
#include <string>
#include <memory> 
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <type_traits> 
#include <cstring> 
#include <any> 
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <optional>

#ifdef _WIN32
#include <Windows.h>
#else

#endif




struct VinaStorageNestedObject;


using VinaStorageValue = std::variant<int, double, bool, std::wstring, std::shared_ptr<VinaStorageNestedObject>>;


using VinaStorageObjectMap = tsl::ordered_map<std::wstring, VinaStorageValue>;

struct VinaStorageNestedObject {
    VinaStorageObjectMap data;
};

enum class VinaStorageError {
    none,
    created,
    recovered_backup,
    invalid_argument,
    not_loaded,
    format_error,
    resource_limit,
    encoding_error,
    io_error,
    recovery_required,
    conflict,
    busy,
    save_error
};

struct VinaStorageResult {
    VinaStorageError error = VinaStorageError::none;
    const char* message = "";

    bool ok() const noexcept {
        return error == VinaStorageError::none || error == VinaStorageError::created ||
            error == VinaStorageError::recovered_backup;
    }
    explicit operator bool() const noexcept { return ok(); }
};



#ifdef _WIN32

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

inline ULONGLONG VinaStorageTickCount() noexcept {
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0600
    return GetTickCount64();
#else
    return static_cast<ULONGLONG>(GetTickCount());
#endif
}

class VinaStorageProcessLock {
public:
    explicit VinaStorageProcessLock(const std::wstring& path) {
        std::error_code ec;
        std::wstring normalized_path = std::filesystem::absolute(path, ec)
            .lexically_normal().wstring();
        if (ec) normalized_path = path;

        std::uint64_t hash = 1469598103934665603ULL;
        for (wchar_t c : normalized_path) {
            const wchar_t normalized = static_cast<wchar_t>(std::towlower(c));
            hash ^= static_cast<std::uint64_t>(normalized);
            hash *= 1099511628211ULL;
        }
        const std::wstring name = L"Local\\VinaStorage_" + std::to_wstring(hash);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) return;
        const DWORD wait_result = WaitForSingleObject(handle_, 5000);
        locked_ = wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
    }

    ~VinaStorageProcessLock() {
        if (locked_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    VinaStorageProcessLock(const VinaStorageProcessLock&) = delete;
    VinaStorageProcessLock& operator=(const VinaStorageProcessLock&) = delete;
    bool locked() const noexcept { return locked_; }

private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

inline bool TryWStringToUTF8(const std::wstring& wstr, std::string& result) {
    result.clear();
    if (wstr.empty()) return true;
    if (wstr.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;

    const int input_size = static_cast<int>(wstr.size());
    const int size_needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wstr.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return false;

    result.resize(static_cast<std::size_t>(size_needed));
    const int converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wstr.data(), input_size,
        result.data(), size_needed, nullptr, nullptr);
    if (converted != size_needed) {
        result.clear();
        return false;
    }
    return true;
}

inline std::string WStringToUTF8(const std::wstring& wstr) {
    std::string result;
    TryWStringToUTF8(wstr, result);
    return result;
}

// 将 std::string (UTF-8) 转换为 std::wstring (UTF-16)
inline bool TryUTF8ToWString(const std::string& str, std::wstring& result) {
    result.clear();
    if (str.empty()) return true;
    if (str.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;

    const int input_size = static_cast<int>(str.size());
    const int size_needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), input_size, nullptr, 0);
    if (size_needed <= 0) return false;

    result.resize(static_cast<std::size_t>(size_needed));
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), input_size, result.data(), size_needed);
    if (converted != size_needed) {
        result.clear();
        return false;
    }
    return true;
}

inline std::wstring UTF8ToWString(const std::string& str) {
    std::wstring result;
    TryUTF8ToWString(str, result);
    return result;
}
#else
inline bool TryWStringToUTF8(const std::wstring& wstr, std::string& result) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
        result = converter.to_bytes(wstr);
        return true;
    }
    catch (const std::range_error&) {
        result.clear();
        return false;
    }
}

inline bool TryUTF8ToWString(const std::string& str, std::wstring& result) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
        result = converter.from_bytes(str);
        return true;
    }
    catch (const std::range_error&) {
        result.clear();
        return false;
    }
}

inline std::string WStringToUTF8(const std::wstring& wstr) {
    std::string result;
    TryWStringToUTF8(wstr, result);
    return result;
}

inline std::wstring UTF8ToWString(const std::string& str) {
    std::wstring result;
    TryUTF8ToWString(str, result);
    return result;
}
#endif

// --- VinaStorage 类定义 ---
class VinaStorage {
private:
    struct FileStamp {
        bool exists = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type write_time{};

        bool operator==(const FileStamp& other) const noexcept {
            return exists == other.exists && (!exists ||
                (size == other.size && write_time == other.write_time));
        }
    };

    std::wstring filename_;
    std::wstring backup_path_;
    // 根对象列表也改为有序 Map，以保持多个根对象的顺序
    tsl::ordered_map<std::wstring, VinaStorageObjectMap> root_objects_;
    bool loaded_ = false; // Add a flag to track if file is loaded
    bool writable_ = false;
    mutable std::mutex io_mutex_;
    VinaStorageError state_error_ = VinaStorageError::none;
    vui::parser::parser_limits parser_limits_{};
    vui::parser::parser_error parser_error_ = vui::parser::parser_error::none;
    VinaStorageError load_error_ = VinaStorageError::none;
    std::optional<FileStamp> loaded_stamp_;
    std::atomic<std::uint64_t> mutation_generation_{ 0 };
    std::uint64_t saved_generation_ = 0;
    mutable std::atomic_bool mutation_tracking_uncertain_{ false };

    static FileStamp getFileStamp(const std::wstring& filename) {
        FileStamp stamp;
        std::error_code ec;
        const std::filesystem::path path(filename);
        stamp.exists = std::filesystem::is_regular_file(path, ec);
        if (ec || !stamp.exists) return stamp;
        stamp.size = std::filesystem::file_size(path, ec);
        if (ec) return FileStamp{};
        stamp.write_time = std::filesystem::last_write_time(path, ec);
        if (ec) return FileStamp{};
        return stamp;
    }

    void markMutated() noexcept {
        mutation_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    void markMutationTrackingUncertain() const noexcept {
        mutation_tracking_uncertain_.store(true, std::memory_order_relaxed);
    }

    // ... (convertBasicObjectToMap, isEscapedPath, hasMultipleConsecutiveBackslashes, populateVinaObject 保持不变) ...
    // 为了保持您提供的代码的完整性和结构，我将这些函数体粘贴在下面

    // START: 保持不变的代码片段

    VinaStorageObjectMap convertBasicObjectToMap(const vui::parser::basic_object<wchar_t>& basic_obj) {
        VinaStorageObjectMap converted_map;

        for (const std::wstring& key : basic_obj.order()) {
            const std::any& value_any = basic_obj[key];

            if (value_any.type() == typeid(std::wstring)) {
                converted_map[key] = std::any_cast<const std::wstring&>(value_any);
            }
            else if (value_any.type() == typeid(int)) {
                converted_map[key] = std::any_cast<int>(value_any);
            }
            else if (value_any.type() == typeid(double)) {
                converted_map[key] = std::any_cast<double>(value_any);
            }
            else if (value_any.type() == typeid(bool)) {
                converted_map[key] = std::any_cast<bool>(value_any);
            }
            else if (value_any.type() == typeid(vui::parser::basic_object<wchar_t>)) {
                const auto& nested_basic_obj =
                    std::any_cast<const vui::parser::basic_object<wchar_t>&>(value_any);
                auto nested_map = convertBasicObjectToMap(nested_basic_obj);
                auto nested_obj_ptr = std::make_shared<VinaStorageNestedObject>();
                nested_obj_ptr->data = std::move(nested_map);
                converted_map[key] = nested_obj_ptr;
            }
            else {
                std::wcerr << L"Warning: Unknown type for key '" << key << L"' during conversion. Skipping." << std::endl;
            }
        }

        return converted_map;
    }

    // 检查字符串是否已经是转义过的路径格式 (例如 C:\\path\\to\\file)
    bool isEscapedPath(const std::wstring& str) {
        if (str.length() < 3) return false;

        // 检查是否是驱动器路径格式 C:\\ 或者网络路径 \\server
        if ((str[1] == L':' && str[2] == L'\\')) { // C:\ 格式
            // 检查后续是否有连续的反斜杠
            for (size_t i = 3; i < str.length(); ++i) {
                if (str[i] == L'\\') {
                    // 检查前一个字符是否也是反斜杠
                    if (i > 0 && str[i - 1] == L'\\') {
                        // 这是一个已转义的路径
                        continue;
                    }
                }
            }
            // 简单检查：如果字符串包含冒号后跟反斜杠，很可能是路径
            size_t colon_pos = str.find(L':');
            if (colon_pos != std::wstring::npos && colon_pos + 1 < str.length() && str[colon_pos + 1] == L'\\') {
                // 统计反斜杠的数量，如果是偶数个连续的反斜杠，可能是已转义的
                size_t backslash_count = 0;
                bool has_consecutive_backslashes = false;
                for (size_t i = 0; i < str.length(); ++i) {
                    if (str[i] == L'\\') {
                        backslash_count++;
                        if (i > 0 && str[i - 1] == L'\\') {
                            has_consecutive_backslashes = true;
                        }
                    }
                    else {
                        backslash_count = 0;
                    }
                }
                // 如果有连续的反斜杠，可能是已转义的
                return has_consecutive_backslashes;
            }
        }
        // 检查网络路径 \\server\share
        else if (str.length() >= 2 && str[0] == L'\\' && str[1] == L'\\') {
            size_t third_slash = str.find(L'\\', 2);
            if (third_slash != std::wstring::npos) {
                return true;
            }
        }

        return false;
    }

    // 检查字符串是否包含多个连续的反斜杠
    bool hasMultipleConsecutiveBackslashes(const std::wstring& str) {
        for (size_t i = 1; i < str.length(); ++i) {
            if (str[i] == L'\\' && str[i - 1] == L'\\') {
                // 检查是否是很多个连续的反斜杠
                size_t count = 2;
                size_t j = i + 1;
                while (j < str.length() && str[j] == L'\\') {
                    count++;
                    j++;
                }
                if (count >= 2) {
                    return true;
                }
                i = j - 1; // 跳过已检查的部分
            }
        }
        return false;
    }

    static bool isSyntaxWhitespace(wchar_t c) noexcept {
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
    }

    static bool isValidObjectName(const std::wstring& name) noexcept {
        if (name.empty() || name.front() == L'@' || isSyntaxWhitespace(name.front()) ||
            isSyntaxWhitespace(name.back())) {
            return false;
        }
        return name.find(L'{') == std::wstring::npos && name.find(L'^') == std::wstring::npos;
    }

    static bool isValidMemberName(const std::wstring& name) noexcept {
        if (name.empty()) return false;
        for (wchar_t c : name) {
            if (isSyntaxWhitespace(c) || c == L'(' || c == L':' || c == L'{' ||
                c == L',' || c == L'}') {
                return false;
            }
        }
        return true;
    }

    static std::wstring formatDouble(double value) {
        char buffer[64]{};
        const auto converted = std::to_chars(
            buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        if (converted.ec != std::errc{}) {
            throw std::runtime_error("Could not format floating-point value.");
        }
        std::wstring result;
        result.reserve(static_cast<std::size_t>(converted.ptr - buffer) + 2);
        for (const char* p = buffer; p != converted.ptr; ++p) {
            result.push_back(static_cast<wchar_t>(*p));
        }
        if (result.find_first_of(L".eE") == std::wstring::npos) result += L".0";
        return result;
    }

    static double normalizeFloat(float value) {
        if (!std::isfinite(value)) return static_cast<double>(value);
        char buffer[64]{};
        const auto written = std::to_chars(
            buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        if (written.ec != std::errc{}) return static_cast<double>(value);
        double normalized = 0.0;
        const auto parsed = std::from_chars(buffer, written.ptr, normalized, std::chars_format::general);
        return parsed.ec == std::errc{} && parsed.ptr == written.ptr
            ? normalized : static_cast<double>(value);
    }

    void populateVinaObject(VinaObject* vina_obj, const VinaStorageObjectMap& data_map) {
        // 由于 data_map 现在是 tsl::ordered_map，迭代顺序将严格按照文件中的顺序
        for (const auto& item : data_map) {
            const std::wstring& key = item.first;
            const VinaStorageValue& value_variant = item.second;

            if (!isValidMemberName(key)) {
                throw std::runtime_error("Storage member name contains unsupported syntax characters: " +
                    WStringToUTF8(key));
            }

            std::visit([vina_obj, &key, this](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, int>) {
                    vina_obj->AddData(key, std::to_wstring(val));
                }
                else if constexpr (std::is_same_v<T, double>) {
                    if (!std::isfinite(val)) {
                        throw std::runtime_error("Storage does not support non-finite floating-point values.");
                    }
                    vina_obj->AddData(key, formatDouble(val));
                }
                else if constexpr (std::is_same_v<T, bool>) {
                    vina_obj->AddData(key, val ? L"true" : L"false");
                }
                else if constexpr (std::is_same_v<T, std::wstring>) {
                    std::wstring escaped_val;
                    escaped_val.reserve(val.length() * 2 + 2);
                    escaped_val += L'"';
                    for (wchar_t c : val) {
                        if (c == L'\\') {
                            escaped_val += L"\\\\";
                        }
                        else if (c == L'"') {
                            escaped_val += L"\\\"";
                        }
                        else {
                            escaped_val += c;
                        }
                    }
                    escaped_val += L'"';
                    vina_obj->AddData(key, escaped_val);
                }
                else if constexpr (std::is_same_v<T, std::shared_ptr<VinaStorageNestedObject>>) {
                    if (val) {
                        VinaObject* nested_vina_obj = vina_obj->AddObject(key);
                        populateVinaObject(nested_vina_obj, val->data);
                    }
                }
                }, value_variant);
        }
    }

private:
    bool LoadInternal(const std::wstring& filename) {
        load_error_ = VinaStorageError::io_error;
        std::ifstream file_stream{
            std::filesystem::path(filename), std::ios::in | std::ios::binary | std::ios::ate
        };
        if (!file_stream.is_open()) {
            return false;
        }

        const std::streamoff file_size = file_stream.tellg();
        if (file_size < 0 ||
            static_cast<std::uintmax_t>(file_size) > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::uintmax_t>(file_size) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        if (parser_limits_.max_input_bytes != 0 &&
            static_cast<std::uintmax_t>(file_size) > parser_limits_.max_input_bytes) {
            load_error_ = VinaStorageError::resource_limit;
            return false;
        }

        std::string utf8_content(static_cast<std::size_t>(file_size), '\0');
        file_stream.seekg(0, std::ios::beg);
        if (!utf8_content.empty() &&
            !file_stream.read(utf8_content.data(), static_cast<std::streamsize>(utf8_content.size()))) {
            return false;
        }
        if (utf8_content.size() >= 3 &&
            static_cast<unsigned char>(utf8_content[0]) == 0xEF &&
            static_cast<unsigned char>(utf8_content[1]) == 0xBB &&
            static_cast<unsigned char>(utf8_content[2]) == 0xBF) {
            utf8_content.erase(0, 3);
        }

        std::wstring wcontent;
        if (!TryUTF8ToWString(utf8_content, wcontent)) {
            load_error_ = VinaStorageError::encoding_error;
            return false;
        }
        if (std::all_of(wcontent.begin(), wcontent.end(), isSyntaxWhitespace)) {
            root_objects_.clear();
            return true;
        }
        std::wstringstream wide_stream(wcontent);
        vui::parser::basic_parser<std::wstringstream, wchar_t> parser(std::move(wide_stream));
        parser.set_limits(parser_limits_);

        if (!parser.parse()) {
            parser_error_ = parser.error();
            load_error_ = parser_error_ == vui::parser::parser_error::resource_limit
                ? VinaStorageError::resource_limit : VinaStorageError::format_error;
            return false;
        }
        parser_error_ = vui::parser::parser_error::none;

        tsl::ordered_map<std::wstring, VinaStorageObjectMap> parsed_objects;
        for (auto parsed_root_obj : parser) {
            std::wstring obj_name = parsed_root_obj.name();
            VinaStorageObjectMap converted_map = convertBasicObjectToMap(parsed_root_obj);
            parsed_objects[obj_name] = std::move(converted_map);
        }

        root_objects_ = std::move(parsed_objects);
        load_error_ = VinaStorageError::none;
        return true;
    }


public:

    VinaStorage() : filename_(L""), loaded_(false) {}

    void SetLimits(vui::parser::parser_limits limits) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        parser_limits_ = limits;
    }

    vui::parser::parser_limits GetLimits() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return parser_limits_;
    }

    VinaStorageError LastError() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return state_error_;
    }

    VinaStorageResult TryLoad(const std::wstring& filename) noexcept {
        try {
            Load(filename);
            return { state_error_, "" };
        }
        catch (const std::bad_alloc&) {
            state_error_ = VinaStorageError::resource_limit;
            loaded_ = false;
            writable_ = false;
            return { VinaStorageError::resource_limit, "Storage load ran out of memory." };
        }
        catch (const std::exception& e) {
            (void)e;
            if (state_error_ == VinaStorageError::none) {
                state_error_ = VinaStorageError::recovery_required;
            }
            loaded_ = false;
            writable_ = false;
            return { state_error_, "Storage load failed; inspect the error code and source files." };
        }
        catch (...) {
            state_error_ = VinaStorageError::recovery_required;
            loaded_ = false;
            writable_ = false;
            return { VinaStorageError::recovery_required, "Unknown storage load failure." };
        }
    }

    VinaStorageResult TrySave() noexcept {
        try {
            Save();
            return { VinaStorageError::none, "" };
        }
        catch (const std::bad_alloc&) {
            state_error_ = VinaStorageError::resource_limit;
            return { VinaStorageError::resource_limit, "Storage save ran out of memory." };
        }
        catch (const std::exception& e) {
            (void)e;
            if (state_error_ != VinaStorageError::conflict && state_error_ != VinaStorageError::busy &&
                state_error_ != VinaStorageError::not_loaded) {
                state_error_ = VinaStorageError::save_error;
            }
            return { state_error_, "Storage save failed; existing files were not overwritten." };
        }
        catch (...) {
            state_error_ = VinaStorageError::save_error;
            return { VinaStorageError::save_error, "Unknown storage save failure." };
        }
    }

    void SetBackupPath(const std::wstring& backup_path) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        backup_path_ = backup_path;
    }

    void Load(const std::wstring& filename) {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        filename_ = filename;
        loaded_ = false;
        writable_ = false;
        state_error_ = VinaStorageError::none;
        load_error_ = VinaStorageError::none;

        if (filename.empty()) {
            state_error_ = VinaStorageError::invalid_argument;
            throw std::invalid_argument("Storage filename cannot be empty.");
        }

        const std::wstring backup_filename =
            backup_path_.empty() ? (filename + L".bak") : backup_path_;

        // Check if primary file exists
#ifdef _WIN32
        DWORD dwAttrs = GetFileAttributesW(filename.c_str());
        if (dwAttrs != INVALID_FILE_ATTRIBUTES && (dwAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            state_error_ = VinaStorageError::invalid_argument;
            throw std::invalid_argument("Storage path refers to a directory.");
        }
        bool file_exists = (dwAttrs != INVALID_FILE_ATTRIBUTES && !(dwAttrs & FILE_ATTRIBUTE_DIRECTORY));
        DWORD dwBackupAttrs = GetFileAttributesW(backup_filename.c_str());
        bool backup_exists =
            (dwBackupAttrs != INVALID_FILE_ATTRIBUTES && !(dwBackupAttrs & FILE_ATTRIBUTE_DIRECTORY));
#else
        std::ifstream file_check(std::filesystem::path(filename), std::ios::binary);
        bool file_exists = file_check.good();
        file_check.close();
        std::ifstream backup_check(std::filesystem::path(backup_filename), std::ios::binary);
        bool backup_exists = backup_check.good();
        backup_check.close();
#endif

        if (!file_exists) {
            if (backup_exists && LoadInternal(backup_filename)) {
                loaded_ = true;
                writable_ = false;
                state_error_ = VinaStorageError::recovered_backup;
                std::wcout << L"VinaStorage primary file was missing; loaded backup '"
                    << backup_filename << L"'." << std::endl;
#ifdef _WIN32
                static std::atomic<unsigned long long> missing_restore_sequence{ 0 };
                const std::wstring restore_temp = filename + L".restore." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(VinaStorageTickCount()) + L"." +
                    std::to_wstring(++missing_restore_sequence);
                if (CopyFileW(backup_filename.c_str(), restore_temp.c_str(), TRUE)) {
                    if (!MoveFileExW(restore_temp.c_str(), filename.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        DeleteFileW(restore_temp.c_str());
                    }
                    else {
                        writable_ = true;
                    }
                }
#else
                std::ifstream src(std::filesystem::path(backup_filename), std::ios::binary);
                std::ofstream dst(std::filesystem::path(filename), std::ios::binary | std::ios::trunc);
                dst << src.rdbuf();
                writable_ = src.good() && dst.good();
#endif
                loaded_stamp_ = getFileStamp(filename);
                mutation_generation_.store(0, std::memory_order_relaxed);
                saved_generation_ = 0;
                mutation_tracking_uncertain_.store(false, std::memory_order_relaxed);
                if (!writable_) {
                    state_error_ = VinaStorageError::recovery_required;
                    throw std::runtime_error("Backup loaded, but the primary file could not be restored safely.");
                }
                return;
            }
            if (backup_exists) {
                state_error_ = load_error_ == VinaStorageError::resource_limit
                    ? VinaStorageError::resource_limit : VinaStorageError::recovery_required;
                loaded_ = false;
                writable_ = false;
                throw std::runtime_error(
                    "Storage primary is missing and the existing backup could not be loaded safely.");
            }
            std::wcout << L"Info: File '" << filename << L"' does not exist. Creating new storage." << std::endl;
            root_objects_.clear();
            loaded_ = true;
            writable_ = true;
            state_error_ = VinaStorageError::created;
            loaded_stamp_ = getFileStamp(filename);
            mutation_generation_.store(0, std::memory_order_relaxed);
            saved_generation_ = 0;
            mutation_tracking_uncertain_.store(false, std::memory_order_relaxed);
            return;
        }

        // Try loading primary file
        if (LoadInternal(filename)) {
            loaded_ = true;
            writable_ = true;
            state_error_ = VinaStorageError::none;
            loaded_stamp_ = getFileStamp(filename);
            mutation_generation_.store(0, std::memory_order_relaxed);
            saved_generation_ = 0;
            mutation_tracking_uncertain_.store(false, std::memory_order_relaxed);
            std::wcout << L"VinaStorage loaded from '" << filename << L"' (" << root_objects_.size() << L" root objects)." << std::endl;
            return;
        }

        const VinaStorageError primary_error = load_error_;
        if (primary_error != VinaStorageError::format_error &&
            primary_error != VinaStorageError::encoding_error) {
            state_error_ = primary_error == VinaStorageError::resource_limit
                ? VinaStorageError::resource_limit : VinaStorageError::recovery_required;
            throw std::runtime_error("Storage primary file could not be read safely; retry is required.");
        }

        // If primary file failed to load, try backup file
        if (backup_exists) {
            std::wcerr << L"Warning: Failed to load primary file '" << filename << L"'. Attempting to restore from backup '" << backup_filename << L"'." << std::endl;
            if (LoadInternal(backup_filename)) {
                loaded_ = true;
                writable_ = false;
                state_error_ = VinaStorageError::recovered_backup;
                std::wcout << L"VinaStorage successfully restored and loaded from backup." << std::endl;
                
                // Copy backup back to primary file to fix it
#ifdef _WIN32
                static std::atomic<unsigned long long> restore_sequence{ 0 };
                const std::wstring restore_temp = filename + L".restore." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(VinaStorageTickCount()) + L"." +
                    std::to_wstring(++restore_sequence);
                if (CopyFileW(backup_filename.c_str(), restore_temp.c_str(), TRUE)) {
                    static std::atomic<unsigned long long> recovery_artifact_sequence{ 0 };
                    std::wstring recovery_artifact = filename + L".corrupted";
                    if (GetFileAttributesW(recovery_artifact.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        recovery_artifact += L"." + std::to_wstring(VinaStorageTickCount()) + L"." +
                            std::to_wstring(++recovery_artifact_sequence);
                    }
                    if (!ReplaceFileW(filename.c_str(), restore_temp.c_str(), recovery_artifact.c_str(),
                            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
                        DeleteFileW(restore_temp.c_str());
                    }
                    else {
                        writable_ = true;
                    }
                }
#else
                std::ifstream src(std::filesystem::path(backup_filename), std::ios::binary);
                std::ofstream dst(std::filesystem::path(filename), std::ios::binary | std::ios::trunc);
                dst << src.rdbuf();
                writable_ = src.good() && dst.good();
#endif
                loaded_stamp_ = getFileStamp(filename);
                mutation_generation_.store(0, std::memory_order_relaxed);
                saved_generation_ = 0;
                mutation_tracking_uncertain_.store(false, std::memory_order_relaxed);
                if (!writable_) {
                    state_error_ = VinaStorageError::recovery_required;
                    throw std::runtime_error("Backup loaded, but the primary file could not be restored safely.");
                }
                return;
            }
        }

        // Do not overwrite or rename files after an I/O/resource failure. Only definite
        // format/encoding corruption is preserved as a .corrupted artifact.
        std::wcerr << L"Error: Failed to load both primary and backup storage files for '" << filename
            << L"'. Preserving the damaged primary for manual recovery." << std::endl;

        if (load_error_ != VinaStorageError::format_error && load_error_ != VinaStorageError::encoding_error) {
            state_error_ = load_error_ == VinaStorageError::resource_limit
                ? VinaStorageError::resource_limit : VinaStorageError::recovery_required;
            loaded_ = false;
            writable_ = false;
            throw std::runtime_error("Storage could not be read safely; manual recovery or retry is required.");
        }
        
        std::wstring corrupted_filename = filename + L".corrupted";
#ifdef _WIN32
        static std::atomic<unsigned long long> corrupted_sequence{ 0 };
        if (GetFileAttributesW(corrupted_filename.c_str()) != INVALID_FILE_ATTRIBUTES) {
            corrupted_filename += L"." + std::to_wstring(VinaStorageTickCount()) + L"." +
                std::to_wstring(++corrupted_sequence);
        }
        if (!MoveFileExW(filename.c_str(), corrupted_filename.c_str(), MOVEFILE_WRITE_THROUGH)) {
            loaded_ = false;
            writable_ = false;
            throw std::runtime_error(
                "Could not preserve corrupted storage before reset (Error code: " +
                std::to_string(GetLastError()) + ")");
        }
#else
        std::rename(WStringToUTF8(filename).c_str(), WStringToUTF8(corrupted_filename).c_str());
#endif

        root_objects_.clear();
        state_error_ = VinaStorageError::recovery_required;
        loaded_ = false;
        writable_ = false;
        throw std::runtime_error(
            "Storage primary and backup files are unusable; manual recovery is required.");
    }

    // Check if a file is currently loaded
    bool IsLoaded() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return loaded_;
    }

    bool IsWritable() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return loaded_ && writable_;
    }

    // Save
    void Save() {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (!loaded_ || !writable_ || filename_.empty()) {
            state_error_ = VinaStorageError::not_loaded;
            throw std::runtime_error("Cannot save: No writable storage is currently loaded.");
        }
        const std::uint64_t generation = mutation_generation_.load(std::memory_order_relaxed);
        if (loaded_stamp_.has_value() && !(getFileStamp(filename_) == loaded_stamp_.value())) {
            state_error_ = VinaStorageError::conflict;
            throw std::runtime_error("Storage file changed externally; refusing to overwrite it.");
        }
        if (generation == saved_generation_ &&
            !mutation_tracking_uncertain_.load(std::memory_order_relaxed) &&
            loaded_stamp_.has_value() && loaded_stamp_->exists) {
            state_error_ = VinaStorageError::none;
            return;
        }

#ifdef _WIN32
        VinaStorageProcessLock process_lock(filename_);
        if (!process_lock.locked()) {
            state_error_ = VinaStorageError::busy;
            throw std::runtime_error("Storage is busy in another process.");
        }
#endif
        if (loaded_stamp_.has_value() && !(getFileStamp(filename_) == loaded_stamp_.value())) {
            state_error_ = VinaStorageError::conflict;
            throw std::runtime_error("Storage file changed externally; refusing to overwrite it.");
        }

        VinaBuilder builder;

        // Iteration over root_objects_ is now ordered as well
        for (const auto& [obj_name, obj_data] : root_objects_) {
            if (!isValidObjectName(obj_name)) {
                throw std::runtime_error("Storage object name contains unsupported syntax characters: " +
                    WStringToUTF8(obj_name));
            }
            VinaObject* root_obj = builder.AddObject(obj_name);
            populateVinaObject(root_obj, obj_data);
        }

        std::wstring wcontent = builder.GetContent();
        std::string utf8_content;
        if (!TryWStringToUTF8(wcontent, utf8_content)) {
            throw std::runtime_error("Storage contains text that cannot be encoded as valid UTF-8.");
        }

#ifdef _WIN32
        static std::atomic<unsigned long long> save_sequence{ 0 };
        std::wstring temp_filename = filename_ + L".tmp." +
            std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(VinaStorageTickCount()) + L"." +
            std::to_wstring(++save_sequence);
        std::wstring backup_filename = backup_path_.empty() ? (filename_ + L".bak") : backup_path_;

        std::error_code primary_path_ec;
        std::error_code backup_path_ec;
        auto primary_path = std::filesystem::absolute(filename_, primary_path_ec)
            .lexically_normal().wstring();
        auto backup_path = std::filesystem::absolute(backup_filename, backup_path_ec)
            .lexically_normal().wstring();
#ifdef _WIN32
        std::transform(primary_path.begin(), primary_path.end(), primary_path.begin(), ::towlower);
        std::transform(backup_path.begin(), backup_path.end(), backup_path.begin(), ::towlower);
#endif
        if (!primary_path_ec && !backup_path_ec && primary_path == backup_path) {
            throw std::runtime_error("Storage primary and backup paths must be different.");
        }

        // 1. Ensure parent directories exist before creating the temp or backup files.
        const auto ensure_parent_directory = [](const std::wstring& path, const char* description) {
            std::filesystem::path p(path);
            std::error_code ec;
            if (!p.parent_path().empty()) {
                std::filesystem::create_directories(p.parent_path(), ec);
                if (ec) {
                    throw std::runtime_error(std::string("Could not create storage ") + description + " directory: " +
                        WStringToUTF8(p.parent_path().wstring()));
                }
            }
        };
        ensure_parent_directory(filename_, "primary");
        ensure_parent_directory(backup_filename, "backup");

        // 2. Write content to temp file
        bool write_ok = false;
        {
            std::ofstream file_stream{
                std::filesystem::path(temp_filename), std::ios::out | std::ios::binary
            };
            if (!file_stream.is_open()) {
                std::string filename_utf8 = WStringToUTF8(temp_filename);
                throw std::runtime_error("Could not open temp file for writing: " + filename_utf8);
            }
            file_stream.write(utf8_content.data(), static_cast<std::streamsize>(utf8_content.size()));
            file_stream.flush();
            write_ok = file_stream.good();
        } // file_stream closed here
        if (!write_ok) {
            DeleteFileW(temp_filename.c_str());
            throw std::runtime_error("Error writing to temp file: " + WStringToUTF8(temp_filename));
        }

        // 3. Flush OS file buffers to physical storage to prevent data loss on power cut / forced shutdown
        HANDLE hFile = CreateFileW(
            temp_filename.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH,
            NULL
        );
        if (hFile != INVALID_HANDLE_VALUE) {
            if (!FlushFileBuffers(hFile)) {
                const DWORD flush_error = GetLastError();
                CloseHandle(hFile);
                DeleteFileW(temp_filename.c_str());
                throw std::runtime_error("Could not flush storage temp file (Error code: " +
                    std::to_string(flush_error) + ")");
            }
            CloseHandle(hFile);
        }
        else {
            const DWORD open_error = GetLastError();
            DeleteFileW(temp_filename.c_str());
            throw std::runtime_error("Could not reopen storage temp file for flush (Error code: " +
                std::to_string(open_error) + ")");
        }

        // Serialization and temp-file I/O can take time for large stores. Recheck
        // immediately before replacement so an external edit is not overwritten.
        if (loaded_stamp_.has_value() && !(getFileStamp(filename_) == loaded_stamp_.value())) {
            DeleteFileW(temp_filename.c_str());
            state_error_ = VinaStorageError::conflict;
            throw std::runtime_error("Storage file changed while saving; refusing to overwrite it.");
        }

        // 4. Atomically replace the original file with retry loop for anti-virus/sync locks
        DWORD dwAttrs = GetFileAttributesW(filename_.c_str());
        bool original_exists = (dwAttrs != INVALID_FILE_ATTRIBUTES && !(dwAttrs & FILE_ATTRIBUTE_DIRECTORY));

        bool replace_success = false;
        DWORD atomic_error = ERROR_SUCCESS;
        int max_retries = 5;
        for (int i = 0; i < max_retries; ++i) {
            if (original_exists) {
                // Replace original file atomically and create backup
                if (ReplaceFileW(filename_.c_str(), temp_filename.c_str(), backup_filename.c_str(), REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
                    replace_success = true;
                    break;
                }
            } else {
                // Original file does not exist, just rename temp to original
                if (MoveFileExW(temp_filename.c_str(), filename_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    replace_success = true;
                    break;
                }
            }
            
            DWORD err = GetLastError();
            atomic_error = err;
            // If it's a sharing violation or access denied, sleep and retry
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
                Sleep(20 + i * 20); // Exponential backoff: 20ms, 40ms, 60ms, 80ms, 100ms
                continue;
            } else {
                break; // Other error, exit retry loop
            }
        }

        // 5. Fallback if ReplaceFileW / MoveFileExW failed
        if (!replace_success) {
            std::wcerr << L"Warning: Atomic save failed (error " << atomic_error << L"). Falling back to manual backup copy." << std::endl;
            
            // Manual backup copy
            if (original_exists) {
                const std::wstring backup_temp = backup_filename + L".tmp." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(VinaStorageTickCount()) + L"." +
                    std::to_wstring(++save_sequence);
                if (!CopyFileW(filename_.c_str(), backup_temp.c_str(), TRUE)) {
                    const DWORD backup_error = GetLastError();
                    throw std::runtime_error(
                        "Could not create storage backup before fallback replacement. Latest data remains at: " +
                        WStringToUTF8(temp_filename) + " (Error code: " +
                        std::to_string(backup_error) + ")");
                }
                if (!MoveFileExW(backup_temp.c_str(), backup_filename.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    const DWORD backup_error = GetLastError();
                    DeleteFileW(backup_temp.c_str());
                    throw std::runtime_error(
                        "Could not commit storage backup before fallback replacement. Latest data remains at: " +
                        WStringToUTF8(temp_filename) + " (Error code: " +
                        std::to_string(backup_error) + ")");
                }
            }
            
            // Move temp to original with retry loop
            bool move_success = false;
            for (int i = 0; i < max_retries; ++i) {
                if (MoveFileExW(temp_filename.c_str(), filename_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    move_success = true;
                    break;
                }
                DWORD err = GetLastError();
                if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
                    Sleep(20 + i * 20);
                    continue;
                } else {
                    break;
                }
            }

            if (!move_success) {
                const DWORD move_error = GetLastError();
                std::string filename_utf8 = WStringToUTF8(filename_);
                throw std::runtime_error("Could not replace original file (MoveFileExW fallback): " +
                    filename_utf8 + ". Latest data remains at: " + WStringToUTF8(temp_filename) +
                    " (Error code: " + std::to_string(move_error) + ")");
            }
        }
#else
        // Non-Windows platform fallback
        static std::atomic<unsigned long long> save_sequence{ 0 };
        const std::wstring temp_filename = filename_ + L".tmp." +
            std::to_wstring(++save_sequence);
        const std::wstring backup_filename = backup_path_.empty() ? (filename_ + L".bak") : backup_path_;

        const auto ensure_parent_directory = [](const std::filesystem::path& path) {
            if (path.parent_path().empty()) return;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) throw std::runtime_error("Could not create storage directory: " + ec.message());
        };
        const std::filesystem::path primary_path(filename_);
        const std::filesystem::path backup_file_path(backup_filename);
        const std::filesystem::path temp_path(temp_filename);
        ensure_parent_directory(primary_path);
        ensure_parent_directory(backup_file_path);

        {
            std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
            if (!output.is_open()) throw std::runtime_error("Could not open storage temp file.");
            output.write(utf8_content.data(), static_cast<std::streamsize>(utf8_content.size()));
            output.flush();
            if (!output.good()) {
                std::error_code cleanup_ec;
                std::filesystem::remove(temp_path, cleanup_ec);
                throw std::runtime_error("Could not write storage temp file.");
            }
        }

        if (loaded_stamp_.has_value() && !(getFileStamp(filename_) == loaded_stamp_.value())) {
            std::error_code cleanup_ec;
            std::filesystem::remove(temp_path, cleanup_ec);
            state_error_ = VinaStorageError::conflict;
            throw std::runtime_error("Storage file changed while saving; refusing to overwrite it.");
        }

        std::error_code ec;
        if (std::filesystem::is_regular_file(primary_path, ec)) {
            ec.clear();
            std::filesystem::copy_file(
                primary_path, backup_file_path, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::filesystem::remove(temp_path, ec);
                throw std::runtime_error("Could not create storage backup.");
            }
        }

        ec.clear();
        std::filesystem::rename(temp_path, primary_path, ec);
        if (ec) {
            std::error_code cleanup_ec;
            std::filesystem::remove(temp_path, cleanup_ec);
            throw std::runtime_error("Could not atomically replace storage file: " + ec.message());
        }
#endif
        loaded_stamp_ = getFileStamp(filename_);
        saved_generation_ = generation;
        state_error_ = VinaStorageError::none;
        std::wcout << L"VinaStorage saved to '" << filename_ << L"'." << std::endl;
    }

    class Proxy {
    private:
        // map_ptr_ 指向当前 Proxy 应该操作的 Map (root_objects_ / nested_obj_ptr->data)
        VinaStorageObjectMap* map_ptr_;
        // current_key_ 是 map_ptr_ 中要操作的键（对于 root proxy, current_key_ 是空字符串）
        std::wstring current_key_;
        // 只有当 is_root_proxy 为 true 时，map_ptr_ 才直接指向 root_objects_ 中的一个元素
        bool is_root_proxy;
        VinaStorage* owner_;

    public:
        // 迭代器返回 pair<const std::wstring, VinaStorageValue&>
        using MapIterator = VinaStorageObjectMap::iterator;
        using PairType = std::pair<const std::wstring, VinaStorageValue&>;

        class Iterator {
        private:
            MapIterator current_it_;
            bool is_null_;

        public:
            using iterator_category = std::bidirectional_iterator_tag;
            using value_type = PairType;
            using difference_type = std::ptrdiff_t;
            using pointer = PairType*; // 无法直接使用指针，只能用引用的指针或代理
            using reference = PairType;// 返回 pair<const WString, VinaStorageValue&>

            // 构造函数
            Iterator(MapIterator it) : current_it_(it), is_null_(false) {}

            // 默认构造函数 (null iterator)
            Iterator() : is_null_(true) {}

            // 解引用操作符
            reference operator*() const {
                if (is_null_) {
                    throw std::runtime_error("Dereferencing null VinaStorage::Proxy::Iterator");
                }
                // 强制转换以匹配 PairType 的 VinaStorageValue&
                return { current_it_->first, const_cast<VinaStorageValue&>(current_it_->second) };
            }

            // 前置自增
            Iterator& operator++() {
                if (!is_null_) {
                    ++current_it_;
                }
                return *this;
            }

            // 后置自增
            Iterator operator++(int) {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            // 相等性比较
            bool operator==(const Iterator& other) const {
                if (is_null_ && other.is_null_) {
                    return true;
                }
                if (is_null_ || other.is_null_) {
                    return false;
                }
                return current_it_ == other.current_it_;
            }

            // 不相等性比较
            bool operator!=(const Iterator& other) const {
                return !(*this == other);
            }
        };

        // 用于范围 for 循环的 begin 方法 (新增)
        Iterator begin() {
            if (owner_) owner_->markMutationTrackingUncertain();
            // 尝试获取当前 Proxy 所代表的 map
            VinaStorageObjectMap* target_map = nullptr;
            if (is_root_proxy) {
                target_map = map_ptr_;
            }
            else if (map_ptr_) {
                auto it = map_ptr_->find(current_key_);
                if (it != map_ptr_->end() && std::holds_alternative<std::shared_ptr<VinaStorageNestedObject>>(it->second)) {
                    auto nested_obj_ptr = std::get<std::shared_ptr<VinaStorageNestedObject>>(it->second);
                    if (nested_obj_ptr) {
                        target_map = &nested_obj_ptr->data;
                    }
                }
            }

            if (target_map) {
                return Iterator(target_map->begin());
            }
            // 如果不是嵌套对象或根对象，则返回一个空迭代器
            return Iterator();
        }

        // 用于范围 for 循环的 end 方法 (新增)
        Iterator end() {
            VinaStorageObjectMap* target_map = nullptr;
            if (is_root_proxy) {
                target_map = map_ptr_;
            }
            else if (map_ptr_) {
                auto it = map_ptr_->find(current_key_);
                if (it != map_ptr_->end() && std::holds_alternative<std::shared_ptr<VinaStorageNestedObject>>(it->second)) {
                    auto nested_obj_ptr = std::get<std::shared_ptr<VinaStorageNestedObject>>(it->second);
                    if (nested_obj_ptr) {
                        target_map = &nested_obj_ptr->data;
                    }
                }
            }

            if (target_map) {
                return Iterator(target_map->end());
            }
            return Iterator();
        }


        Proxy(VinaStorageObjectMap* map, const std::wstring& key, bool is_root, VinaStorage* owner)
            : map_ptr_(map), current_key_(key), is_root_proxy(is_root), owner_(owner) {
        }

        Proxy(VinaStorageObjectMap* map, const std::wstring& key, VinaStorage* owner)
            : map_ptr_(map), current_key_(key), is_root_proxy(false), owner_(owner) {
        }

        Proxy operator[](const std::wstring& key) {
            if (is_root_proxy) {
                return Proxy(map_ptr_, key, false, owner_);
            }
            else {
                if (!map_ptr_) {
                    // 防止空指针解引用崩溃
                    throw std::runtime_error("VinaStorage::Proxy: internal map pointer is null.");
                }
                // Nested proxy: 先找到 current_key_ 对应的 VinaStorageNestedObject
                auto it = map_ptr_->find(current_key_);
                if (it == map_ptr_->end()) {
                    auto new_nested_obj_ptr = std::make_shared<VinaStorageNestedObject>();
                    (*map_ptr_)[current_key_] = new_nested_obj_ptr;
                    if (owner_) owner_->markMutated();
                    it = map_ptr_->find(current_key_);
                }

                if (std::holds_alternative<std::shared_ptr<VinaStorageNestedObject>>(it->second)) {
                    auto nested_obj_ptr = std::get<std::shared_ptr<VinaStorageNestedObject>>(it->second);
                    if (!nested_obj_ptr) {
                        // 如果指针为空，创建一个新的
                        nested_obj_ptr = std::make_shared<VinaStorageNestedObject>();
                        (*map_ptr_)[current_key_] = nested_obj_ptr; // 更新 map_ptr_
                        if (owner_) owner_->markMutated();
                    }
                    // 返回一个指向 nested_obj_ptr->data 的 Proxy
                    return Proxy(&nested_obj_ptr->data, key, false, owner_);
                }
                else {
                    auto new_nested_obj_ptr = std::make_shared<VinaStorageNestedObject>();
                    (*map_ptr_)[current_key_] = new_nested_obj_ptr;
                    if (owner_) owner_->markMutated();
                    // 返回一个指向新嵌套对象->data 的 Proxy
                    return Proxy(&new_nested_obj_ptr->data, key, false, owner_);
                }
            }
        }

        template<typename T>
        void operator=(const T& value) {
            if (is_root_proxy) {
                std::wcerr << L"Warning: Attempting to assign value to a root proxy directly. Use storage[L\"obj\"][L\"key\"] = value; instead." << std::endl;
                return;
            }

            if (!map_ptr_) {
                std::wcerr << L"Error: Internal map pointer is null during assignment for key '" << current_key_ << L"'." << std::endl;
                return;
            }

            using DecayedT = std::decay_t<T>;
            // 宽字符串字面量和指针
            if constexpr (std::is_same_v<DecayedT, const wchar_t*>) {
                if (value) {
                    (*map_ptr_)[current_key_] = std::wstring(value);
                } else {
                    (*map_ptr_)[current_key_] = std::wstring();
                }
            }
            else if constexpr (std::is_same_v<DecayedT, wchar_t*>) {
                if (value) {
                    (*map_ptr_)[current_key_] = std::wstring(value);
                } else {
                    (*map_ptr_)[current_key_] = std::wstring();
                }
            }
            // 窄字符串字面量和指针 (转换为宽字符串)
            else if constexpr (std::is_same_v<DecayedT, const char*>) {
                // 假设值是 ASCII 或单字节编码，进行简单转换
                if (value) {
                    std::string s(value);
                    (*map_ptr_)[current_key_] = std::wstring(s.begin(), s.end());
                } else {
                    (*map_ptr_)[current_key_] = std::wstring();
                }
            }
            else if constexpr (std::is_same_v<DecayedT, char*>) {
                // 假设值是 ASCII 或单字节编码，进行简单转换
                if (value) {
                    std::string s(value);
                    (*map_ptr_)[current_key_] = std::wstring(s.begin(), s.end());
                } else {
                    (*map_ptr_)[current_key_] = std::wstring();
                }
            }
            // 其他可直接存入 VinaStorageValue 的类型
            else if constexpr (std::is_same_v<DecayedT, int> ||
                std::is_same_v<DecayedT, double> ||
                std::is_same_v<DecayedT, bool> ||
                std::is_same_v<DecayedT, std::wstring>) {
                (*map_ptr_)[current_key_] = value;
            }
            else if constexpr (std::is_same_v<DecayedT, float>) {
                (*map_ptr_)[current_key_] = VinaStorage::normalizeFloat(value);
            }
            else {
                // 尝试将其他类型转换为 std::wstring 或抛出错误
                // 由于不知道 T 的具体类型，这里只处理上面列出的，其他情况可能需要额外的 to_wstring 或 cast
                std::wcerr << L"Warning: Unsupported type assignment for key '" << current_key_ << L"'. Skipping." << std::endl;
                return;
            }
            if (owner_) owner_->markMutated();
        }

        template<typename T>
        T get(const T& default_value = T{}) const {
            if (is_root_proxy) {
                // 根代理不持有值，它只代表一个 Map
                std::wcerr << L"Warning: Attempting to get value from a root object proxy directly. Must use [] to access a key." << std::endl;
                return default_value;
            }

            if (!map_ptr_) {
                return default_value;
            }
            auto it = map_ptr_->find(current_key_);
            if (it == map_ptr_->end()) {
                return default_value;
            }
            if constexpr (std::is_same_v<T, std::wstring> || std::is_same_v<T, bool> ||
                std::is_same_v<T, std::shared_ptr<VinaStorageNestedObject>>) {
                if (const auto* value = std::get_if<T>(&it->second)) {
                    if constexpr (std::is_same_v<T, std::shared_ptr<VinaStorageNestedObject>>) {
                        if (owner_) owner_->markMutationTrackingUncertain();
                    }
                    return *value;
                }
                return default_value;
            }
            else if constexpr (std::is_integral_v<T>) {
                long double number = 0;
                if (const auto* value = std::get_if<int>(&it->second)) number = *value;
                else if (const auto* value = std::get_if<double>(&it->second)) {
                    if (!std::isfinite(*value)) return default_value;
                    number = *value;
                }
                else return default_value;
                if (number < static_cast<long double>(std::numeric_limits<T>::lowest()) ||
                    number > static_cast<long double>(std::numeric_limits<T>::max())) {
                    return default_value;
                }
                return static_cast<T>(number);
            }
            else if constexpr (std::is_floating_point_v<T>) {
                long double number = 0;
                if (const auto* value = std::get_if<double>(&it->second)) number = *value;
                else if (const auto* value = std::get_if<int>(&it->second)) number = *value;
                else return default_value;
                if (!std::isfinite(number) ||
                    number < -static_cast<long double>(std::numeric_limits<T>::max()) ||
                    number > static_cast<long double>(std::numeric_limits<T>::max())) {
                    return default_value;
                }
                return static_cast<T>(number);
            }
            else {
                if (const auto* value = std::get_if<T>(&it->second)) return *value;
                return default_value;
            }
        }

        bool remove() {
            if (is_root_proxy) {
                std::wcerr << L"Error: Cannot call remove() on a root object proxy directly. Use VinaStorage::RemoveRootObject() instead if needed." << std::endl;
                return false;
            }
            if (!map_ptr_) {
                return false;
            }
            size_t count = map_ptr_->erase(current_key_);
            if (count > 0 && owner_) owner_->markMutated();
            return count > 0;
        }

        const VinaStorageObjectMap* GetMapPtr() const {
            if (is_root_proxy) {
                if (owner_) owner_->markMutationTrackingUncertain();
                return map_ptr_;
            }
            return nullptr;
        }
    };

    Proxy operator[](const std::wstring& root_obj_name) {
        auto it = root_objects_.find(root_obj_name);
        if (it == root_objects_.end()) {
            // 如果不存在，插入一个空的根对象 Map
            root_objects_[root_obj_name] = VinaStorageObjectMap{};
            markMutated();
            it = root_objects_.find(root_obj_name);
        }
        // Proxy(MapPtr, Key, is_root)
        // 对于根代理，map_ptr_ 指向根对象 map，current_key_ 是空字符串，is_root_proxy 为 true
        return Proxy(const_cast<VinaStorageObjectMap*>(&it->second), std::wstring(L""), true, this);
    }

    // 专门移除根对象的方法（Proxy::remove() 不允许移除根对象）
    bool RemoveRootObject(const std::wstring& root_obj_name) {
        const bool removed = root_objects_.erase(root_obj_name) > 0;
        if (removed) markMutated();
        return removed;
    }

    template<typename T>
    T GetFromRoot(const std::wstring& root_obj_name, const std::wstring& key, const T& default_value = T{}) {
        if (!loaded_) {
            return default_value;
        }
        auto root_it = root_objects_.find(root_obj_name);
        if (root_it != root_objects_.end()) {
            auto& root_map = root_it->second;
            return Proxy(const_cast<VinaStorageObjectMap*>(&root_map), key, false, this).template get<T>(default_value);
        }
        return default_value;
    }

    bool HasNestedObject(const std::wstring& root_obj_name, const std::wstring& key) const {
        const auto root_it = root_objects_.find(root_obj_name);
        if (root_it == root_objects_.end()) return false;
        const auto value_it = root_it->second.find(key);
        if (value_it == root_it->second.end()) return false;
        const auto* nested = std::get_if<std::shared_ptr<VinaStorageNestedObject>>(&value_it->second);
        return nested && static_cast<bool>(*nested);
    }

    const VinaStorageObjectMap& GetRootObjectMap(const std::wstring& root_obj_name) const {
        markMutationTrackingUncertain();
        auto it = root_objects_.find(root_obj_name);
        if (it == root_objects_.end()) {
            static const VinaStorageObjectMap empty_map;
            return empty_map;
        }
        return it->second;
    }

};

inline void print_value(const VinaStorageValue& value) {
    std::visit([](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>)
            std::wcout << L" (int): " << arg;
        else if constexpr (std::is_same_v<T, double>)
            std::wcout << L" (double): " << arg;
        else if constexpr (std::is_same_v<T, bool>)
            std::wcout << L" (bool): " << (arg ? L"true" : L"false");
        else if constexpr (std::is_same_v<T, std::wstring>)
            std::wcout << L" (wstring): " << arg;
        else if constexpr (std::is_same_v<T, std::shared_ptr<VinaStorageNestedObject>>)
            std::wcout << L" (Nested Object) -> " << (arg ? arg->data.size() : 0) << L" sub-keys";
        else
            std::wcout << L" (Unknown Type)";
        }, value);
}

inline void traverse_object_map(const VinaStorageObjectMap& data_map, int depth = 0) {
    std::wstring indent(depth * 4, L' ');
    for (const auto& pair : data_map) {
        std::wcout << indent << L"Key: " << pair.first;
        print_value(pair.second);
        std::wcout << std::endl;

        if (std::holds_alternative<std::shared_ptr<VinaStorageNestedObject>>(pair.second)) {
            auto nested_obj_ptr = std::get<std::shared_ptr<VinaStorageNestedObject>>(pair.second);
            if (nested_obj_ptr) {
                traverse_object_map(nested_obj_ptr->data, depth + 1);
            }
        }
    }
}

#endif // VINA_STORAGE_HPP
