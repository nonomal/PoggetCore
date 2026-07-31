#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace PoggetCore {

    enum class FileOperationType {
        Create,
        Copy,
        Move,
        Rename,
        Delete,
        Recycle,
        Modify
    };

    /*
     * Process-wide, asynchronous logger owned by PoggetCore.
     *
     * File-operation audit records are never discarded. Ordinary diagnostic
     * messages are bounded so a noisy subsystem cannot consume unbounded memory
     * if the log device becomes slow. The producer side never performs log-file
     * I/O and therefore does not contend with UI file operations.
     */
    class PoggetLogger final {
    public:
        static PoggetLogger& GetInstance() noexcept {
            // Intentionally process-lived: file operations may be issued by other
            // static objects during shutdown, after normal static destruction order.
            static PoggetLogger* instance = new PoggetLogger();
            return *instance;
        }

        PoggetLogger(const PoggetLogger&) = delete;
        PoggetLogger& operator=(const PoggetLogger&) = delete;

        void Init(const std::wstring& baseDirectory) noexcept {
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                if (running_ || stopRequested_) return;

                std::error_code ec;
                const auto logsDirectory = std::filesystem::path(baseDirectory) / L"logs";
                bool opened = OpenLogFile(logsDirectory, ec);
                if (!opened) {
                    ReportInitializationFailure("cannot open primary log directory", ec);
                    ec.clear();
                    const auto temporaryRoot = std::filesystem::temp_directory_path(ec);
                    if (!ec) {
                        opened = OpenLogFile(temporaryRoot / L"Pogget" / L"logs", ec);
                    }
                }
                if (!opened) {
                    ReportInitializationFailure("cannot open fallback audit log", ec);
                    logFilePath_.clear();
                    return;
                }

#ifdef _WIN32
                const auto nativePath = logFilePath_.native();
                const auto pathLength = (std::min)(
                    nativePath.size(), crashLogPath_.size() - 1);
                std::copy_n(nativePath.data(), pathLength, crashLogPath_.data());
                crashLogPath_[pathLength] = L'\0';
                crashLogger_.store(this, std::memory_order_release);
                SetUnhandledExceptionFilter(&PoggetLogger::UnhandledExceptionFilter);
#endif

                running_ = true;
                workerThread_ = std::thread(&PoggetLogger::ProcessLogs, this);
                lock.unlock();
                condition_.notify_one();
            }
            catch (const std::exception& error) {
                std::cerr << "PoggetCore: logger initialization failed: " << error.what() << '\n';
            }
            catch (...) {
                std::cerr << "PoggetCore: logger initialization failed\n";
            }
        }

        void Stop() noexcept {
            try {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopRequested_) return;
                    stopRequested_ = true;
                }
                condition_.notify_all();
                if (workerThread_.joinable()) workerThread_.join();

                std::lock_guard<std::mutex> lock(mutex_);
                if (logFile_.is_open()) {
                    logFile_.flush();
                    logFile_.close();
                }
                running_ = false;
            }
            catch (...) {
                // Logging must never make application shutdown fail.
            }
        }

        static void Log(
            const std::wstring& subsystem,
            const std::wstring& level,
            const std::wstring& message) noexcept {
            try {
                auto& logger = GetInstance();
                std::ostringstream line;
                line << '[' << logger.NextSequence() << "]"
                    << '[' << CurrentTimestamp() << "]"
                    << '[' << Utf8(EscapeField(subsystem)) << "]"
                    << '[' << Utf8(EscapeField(level)) << "] "
                    << Utf8(EscapeField(message));
                logger.Enqueue({ line.str(), false });
            }
            catch (...) {
                // Diagnostics are best-effort and must not affect application work.
            }
        }

        static void AuditFileOperation(
            FileOperationType operation,
            const std::filesystem::path& source,
            const std::filesystem::path& destination,
            bool success,
            const std::error_code& error = {},
            const std::wstring& context = {}) noexcept {
            try {
                auto& logger = GetInstance();
                std::ostringstream line;
                line << '[' << logger.NextSequence() << "]"
                    << '[' << CurrentTimestamp() << "]"
                    << "[FileAudit][" << (success ? "SUCCESS" : "FAILURE") << ']'
                    << " operation=" << OperationName(operation)
                    << " source=\"" << Utf8(EscapeField(source.wstring())) << '\"'
                    << " destination=\"" << Utf8(EscapeField(destination.wstring())) << '\"'
                    << " error=" << error.value()
                    << " category=\"" << EscapeNarrow(error.category().name()) << '\"'
                    << " context=\"" << Utf8(EscapeField(context)) << '\"'
                    << " thread=" << std::this_thread::get_id();
                logger.Enqueue({ line.str(), true });
            }
            catch (...) {
                // An audit failure must never change the result of the file operation.
            }
        }

        std::filesystem::path GetLogFilePath() const noexcept {
            try {
                std::lock_guard<std::mutex> lock(mutex_);
                return logFilePath_;
            }
            catch (...) {
                return {};
            }
        }

        void FlushSync() noexcept {
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!running_) return;
                flushRequested_ = true;
                condition_.notify_one();
                drainedCondition_.wait_for(lock, std::chrono::seconds(2), [this]() {
                    return queue_.empty() && !writingBatch_ && !flushRequested_;
                });
            }
            catch (...) {}
        }

    private:
        struct Entry {
            std::string text;
            bool audit = false;
        };

        static constexpr std::size_t MaxQueuedDiagnostics = 32768;
        static constexpr std::size_t WriteBatchSize = 256;

        PoggetLogger() = default;

        std::uint64_t NextSequence() noexcept {
            return sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        void Enqueue(Entry entry) noexcept {
            try {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopRequested_) return;

                if (queue_.size() >= MaxQueuedDiagnostics) {
                    auto ordinary = queue_.begin();
                    while (ordinary != queue_.end() && ordinary->audit) ++ordinary;
                    if (ordinary != queue_.end()) {
                        queue_.erase(ordinary);
                        ++droppedDiagnostics_;
                    }
                    else if (!entry.audit) {
                        ++droppedDiagnostics_;
                        return;
                    }
                    // If the queue contains audit entries only, retain another audit
                    // entry rather than hiding a file's destination.
                }
                queue_.push_back(std::move(entry));
                condition_.notify_one();
            }
            catch (...) {}
        }

        void ProcessLogs() noexcept {
            try {
                auto lastFlush = std::chrono::steady_clock::now();
                for (;;) {
                    std::vector<Entry> batch;
                    std::uint64_t dropped = 0;
                    bool shouldFlush = false;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        condition_.wait(lock, [this]() {
                            return stopRequested_ || flushRequested_ || !queue_.empty();
                        });

                        const auto count = (std::min)(WriteBatchSize, queue_.size());
                        batch.reserve(count);
                        for (std::size_t index = 0; index < count; ++index) {
                            batch.push_back(std::move(queue_.front()));
                            queue_.pop_front();
                        }
                        dropped = std::exchange(droppedDiagnostics_, 0);
                        const auto now = std::chrono::steady_clock::now();
                        shouldFlush = flushRequested_ || stopRequested_ || queue_.empty() ||
                            now - lastFlush >= std::chrono::seconds(1);
                        writingBatch_ = !batch.empty() || dropped != 0 || shouldFlush;
                    }

                    if (dropped != 0 && logFile_.is_open()) {
                        logFile_ << "[PoggetCore][WARNING] dropped " << dropped
                            << " non-audit diagnostic messages due to queue pressure\n";
                    }
                    for (const auto& entry : batch) {
                        if (logFile_.is_open()) logFile_ << entry.text << '\n';
                    }
                    if (shouldFlush && logFile_.is_open()) {
                        logFile_.flush();
                        lastFlush = std::chrono::steady_clock::now();
                    }

                    bool finished = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        writingBatch_ = false;
                        if (shouldFlush) flushRequested_ = false;
                        finished = stopRequested_ && queue_.empty();
                    }
                    drainedCondition_.notify_all();
                    if (finished) break;
                }
            }
            catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                writingBatch_ = false;
                flushRequested_ = false;
                drainedCondition_.notify_all();
            }
        }

        static std::filesystem::path MakeLogFilePath(
            const std::filesystem::path& directory) {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            const auto local = LocalTime(time);
            std::wostringstream name;
            name << L"Pogget_" << std::put_time(&local, L"%Y_%m_%d_%H%M%S")
                << L'_' << ProcessId();

            for (unsigned int suffix = 0; suffix < 1000; ++suffix) {
                auto candidate = directory /
                    (name.str() + (suffix == 0 ? L"" : L"_" + std::to_wstring(suffix)) + L".log");
                std::error_code ec;
                if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
            }
            return directory / (name.str() + L"_fallback.log");
        }

        bool OpenLogFile(
            const std::filesystem::path& directory,
            std::error_code& error) noexcept {
            try {
                error.clear();
                std::filesystem::create_directories(directory, error);
                if (error) return false;

                logFilePath_ = MakeLogFilePath(directory);
                logFile_.clear();
                logFile_.open(logFilePath_, std::ios::binary | std::ios::out | std::ios::app);
                if (logFile_.is_open()) return true;

                error = std::make_error_code(std::errc::io_error);
                logFilePath_.clear();
                return false;
            }
            catch (...) {
                error = std::make_error_code(std::errc::io_error);
                logFilePath_.clear();
                return false;
            }
        }

        static std::tm LocalTime(std::time_t value) noexcept {
            std::tm result{};
#ifdef _WIN32
            localtime_s(&result, &value);
#else
            localtime_r(&value, &result);
#endif
            return result;
        }

        static std::string CurrentTimestamp() {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 1000;
            const auto local = LocalTime(time);
            std::ostringstream text;
            text << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
                << '.' << std::setw(3) << std::setfill('0') << millis;
            return text.str();
        }

        static unsigned long ProcessId() noexcept {
#ifdef _WIN32
            return static_cast<unsigned long>(_getpid());
#else
            return static_cast<unsigned long>(getpid());
#endif
        }

        static const char* OperationName(FileOperationType operation) noexcept {
            switch (operation) {
            case FileOperationType::Create: return "create";
            case FileOperationType::Copy: return "copy";
            case FileOperationType::Move: return "move";
            case FileOperationType::Rename: return "rename";
            case FileOperationType::Delete: return "delete";
            case FileOperationType::Recycle: return "recycle";
            case FileOperationType::Modify: return "modify";
            }
            return "unknown";
        }

        static std::wstring EscapeField(const std::wstring& value) {
            std::wstring escaped;
            escaped.reserve(value.size());
            for (const auto character : value) {
                switch (character) {
                case L'\\': escaped += L"\\\\"; break;
                case L'\"': escaped += L"\\\""; break;
                case L'\r': escaped += L"\\r"; break;
                case L'\n': escaped += L"\\n"; break;
                case L'\t': escaped += L"\\t"; break;
                default: escaped.push_back(character); break;
                }
            }
            return escaped;
        }

        static std::string EscapeNarrow(const std::string& value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (const auto character : value) {
                if (character == '\\' || character == '\"') escaped.push_back('\\');
                if (character == '\r') escaped += "\\r";
                else if (character == '\n') escaped += "\\n";
                else if (character == '\t') escaped += "\\t";
                else escaped.push_back(character);
            }
            return escaped;
        }

        static std::string Utf8(const std::wstring& value) {
            std::string result;
            result.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                std::uint32_t codePoint = static_cast<std::uint32_t>(value[index]);
#if WCHAR_MAX <= 0xFFFF
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < value.size()) {
                    const auto low = static_cast<std::uint32_t>(value[index + 1]);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                        ++index;
                    }
                }
#endif
                if (codePoint <= 0x7F) {
                    result.push_back(static_cast<char>(codePoint));
                }
                else if (codePoint <= 0x7FF) {
                    result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else if (codePoint <= 0xFFFF) {
                    result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else if (codePoint <= 0x10FFFF) {
                    result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else {
                    result += "\xEF\xBF\xBD";
                }
            }
            return result;
        }

        static void ReportInitializationFailure(
            const char* message,
            const std::error_code& error) noexcept {
            std::cerr << "PoggetCore: " << message << " (" << error.value() << ")\n";
        }

#ifdef _WIN32
        static LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) noexcept {
            static std::atomic_flag handling = ATOMIC_FLAG_INIT;
            if (handling.test_and_set(std::memory_order_acq_rel)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto* logger = crashLogger_.load(std::memory_order_acquire);
            if (logger != nullptr && logger->crashLogPath_[0] != L'\0') {
                const HANDLE file = CreateFileW(
                    logger->crashLogPath_.data(),
                    FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    const auto code = exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr
                        ? exceptionInfo->ExceptionRecord->ExceptionCode
                        : 0UL;
                    const auto address = exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr
                        ? exceptionInfo->ExceptionRecord->ExceptionAddress
                        : nullptr;
                    char message[256]{};
                    const int length = std::snprintf(
                        message,
                        sizeof(message),
                        "[PoggetCore][FATAL] unhandled exception code=0x%08lX address=%p\r\n",
                        code,
                        address);
                    if (length > 0) {
                        DWORD written = 0;
                        WriteFile(
                            file,
                            message,
                            static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(message) - 1))),
                            &written,
                            nullptr);
                        FlushFileBuffers(file);
                    }
                    CloseHandle(file);
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
#endif

        mutable std::mutex mutex_;
        std::condition_variable condition_;
        std::condition_variable drainedCondition_;
        std::deque<Entry> queue_;
        std::thread workerThread_;
        std::ofstream logFile_;
        std::filesystem::path logFilePath_;
        std::atomic<std::uint64_t> sequence_{ 0 };
        std::uint64_t droppedDiagnostics_ = 0;
        bool running_ = false;
        bool stopRequested_ = false;
        bool flushRequested_ = false;
        bool writingBatch_ = false;
#ifdef _WIN32
        std::array<wchar_t, 32768> crashLogPath_{};
        inline static std::atomic<PoggetLogger*> crashLogger_{ nullptr };
#endif
    };

} // namespace PoggetCore

// Transitional source compatibility for existing UI listeners. The logger
// implementation and ownership live entirely in PoggetCore.
using PoggetLogger = PoggetCore::PoggetLogger;
