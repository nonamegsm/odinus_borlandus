#include <windows.h>
#include <stdio.h>
#include <string>
static LONG writeMode = 0;
static LONG flushMode = 0;
static LONG writeCalls = 0;
static LONG flushCalls = 0;
static BOOL WINAPI testWrite(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    InterlockedIncrement(&writeCalls);
    if (writeMode == 2) { *written = 0; SetLastError(ERROR_DISK_FULL); return FALSE; }
    if (writeMode == 3) { *written = 0; return TRUE; }
    if (writeMode == 1 && size > 7) size = 7;
    return WriteFile(file, data, size, written, overlapped);
}
static BOOL WINAPI testFlush(HANDLE file) {
    InterlockedIncrement(&flushCalls);
    if (flushMode) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return FlushFileBuffers(file);
}
#define WriteFile testWrite
#define FlushFileBuffers testFlush
#include "SessionLog.h"
#undef WriteFile
#undef FlushFileBuffers
static unsigned checks = 0;
static unsigned failures = 0;
static void check(bool success, const char* name) {
    ++checks;
    if (!success) { ++failures; printf("FAIL: %s\n", name); }
}
static std::wstring path(unsigned index) {
    wchar_t directory[MAX_PATH], suffix[80];
    GetCurrentDirectoryW(MAX_PATH, directory);
    wsprintfW(suffix, L"\\session-\x0416-\x6F22-%lu-%u.log", GetCurrentProcessId(), index);
    return std::wstring(directory) + suffix;
}
static std::string read(const std::wstring& name) {
    HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string text;
    char chunk[4096];
    DWORD count = 0;
    while (ReadFile(file, chunk, sizeof(chunk), &count, 0) && count) text.append(chunk, count);
    CloseHandle(file);
    return text;
}
static bool stamped(const std::string& line) {
    if (line.size() < 26 || line[0] != '[' || line[5] != '-' || line[8] != '-' ||
        line[11] != ' ' || line[14] != ':' || line[17] != ':' || line[20] != '.' ||
        line[24] != ']' || line[25] != ' ') return false;
    for (size_t i = 1; i < 24; ++i) {
        if (i == 5 || i == 8 || i == 11 || i == 14 || i == 17 || i == 20) continue;
        if (line[i] < '0' || line[i] > '9') return false;
    }
    return true;
}
struct Worker { OdinLog::SessionLog* log; wchar_t tag; };
static DWORD WINAPI worker(LPVOID argument) {
    Worker* task = static_cast<Worker*>(argument);
    for (unsigned i = 0; i < 100; ++i) {
        wchar_t line[64];
        wsprintfW(line, L"worker-%c-%03u 100%% <plain>", task->tag, i);
        if (!task->log->Append(line)) return 1;
    }
    return 0;
}
int main() {
    for (unsigned i = 0; i < 12; ++i) DeleteFileW(path(i).c_str());
    OdinLog::SessionLog log;
    check(!log.IsOpen() && !log.Failed(), "initial state");
    check(log.Open(path(0)) && log.IsOpen(), "Unicode path");
    const LONG flushBefore = flushCalls;
    check(log.Append(L"Hello \x0416 \x6F22 100% <b>plain</b>\r\nsecond\nthird\rfourth"), "Unicode multiline append");
    check(flushCalls == flushBefore + 1, "one durable flush per semantic append");
    const std::string text = read(path(0));
    check(text.find("Hello \xD0\x96 \xE6\xBC\xA2 100% <b>plain</b>") != std::string::npos,
          "UTF8 and literal percent/HTML retained while open");
    size_t cursor = 0;
    unsigned lines = 0;
    while (cursor < text.size()) {
        const size_t end = text.find("\r\n", cursor);
        check(end != std::string::npos && stamped(text.substr(cursor, end - cursor)), "each line has full timestamp");
        if (end == std::string::npos) break;
        ++lines; cursor = end + 2;
    }
    check(lines == 4, "CRLF LF CR normalized without duplicate lines");
    OdinLog::SessionLog duplicate;
    check(!duplicate.Open(path(0)) && duplicate.Failed(), "CREATE_NEW rejects existing file");
    duplicate.Close();
    check(duplicate.Failed() && !duplicate.IsOpen(), "close preserves sticky Open failure");
    check(read(path(0)) == text, "existing log not overwritten");
    log.Close();
    check(!log.IsOpen() && !log.Failed(), "normal close state");
    check(!log.Append(L"closed") && log.Failed(), "closed append returns failure");
    check(log.Open(path(1)) && !log.Failed(), "new Open clears sticky failure");
    writeMode = 1;
    const LONG writesBefore = writeCalls;
    check(log.Append(L"partial writes must complete exactly"), "partial WriteFile loop");
    check(writeCalls > writesBefore + 1, "multiple writes actually exercised");
    check(read(path(1)).find("partial writes must complete exactly\r\n") != std::string::npos,
          "partial writes preserve complete record");
    writeMode = 2;
    check(!log.Append(L"disk full") && log.Failed() && !log.IsOpen(), "write failure makes log unusable");
    writeMode = 0;
    const LONG stickyBefore = writeCalls;
    check(!log.Append(L"must not resume") && writeCalls == stickyBefore, "write failure remains sticky");
    log.Close();
    check(log.Failed(), "Close retains write failure");
    check(log.Open(path(2)), "reopen after write failure");
    writeMode = 3;
    check(!log.Append(L"zero byte write") && log.Failed(), "zero byte write cannot loop forever");
    writeMode = 0;
    check(log.Open(path(3)), "reopen after zero write");
    flushMode = 1;
    check(!log.Append(L"flush failure") && log.Failed() && !log.IsOpen(), "flush failure makes log unusable");
    flushMode = 0;
    check(!log.Append(L"must remain failed"), "flush failure sticky");
    check(log.Open(path(4)), "new log after flush failure");
    Worker tasks[2] = {{&log, L'A'}, {&log, L'B'}};
    HANDLE threads[2];
    threads[0] = CreateThread(0, 0, worker, &tasks[0], 0, 0);
    threads[1] = CreateThread(0, 0, worker, &tasks[1], 0, 0);
    check(threads[0] && threads[1], "two native workers created");
    if (threads[0] && threads[1]) {
        check(WaitForMultipleObjects(2, threads, TRUE, 20000) == WAIT_OBJECT_0, "workers completed");
        DWORD first = 1, second = 1;
        GetExitCodeThread(threads[0], &first); GetExitCodeThread(threads[1], &second);
        check(first == 0 && second == 0 && !log.Failed(), "concurrent append success");
        const std::string concurrent = read(path(4));
        for (unsigned tag = 0; tag < 2; ++tag) {
            for (unsigned i = 0; i < 100; ++i) {
                char token[64];
                sprintf(token, "worker-%c-%03u 100%% <plain>\r\n", 'A' + tag, i);
                const size_t at = concurrent.find(token);
                check(at >= 26 && at != std::string::npos && stamped(concurrent.substr(at - 26, 26)) &&
                      concurrent.find(token, at + 1) == std::string::npos, "concurrent complete record exactly once");
            }
        }
    } else {
        // A failed second creation must not leave the first worker using a destroyed logger.
        if (threads[0]) WaitForSingleObject(threads[0], INFINITE);
        if (threads[1]) WaitForSingleObject(threads[1], INFINITE);
    }
    if (threads[0]) CloseHandle(threads[0]);
    if (threads[1]) CloseHandle(threads[1]);
    log.Close();
    check(!duplicate.Open(path(10) + L"\\missing.log") && duplicate.Failed(), "missing parent returns failure");
    { OdinLog::SessionLog temporary; check(temporary.Open(path(5)) && temporary.Append(L"destructor closes"), "scoped log"); }
    HANDLE exclusive = CreateFileW(path(5).c_str(), GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    check(exclusive != INVALID_HANDLE_VALUE, "destructor released file handle");
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    for (unsigned i = 0; i < 12; ++i) DeleteFileW(path(i).c_str());
    printf("Session log: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
