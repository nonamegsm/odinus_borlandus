/* Native classic BCC regression client: links owned C API objects, never loads a DLL. */
#define EJ_ODIN4_CORE_STATIC
#define NOMINMAX
#include "../engine/include/EasyJtagOdin4Core.h"
#include "../engine/src/BccCompat.h"
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

BCC_STATIC_ASSERT(sizeof(EJODIN4_PitEntry) == 132);
BCC_STATIC_ASSERT(offsetof(EJODIN4_PitEntry, partition_name) == 36);
BCC_STATIC_ASSERT(offsetof(EJODIN4_PitEntry, file_name) == 68);
BCC_STATIC_ASSERT(offsetof(EJODIN4_PitEntry, fota_name) == 100);
BCC_STATIC_ASSERT(sizeof(EJODIN4_Response) == 8);
BCC_STATIC_ASSERT(sizeof(EJODIN4_TrailerPosition) == 16);

BCC_STATIC_ASSERT(sizeof(EJODIN4_AnalyzedImage) == 320);
BCC_STATIC_ASSERT(sizeof(EJODIN4_AnalysisSummary) == 20);
BCC_STATIC_ASSERT(offsetof(EJODIN4_AnalyzedImage, partition_name) == 288);
BCC_STATIC_ASSERT(offsetof(EJODIN4_AnalysisSummary, decoded_bytes) == 12);
BCC_STATIC_ASSERT(sizeof(EJODIN4_ArchiveEntry) == 276);
BCC_STATIC_ASSERT(sizeof(EJODIN4_ArchiveSummary) == 98);
BCC_STATIC_ASSERT(offsetof(EJODIN4_ArchiveSummary, actual_md5) == 32);

BCC_STATIC_ASSERT(sizeof(EJODIN4_UsbDevice) == 1608);
BCC_STATIC_ASSERT(offsetof(EJODIN4_UsbDevice, device_path) == 8);
BCC_STATIC_ASSERT(offsetof(EJODIN4_UsbDevice, instance_id) == 1032);
BCC_STATIC_ASSERT(offsetof(EJODIN4_UsbDevice, description) == 1288);
BCC_STATIC_ASSERT(offsetof(EJODIN4_UsbDevice, driver) == 1544);
BCC_STATIC_ASSERT(sizeof(EJODIN4_UsbProbe) == 14);
BCC_STATIC_ASSERT(sizeof(EJODIN4_DownloadProbe) == 12);
BCC_STATIC_ASSERT(offsetof(EJODIN4_UsbProbe, handshake_complete) == 12);

BCC_STATIC_ASSERT(EJODIN4_FLASH_REBOOT == 1);
BCC_STATIC_ASSERT(EJODIN4_FLASH_PER_PARTITION_PIT_UNITS == 32);
BCC_STATIC_ASSERT(UINT64_C(1099511627776)==(uint64_t(1)<<40));
namespace {
template<class T> T* vector_data(std::vector<T>& value){return value.empty()?NULL:&value[0];}
template<class T> const T* vector_data(const std::vector<T>& value){return value.empty()?NULL:&value[0];}
unsigned checks = 0;
void check(bool ok, const char* description) {
    ++checks;
    if (!ok) throw std::runtime_error(description);
}

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
std::vector<uint8_t> pit_fixture(uint32_t count) {
    std::vector<uint8_t> bytes(28 + count * 132, 0);
    put32(bytes, 0, UINT32_C(0x12349876));
    put32(bytes, 4, count);
    for (uint32_t e = 0; e < count; ++e) {
        const size_t offset = 28 + e * 132;
        for (uint32_t field = 0; field < 9; ++field)
            put32(bytes, offset + field * 4, UINT32_C(0x10203040) + e * 16 + field);
        // Full-width fields prove the C ABI does not append terminators.
        for (unsigned i = 0; i < 32; ++i) {
            bytes[offset + 36 + i] = static_cast<uint8_t>('A' + e % 26);
            bytes[offset + 68 + i] = static_cast<uint8_t>('a' + e % 26);
            bytes[offset + 100 + i] = static_cast<uint8_t>('0' + e % 10);
        }
    }
    return bytes;
}

bool all_a5(const uint8_t* bytes,size_t count){for(size_t i=0;i<count;++i)if(bytes[i]!=0xa5)return false;return true;}
struct PitInvalid {EJODIN4_PitEntry* output;uint32_t& count;const std::vector<uint8_t>& untouched;PitInvalid(EJODIN4_PitEntry* p,uint32_t& n,const std::vector<uint8_t>& b):output(p),count(n),untouched(b){}
 void operator()(const std::vector<uint8_t>& data,uint32_t size,EJODIN4_Status expected){std::memset(output,0xa5,3*sizeof(*output));count=99;check(EJODIN4_ParsePit(vector_data(data),size,output,3,&count,NULL,0)==expected&&count==0,"Invalid PIT status/count");check(std::memcmp(output,vector_data(untouched),3*sizeof(*output))==0,"Invalid PIT preserves every output byte");}};
struct ResponseInvalid {EJODIN4_Response& output;ResponseInvalid(EJODIN4_Response& p):output(p){} void operator()(const uint8_t* data,uint32_t size,int32_t expected,EJODIN4_Status status){output.id=-9;output.ack=-8;check(EJODIN4_DecodeResponse(data,size,expected,&output,NULL,0)==status,"Response error status");check(output.id==-9&&output.ack==-8,"Response failure preserves caller output");}};
struct TrailerInvalid {EJODIN4_TrailerPosition& output;TrailerInvalid(EJODIN4_TrailerPosition& p):output(p){} void operator()(const uint8_t* data,uint32_t size,uint64_t offset,EJODIN4_Status status){output.md5_offset=1;output.trailer_start=2;check(EJODIN4_LocateTarMd5Trailer(data,size,offset,&output,NULL,0)==status,"Trailer error status");check(output.md5_offset==-1&&output.trailer_start==-1,"Trailer failure sentinel");}};

void test_requests() {
    std::vector<uint8_t> output(1031, 0xa5);
    const int32_t ints[] = {INT32_C(0x12345678), -1, std::numeric_limits<int32_t>::min()};
    const uint8_t chars[] = {'O', 0, 0xff, 'N'};
    check(EJODIN4_BuildRequest(0x66, 7, ints, 3, chars, 4, vector_data(output), 1031, NULL, 0) == EJODIN4_OK, "Request success");
    std::vector<uint8_t> golden(1031, 0);
    golden[0] = 0x66; golden[4] = 7;
    golden[8] = 0x78; golden[9] = 0x56; golden[10] = 0x34; golden[11] = 0x12;
    golden[12] = 0xff; golden[13] = 0xff; golden[14] = 0xff; golden[15] = 0xff;
    golden[19] = 0x80;
    golden[44] = 'O'; golden[46] = 0xff; golden[47] = 'N';
    std::fill(golden.begin() + 1024, golden.end(), static_cast<uint8_t>(0xa5));
    check(output == golden, "Independent request golden wire bytes, MD5/dummy zero and guard bytes");

    const std::vector<uint8_t> before = output;
    const uint32_t excessive[] = {10, 100, UINT32_MAX};
    for (unsigned excessive_index=0;excessive_index<3;++excessive_index) {
        uint32_t count=excessive[excessive_index];
        check(EJODIN4_BuildRequest(0, 0, ints, count, NULL, 0, vector_data(output), 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject excess integer count");
        check(output == before, "Invalid request preserves output");
    }
    check(EJODIN4_BuildRequest(0, 0, NULL, 1, NULL, 0, vector_data(output), 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject null integer array");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, NULL, 1, vector_data(output), 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject null character array");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, chars, 129, vector_data(output), 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject character truncation");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, chars, UINT32_MAX, vector_data(output), 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject huge character count");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, NULL, 0, vector_data(output), 1023, NULL, 0) == EJODIN4_BUFFER_TOO_SMALL, "Reject short output");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == EJODIN4_BUFFER_TOO_SMALL, "Null zero-capacity query");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, NULL, 0, NULL, 1024, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Reject null output with capacity");
    check(output == before, "All failed requests preserve output");
    int32_t maximum_ints[9]; uint8_t maximum_chars[128];
    std::fill(maximum_ints, maximum_ints + 9, 1);
    std::fill(maximum_chars, maximum_chars + 128, static_cast<uint8_t>(0xee));
    check(EJODIN4_BuildRequest(-1, -2, maximum_ints, 9, maximum_chars, 128, vector_data(output), 1024, NULL, 0) == EJODIN4_OK, "Accept maximum input and pass through command");
    check(output[0] == 0xff && output[4] == 0xfe && output[40] == 1 && output[43] == 0 && output[44] == 0xee && output[171] == 0xee && output[172] == 0, "Maximum request field boundaries");
    char diagnostic[6] = {'x','x','x','x','x','x'};
    check(EJODIN4_BuildRequest(0, 0, NULL, 1, NULL, 0, vector_data(output), 1024, diagnostic, 5) == EJODIN4_INVALID_ARGUMENT, "Bounded diagnostic failure");
    check(diagnostic[4] == 0 && diagnostic[5] == 'x', "Diagnostic termination and guard");
    diagnostic[0] = 'x';
    EJODIN4_BuildRequest(0, 0, NULL, 1, NULL, 0, vector_data(output), 1024, diagnostic, 1);
    check(diagnostic[0] == 0, "One-byte diagnostic");
    check(EJODIN4_BuildRequest(0, 0, NULL, 0, NULL, 0, vector_data(output), 1024, NULL, 1) == EJODIN4_INVALID_ARGUMENT, "Reject invalid diagnostic pair");
}

void test_pit() {
    std::vector<uint8_t> bytes = pit_fixture(2);
    EJODIN4_PitEntry output[3];
    std::memset(output, 0xa5, sizeof(output));
    uint32_t count = 99;
    check(EJODIN4_ParsePit(vector_data(bytes), static_cast<uint32_t>(bytes.size()), NULL, 0, &count, NULL, 0) == EJODIN4_BUFFER_TOO_SMALL && count == 2, "PIT count query");
    check(EJODIN4_ParsePit(vector_data(bytes), static_cast<uint32_t>(bytes.size()), output, 1, &count, NULL, 0) == EJODIN4_BUFFER_TOO_SMALL && count == 2, "PIT insufficient capacity");
    const std::vector<uint8_t> untouched(reinterpret_cast<uint8_t*>(output), reinterpret_cast<uint8_t*>(output) + sizeof(output));
    check(all_a5(vector_data(untouched),untouched.size()), "No partial PIT writes");
    check(EJODIN4_ParsePit(vector_data(bytes), static_cast<uint32_t>(bytes.size()), output, 3, &count, NULL, 0) == EJODIN4_OK && count == 2, "PIT multi-entry decode");
    for (unsigned e = 0; e < 2; ++e) {
        uint32_t fields[9]; std::memcpy(fields, &output[e], sizeof(fields));
        for (unsigned field = 0; field < 9; ++field)
            check(fields[field] == UINT32_C(0x10203040) + e * 16 + field, "Every PIT numeric field");
        for (unsigned i = 0; i < 32; ++i)
            check(output[e].partition_name[i] == 'A' + static_cast<int>(e) && output[e].file_name[i] == 'a' + static_cast<int>(e) && output[e].fota_name[i] == '0' + static_cast<int>(e), "Full-width raw PIT names");
    }
    const uint8_t* guard = reinterpret_cast<const uint8_t*>(&output[2]);
    check(all_a5(guard,sizeof(output[2])), "PIT capacity guard entry");

    PitInvalid invalid(output,count,untouched);
    for (uint32_t size = 0; size < bytes.size(); ++size) invalid(bytes, size, EJODIN4_INVALID_PIT);
    std::vector<uint8_t> bad = bytes; bad[0] = 0; invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    const uint32_t invalid_counts[] = {0, 513, UINT32_MAX};
    for (unsigned invalid_count_index=0;invalid_count_index<3;++invalid_count_index) {
        uint32_t n=invalid_counts[invalid_count_index];
        bad = bytes; put32(bad, 4, n); invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    }
    bad = bytes; put32(bad, 36, 0); invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    bad = bytes; put32(bad, 28 + 132 + 8, UINT32_C(0x10203042)); invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    bad = bytes; std::fill(bad.begin() + 64, bad.begin() + 96, static_cast<uint8_t>(0)); invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    bad = bytes; std::fill(bad.begin() + 64, bad.begin() + 96, static_cast<uint8_t>(' ')); invalid(bad, static_cast<uint32_t>(bad.size()), EJODIN4_INVALID_PIT);
    invalid(bytes, 1024u * 1024u + 1u, EJODIN4_INVALID_ARGUMENT);
    count = 99;
    check(EJODIN4_ParsePit(NULL, 28, output, 3, &count, NULL, 0) == EJODIN4_INVALID_ARGUMENT && count == 0, "PIT null bytes");
    count = 99;
    check(EJODIN4_ParsePit(vector_data(bytes), 292, NULL, 1, &count, NULL, 0) == EJODIN4_INVALID_ARGUMENT && count == 0, "PIT null output capacity");
    check(EJODIN4_ParsePit(vector_data(bytes), 292, output, 3, NULL, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "PIT null count");
    bad = pit_fixture(512);
    count = 0;
    check(EJODIN4_ParsePit(vector_data(bad), static_cast<uint32_t>(bad.size()), NULL, 0, &count, NULL, 0) == EJODIN4_BUFFER_TOO_SMALL && count == 512, "Maximum PIT entries query");
    bad = bytes; bad.push_back(0xee);
    check(EJODIN4_ParsePit(vector_data(bad), static_cast<uint32_t>(bad.size()), output, 3, &count, NULL, 0) == EJODIN4_OK, "PIT trailing bytes match upstream");
}

void test_responses() {
    const uint8_t success[] = {0x64,0,0,0, 0x78,0x56,0x34,0x12};
    EJODIN4_Response output = {-9, -8};
    check(EJODIN4_DecodeResponse(success, 8, 0x64, &output, NULL, 0) == EJODIN4_OK && output.id == 0x64 && output.ack == INT32_C(0x12345678), "Response known bytes");
    ResponseInvalid invalid(output);
    for (uint32_t size = 0; size < 8; ++size) invalid(success, size, 0x64, EJODIN4_INVALID_RESPONSE);
    invalid(success, 9, 0x64, EJODIN4_INVALID_RESPONSE);
    invalid(success, UINT32_MAX, 0x64, EJODIN4_INVALID_RESPONSE);
    invalid(success, 8, 0x65, EJODIN4_INVALID_RESPONSE);
    invalid(NULL, 8, 0x64, EJODIN4_INVALID_ARGUMENT);
    const uint8_t failure[] = {0xff,0xff,0xff,0xff, 0,0,0,0};
    const uint8_t minimum[] = {0,0,0,0x80, 0,0,0,0};
    const uint8_t negative[] = {0x64,0,0,0, 0xff,0xff,0xff,0xff};
    invalid(failure, 8, -1, EJODIN4_INVALID_RESPONSE);
    invalid(minimum, 8, std::numeric_limits<int32_t>::min(), EJODIN4_INVALID_RESPONSE);
    invalid(negative, 8, 0x64, EJODIN4_INVALID_RESPONSE);
    check(EJODIN4_DecodeResponse(success, 8, 0x64, NULL, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Response null output");
}

void test_trailers() {
    const std::string digest = "0123456789abcdef0123456789ABCDEF";
    std::vector<uint8_t> bytes(512, 0x80);
    bytes[100] = '\n'; bytes[500] = '\n';
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    const std::string name = "  firmware.tar\n";
    bytes.insert(bytes.end(), name.begin(), name.end());
    EJODIN4_TrailerPosition output = {-2, -2};
    check(EJODIN4_LocateTarMd5Trailer(vector_data(bytes), static_cast<uint32_t>(bytes.size()), 0, &output, NULL, 0) == EJODIN4_OK && output.md5_offset == 512 && output.trailer_start == 512, "Block-aligned MD5 ignores binary newlines");
    const uint64_t large_offset = (UINT64_C(1)<<32);
    check(EJODIN4_LocateTarMd5Trailer(vector_data(bytes), static_cast<uint32_t>(bytes.size()), large_offset, &output, NULL, 0) == EJODIN4_OK && output.md5_offset == ((INT64_C(1)<<32)+512) && output.trailer_start == ((INT64_C(1)<<32)+512), "Offsets beyond 4 GiB remain 64-bit");
    std::vector<uint8_t> fallback(530, 0x80); fallback[519] = '\n';
    fallback.insert(fallback.end(), digest.begin(), digest.end());
    check(EJODIN4_LocateTarMd5Trailer(vector_data(fallback), static_cast<uint32_t>(fallback.size()), 0, &output, NULL, 0) == EJODIN4_OK && output.md5_offset == 530 && output.trailer_start == 520, "Nonaligned fallback bounded newline");
    TrailerInvalid invalid(output);
    invalid(NULL, 0, 0, EJODIN4_TRAILER_NOT_FOUND);
    for (uint32_t size = 0; size < 32; ++size) invalid(vector_data(bytes), size, 0, EJODIN4_TRAILER_NOT_FOUND);
    invalid(NULL, 1, 0, EJODIN4_INVALID_ARGUMENT);
    invalid(vector_data(bytes), 65537, 0, EJODIN4_INVALID_ARGUMENT);
    invalid(vector_data(bytes), 1, UINT64_MAX, EJODIN4_INVALID_ARGUMENT);
    invalid(vector_data(bytes), 1, static_cast<uint64_t>(INT64_MAX), EJODIN4_INVALID_ARGUMENT);
    invalid(vector_data(bytes), 32, static_cast<uint64_t>(INT64_MAX) - 31, EJODIN4_INVALID_ARGUMENT);
    std::vector<uint8_t> no_digest(65536, 0x80);
    invalid(vector_data(no_digest), 65536, 0, EJODIN4_TRAILER_NOT_FOUND);
    check(EJODIN4_LocateTarMd5Trailer(reinterpret_cast<const uint8_t*>(digest.data()), 32, 0, &output, NULL, 0) == EJODIN4_OK && output.md5_offset == 0 && output.trailer_start == 0, "Exactly 32 bytes accepted as candidate only");
    check(EJODIN4_LocateTarMd5Trailer(vector_data(bytes), static_cast<uint32_t>(bytes.size()), 0, NULL, NULL, 0) == EJODIN4_INVALID_ARGUMENT, "Trailer null output");
}
}

int main(){try{check(EJODIN4_GetAbiVersion()==UINT32_C(0x1000b),"Native ABI version");test_requests();test_pit();test_responses();test_trailers();std::cout<<"PASS native classic BCC core: "<<checks<<" checks; no DLL loads or device I/O\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<"\n";return 1;}}
