#pragma once
#include <string>
#include <functional>
#include <system_error>

namespace PoggetMeta {

    class IPoggetMetaListener;

    enum class MetaOpType {
        Copy,
        Move,
        Rename,
        Delete,
        Recycle,
        RecycleWithUndoBackup
    };

    struct AsyncFileTask {
        MetaOpType opType = MetaOpType::Copy;
        std::wstring src;
        std::wstring dest;
        std::wstring origSrc;
        int index = -1;
        float scaledX = 0;
        float scaledY = 0;
        void* targetWin = nullptr;
        bool isMenuPaste = false; // Legacy fallback
        int batchCollisionChoice = 0;
        uint64_t batchId = 0;
        std::wstring historyBackupRoot;
        std::wstring replacedBackupPath;
        std::wstring preflightError;
        // Optional deletion boundary. A destructive task is rejected unless src
        // is an immediate child of this exact directory.
        std::wstring requiredSourceParent;
        bool verifyContent = false;
        IPoggetMetaListener* listener = nullptr;
        bool notifyListener = true;
        std::function<void(std::function<void()>)> dispatchToMainThread = nullptr;
        // Optional UI-layer decision hook. It may marshal to the UI thread and
        // block this worker until the modal retry/cancel decision is complete.
        // PoggetMeta itself never creates or references GUI objects.
        std::function<bool(const std::wstring&, const std::error_code&)> requestRetry = nullptr;
        std::wstring failureReason;
        std::error_code failureError;
        
        std::function<void()> onSuccess = nullptr;
        std::function<void()> onError = nullptr;
    };

}
