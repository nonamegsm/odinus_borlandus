#include "EasyJtagOdin4Core.h"
#include "Odin4FirmwareStream.h"
#include "Odin4BccTrailer.h"
#include "lz4frame.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include "BccCompat.h"
#include <cstddef>
#include <cstring>

#include <map>
#include <set>
#include <string>
#include <vector>

BCC_STATIC_ASSERT(sizeof(EJODIN4_ArchiveEntry) == 276);
BCC_STATIC_ASSERT(sizeof(EJODIN4_ArchiveSummary) == 98);
BCC_STATIC_ASSERT(offsetof(EJODIN4_ArchiveEntry, data_offset) == 256);
BCC_STATIC_ASSERT(offsetof(EJODIN4_ArchiveSummary, actual_md5) == 32);
BCC_STATIC_ASSERT(sizeof(EJODIN4_AnalyzedImage) == 320);
BCC_STATIC_ASSERT(sizeof(EJODIN4_AnalysisSummary) == 20);

namespace {
template<class T> T* vector_data(std::vector<T>& value){return value.empty()?0:&value[0];}
template<class T> const T* vector_data(const std::vector<T>& value){return value.empty()?0:&value[0];}
struct Failure { EJODIN4_Status status; const char* message; Failure(EJODIN4_Status s,const char* m):status(s),message(m){} };
void fail(EJODIN4_Status status, const char* message) { throw Failure(status,message); }
EJODIN4_Status result(EJODIN4_Status status, const char* message, char* text, uint32_t capacity)  {
    if (text && capacity) {
        const size_t copied = std::min(std::strlen(message), static_cast<size_t>(capacity - 1));
        std::memcpy(text,message,copied); text[copied] = 0;
    }
    return status;
}
struct Cancellation {
    EJODIN4_CancelCallback callback; void* context;
    Cancellation(EJODIN4_CancelCallback c,void* p):callback(c),context(p){}
    void check() const { if (callback && callback(context)) fail(EJODIN4_CANCELLED,"Archive inspection cancelled"); }
};
struct File {
    HANDLE handle;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File():handle(INVALID_HANDLE_VALUE) {}
    private: File(const File&); public:
    private: File& operator=(const File&); public:
    void seek(uint64_t position) const {
        LARGE_INTEGER offset; offset.QuadPart = static_cast<LONGLONG>(position);
        if (!SetFilePointerEx(handle,offset,0,FILE_BEGIN)) fail(EJODIN4_FILE_IO_ERROR,"Cannot seek archive file");
    }
    void read(void* data, DWORD size, const Cancellation& cancellation) const {
        cancellation.check();
        DWORD actual = 0;
        if (!ReadFile(handle,data,size,&actual,0) || actual != size)
            fail(EJODIN4_FILE_IO_ERROR,"Cannot read complete archive file range");
        cancellation.check();
    }
};
struct Md5 {
    HCRYPTPROV provider; HCRYPTHASH hash;
    Md5():provider(0),hash(0) {
        if (!CryptAcquireContextW(&provider,0,0,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT))
            fail(EJODIN4_INTERNAL_ERROR,"Cannot initialize Windows MD5 provider");
        if (!CryptCreateHash(provider,CALG_MD5,0,0,&hash)) {
            CryptReleaseContext(provider,0); provider = 0;
            fail(EJODIN4_INTERNAL_ERROR,"Cannot initialize Windows MD5 hash");
        }
    }
    ~Md5() { if (hash) CryptDestroyHash(hash); if (provider) CryptReleaseContext(provider,0); }
    private: Md5(const Md5&); public:
    private: Md5& operator=(const Md5&); public:
    void add(const void* data, DWORD bytes) const {
        /* Legacy MinGW headers omit const on this read-only Windows input. */
        if (!CryptHashData(hash,const_cast<BYTE*>(static_cast<const BYTE*>(data)),bytes,0))
            fail(EJODIN4_INTERNAL_ERROR,"Windows MD5 update failed");
    }
    void finish(char (&text)[33]) const {
        BYTE digest[16]; DWORD hash_size = 0; DWORD size_bytes = sizeof(hash_size);
        if (!CryptGetHashParam(hash,HP_HASHSIZE,reinterpret_cast<BYTE*>(&hash_size),&size_bytes,0) ||
            size_bytes != sizeof(hash_size) || hash_size != sizeof(digest))
            fail(EJODIN4_INTERNAL_ERROR,"Windows MD5 provider returned an invalid digest size");
        DWORD size = sizeof(digest);
        if (!CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0) || size != sizeof(digest))
            fail(EJODIN4_INTERNAL_ERROR,"Windows MD5 completion failed");
        const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(digest); ++i) {
            text[i * 2] = hex[digest[i] >> 4]; text[i * 2 + 1] = hex[digest[i] & 15];
        }
        text[32] = 0;
    }
};
char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
std::string lower_ascii(std::string text) { for (size_t i=0;i<text.size();++i) text[i] = lower_ascii(text[i]); return text; }
bool ends_with(const std::string& text, const char* suffix) {
    const size_t size = std::strlen(suffix);
    return text.size() >= size && text.compare(text.size() - size,size,suffix) == 0;
}
bool utf8(const std::string& text) {
    return !text.empty() && text.size() <= 32767 &&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),0,0) > 0;
}
std::wstring native_path(const char* path, uint32_t bytes) {
    if (!path || bytes < 2 || bytes > 32768 || path[bytes - 1] != 0 ||
        std::memchr(path,0,bytes - 1)) fail(EJODIN4_INVALID_ARGUMENT,"Archive path must contain one terminating NUL");
    const std::string text(path,bytes - 1);
    if (!utf8(text)) fail(EJODIN4_INVALID_ARGUMENT,"Archive path is not valid UTF-8");
    const bool drive = text.size() >= 3 && ((text[0] >= 'A' && text[0] <= 'Z') ||
        (text[0] >= 'a' && text[0] <= 'z')) && text[1] == ':' && (text[2] == '\\' || text[2] == '/');
    const bool unc = text.size() >= 5 && text[0] == '\\' && text[1] == '\\' && text[2] != '.' && text[2] != '?';
    if (!drive && !unc) fail(EJODIN4_INVALID_ARGUMENT,"Archive path must be an absolute drive or UNC file path");
    size_t start = drive ? 3u : 2u;
    size_t components = 0;
    for (size_t i = start; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '\\' && text[i] != '/') {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 32 || c == ':' || c == '?' || c == '*') fail(EJODIN4_INVALID_ARGUMENT,"Archive path contains a device namespace or invalid character");
            continue;
        }
        const std::string part = text.substr(start,i - start);
        if (part.empty() || part == "." || part == ".." || part[part.size()-1] == '.' || part[part.size()-1] == ' ')
            fail(EJODIN4_INVALID_ARGUMENT,"Archive path contains an ambiguous component");
        std::string base = lower_ascii(part.substr(0,part.find('.')));
        while (!base.empty() && base[base.size()-1] == ' ') base.resize(base.size()-1);
        const bool serial_device = base.size() == 4 && (base.compare(0,3,"com") == 0 || base.compare(0,3,"lpt") == 0) && base[3] >= '0' && base[3] <= '9';
        const bool unicode_device = base.size() == 5 && (base.compare(0,3,"com") == 0 || base.compare(0,3,"lpt") == 0) &&
            static_cast<unsigned char>(base[3]) == 0xc2 && (static_cast<unsigned char>(base[4]) == 0xb9 || static_cast<unsigned char>(base[4]) == 0xb2 || static_cast<unsigned char>(base[4]) == 0xb3);
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "clock$" || base == "conin$" || base == "conout$" || serial_device || unicode_device ||
            (unc && components == 1 && (lower_ascii(part) == "pipe" || lower_ascii(part) == "mailslot")))
            fail(EJODIN4_INVALID_ARGUMENT,"Archive path names a reserved Windows device or pipe");
        ++components; start = i + 1;
    }
    if (unc && components < 3) fail(EJODIN4_INVALID_ARGUMENT,"UNC archive path requires server, share and file");
    const int required = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),0,0);
    std::wstring wide(static_cast<size_t>(required),L'\0');
    if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),&wide[0],required) != required)
        fail(EJODIN4_INVALID_ARGUMENT,"Cannot decode archive path");
    return wide;
}
uint64_t file_size(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}
bool all_zero(const unsigned char* data, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (data[i] != 0) return false;
    return true;
}
uint64_t number(const unsigned char* field, size_t bytes, bool base256) {
    uint64_t value = 0;
    if ((field[0] & 0x80) != 0) {
        if (!base256 || (field[0] & 0x40) != 0) fail(EJODIN4_INVALID_ARCHIVE,"Negative or unsupported TAR numeric field");
        value = field[0] & 0x3fu;
        for (size_t i = 1; i < bytes; ++i) {
            if (value > (UINT64_MAX - field[i]) / 256) fail(EJODIN4_INVALID_ARCHIVE,"TAR size overflows 64 bits");
            value = value * 256 + field[i];
        }
        return value;
    }
    size_t i = 0;
    while (i < bytes && (field[i] == ' ' || field[i] == '\t')) ++i;
    while (i < bytes && field[i] >= '0' && field[i] <= '7') {
        const unsigned digit = field[i++] - '0';
        if (value > (UINT64_MAX - digit) / 8) fail(EJODIN4_INVALID_ARCHIVE,"TAR octal field overflows 64 bits");
        value = value * 8 + digit;
    }
    for (; i < bytes; ++i) if (field[i] != 0 && field[i] != ' ' && field[i] != '\t')
        fail(EJODIN4_INVALID_ARCHIVE,"TAR numeric field contains invalid characters");
    return value;
}
std::string string_field(const unsigned char* field, size_t bytes) {
    const void* terminator = std::memchr(field,0,bytes);
    const size_t size = terminator ? static_cast<size_t>(static_cast<const unsigned char*>(terminator) - field) : bytes;
    return std::string(reinterpret_cast<const char*>(field),size);
}
std::string entry_name(const unsigned char* header, bool directory) {
    const bool ustar = std::memcmp(header + 257,"ustar\0",6) == 0;
    const bool gnu = std::memcmp(header + 257,"ustar ",6) == 0;
    if (!ustar && !gnu && !all_zero(header + 257,6)) fail(EJODIN4_UNSUPPORTED_ARCHIVE,"Unsupported TAR header format");
    std::string name = string_field(header,100);
    if (ustar) {
        const std::string prefix = string_field(header + 345,155);
        if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (directory && !name.empty() && name[name.size()-1] == '/') name.resize(name.size()-1);
    if (name.size() > 255 || !utf8(name) || name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        fail(EJODIN4_INVALID_ARCHIVE,"Unsafe, invalid UTF-8 or overlong TAR entry path");
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i != name.size() && name[i] != '/') {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (c < 32 || c == 127) fail(EJODIN4_INVALID_ARCHIVE,"TAR entry path contains a control character");
            continue;
        }
        const std::string part = name.substr(start,i - start);
        if (part.empty() || part == "." || part == "..") fail(EJODIN4_INVALID_ARCHIVE,"TAR entry path contains traversal or empty components");
        start = i + 1;
    }
    return name;
}
bool trailer_text(const unsigned char* tail, size_t bytes, size_t digest, size_t start, std::string& expected) {
    if (digest != start || digest > bytes || bytes - digest < 32) return false;
    size_t end = bytes;
    if (end > digest + 32 && tail[end - 1] == '\n') { --end; if (end > digest + 32 && tail[end - 1] == '\r') --end; }
    for (size_t i = digest; i < digest + 32; ++i) if (!tar_md5::is_hex_char(tail[i])) return false;
    if (end != digest + 32) {
        size_t i = digest + 32;
        if (i >= end || (tail[i] != ' ' && tail[i] != '\t')) return false;
        while (i < end && (tail[i] == ' ' || tail[i] == '\t')) ++i;
        if (i < end && tail[i] == '*') ++i;
        if (i == end) return false;
        const std::string filename(reinterpret_cast<const char*>(tail + i),end - i);
        if (!utf8(filename)) return false;
        for (; i < end; ++i) if (tail[i] < 32 || tail[i] == 127) return false;
    }
    expected = lower_ascii(std::string(reinterpret_cast<const char*>(tail + digest),32));
    return true;
}
struct ScanNotifier {
    const ejodin4::ArchiveScanCallback& progress;uint64_t total;const Cancellation& cancellation;
    ScanNotifier(const ejodin4::ArchiveScanCallback& p,uint64_t t,const Cancellation& c):progress(p),total(t),cancellation(c){}
    void operator()(uint64_t completed)const{if(progress)progress(completed,total);cancellation.check();}
};
void inspect(const char* path, uint32_t path_bytes, std::vector<EJODIN4_ArchiveEntry>& entries,
             EJODIN4_ArchiveSummary& summary, const Cancellation& cancellation,
             File& file, BY_HANDLE_FILE_INFORMATION& before,
             const ejodin4::ArchiveScanCallback& progress = ejodin4::ArchiveScanCallback()) {
    const std::wstring native = native_path(path,path_bytes);
    cancellation.check();
    file.handle = CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if (file.handle == INVALID_HANDLE_VALUE) fail(EJODIN4_FILE_IO_ERROR,"Cannot open archive read-only (missing file or writer sharing conflict)");
    if (GetFileType(file.handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.handle,&before) ||
        (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0)
        fail(EJODIN4_FILE_IO_ERROR,"Archive must be a regular filesystem disk file");
    summary.archive_bytes = file_size(before);
    if (summary.archive_bytes > static_cast<uint64_t>(INT64_MAX))
        fail(EJODIN4_UNSUPPORTED_ARCHIVE,"Archive exceeds supported signed 64-bit file offsets");
    if (summary.archive_bytes < 1536) fail(EJODIN4_INVALID_ARCHIVE,"Archive is truncated or contains no image data");
    const DWORD tail_bytes = static_cast<DWORD>(std::min(summary.archive_bytes,UINT64_C(65536)));
    const uint64_t tail_offset = summary.archive_bytes - tail_bytes;
    std::vector<unsigned char> tail(tail_bytes);
    file.seek(tail_offset); file.read(vector_data(tail),tail_bytes,cancellation);
    const tar_md5::TrailerPos located = tar_md5::locate_trailer(reinterpret_cast<const char*>(vector_data(tail)),tail.size(),tail_offset);
    summary.payload_bytes = summary.archive_bytes;
    std::string expected;
    if (located.md5_pos >= 0 && located.trailer_start >= 0 &&
        (tail_offset + static_cast<uint64_t>(located.trailer_start)) % 512 == 0 &&
        trailer_text(vector_data(tail),tail.size(),static_cast<size_t>(located.md5_pos),static_cast<size_t>(located.trailer_start),expected)) {
        summary.payload_bytes = tail_offset + static_cast<uint64_t>(located.trailer_start);
        summary.flags = EJODIN4_ARCHIVE_MD5_PRESENT;
        std::memcpy(summary.expected_md5,expected.c_str(),33);
    }
    /* A md5sum filename may itself contain 32 hex digits on a block boundary.
     * Upstream's last-candidate heuristic can select those; prefer the first
     * block-aligned candidate whose complete remaining line is strict text. */
    for (size_t candidate = static_cast<size_t>((512 - tail_offset % 512) % 512);
         candidate + 32 <= tail.size(); candidate += 512) {
        std::string digest;
        if (trailer_text(vector_data(tail),tail.size(),candidate,candidate,digest)) {
            expected = digest;
            summary.payload_bytes = tail_offset + candidate;
            summary.flags = EJODIN4_ARCHIVE_MD5_PRESENT;
            std::memcpy(summary.expected_md5,expected.c_str(),33);
            break;
        }
    }
    if (ends_with(lower_ascii(std::string(path)),".tar.md5") && expected.empty())
        fail(EJODIN4_INVALID_ARCHIVE,"TAR.MD5 file is missing a strictly valid MD5 trailer");
    if (summary.payload_bytes < 1536 || summary.payload_bytes % 512 != 0)
        fail(EJODIN4_INVALID_ARCHIVE,"TAR payload size is not a complete block sequence");
    file.seek(0); Md5 md5;
    ScanNotifier notify_progress(progress,summary.payload_bytes,cancellation);
    notify_progress(0);
    bcc::Array<unsigned char,65536> buffer;
    std::set<std::string> names;
    uint64_t offset = 0;
    bool ended = false;
    uint32_t header_count = 0;
    while (offset < summary.payload_bytes) {
        file.read(buffer.data(),512,cancellation); md5.add(buffer.data(),512); offset += 512;
        if(offset<summary.payload_bytes)notify_progress(offset);
        if (all_zero(buffer.data(),512)) {
            if (summary.payload_bytes - offset < 512) fail(EJODIN4_INVALID_ARCHIVE,"TAR requires two zero end blocks");
            file.read(buffer.data(),512,cancellation); md5.add(buffer.data(),512); offset += 512;
            if(offset<summary.payload_bytes)notify_progress(offset);
            if (!all_zero(buffer.data(),512)) fail(EJODIN4_INVALID_ARCHIVE,"TAR second end block is not zero");
            while (offset < summary.payload_bytes) {
                const DWORD bytes = static_cast<DWORD>(std::min(UINT64_C(65536),summary.payload_bytes - offset));
                file.read(buffer.data(),bytes,cancellation); md5.add(buffer.data(),bytes); offset += bytes;
                if(offset<summary.payload_bytes)notify_progress(offset);
                if (!all_zero(buffer.data(),bytes)) fail(EJODIN4_INVALID_ARCHIVE,"TAR has appended data or a second archive");
            }
            ended = true; break;
        }
        const uint64_t checksum = number(buffer.data() + 148,8,false);
        if (++header_count > 1024) fail(EJODIN4_UNSUPPORTED_ARCHIVE,"TAR exceeds the 1024 total-header limit");
        uint64_t unsigned_sum = 0; int64_t signed_sum = 0;
        for (size_t i = 0; i < 512; ++i) {
            const unsigned char c = i >= 148 && i < 156 ? static_cast<unsigned char>(' ') : buffer[i];
            unsigned_sum += c; signed_sum += static_cast<signed char>(c);
        }
        if (checksum != unsigned_sum && (signed_sum < 0 || checksum != static_cast<uint64_t>(signed_sum)))
            fail(EJODIN4_INVALID_ARCHIVE,"TAR header checksum does not match");
        const unsigned char type = buffer[156];
        const bool directory = type == '5';
        if (type != 0 && type != '0' && !directory)
            fail(EJODIN4_UNSUPPORTED_ARCHIVE,"TAR links, sparse files and PAX/GNU extensions are unsupported");
        const uint64_t size = number(buffer.data() + 124,12,true);
        if (directory && size) fail(EJODIN4_INVALID_ARCHIVE,"TAR directory contains data");
        const std::string name = entry_name(buffer.data(),directory);
        if (!names.insert(lower_ascii(name)).second) fail(EJODIN4_INVALID_ARCHIVE,"TAR contains duplicate entry paths");
        if (summary.payload_bytes - offset < 1024 || size > summary.payload_bytes - offset - 1024)
            fail(EJODIN4_INVALID_ARCHIVE,"TAR entry data extent exceeds payload bounds");
        const uint64_t padding = (512 - size % 512) % 512;
        if (padding > summary.payload_bytes - offset - 1024 - size)
            fail(EJODIN4_INVALID_ARCHIVE,"TAR entry padding exceeds payload bounds");
        if (!directory) {
            if (entries.size() >= 512) fail(EJODIN4_UNSUPPORTED_ARCHIVE,"TAR exceeds the 512 image limit");
            EJODIN4_ArchiveEntry entry;std::memset(&entry,0,sizeof(entry));
            std::memcpy(entry.name,name.c_str(),name.size() + 1);
            entry.data_offset = offset; entry.data_bytes = size;
            const std::string folded = lower_ascii(name);
            if (ends_with(folded,".lz4")) entry.flags |= EJODIN4_ARCHIVE_ENTRY_LZ4;
            if (ends_with(folded,".pit") || ends_with(folded,".pit.lz4")) entry.flags |= EJODIN4_ARCHIVE_ENTRY_PIT;
            entries.push_back(entry);
            if (size > UINT64_MAX - summary.image_bytes) fail(EJODIN4_INVALID_ARCHIVE,"TAR image byte total overflows");
            summary.image_bytes += size;
        }
        uint64_t remaining = size;
        while (remaining) {
            const DWORD bytes = static_cast<DWORD>(std::min(UINT64_C(65536),remaining));
            file.read(buffer.data(),bytes,cancellation); md5.add(buffer.data(),bytes); remaining -= bytes; offset += bytes;
            if(offset<summary.payload_bytes)notify_progress(offset);
        }
        if (padding) {
            const DWORD bytes = static_cast<DWORD>(padding);
            file.read(buffer.data(),bytes,cancellation); md5.add(buffer.data(),bytes); offset += bytes;
            if(offset<summary.payload_bytes)notify_progress(offset);
        }
    }
    if (!ended || entries.empty() || !summary.image_bytes) fail(EJODIN4_INVALID_ARCHIVE,"TAR requires image data and a complete two-block terminator");
    md5.finish(summary.actual_md5);
    if (!expected.empty()) {
        if (expected != summary.actual_md5) fail(EJODIN4_CHECKSUM_MISMATCH,"TAR.MD5 checksum does not match the TAR payload");
        summary.flags |= EJODIN4_ARCHIVE_MD5_VERIFIED;
    }
    cancellation.check();
    BY_HANDLE_FILE_INFORMATION after;std::memset(&after,0,sizeof(after));
    if (!GetFileInformationByHandle(file.handle,&after) || file_size(after) != summary.archive_bytes ||
        CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime) != 0 ||
        before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
        fail(EJODIN4_FILE_IO_ERROR,"Archive identity, length or timestamp changed during inspection");
    summary.entry_count = static_cast<uint32_t>(entries.size());
    notify_progress(summary.payload_bytes);
}

void unchanged(const File& file, const BY_HANDLE_FILE_INFORMATION& before, const char* message) {
    BY_HANDLE_FILE_INFORMATION after;std::memset(&after,0,sizeof(after));
    if (!GetFileInformationByHandle(file.handle,&after) || file_size(after) != file_size(before) ||
        CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime) != 0 ||
        before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
        fail(EJODIN4_FILE_IO_ERROR,message);
}
void retain_pit(std::vector<uint8_t>* pit, const unsigned char* bytes, size_t count) {
    if (!pit) return;
    if (count > 1024u * 1024u - pit->size()) fail(EJODIN4_INVALID_PIT,"Selected decoded PIT exceeds 1 MiB");
    pit->insert(pit->end(),bytes,bytes + count);
}
struct Decoder {
    LZ4F_dctx* context;
    Decoder():context(0) {
        if (LZ4F_isError(LZ4F_createDecompressionContext(&context,LZ4F_VERSION)))
            fail(EJODIN4_INTERNAL_ERROR,"Cannot initialize LZ4 frame decoder");
    }
    ~Decoder() { if (context) LZ4F_freeDecompressionContext(context); }
    private: Decoder(const Decoder&); public:
    private: Decoder& operator=(const Decoder&); public:
};
class MemberReader  : public ejodin4::ImageReader {
    const File& file_; EJODIN4_ArchiveEntry entry_;
    uint64_t stored_position_, decoded_, decoded_limit_;
    bcc::Array<unsigned char,65536> input_;
    size_t available_, position_;
    std::unique_ptr<Decoder> decoder_;
    LZ4F_frameInfo_t info_;
    bool initialized_, ended_;
    void fill(const Cancellation& cancel) {
        const DWORD bytes = static_cast<DWORD>(std::min(UINT64_C(65536),entry_.data_bytes - stored_position_));
        file_.seek(entry_.data_offset + stored_position_);
        file_.read(input_.data(),bytes,cancel); stored_position_ += bytes;
        available_ = bytes; position_ = 0;
    }
public:
    MemberReader(const File& file, const EJODIN4_ArchiveEntry& entry, uint64_t limit = UINT64_MAX) : file_(file),entry_(entry),stored_position_(0),decoded_(0),decoded_limit_(limit),available_(0),position_(0),initialized_(false),ended_(false) {std::memset(&info_,0,sizeof(info_));}
    uint64_t stored_bytes_processed() const  {
        return (entry_.flags&EJODIN4_ARCHIVE_ENTRY_LZ4)?stored_position_-uint64_t(available_-position_):stored_position_;
    }
    EJODIN4_Status read(uint8_t* data, uint32_t capacity, uint32_t& actual,
        const ejodin4::Cancellation& cancel, std::string& error)  {
        actual = 0;
        try {
            if (!data || !capacity) fail(EJODIN4_INVALID_ARGUMENT,"Image reader requires a nonempty output buffer");
            const Cancellation cancellation(cancel.callback,cancel.context); cancellation.check();
            if (ended_) return EJODIN4_OK;
            if (!(entry_.flags & EJODIN4_ARCHIVE_ENTRY_LZ4) && entry_.data_bytes > decoded_limit_)
                fail(EJODIN4_DECODE_LIMIT_EXCEEDED,"Decoded firmware byte limit exceeded");
            if (!(entry_.flags & EJODIN4_ARCHIVE_ENTRY_LZ4)) {
                const DWORD bytes = static_cast<DWORD>(std::min(static_cast<uint64_t>(capacity),entry_.data_bytes - stored_position_));
                if (!bytes) { ended_ = true; return EJODIN4_OK; }
                file_.seek(entry_.data_offset + stored_position_); file_.read(data,bytes,cancellation);
                stored_position_ += bytes; actual = bytes; return EJODIN4_OK;
            }
            if (!initialized_) {
                if (entry_.data_bytes < 7) fail(EJODIN4_INVALID_COMPRESSED_DATA,"LZ4 frame header is truncated");
                fill(cancellation);
                if (input_[0] != 4 || input_[1] != 0x22 || input_[2] != 0x4d || input_[3] != 0x18)
                    fail(EJODIN4_INVALID_COMPRESSED_DATA,"Only modern standard LZ4 frames are supported");
                decoder_.reset(new Decoder); position_ = available_;
                const size_t hint = LZ4F_getFrameInfo(decoder_->context,&info_,input_.data(),&position_);
                if (LZ4F_isError(hint) || info_.frameType != LZ4F_frame || info_.dictID != 0)
                    fail(EJODIN4_INVALID_COMPRESSED_DATA,"Invalid LZ4 frame header or unsupported dictionary");
                if (info_.contentSize && info_.contentSize > decoded_limit_)
                    fail(EJODIN4_DECODE_LIMIT_EXCEEDED,"Declared LZ4 content size exceeds decoded byte limit");
                initialized_ = true;
            }
            for (;;) {
                cancellation.check();
                if (position_ == available_ && stored_position_ < entry_.data_bytes) fill(cancellation);
                size_t consumed = available_ - position_, produced = capacity;
                const size_t hint = LZ4F_decompress(decoder_->context,data,&produced,input_.data() + position_,&consumed,0);
                if (LZ4F_isError(hint)) fail(EJODIN4_INVALID_COMPRESSED_DATA,"LZ4 decompression, checksum or content-size validation failed");
                position_ += consumed;
                if (produced > UINT64_MAX - decoded_) fail(EJODIN4_DECODE_LIMIT_EXCEEDED,"Decoded image size overflows");
                decoded_ += produced;
                if (!hint) {
                    if (position_ != available_ || stored_position_ != entry_.data_bytes)
                        fail(EJODIN4_INVALID_COMPRESSED_DATA,"LZ4 member contains concatenated frames or trailing bytes");
                    if (info_.contentSize && info_.contentSize != decoded_)
                        fail(EJODIN4_INVALID_COMPRESSED_DATA,"LZ4 decoded size differs from frame header");
                    ended_ = true;
                }
                if (produced || ended_) { actual = static_cast<uint32_t>(produced); return EJODIN4_OK; }
                if (!consumed) fail(EJODIN4_INVALID_COMPRESSED_DATA,"LZ4 frame is truncated or decoder cannot make progress");
            }
        } catch (const Failure& failure) { error = failure.message; return failure.status; }
        catch (...) { error = "Image reader exception"; return EJODIN4_INTERNAL_ERROR; }
    }
};
class EncodedMemberReader : public ejodin4::EncodedImageReader {
    const File& file_; EJODIN4_ArchiveEntry entry_; ejodin4::PlanImage image_;
    Decoder decoder_; HCRYPTPROV provider_; HCRYPTHASH hash_;
    uint64_t position_,decoded_;uint32_t block_max_;
    bool initialized_,ended_,unsupported_short_,audit_only_;
    bool block_checksum_,content_checksum_;
    static uint32_t word(const unsigned char* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
    void read(void* data,uint32_t bytes,const Cancellation& cancel){
        if(position_>entry_.data_bytes||bytes>entry_.data_bytes-position_)fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 record is truncated");
        file_.seek(entry_.data_offset+position_);file_.read(data,bytes,cancel);position_+=bytes;
    }

    void start_hash(){
        if(!CryptAcquireContextW(&provider_,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)||!CryptCreateHash(provider_,0x0000800c,0,0,&hash_))
            fail(EJODIN4_INTERNAL_ERROR,"Cannot initialize encoded image SHA256");
    }
    void check_digest(){
        bcc::Array<unsigned char,32> digest;DWORD bytes=32;
        if(!CryptGetHashParam(hash_,HP_HASHVAL,digest.data(),&bytes,0)||bytes!=32)fail(EJODIN4_INTERNAL_ERROR,"Encoded image SHA256 completion failed");
        if(decoded_!=image_.decoded_bytes||digest!=image_.digest)fail(EJODIN4_CHECKSUM_MISMATCH,"Encoded image second-pass SHA256 differs from plan");
    }
    void initialize(const Cancellation& cancel){
        if(initialized_)return;
        if(!(entry_.flags&EJODIN4_ARCHIVE_ENTRY_LZ4))fail(EJODIN4_DRIVER_UNSUPPORTED,"Raw image has no encoded LZ4 records");
        unsigned char header[19]={0};read(header,6,cancel);
        if(word(header)!=UINT32_C(0x184d2204))fail(EJODIN4_INVALID_COMPRESSED_DATA,"Invalid encoded LZ4 frame magic");
        const size_t length=7+((header[4]&8)?8:0)+((header[4]&1)?4:0);read(header+6,static_cast<uint32_t>(length-6),cancel);
        LZ4F_frameInfo_t info;std::memset(&info,0,sizeof(info));size_t consumed=length;
        const size_t hint=LZ4F_getFrameInfo(decoder_.context,&info,header,&consumed);
        if(LZ4F_isError(hint)||consumed!=length||info.frameType!=LZ4F_frame)fail(EJODIN4_INVALID_COMPRESSED_DATA,"Invalid encoded LZ4 frame header/checksum");
        if((header[4]&8)&&info.contentSize!=image_.decoded_bytes)fail(EJODIN4_CHECKSUM_MISMATCH,"Encoded LZ4 declared size differs from planned image");
        if(!(header[4]&8)||!(header[4]&0x20)||(header[4]&1)||info.blockSizeID<LZ4F_max64KB||info.blockSizeID>LZ4F_max1MB)
            fail(EJODIN4_DRIVER_UNSUPPORTED,"LZ4 frame profile requires host decoding");
        block_max_=info.blockSizeID==LZ4F_max64KB?65536:info.blockSizeID==LZ4F_max256KB?262144:1048576;
        block_checksum_=(header[4]&0x10)!=0;content_checksum_=(header[4]&4)!=0;
        start_hash();initialized_=true;
    }
    void finish(const Cancellation& cancel){
        unsigned char footer[8]={0};const uint32_t length=content_checksum_?8:4;read(footer,length,cancel);
        if(word(footer))fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 exceeds declared image length");
        unsigned char unused=0;size_t consumed=length,produced=1;
        const size_t hint=LZ4F_decompress(decoder_.context,&unused,&produced,footer,&consumed,0);
        if(LZ4F_isError(hint)||hint||produced||consumed!=length||position_!=entry_.data_bytes)
            fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 footer/content checksum or frame end is invalid");
        check_digest();cancel.check();ended_=true;
    }
public:
    EncodedMemberReader(const File& file,const EJODIN4_ArchiveEntry& entry,const ejodin4::PlanImage& image,bool audit_only=false):file_(file),entry_(entry),image_(image),provider_(0),hash_(0),position_(0),decoded_(0),block_max_(0),initialized_(false),ended_(false),unsupported_short_(false),audit_only_(audit_only),block_checksum_(false),content_checksum_(false){}
    ~EncodedMemberReader(){if(hash_)CryptDestroyHash(hash_);if(provider_)CryptReleaseContext(provider_,0);}
    // A fallback is permitted only after validating the complete alternate frame.
    EJODIN4_Status validate_host_profile(const ejodin4::Cancellation& cancel,std::string& error,const ejodin4::ArchiveScanCallback& progress){
        try{
            start_hash();MemberReader reader(file_,entry_,image_.decoded_bytes);std::vector<uint8_t> output(65536);
            uint32_t bytes=0;do{
                const EJODIN4_Status status=reader.read(vector_data(output),static_cast<uint32_t>(output.size()),bytes,cancel,error);if(status!=EJODIN4_OK)return status;
                if(bytes>image_.decoded_bytes-decoded_)fail(EJODIN4_CHECKSUM_MISMATCH,"Host-decoded fallback exceeds planned size");
                if(bytes&&!CryptHashData(hash_,vector_data(output),bytes,0))fail(EJODIN4_INTERNAL_ERROR,"Fallback image SHA256 update failed");
                decoded_+=bytes;if(progress)progress(decoded_,image_.decoded_bytes);
            }while(bytes);
            check_digest();if(cancel.requested())fail(EJODIN4_CANCELLED,"Encoded profile audit cancelled");return EJODIN4_OK;
        }catch(const Failure& failure){error=failure.message;return failure.status;}
        catch(...){error="Fallback image audit exception";return EJODIN4_INTERNAL_ERROR;}
    }
    bool unsupported_short_blocks()const{return unsupported_short_;}
    EJODIN4_Status read_sequence(uint32_t limit,std::vector<uint8_t>& records,uint32_t& count,
        const ejodin4::Cancellation& cancellation,std::string& error){
        records.clear();count=0;
        try{
            const Cancellation cancel(cancellation.callback,cancellation.context);cancel.check();initialize(cancel);
            if(!limit||limit>30u*1024*1024)fail(EJODIN4_INVALID_ARGUMENT,"Encoded sequence limit must be within 30 MiB");
            if(ended_)return EJODIN4_OK;
            std::vector<uint8_t> pending,output(block_max_+1);uint32_t decoded_sequence=0;
            while(decoded_<image_.decoded_bytes){
                cancel.check();const uint64_t remaining=image_.decoded_bytes-decoded_;
                if(std::min(uint64_t(block_max_),remaining)>limit-decoded_sequence){
                    if(!decoded_sequence)fail(EJODIN4_INVALID_ARGUMENT,"Encoded sequence limit cannot fit one LZ4 block");
                    break;
                }
                unsigned char length[4]={0};read(length,4,cancel);const uint32_t raw=word(length),stored=raw&UINT32_C(0x7fffffff);
                if(!stored||stored>block_max_)fail(EJODIN4_INVALID_COMPRESSED_DATA,"Invalid encoded LZ4 block length");
                std::vector<uint8_t> record(size_t(stored)+4+(block_checksum_?4:0));std::memcpy(vector_data(record),length,4);read(vector_data(record)+4,static_cast<uint32_t>(record.size()-4),cancel);
                size_t consumed=record.size(),produced=output.size();const size_t hint=LZ4F_decompress(decoder_.context,vector_data(output),&produced,vector_data(record),&consumed,0);
                if(LZ4F_isError(hint)||!hint||consumed!=record.size()||!produced||produced>block_max_||produced>remaining)
                    fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 block decoding or checksum failed");
                decoded_+=produced;if(decoded_<image_.decoded_bytes&&produced!=block_max_){
                    if(!audit_only_)fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 block geometry changed after audit");
                    unsupported_short_=true;
                }
                if(produced>limit-decoded_sequence)fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded LZ4 sequence exceeds decoded limit");
                if(!CryptHashData(hash_,vector_data(output),static_cast<DWORD>(produced),0))fail(EJODIN4_INTERNAL_ERROR,"Encoded image SHA256 update failed");
                if(!audit_only_)pending.insert(pending.end(),record.begin(),record.end());
                decoded_sequence+=static_cast<uint32_t>(produced);
            }
            if(decoded_==image_.decoded_bytes)finish(cancel);
            cancel.check();records.swap(pending);count=decoded_sequence;return EJODIN4_OK;
        }catch(const Failure& failure){error=failure.message;return failure.status;}
        catch(...){error="Encoded image reader exception";return EJODIN4_INTERNAL_ERROR;}
    }
};

uint64_t decode_entry(const File& file, const EJODIN4_ArchiveEntry& entry,
                      uint64_t limit, uint64_t& total, std::vector<uint8_t>* pit,
                      const Cancellation& cancellation) {
    // The same bounded reader validates old analysis and second-pass transfers.
    MemberReader reader(file,entry,limit - total); bcc::Array<uint8_t,65536> output;
    const ejodin4::Cancellation native_cancel={cancellation.callback,cancellation.context};
    uint64_t decoded = 0;
    for (;;) {
        uint32_t actual = 0; std::string error;
        const EJODIN4_Status status = reader.read(output.data(),static_cast<uint32_t>(output.size()),actual,
            native_cancel,error);
        if (status != EJODIN4_OK) {
            // Failure carries static diagnostic storage; preserve status with bounded reader context.
            fail(status,status == EJODIN4_CANCELLED ? "Archive inspection cancelled" :
                status == EJODIN4_FILE_IO_ERROR ? "Cannot read complete archive file range" : "Invalid compressed firmware member");
        }
        if (actual > limit - total) fail(EJODIN4_DECODE_LIMIT_EXCEEDED,"Decoded firmware byte limit exceeded");
        total += actual; decoded += actual; retain_pit(pit,output.data(),actual);
        if (!actual) return decoded;
    }
}
std::vector<uint8_t> external_pit(const std::wstring& path, const Cancellation& cancellation) {
    cancellation.check();
    File file;
    file.handle = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if (file.handle == INVALID_HANDLE_VALUE) fail(EJODIN4_FILE_IO_ERROR,"Cannot open selected external PIT read-only");
    BY_HANDLE_FILE_INFORMATION before;std::memset(&before,0,sizeof(before));
    if (GetFileType(file.handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.handle,&before) ||
        (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0)
        fail(EJODIN4_FILE_IO_ERROR,"Selected external PIT must be a regular filesystem disk file");
    const uint64_t size = file_size(before);
    if (size > 1024u * 1024u || size < 28) fail(EJODIN4_INVALID_PIT,"External PIT size is outside 28 bytes..1 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    size_t position = 0;
    try {
        while (position < bytes.size()) {
            const DWORD count = static_cast<DWORD>(std::min(bytes.size() - position,size_t(65536)));
            file.read(vector_data(bytes) + position,count,cancellation); position += count;
        }
    } catch (const Failure& error) {
        if (error.status == EJODIN4_FILE_IO_ERROR) fail(EJODIN4_FILE_IO_ERROR,"Cannot read selected external PIT file");
        throw;
    }
    unchanged(file,before,"External PIT identity, length or timestamp changed during analysis");
    return bytes;
}
std::string bounded_name(const char* bytes, size_t capacity) {
    const char* end = static_cast<const char*>(std::memchr(bytes,0,capacity));
    return std::string(bytes,end ? static_cast<size_t>(end - bytes) : capacity);
}
bool invalid_partition_text(const std::string& partition){for(size_t i=0;i<partition.size();++i)if(static_cast<unsigned char>(partition[i])<32||static_cast<unsigned char>(partition[i])==127)return true;return false;}
void map_images(std::vector<EJODIN4_AnalyzedImage>& images, const std::vector<uint8_t>& bytes,
                EJODIN4_AnalysisSummary& summary, const Cancellation& cancellation) {
    std::vector<EJODIN4_PitEntry> entries(512);
    uint32_t count = 0; char diagnostic[256] = {};
    const EJODIN4_Status status = EJODIN4_ParsePit(vector_data(bytes),static_cast<uint32_t>(bytes.size()),vector_data(entries),
        static_cast<uint32_t>(entries.size()),&count,diagnostic,sizeof(diagnostic));
    if (status != EJODIN4_OK) fail(EJODIN4_INVALID_PIT,"Selected PIT failed existing Odin PIT validation");
    std::map<std::string,const EJODIN4_PitEntry*> filenames;
    for (uint32_t i = 0; i < count; ++i) {
        cancellation.check();
        const std::string name = bounded_name(entries[i].file_name,sizeof(entries[i].file_name));
        if (!name.empty() && !filenames.insert(std::make_pair(lower_ascii(name),&entries[i])).second)
            fail(EJODIN4_PIT_MAPPING_ERROR,"PIT contains ambiguous case-insensitive file names");
    }
    std::set<uint32_t> destinations;
    uint32_t non_pit = 0, mapped = 0;
    for (size_t image_index=0;image_index<images.size();++image_index) {
        EJODIN4_AnalyzedImage& image=images[image_index];
        cancellation.check();
        if (image.flags & EJODIN4_IMAGE_PIT) continue;
        ++non_pit;
        std::string name(image.name);
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name.erase(0,slash + 1);
        if (ends_with(lower_ascii(name),".lz4")) name.resize(name.size() - 4);
        const std::map<std::string,const EJODIN4_PitEntry*>::const_iterator found = filenames.find(lower_ascii(name));
        if (found == filenames.end()) continue;
        const EJODIN4_PitEntry& entry = *found->second;
        if (!entry.identifier || entry.identifier == UINT32_MAX || !destinations.insert(entry.identifier).second)
            fail(EJODIN4_PIT_MAPPING_ERROR,"Images have an ambiguous or duplicate PIT destination identifier");
        const std::string partition = bounded_name(entry.partition_name,sizeof(entry.partition_name));
        if (partition.size() >= sizeof(image.partition_name) || !utf8(partition) ||
            invalid_partition_text(partition))
            fail(EJODIN4_PIT_MAPPING_ERROR,"Mapped PIT partition name must be nonempty terminated UTF-8");
        image.flags |= EJODIN4_IMAGE_MAPPED; image.pit_identifier = entry.identifier;
        image.binary_type = entry.binary_type; image.device_type = entry.device_type;
        std::memcpy(image.partition_name,partition.c_str(),partition.size() + 1); ++mapped;
    }
    summary.pit_entry_count = count; summary.flags |= EJODIN4_ANALYSIS_PIT_AVAILABLE;
    if (non_pit && mapped == non_pit) summary.flags |= EJODIN4_ANALYSIS_ALL_IMAGES_MAPPED;
}
}

EJODIN4_Status EJODIN4_CALL EJODIN4_InspectFirmwareArchive(
    const char* path, uint32_t path_bytes, EJODIN4_ArchiveEntry* entries, uint32_t capacity,
    uint32_t* count, EJODIN4_ArchiveSummary* summary, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity) {
    if (count) *count = 0;
    try {
        if (!count || !summary || (!entries && capacity) || (!diagnostic && diagnostic_capacity))
            return result(EJODIN4_INVALID_ARGUMENT,"Invalid archive output arguments",diagnostic,diagnostic_capacity);
        std::vector<EJODIN4_ArchiveEntry> validated;
        EJODIN4_ArchiveSummary checked;std::memset(&checked,0,sizeof(checked));
        File file; BY_HANDLE_FILE_INFORMATION before;std::memset(&before,0,sizeof(before));
        inspect(path,path_bytes,validated,checked,Cancellation(cancel,context),file,before);
        std::memcpy(summary,&checked,sizeof(checked)); *count = checked.entry_count;
        if (capacity < checked.entry_count)
            return result(EJODIN4_BUFFER_TOO_SMALL,"Archive entry capacity is insufficient",diagnostic,diagnostic_capacity);
        std::memcpy(entries,vector_data(validated),validated.size() * sizeof(EJODIN4_ArchiveEntry));
        return result(EJODIN4_OK,"",diagnostic,diagnostic_capacity);
    } catch (const Failure& error) {
        return result(error.status,error.message,diagnostic,diagnostic_capacity);
    } catch (...) {
        return result(EJODIN4_INTERNAL_ERROR,"Archive inspection raised an exception",diagnostic,diagnostic_capacity);
    }
}

EJODIN4_Status EJODIN4_CALL EJODIN4_AnalyzeFirmwareArchive(
    const char* path, uint32_t path_bytes, const char* optional_pit_path, uint32_t pit_path_bytes,
    uint64_t decoded_limit, EJODIN4_AnalyzedImage* images, uint32_t capacity, uint32_t* count,
    EJODIN4_AnalysisSummary* summary, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity) {
    if (count) *count = 0;
    try {
        if (!count || !summary || (!images && capacity) || (!diagnostic && diagnostic_capacity) ||
            decoded_limit == 0 || decoded_limit > UINT64_C(1099511627776) ||
            (!optional_pit_path && pit_path_bytes) || (optional_pit_path && !pit_path_bytes))
            return result(EJODIN4_INVALID_ARGUMENT,"Invalid firmware analysis arguments",diagnostic,diagnostic_capacity);
        std::wstring selected_external;
        if (optional_pit_path && !(pit_path_bytes == 1 && optional_pit_path[0] == 0))
            selected_external = native_path(optional_pit_path,pit_path_bytes);
        const Cancellation cancellation(cancel,context);
        File file; BY_HANDLE_FILE_INFORMATION before;std::memset(&before,0,sizeof(before));
        std::vector<EJODIN4_ArchiveEntry> entries;
        EJODIN4_ArchiveSummary archive;std::memset(&archive,0,sizeof(archive));
        inspect(path,path_bytes,entries,archive,cancellation,file,before);
        size_t selected_pit = entries.size();
        if (selected_external.empty()) {
            for (size_t i = 0; i < entries.size(); ++i) if (entries[i].flags & EJODIN4_ARCHIVE_ENTRY_PIT) {
                if (selected_pit != entries.size()) fail(EJODIN4_PIT_MAPPING_ERROR,"Multiple embedded PIT members require an explicit external PIT");
                selected_pit = i;
            }
        }
        std::vector<uint8_t> pit;
        std::vector<EJODIN4_AnalyzedImage> analyzed;
        EJODIN4_AnalysisSummary checked;std::memset(&checked,0,sizeof(checked));
        for (size_t i = 0; i < entries.size(); ++i) {
            cancellation.check();
            EJODIN4_AnalyzedImage image;std::memset(&image,0,sizeof(image));
            std::memcpy(image.name,entries[i].name,sizeof(image.name));
            image.stored_bytes = entries[i].data_bytes;
            image.pit_identifier = UINT32_MAX;
            image.decoded_bytes = decode_entry(file,entries[i],decoded_limit,checked.decoded_bytes,
                i == selected_pit ? &pit : 0,cancellation);
            if (entries[i].flags & EJODIN4_ARCHIVE_ENTRY_LZ4) image.flags |= EJODIN4_IMAGE_LZ4;
            if (entries[i].flags & EJODIN4_ARCHIVE_ENTRY_PIT) image.flags |= EJODIN4_IMAGE_PIT;
            analyzed.push_back(image);
        }
        if (!selected_external.empty()) {
            pit = external_pit(selected_external,cancellation);
            checked.flags |= EJODIN4_ANALYSIS_EXTERNAL_PIT;
        }
        if (!selected_external.empty() || selected_pit != entries.size()) map_images(analyzed,pit,checked,cancellation);
        cancellation.check();
        unchanged(file,before,"Firmware archive identity, length or timestamp changed during analysis");
        checked.entry_count = static_cast<uint32_t>(analyzed.size());
        std::memcpy(summary,&checked,sizeof(checked)); *count = checked.entry_count;
        if (capacity < checked.entry_count)
            return result(EJODIN4_BUFFER_TOO_SMALL,"Analyzed image capacity is insufficient",diagnostic,diagnostic_capacity);
        std::memcpy(images,vector_data(analyzed),analyzed.size() * sizeof(EJODIN4_AnalyzedImage));
        return result(EJODIN4_OK,"",diagnostic,diagnostic_capacity);
    } catch (const Failure& error) {
        return result(error.status,error.message,diagnostic,diagnostic_capacity);
    } catch (...) {
        return result(EJODIN4_INTERNAL_ERROR,"Firmware analysis raised an exception",diagnostic,diagnostic_capacity);
    }
}

namespace ejodin4 {
namespace {
class NativeArchive  : public HeldArchive {
public:
    EJODIN4_Status open_encoded_member(size_t index,const PlanImage& image,
        std::unique_ptr<EncodedImageReader>& reader,std::string& error,
        const ejodin4::Cancellation& cancel,const ArchiveScanCallback& progress) {
        reader.reset();if(index>=entries.size()){error="Invalid encoded member index";return EJODIN4_INVALID_ARGUMENT;}
        EJODIN4_Status status=verify_unchanged(error);if(status!=EJODIN4_OK)return status;
        try{
            // Full bounded preflight rules out short intermediate blocks before any image command.
            EncodedMemberReader audit(file,entries[index],image,true);std::vector<uint8_t> records;uint32_t bytes=0;uint64_t total=0;
            do{status=audit.read_sequence(30u*1024*1024,records,bytes,cancel,error);
                if(status==EJODIN4_DRIVER_UNSUPPORTED){const EJODIN4_Status checked=audit.validate_host_profile(cancel,error,progress);return checked==EJODIN4_OK?status:checked;}
                if(status!=EJODIN4_OK)return status;
                total+=bytes;if(progress)progress(total,image.decoded_bytes);
            }while(bytes);
            if(audit.unsupported_short_blocks()){error="Short intermediate LZ4 blocks require host decoding";return EJODIN4_DRIVER_UNSUPPORTED;}
            status=verify_unchanged(error);if(status!=EJODIN4_OK)return status;
            reader.reset(new EncodedMemberReader(file,entries[index],image));return EJODIN4_OK;
        }catch(const Failure& failure){error=failure.message;return failure.status;}
        catch(...){error="Cannot create bounded encoded reader";return EJODIN4_INTERNAL_ERROR;}
    }

    File file;
    BY_HANDLE_FILE_INFORMATION before;
    NativeArchive(){std::memset(&before,0,sizeof(before));}
    EJODIN4_Status verify_unchanged(std::string& error) const  {
        try { unchanged(file,before,"Held firmware archive changed"); return EJODIN4_OK; }
        catch (const Failure& failure) { error=failure.message; return failure.status; }
    }
    EJODIN4_Status open_stored_member(size_t index,std::unique_ptr<ImageReader>& reader,std::string& error) {
        if(index>=entries.size()){error="Invalid stored archive member";return EJODIN4_INVALID_ARGUMENT;}
        const EJODIN4_Status status=verify_unchanged(error);if(status!=EJODIN4_OK)return status;
        try {EJODIN4_ArchiveEntry entry=entries[index];entry.flags&=~uint32_t(EJODIN4_ARCHIVE_ENTRY_LZ4);reader.reset(new MemberReader(file,entry));return EJODIN4_OK;}
        catch(...){error="Cannot create stored archive reader";return EJODIN4_INTERNAL_ERROR;}
    }
    EJODIN4_Status open_member(size_t index, std::unique_ptr<ImageReader>& reader,
        std::string& error)  {
        if (index >= entries.size()) { error="Invalid archive member index"; return EJODIN4_INVALID_ARGUMENT; }
        const EJODIN4_Status status=verify_unchanged(error); if(status!=EJODIN4_OK)return status;
        try { reader.reset(new MemberReader(file,entries[index])); return EJODIN4_OK; }
        catch (...) { error="Cannot allocate bounded image reader"; return EJODIN4_INTERNAL_ERROR; }
    }
};
}
EJODIN4_Status load_target_pit(const EJODIN4_FirmwareFile& source,const Cancellation& cancel,
    std::vector<uint8_t>& output,std::string& error) {
    output.clear();
    try {
        std::vector<uint8_t> bytes=external_pit(native_path(source.path,source.path_bytes),::Cancellation(cancel.callback,cancel.context));
        std::vector<EJODIN4_PitEntry> entries(512);uint32_t count=0;char text[256]={0};
        const EJODIN4_Status status=EJODIN4_ParsePit(&bytes[0],uint32_t(bytes.size()),&entries[0],512,&count,text,sizeof(text));
        if(status!=EJODIN4_OK){error=std::string("Target PIT rejected: ")+text;return status;}
        output.swap(bytes);return EJODIN4_OK;
    } catch(const Failure& failure){error=failure.message;return failure.status;}
    catch(...){error="Cannot snapshot raw target PIT";return EJODIN4_INTERNAL_ERROR;}
}
EJODIN4_Status open_held_archive(const EJODIN4_FirmwareFile& source,
    const Cancellation& cancel, std::unique_ptr<HeldArchive>& output, std::string& error,
    const ArchiveScanCallback& progress) {
    try {
        std::unique_ptr<NativeArchive> archive(new NativeArchive);
        inspect(source.path,source.path_bytes,archive->entries,archive->summary,
            ::Cancellation(cancel.callback,cancel.context),archive->file,archive->before,progress);
        output.reset(archive.release()); return EJODIN4_OK;
    } catch(const Failure& failure) { error=failure.message; return failure.status; }
    catch(...) {error="Cannot open held firmware archive";return EJODIN4_INTERNAL_ERROR;}
}
}
