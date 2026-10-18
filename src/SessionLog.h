#ifndef ODIN_SESSION_LOG_H
#define ODIN_SESSION_LOG_H

// Native C++03 logger; writes and flushes each complete semantic record.
#include <windows.h>
#include <string>
#include <limits.h>

namespace OdinLog {
class SessionLog {
public:
    SessionLog() : file_(INVALID_HANDLE_VALUE), failed_(false) {
        InitializeCriticalSection(&lock_);
    }
    ~SessionLog() {
        Close();
        DeleteCriticalSection(&lock_);
    }
    bool Open(const std::wstring& filename) {
        Guard guard(lock_);
        CloseLocked();
        failed_ = false;
        file_ = CreateFileW(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ, 0,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
        if (file_ == INVALID_HANDLE_VALUE) failed_ = true;
        return !failed_;
    }
    bool Append(const std::wstring& text) {
        Guard guard(lock_);
        if (failed_ || file_ == INVALID_HANDLE_VALUE) {
            failed_ = true;
            return false;
        }
        try {
            SYSTEMTIME now;
            GetLocalTime(&now);
            wchar_t stamp[40];
            wsprintfW(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                      now.wYear, now.wMonth, now.wDay, now.wHour,
                      now.wMinute, now.wSecond, now.wMilliseconds);
            std::wstring record;
            size_t begin = 0;
            do {
                const size_t end = text.find_first_of(L"\r\n", begin);
                record += stamp;
                record.append(text, begin, end == std::wstring::npos ? end : end - begin);
                record += L"\r\n";
                if (end == std::wstring::npos) break;
                begin = end + 1;
                if (text[end] == L'\r' && begin < text.size() && text[begin] == L'\n') ++begin;
            } while (begin < text.size());
            if (record.size() > INT_MAX) return Fail();
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, record.data(),
                                                  static_cast<int>(record.size()), 0, 0, 0, 0);
            if (!bytes) return Fail();
            std::string encoded(static_cast<size_t>(bytes), '\0');
            if (WideCharToMultiByte(CP_UTF8, 0, record.data(), static_cast<int>(record.size()),
                                    &encoded[0], bytes, 0, 0) != bytes) return Fail();
            size_t offset = 0;
            while (offset < encoded.size()) {
                DWORD written = 0;
                const DWORD requested = static_cast<DWORD>(encoded.size() - offset);
                if (!WriteFile(file_, encoded.data() + offset, requested, &written, 0) ||
                    written == 0 || written > requested) return Fail();
                offset += written;
            }
            if (!FlushFileBuffers(file_)) return Fail();
            return true;
        } catch (...) {
            return Fail();
        }
    }
    bool Failed() const {
        Guard guard(lock_);
        return failed_;
    }
    bool IsOpen() const {
        Guard guard(lock_);
        return file_ != INVALID_HANDLE_VALUE && !failed_;
    }
    void Close() {
        Guard guard(lock_);
        CloseLocked();
    }
private:
    class Guard {
    public:
        explicit Guard(CRITICAL_SECTION& lock) : lock_(lock) { EnterCriticalSection(&lock_); }
        ~Guard() { LeaveCriticalSection(&lock_); }
    private:
        Guard(const Guard&);
        Guard& operator=(const Guard&);
        CRITICAL_SECTION& lock_;
    };
    SessionLog(const SessionLog&);
    SessionLog& operator=(const SessionLog&);
    bool Fail() { failed_ = true; return false; }
    void CloseLocked() {
        if (file_ != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(file_)) failed_ = true;
            file_ = INVALID_HANDLE_VALUE;
        }
    }
    mutable CRITICAL_SECTION lock_;
    HANDLE file_;
    bool failed_;
};
}
#endif
