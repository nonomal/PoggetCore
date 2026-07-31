#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "PoggetLogs.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#undef min
#undef max
#endif
namespace PoggetCore::HistoryFileSystem {

    inline constexpr std::uintmax_t FullContentVerificationLimit = 64ULL * 1024ULL * 1024ULL;
    inline constexpr std::uintmax_t SampleVerificationBlockSize = 1ULL * 1024ULL * 1024ULL;
    inline constexpr size_t SampleVerificationBlockCount = 8;
    inline constexpr wchar_t OperationStorageDirectoryName[] = L".pogget_operations";
    inline constexpr wchar_t LegacyUndoStorageDirectoryName[] = L".pogget_undo";

    struct Result {
        bool success = false;
        std::error_code error;
        std::wstring context;

        explicit operator bool() const noexcept { return success; }
    };

    struct MoveStep {
        std::filesystem::path source;
        std::filesystem::path destination;
    };

    struct DestructiveRequest {
        std::filesystem::path source;
        // When supplied, the source must be an immediate child of this directory.
        // UI surfaces use this as a final scope boundary for delete/recycle actions.
        std::filesystem::path requiredParent;
    };

    enum class PathPresence {
        Missing,
        Present,
        Inaccessible
    };

    struct PathInspection {
        PathPresence presence = PathPresence::Missing;
        std::filesystem::file_status status{};
        std::error_code error;
    };

    struct RestoreRequest {
        std::filesystem::path source;
        std::filesystem::path preferredDestination;
    };

    struct RestoreItemResult {
        std::filesystem::path source;
        std::filesystem::path preferredDestination;
        std::filesystem::path finalDestination;
        Result result;
        bool rolledBack = false;
    };

    struct RestoreBatchResult {
        bool success = false;
        bool fullyRolledBack = true;
        std::wstring context;
        std::vector<RestoreItemResult> items;

        explicit operator bool() const noexcept { return success; }
    };

    inline Result AuditedResult(
        FileOperationType operation,
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        Result result) noexcept {
        PoggetLogger::AuditFileOperation(
            operation, source, destination, result.success, result.error, result.context);
        if (operation == FileOperationType::Delete || operation == FileOperationType::Recycle) {
            try {
                const std::wstring action = operation == FileOperationType::Recycle
                    ? L"Recycle"
                    : L"Permanent delete";
                PoggetLogger::Log(
                    L"FileOperation",
                    result.success ? L"SUCCESS" : L"FAILURE",
                    action + (result.success ? L" succeeded" : L" failed") +
                        L": source=\"" + source.wstring() +
                        L"\" destination=\"" + destination.wstring() +
                        L"\" error=" + std::to_wstring(result.error.value()) +
                        L" context=\"" + result.context + L"\"");
            }
            catch (...) {
                // Result logging must never alter the file-operation outcome.
            }
        }
        return result;
    }

    inline std::wstring ComparablePath(const std::filesystem::path& path) {
        if (path.empty()) return {};
        std::error_code ec;
        auto normalized = std::filesystem::absolute(path, ec).lexically_normal().wstring();
        if (ec) normalized = path.lexically_normal().wstring();
#ifdef _WIN32
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
#endif
        return normalized;
    }

    inline PathInspection InspectPath(const std::filesystem::path& path) noexcept {
        if (path.empty()) {
            return { PathPresence::Missing, {},
                std::make_error_code(std::errc::invalid_argument) };
        }

        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (!ec) {
            return {
                status.type() == std::filesystem::file_type::not_found
                    ? PathPresence::Missing
                    : PathPresence::Present,
                status,
                {}
            };
        }
        if (ec == std::errc::no_such_file_or_directory ||
            ec == std::errc::not_a_directory) {
            return { PathPresence::Missing, {}, {} };
        }
        return { PathPresence::Inaccessible, {}, ec };
    }

    inline bool Exists(const std::filesystem::path& path) noexcept {
        return InspectPath(path).presence == PathPresence::Present;
    }

    inline bool IsPathInside(
        const std::filesystem::path& candidate,
        const std::filesystem::path& parent) {
        auto candidateKey = ComparablePath(candidate);
        auto parentKey = ComparablePath(parent);
        if (candidateKey.empty() || parentKey.empty() || candidateKey == parentKey) return false;
        if (parentKey.back() != L'\\' && parentKey.back() != L'/') {
            parentKey.push_back(std::filesystem::path::preferred_separator);
        }
        return candidateKey.starts_with(parentKey);
    }

    inline Result ValidateDestructiveBatch(
        const std::vector<DestructiveRequest>& requests) {
        if (requests.empty()) return { true, {}, L"nothing to remove" };

        std::unordered_set<std::wstring> sourceKeys;
        std::vector<std::filesystem::path> sources;
        sources.reserve(requests.size());

        for (const auto& request : requests) {
            if (request.source.empty()) {
                return { false, std::make_error_code(std::errc::invalid_argument),
                    L"destructive batch contains an empty source path" };
            }

            std::error_code ec;
            const auto source = std::filesystem::absolute(request.source, ec).lexically_normal();
            if (ec) return { false, ec, L"resolving destructive source path" };
            if (source == source.root_path()) {
                return { false, std::make_error_code(std::errc::operation_not_permitted),
                    L"refusing to remove a file-system root" };
            }

            const auto inspection = InspectPath(source);
            if (inspection.presence == PathPresence::Inaccessible) {
                return { false, inspection.error, L"checking destructive source: " + source.wstring() };
            }
            if (inspection.presence == PathPresence::Missing) {
                return { false, std::make_error_code(std::errc::no_such_file_or_directory),
                    L"destructive source does not exist: " + source.wstring() };
            }

            if (!request.requiredParent.empty()) {
                const auto requiredParent =
                    std::filesystem::absolute(request.requiredParent, ec).lexically_normal();
                if (ec) return { false, ec, L"resolving destructive scope directory" };
                if (ComparablePath(source.parent_path()) != ComparablePath(requiredParent)) {
                    return { false, std::make_error_code(std::errc::operation_not_permitted),
                        L"destructive source is outside its required parent: " + source.wstring() };
                }
                const auto parentInspection = InspectPath(requiredParent);
                if (parentInspection.presence != PathPresence::Present ||
                    !std::filesystem::is_directory(parentInspection.status)) {
                    return { false,
                        parentInspection.presence == PathPresence::Inaccessible
                            ? parentInspection.error
                            : std::make_error_code(std::errc::not_a_directory),
                        L"destructive scope directory is unavailable: " +
                            requiredParent.wstring() };
                }
            }

            const auto key = ComparablePath(source);
            if (!sourceKeys.insert(key).second) {
                return { false, std::make_error_code(std::errc::invalid_argument),
                    L"destructive batch contains the same source more than once: " +
                        source.wstring() };
            }
            sources.push_back(source);
        }

        for (std::size_t left = 0; left < sources.size(); ++left) {
            for (std::size_t right = left + 1; right < sources.size(); ++right) {
                if (IsPathInside(sources[left], sources[right]) ||
                    IsPathInside(sources[right], sources[left])) {
                    return { false, std::make_error_code(std::errc::invalid_argument),
                        L"destructive batch contains overlapping parent and child sources" };
                }
            }
        }
        return { true, {}, L"" };
    }

    inline bool IsHistoryStorageRootName(std::wstring name) {
#ifdef _WIN32
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
#endif
        return name == OperationStorageDirectoryName ||
            name == LegacyUndoStorageDirectoryName;
    }

    inline bool IsManagedHistoryPath(const std::filesystem::path& path) {
        if (path.empty()) return false;
        std::vector<std::wstring> components;
        for (const auto& component : path.lexically_normal()) {
            auto value = component.wstring();
#ifdef _WIN32
            std::transform(value.begin(), value.end(), value.begin(), ::towlower);
#endif
            components.push_back(std::move(value));
        }
        for (size_t index = 0; index < components.size(); ++index) {
            if (IsHistoryStorageRootName(components[index]) ||
                components[index] == L"poggetundo") {
                return true;
            }
            if (index >= 2 &&
                (components[index] == L"operations" || components[index] == L"undo") &&
                components[index - 1] == L".temp" &&
                components[index - 2] == L".data") {
                return true;
            }
        }
        return false;
    }

    inline bool EquivalentFileContents(
        const std::filesystem::path& left,
        const std::filesystem::path& right,
        bool verifyContent = false) {
        std::error_code ec;
        const auto leftSize = std::filesystem::file_size(left, ec);
        if (ec) return false;
        const auto rightSize = std::filesystem::file_size(right, ec);
        if (ec || leftSize != rightSize) return false;
        if (!verifyContent) return true;

        std::ifstream leftStream(left, std::ios::binary);
        std::ifstream rightStream(right, std::ios::binary);
        if (!leftStream.is_open() || !rightStream.is_open()) return false;

        std::vector<char> leftBuffer(1024 * 1024);
        std::vector<char> rightBuffer(1024 * 1024);

        auto compareRange = [&](std::uintmax_t offset, std::uintmax_t length) {
            if (offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
                return false;
            }
            leftStream.clear();
            rightStream.clear();
            leftStream.seekg(static_cast<std::streamoff>(offset));
            rightStream.seekg(static_cast<std::streamoff>(offset));
            if (!leftStream.good() || !rightStream.good()) return false;

            while (length > 0) {
                const auto chunk = static_cast<std::streamsize>(
                    std::min<std::uintmax_t>(length, leftBuffer.size()));
                leftStream.read(leftBuffer.data(), chunk);
                rightStream.read(rightBuffer.data(), chunk);
                if (leftStream.gcount() != chunk || rightStream.gcount() != chunk ||
                    !std::equal(leftBuffer.begin(), leftBuffer.begin() + chunk, rightBuffer.begin())) {
                    return false;
                }
                length -= static_cast<std::uintmax_t>(chunk);
            }
            return true;
        };

        if (leftSize <= FullContentVerificationLimit) {
            return compareRange(0, leftSize);
        }

        // Large files are sampled after the operating-system copy has completed
        // successfully. This keeps verification bounded while still checking the
        // beginning, end, and evenly distributed regions of the copied payload.
        const auto blockSize = (std::min)(leftSize, SampleVerificationBlockSize);
        const auto maxOffset = leftSize - blockSize;
        const auto intervals = static_cast<std::uintmax_t>(SampleVerificationBlockCount - 1);
        const auto quotient = maxOffset / intervals;
        const auto remainder = maxOffset % intervals;
        for (std::uintmax_t index = 0; index < SampleVerificationBlockCount; ++index) {
            const auto offset = quotient * index + (remainder * index) / intervals;
            if (!compareRange(offset, blockSize)) return false;
        }
        return true;
    }

    inline bool EquivalentSingleEntry(
        const std::filesystem::path& left,
        const std::filesystem::path& right,
        bool verifyContent = false) {
        std::error_code ec;
        const auto leftStatus = std::filesystem::symlink_status(left, ec);
        if (ec) return false;
        const auto rightStatus = std::filesystem::symlink_status(right, ec);
        if (ec || leftStatus.type() != rightStatus.type()) return false;

        if (std::filesystem::is_regular_file(leftStatus)) {
            return EquivalentFileContents(left, right, verifyContent);
        }
        if (std::filesystem::is_symlink(leftStatus)) {
            const auto leftTarget = std::filesystem::read_symlink(left, ec);
            if (ec) return false;
            const auto rightTarget = std::filesystem::read_symlink(right, ec);
            return !ec && leftTarget == rightTarget;
        }
        return std::filesystem::is_directory(leftStatus);
    }

    inline bool EquivalentPathContents(
        const std::filesystem::path& left,
        const std::filesystem::path& right,
        bool verifyContent = false) {
        if (!EquivalentSingleEntry(left, right, verifyContent)) return false;

        std::error_code ec;
        const auto leftStatus = std::filesystem::symlink_status(left, ec);
        if (ec || !std::filesystem::is_directory(leftStatus)) return !ec;

        size_t leftCount = 0;
        for (std::filesystem::recursive_directory_iterator it(left, ec), end;
            !ec && it != end; it.increment(ec)) {
            ++leftCount;
            const auto relative = it->path().lexically_relative(left);
            if (relative.empty() ||
                !EquivalentSingleEntry(it->path(), right / relative, verifyContent)) return false;
        }
        if (ec) return false;

        size_t rightCount = 0;
        for (std::filesystem::recursive_directory_iterator it(right, ec), end;
            !ec && it != end; it.increment(ec)) {
            ++rightCount;
        }
        return !ec && leftCount == rightCount;
    }
    /*
     Validate an entire move transaction against a simulated file-system state.
     This prevents a late deterministic conflict from exposing a partially applied
     batch and then forcing visible rollback moves.
     */
    inline Result ValidateMoveSequence(const std::vector<MoveStep>& steps) {
        std::unordered_map<std::wstring, bool> simulated;
        auto existsInSimulation = [&](const std::filesystem::path& path) {
            const auto key = ComparablePath(path);
            auto it = simulated.find(key);
            if (it != simulated.end()) return it->second;
            const bool exists = Exists(path);
            simulated.emplace(key, exists);
            return exists;
        };

        for (const auto& step : steps) {
            if (step.source.empty() || step.destination.empty()) {
                return { false, {}, L"history transaction contains an empty path" };
            }
            const auto sourceKey = ComparablePath(step.source);
            const auto destinationKey = ComparablePath(step.destination);
            if (sourceKey == destinationKey) continue;
            if (!existsInSimulation(step.source)) {
                return { false, {}, L"history source is missing: " + step.source.wstring() };
            }
            if (existsInSimulation(step.destination)) {
                return { false, {}, L"history destination is occupied: " + step.destination.wstring() };
            }
            simulated[sourceKey] = false;
            simulated[destinationKey] = true;
        }
        return { true, {}, L"" };
    }

    inline std::filesystem::path MakeUniquePath(
        const std::filesystem::path& root,
        const std::filesystem::path& source,
        const wchar_t* purpose = L"payload",
        bool preserveExtension = false) {
        if (root.empty()) return {};

        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) return {};

        static std::atomic<unsigned long long> sequence{ 0 };
        const auto ticks = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto sourceName = source.filename();
        const std::wstring baseName = sourceName.empty()
            ? L"item"
            : sourceName.wstring();
        const std::wstring extension = preserveExtension
            ? sourceName.extension().wstring()
            : L"";
        const std::wstring nameWithoutExtension = preserveExtension && !extension.empty()
            ? sourceName.stem().wstring()
            : baseName;

        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto id = ++sequence;
            auto candidate = root /
                (nameWithoutExtension + L"." + purpose + L"." + std::to_wstring(ticks) +
                    L"." + std::to_wstring(id) + extension);
            const auto inspection = InspectPath(candidate);
            if (inspection.presence == PathPresence::Missing) return candidate;
            if (inspection.presence == PathPresence::Inaccessible) return {};
        }
        return {};
    }

    inline Result RemovePath(const std::filesystem::path& path) {
        const std::filesystem::path deletedDestination(L"<permanently-deleted>");
        if (path.empty()) {
            return AuditedResult(FileOperationType::Delete, path, deletedDestination,
                { false, {}, L"empty path" });
        }
        std::error_code ec;
        const auto absolutePath = std::filesystem::absolute(path, ec).lexically_normal();
        if (ec) {
            return AuditedResult(FileOperationType::Delete, path, deletedDestination,
                { false, ec, L"resolving removal path" });
        }
        if (absolutePath == absolutePath.root_path()) {
            return AuditedResult(FileOperationType::Delete, absolutePath, deletedDestination,
                { false, std::make_error_code(std::errc::operation_not_permitted),
                    L"refusing to remove a file-system root" });
        }
        const auto inspection = InspectPath(absolutePath);
        if (inspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(FileOperationType::Delete, absolutePath, deletedDestination,
                { false, inspection.error, L"checking path" });
        }
        if (inspection.presence == PathPresence::Missing) {
            return AuditedResult(FileOperationType::Delete, absolutePath, deletedDestination,
                { true, {}, L"already absent" });
        }
        ec.clear();
        if (std::filesystem::is_directory(inspection.status)) {
            std::filesystem::remove_all(absolutePath, ec);
        }
        else {
            std::filesystem::remove(absolutePath, ec);
        }
        return AuditedResult(FileOperationType::Delete, absolutePath, deletedDestination,
            { !ec, ec, ec ? L"removing path" : L"" });
    }

    inline Result RemovePathWithRetries(
        const std::filesystem::path& path,
        unsigned int maxAttempts = 3,
        std::chrono::milliseconds retryDelay = std::chrono::milliseconds(100)) {
        if (maxAttempts == 0) maxAttempts = 1;

#ifdef _WIN32
        auto clearReadOnlyAttribute = [](const std::filesystem::path& candidate) noexcept {
            const auto attributes = GetFileAttributesW(candidate.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_READONLY) == 0) {
                return;
            }
            SetFileAttributesW(candidate.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        };

        const auto inspection = InspectPath(path);
        if (inspection.presence == PathPresence::Present) {
            clearReadOnlyAttribute(path);
            if (std::filesystem::is_directory(inspection.status)) {
                std::error_code iteratorError;
                const auto options = std::filesystem::directory_options::skip_permission_denied;
                for (std::filesystem::recursive_directory_iterator iterator(
                        path, options, iteratorError), end;
                    iterator != end;
                    iterator.increment(iteratorError)) {
                    if (iteratorError) {
                        iteratorError.clear();
                        continue;
                    }
                    clearReadOnlyAttribute(iterator->path());
                }
            }
        }
#endif

        Result lastResult;
        for (unsigned int attempt = 0; attempt < maxAttempts; ++attempt) {
            lastResult = RemovePath(path);
            if (lastResult) return lastResult;
            if (attempt + 1 < maxAttempts) std::this_thread::sleep_for(retryDelay);
        }
        if (!lastResult.context.empty()) {
            lastResult.context += L" after " + std::to_wstring(maxAttempts) + L" attempts";
        }
        return lastResult;
    }

    inline Result RecyclePath(const std::filesystem::path& path) {
        if (path.empty()) {
            return AuditedResult(FileOperationType::Recycle, path, L"<recycle-bin>",
                { false, {}, L"empty path" });
        }

        std::error_code ec;
        const auto absolutePath = std::filesystem::absolute(path, ec).lexically_normal();
        if (ec) {
            return AuditedResult(FileOperationType::Recycle, path, L"<recycle-bin>",
                { false, ec, L"resolving recycle path" });
        }
        if (absolutePath == absolutePath.root_path()) {
            return AuditedResult(FileOperationType::Recycle, absolutePath, L"<recycle-bin>",
                { false, std::make_error_code(std::errc::operation_not_permitted),
                    L"refusing to recycle a file-system root" });
        }
        const auto sourceInspection = InspectPath(absolutePath);
        if (sourceInspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(FileOperationType::Recycle, absolutePath, L"<recycle-bin>",
                { false, sourceInspection.error, L"checking recycle source" });
        }
        if (sourceInspection.presence == PathPresence::Missing) {
            return AuditedResult(FileOperationType::Recycle, absolutePath, L"<recycle-bin>",
                { false, std::make_error_code(std::errc::no_such_file_or_directory),
                    L"source does not exist" });
        }

#ifdef _WIN32
        std::wstring doubleNullPath = absolutePath.wstring();
        doubleNullPath.push_back(L'\0');
        doubleNullPath.push_back(L'\0');

        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = doubleNullPath.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        const int result = SHFileOperationW(&operation);
        const auto finalInspection = InspectPath(absolutePath);
        const bool success = result == 0 && !operation.fAnyOperationsAborted &&
            finalInspection.presence == PathPresence::Missing;
        const auto error = result != 0
            ? std::error_code(result, std::system_category())
            : (finalInspection.presence == PathPresence::Inaccessible
                ? finalInspection.error
                : std::error_code{});
        return AuditedResult(FileOperationType::Recycle, absolutePath, L"<recycle-bin>",
            { success, error,
                operation.fAnyOperationsAborted ? L"recycle operation was aborted"
                    : (success ? L"" : L"recycle operation failed") });
#else
        return AuditedResult(FileOperationType::Recycle, absolutePath, L"<recycle-bin>",
            { false, std::make_error_code(std::errc::operation_not_supported),
                L"recycle bin is only implemented on Windows" });
#endif
    }

    inline Result CopyToStaging(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        std::filesystem::path& staging,
        bool verifyContent = false) {
        staging.clear();
        std::error_code ec;
        const auto parent = destination.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) return { false, ec, L"creating destination parent" };
        }

        auto stagingRoot = parent;
        if (stagingRoot.empty()) {
            stagingRoot = std::filesystem::current_path(ec);
            if (ec) return { false, ec, L"resolving staging directory" };
        }
        staging = MakeUniquePath(stagingRoot, destination, L"staging");
        if (staging.empty()) return { false, {}, L"creating staging path" };

        const auto sourceStatus = std::filesystem::symlink_status(source, ec);
        if (ec) return { false, ec, L"reading source status" };

        if (std::filesystem::is_symlink(sourceStatus)) {
            std::filesystem::copy_symlink(source, staging, ec);
        }
        else if (std::filesystem::is_directory(sourceStatus)) {
            std::filesystem::copy(
                source,
                staging,
                std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::copy_symlinks,
                ec);
        }
        else if (std::filesystem::is_regular_file(sourceStatus)) {
            std::filesystem::copy_file(source, staging, ec);
        }
        else {
            return { false, std::make_error_code(std::errc::operation_not_supported),
                L"unsupported source type" };
        }
        if (ec) {
            RemovePath(staging);
            staging.clear();
            return { false, ec, L"copying to staging path" };
        }
        if (!EquivalentPathContents(source, staging, verifyContent)) {
            RemovePath(staging);
            staging.clear();
            return { false, std::make_error_code(std::errc::io_error),
                L"verifying staged copy" };
        }
        return { true, {}, L"" };
    }

    inline Result CopyPath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool verifyContent = false) {
        if (source.empty() || destination.empty()) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, {}, L"empty source or destination" });
        }
        if (ComparablePath(source) == ComparablePath(destination)) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { true, {}, L"same path" });
        }
        if (IsPathInside(destination, source)) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, {}, L"destination is inside source" });
        }
        const auto sourceInspection = InspectPath(source);
        if (sourceInspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, sourceInspection.error, L"checking source" });
        }
        if (sourceInspection.presence == PathPresence::Missing) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, {}, L"source does not exist" });
        }
        const auto destinationInspection = InspectPath(destination);
        if (destinationInspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, destinationInspection.error, L"checking destination" });
        }
        if (destinationInspection.presence == PathPresence::Present) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, {}, L"destination already exists" });
        }

        std::filesystem::path staging;
        auto stagingResult = CopyToStaging(source, destination, staging, verifyContent);
        if (!stagingResult) {
            return AuditedResult(FileOperationType::Copy, source, destination,
                std::move(stagingResult));
        }

        std::error_code ec;
        std::filesystem::rename(staging, destination, ec);
        if (ec) {
            RemovePath(staging);
            return AuditedResult(FileOperationType::Copy, source, destination,
                { false, ec, L"committing copied path" });
        }
        return AuditedResult(FileOperationType::Copy, source, destination,
            { true, {}, L"" });
    }

    /*
     * Keep a private undo payload while sending the original path to the OS
     * recycle bin. Copying precedes recycling so failure never sacrifices the
     * only recoverable copy. If recycling fails while the source still exists,
     * the new backup is removed and the operation is rolled back completely.
     */
    namespace Detail {
    template <typename RecycleOperation>
    inline Result RecyclePathWithUndoBackupUsing(
        const std::filesystem::path& source,
        const std::filesystem::path& undoBackup,
        bool verifyContent,
        RecycleOperation&& recycleOperation) {
        if (source.empty() || undoBackup.empty()) {
            return { false, {}, L"empty recycle source or undo backup path" };
        }
        if (ComparablePath(source) == ComparablePath(undoBackup)) {
            return { false, std::make_error_code(std::errc::invalid_argument),
                L"recycle source and undo backup are the same path" };
        }

        auto copyResult = CopyPath(source, undoBackup, verifyContent);
        if (!copyResult) {
            copyResult.context = L"creating recycle undo backup: " + copyResult.context;
            return copyResult;
        }

        auto recycleResult = recycleOperation(source);
        if (recycleResult) return recycleResult;

        // Some shell implementations report an error after completing the
        // operation. Absence of the source means the safe state was reached;
        // retain the undo payload instead of deleting the last accessible copy.
        if (!Exists(source)) {
            return { true, recycleResult.error,
                L"source disappeared after recycle reported an error; undo backup retained" };
        }

        auto cleanupResult = RemovePath(undoBackup);
        if (!cleanupResult) {
            return { false, cleanupResult.error,
                L"recycle failed; source remains and undo backup cleanup failed at " +
                    undoBackup.wstring() };
        }
        return { false, recycleResult.error,
            recycleResult.context.empty() ? L"recycle failed; source was restored"
                : recycleResult.context };
    }
    } // namespace Detail

    inline Result RecyclePathWithUndoBackup(
        const std::filesystem::path& source,
        const std::filesystem::path& undoBackup,
        bool verifyContent = false) {
        return Detail::RecyclePathWithUndoBackupUsing(
            source,
            undoBackup,
            verifyContent,
            [](const std::filesystem::path& path) { return RecyclePath(path); });
    }

    inline Result MovePath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool verifyContent = false) {
        const auto operation = ComparablePath(source.parent_path()) ==
            ComparablePath(destination.parent_path())
            ? FileOperationType::Rename
            : FileOperationType::Move;
        if (source.empty() || destination.empty()) {
            return AuditedResult(operation, source, destination,
                { false, {}, L"empty source or destination" });
        }
        if (ComparablePath(source) == ComparablePath(destination)) {
            return AuditedResult(operation, source, destination,
                { true, {}, L"same path" });
        }
        if (IsPathInside(destination, source)) {
            return AuditedResult(operation, source, destination,
                { false, {}, L"destination is inside source" });
        }
        const auto sourceInspection = InspectPath(source);
        if (sourceInspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(operation, source, destination,
                { false, sourceInspection.error, L"checking source" });
        }
        if (sourceInspection.presence == PathPresence::Missing) {
            return AuditedResult(operation, source, destination,
                { false, {}, L"source does not exist" });
        }
        const auto destinationInspection = InspectPath(destination);
        if (destinationInspection.presence == PathPresence::Inaccessible) {
            return AuditedResult(operation, source, destination,
                { false, destinationInspection.error, L"checking destination" });
        }
        if (destinationInspection.presence == PathPresence::Present) {
            return AuditedResult(operation, source, destination,
                { false, {}, L"destination already exists" });
        }

        std::error_code ec;
        const auto parent = destination.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return AuditedResult(operation, source, destination,
                    { false, ec, L"creating destination parent" });
            }
        }

        std::filesystem::rename(source, destination, ec);
        if (!ec) {
            return AuditedResult(operation, source, destination,
                { true, {}, L"" });
        }

        /*
        Cross-volume moves use a two-phase commit. The source is first copied
        and verified without publishing the destination. It is then renamed to
        a same-volume holding path before the destination becomes visible.
        A cleanup failure can therefore only leave an extra recovery copy.
        */

        std::filesystem::path staging;
        auto stagingResult = CopyToStaging(source, destination, staging, verifyContent);
        if (!stagingResult) {
            return AuditedResult(operation, source, destination,
                std::move(stagingResult));
        }

        auto sourceParent = source.parent_path();
        if (sourceParent.empty()) {
            sourceParent = std::filesystem::current_path(ec);
            if (ec) {
                RemovePath(staging);
                return AuditedResult(operation, source, destination,
                    { false, ec, L"resolving source holding directory" });
            }
        }
        const auto sourceHolding = MakeUniquePath(sourceParent, source, L"moving");
        if (sourceHolding.empty()) {
            RemovePath(staging);
            return AuditedResult(operation, source, destination,
                { false, {}, L"creating source holding path" });
        }

        ec.clear();
        std::filesystem::rename(source, sourceHolding, ec);
        if (ec) {
            RemovePath(staging);
            return AuditedResult(operation, source, destination,
                { false, ec, L"moving source to holding path" });
        }

        ec.clear();
        std::filesystem::rename(staging, destination, ec);
        if (ec) {
            std::error_code restoreEc;
            std::filesystem::rename(sourceHolding, source, restoreEc);
            RemovePath(staging);
            if (restoreEc) {
                return AuditedResult(operation, source, destination,
                    { false, restoreEc,
                        L"destination commit failed; source retained at " + sourceHolding.wstring() });
            }
            return AuditedResult(operation, source, destination,
                { false, ec, L"committing cross-volume destination" });
        }

        if (!EquivalentPathContents(sourceHolding, destination, verifyContent)) {
            auto destinationCleanup = RemovePath(destination);
            std::error_code restoreEc;
            std::filesystem::rename(sourceHolding, source, restoreEc);
            if (restoreEc) {
                return AuditedResult(operation, source, destination,
                    { false, restoreEc,
                        L"destination verification failed; source retained at " + sourceHolding.wstring() });
            }
            return AuditedResult(operation, source, destination,
                { false,
                    destinationCleanup ? std::make_error_code(std::errc::io_error)
                        : destinationCleanup.error,
                    L"verifying committed cross-volume destination" });
        }

        auto cleanupResult = RemovePath(sourceHolding);
        if (!cleanupResult) {
            return AuditedResult(operation, source, destination,
                { true, cleanupResult.error,
                    L"move committed; redundant source data retained at " + sourceHolding.wstring() });
        }
        return AuditedResult(operation, source, destination,
            { true, {}, L"" });
    }

    inline Result SelectUniqueDestination(
        const std::filesystem::path& preferredDestination,
        const std::unordered_set<std::wstring>& reservedDestinations,
        std::filesystem::path& selectedDestination) {
        selectedDestination.clear();
        if (preferredDestination.empty() || preferredDestination.filename().empty()) {
            return { false, std::make_error_code(std::errc::invalid_argument),
                L"restore destination has no file name" };
        }

        const auto directory = preferredDestination.parent_path();
        const auto stem = preferredDestination.stem().wstring();
        const auto extension = preferredDestination.extension().wstring();
        for (std::uint64_t suffix = 0; suffix < 100000; ++suffix) {
            const auto candidate = suffix == 0
                ? preferredDestination
                : directory / (stem + L" (" + std::to_wstring(suffix) + L")" + extension);
            const auto key = ComparablePath(candidate);
            if (reservedDestinations.count(key) != 0) continue;

            const auto inspection = InspectPath(candidate);
            if (inspection.presence == PathPresence::Inaccessible) {
                return { false, inspection.error,
                    L"checking restore destination: " + candidate.wstring() };
            }
            if (inspection.presence == PathPresence::Missing) {
                selectedDestination = candidate;
                return { true, {}, L"" };
            }
        }
        return { false, std::make_error_code(std::errc::file_exists),
            L"unable to allocate a unique restore destination" };
    }

    /*
     * Restore a group atomically from the caller's point of view. Every source
     * is validated and every collision-free target is planned before the first
     * move. If a later move fails, earlier moves are rolled back in reverse
     * order. A rollback failure never deletes either copy; the result identifies
     * the published recovery path so the caller can keep its metadata.
     */
    namespace Detail {
    template <typename MoveOperation>
    inline RestoreBatchResult RestoreBatchUsing(
        const std::vector<RestoreRequest>& requests,
        bool verifyContent,
        MoveOperation&& moveOperation) {
        RestoreBatchResult batch;
        batch.items.reserve(requests.size());
        for (const auto& request : requests) {
            batch.items.push_back({
                request.source, request.preferredDestination, {},
                { false, {}, L"not processed" }, false
            });
        }
        if (requests.empty()) {
            batch.success = true;
            batch.context = L"nothing to restore";
            return batch;
        }

        auto failPreflight = [&](std::size_t failedIndex, Result failure) {
            batch.context = failure.context;
            for (std::size_t index = 0; index < batch.items.size(); ++index) {
                batch.items[index].result = index == failedIndex
                    ? failure
                    : Result{ false, {}, L"restore batch aborted during preflight" };
            }
            return batch;
        };

        std::unordered_set<std::wstring> sourceKeys;
        std::vector<std::filesystem::path> sources;
        sources.reserve(requests.size());
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            if (request.source.empty() || request.preferredDestination.empty()) {
                return failPreflight(index,
                    { false, std::make_error_code(std::errc::invalid_argument),
                        L"restore request contains an empty path" });
            }
            const auto inspection = InspectPath(request.source);
            if (inspection.presence == PathPresence::Inaccessible) {
                return failPreflight(index,
                    { false, inspection.error, L"checking restore source" });
            }
            if (inspection.presence == PathPresence::Missing) {
                return failPreflight(index,
                    { false, std::make_error_code(std::errc::no_such_file_or_directory),
                        L"restore source does not exist: " + request.source.wstring() });
            }
            const auto key = ComparablePath(request.source);
            if (!sourceKeys.insert(key).second) {
                return failPreflight(index,
                    { false, std::make_error_code(std::errc::invalid_argument),
                        L"restore batch contains the same source more than once" });
            }
            sources.push_back(request.source);
        }

        for (std::size_t left = 0; left < sources.size(); ++left) {
            for (std::size_t right = left + 1; right < sources.size(); ++right) {
                if (IsPathInside(sources[left], sources[right]) ||
                    IsPathInside(sources[right], sources[left])) {
                    return failPreflight(right,
                        { false, std::make_error_code(std::errc::invalid_argument),
                            L"restore batch contains overlapping parent and child sources" });
                }
            }
        }

        std::unordered_set<std::wstring> reservedDestinations;
        std::vector<std::filesystem::path> plannedDestinations;
        std::vector<bool> noMove(requests.size(), false);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            if (ComparablePath(request.source) == ComparablePath(request.preferredDestination)) {
                batch.items[index].finalDestination = request.source;
                batch.items[index].result = { true, {}, L"already at restore destination" };
                reservedDestinations.insert(ComparablePath(request.source));
                plannedDestinations.push_back(request.source);
                noMove[index] = true;
                continue;
            }

            std::filesystem::path selected;
            auto selection = SelectUniqueDestination(
                request.preferredDestination, reservedDestinations, selected);
            if (!selection) return failPreflight(index, std::move(selection));
            for (const auto& source : sources) {
                if (IsPathInside(selected, source)) {
                    return failPreflight(index,
                        { false, std::make_error_code(std::errc::invalid_argument),
                            L"restore destination is inside a restore source" });
                }
            }
            for (const auto& planned : plannedDestinations) {
                if (IsPathInside(selected, planned) || IsPathInside(planned, selected)) {
                    return failPreflight(index,
                        { false, std::make_error_code(std::errc::invalid_argument),
                            L"restore destinations overlap as parent and child" });
                }
            }
            batch.items[index].finalDestination = selected;
            reservedDestinations.insert(ComparablePath(selected));
            plannedDestinations.push_back(selected);
        }

        std::vector<std::size_t> committed;
        committed.reserve(requests.size());
        for (std::size_t index = 0; index < requests.size(); ++index) {
            if (noMove[index]) continue;
            auto result = moveOperation(
                requests[index].source,
                batch.items[index].finalDestination,
                verifyContent);
            batch.items[index].result = result;
            if (result) {
                committed.push_back(index);
                continue;
            }

            batch.context = result.context.empty()
                ? L"restore move failed"
                : result.context;
            for (auto committedIt = committed.rbegin(); committedIt != committed.rend(); ++committedIt) {
                const auto committedIndex = *committedIt;
                auto rollback = moveOperation(
                    batch.items[committedIndex].finalDestination,
                    requests[committedIndex].source,
                    verifyContent);
                if (rollback) {
                    batch.items[committedIndex].rolledBack = true;
                    batch.items[committedIndex].result = {
                        false, result.error,
                        L"restore batch rolled back after a later item failed"
                    };
                }
                else {
                    batch.fullyRolledBack = false;
                    batch.items[committedIndex].result = {
                        false, rollback.error,
                        L"restore rollback failed; recoverable data remains at: " +
                            batch.items[committedIndex].finalDestination.wstring()
                    };
                }
            }
            for (std::size_t remaining = index + 1; remaining < requests.size(); ++remaining) {
                batch.items[remaining].result = {
                    false, {}, L"restore batch aborted after an earlier failure"
                };
            }
            for (std::size_t prior = 0; prior < requests.size(); ++prior) {
                if (noMove[prior]) {
                    batch.items[prior].result = {
                        false, result.error, L"restore batch failed; item was not moved"
                    };
                }
            }
            return batch;
        }

        batch.success = true;
        batch.context.clear();
        return batch;
    }
    } // namespace Detail

    inline RestoreBatchResult RestoreBatch(
        const std::vector<RestoreRequest>& requests,
        bool verifyContent = false) {
        return Detail::RestoreBatchUsing(
            requests,
            verifyContent,
            [](const std::filesystem::path& source,
                const std::filesystem::path& destination,
                bool verify) {
                return MovePath(source, destination, verify);
            });
    }

    inline Result BackupDestination(
        const std::filesystem::path& destination,
        const std::filesystem::path& backupRoot,
        std::filesystem::path& backupPath) {
        backupPath.clear();
        if (!Exists(destination)) return { true, {}, L"no destination to back up" };
        backupPath = MakeUniquePath(backupRoot, destination, L"replaced");
        if (backupPath.empty()) return { false, {}, L"cannot create replacement backup path" };
        auto result = MovePath(destination, backupPath);
        if (!result) backupPath.clear();
        return result;
    }

} // namespace PoggetCore::HistoryFileSystem
