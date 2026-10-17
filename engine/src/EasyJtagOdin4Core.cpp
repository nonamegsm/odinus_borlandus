#include "EasyJtagOdin4Core.h"
#include "BccCompat.h"
#include "Odin4BccTrailer.h"
#include <stddef.h>
#include <cstring>
#include <set>
#include <vector>

typedef char PitAbiSize[sizeof(EJODIN4_PitEntry)==132?1:-1];
typedef char ResponseAbiSize[sizeof(EJODIN4_Response)==8?1:-1];
typedef char TrailerAbiSize[sizeof(EJODIN4_TrailerPosition)==16?1:-1];
namespace {
bool valid_diagnostic(char* text,uint32_t capacity){return text!=0||capacity==0;}
EJODIN4_Status result(EJODIN4_Status status,const char* message,char* text,uint32_t capacity){
    if(text&&capacity){const size_t length=std::strlen(message),copied=length<capacity-1?length:capacity-1;std::memcpy(text,message,copied);text[copied]=0;}return status;
}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void write32(uint8_t* p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
bool nonempty_partition(const char* field){size_t length=0;while(length<32&&field[length])++length;while(length&&(field[length-1]==' '||field[length-1]=='\t'))--length;return length!=0;}
}
uint32_t EJODIN4_CALL EJODIN4_GetAbiVersion(void){return UINT32_C(0x0001000a);}
EJODIN4_Status EJODIN4_CALL EJODIN4_ParsePit(const uint8_t* bytes,uint32_t byte_count,EJODIN4_PitEntry* entries,uint32_t capacity,uint32_t* count,char* diagnostic,uint32_t diagnostic_capacity){
    if(count)*count=0;
    try{
        if(!count||(!bytes&&byte_count)||(!entries&&capacity)||byte_count>1024u*1024u||!valid_diagnostic(diagnostic,diagnostic_capacity))return result(EJODIN4_INVALID_ARGUMENT,"Invalid PIT arguments",diagnostic,diagnostic_capacity);
        if(byte_count<28)return result(EJODIN4_INVALID_PIT,"PIT header is truncated",diagnostic,diagnostic_capacity);
        const uint32_t required=read32(bytes+4);
        if(read32(bytes)!=UINT32_C(0x12349876)||required==0||required>512||byte_count<28u+required*132u)return result(EJODIN4_INVALID_PIT,"Upstream PIT validation failed",diagnostic,diagnostic_capacity);
        std::vector<EJODIN4_PitEntry> validated(required);std::set<uint32_t> identifiers;
        for(uint32_t i=0;i<required;++i){
            const uint8_t* input=bytes+28u+i*132u;EJODIN4_PitEntry& entry=validated[i];
            entry.binary_type=read32(input);entry.device_type=read32(input+4);entry.identifier=read32(input+8);entry.attributes=read32(input+12);entry.update_attributes=read32(input+16);entry.block_size_or_offset=read32(input+20);entry.block_count=read32(input+24);entry.file_offset=read32(input+28);entry.file_size=read32(input+32);
            std::memcpy(entry.partition_name,input+36,32);std::memcpy(entry.file_name,input+68,32);std::memcpy(entry.fota_name,input+100,32);
            if(!entry.identifier||!identifiers.insert(entry.identifier).second||!nonempty_partition(entry.partition_name))return result(EJODIN4_INVALID_PIT,"Upstream PIT validation failed",diagnostic,diagnostic_capacity);
        }
        if(capacity<required){*count=required;return result(EJODIN4_BUFFER_TOO_SMALL,"PIT entry capacity is insufficient",diagnostic,diagnostic_capacity);}
        std::memcpy(entries,&validated[0],required*sizeof(EJODIN4_PitEntry));*count=required;return result(EJODIN4_OK,"",diagnostic,diagnostic_capacity);
    }catch(...){if(count)*count=0;return result(EJODIN4_INTERNAL_ERROR,"PIT parsing raised an exception",diagnostic,diagnostic_capacity);}
}
EJODIN4_Status EJODIN4_CALL EJODIN4_BuildRequest(int32_t command,int32_t parameter,const int32_t* integers,uint32_t integer_count,const uint8_t* characters,uint32_t character_count,uint8_t* output,uint32_t capacity,char* diagnostic,uint32_t diagnostic_capacity){
    try{
        if(integer_count>9||character_count>128||(!integers&&integer_count)||(!characters&&character_count)||(!output&&capacity)||!valid_diagnostic(diagnostic,diagnostic_capacity))return result(EJODIN4_INVALID_ARGUMENT,"Invalid request arguments",diagnostic,diagnostic_capacity);
        if(capacity<1024)return result(EJODIN4_BUFFER_TOO_SMALL,"Request requires 1024 output bytes",diagnostic,diagnostic_capacity);
        uint8_t request[1024];std::memset(request,0,sizeof(request));write32(request,uint32_t(command));write32(request+4,uint32_t(parameter));
        for(uint32_t i=0;i<integer_count;++i)write32(request+8+i*4,uint32_t(integers[i]));if(character_count)std::memcpy(request+44,characters,character_count);std::memcpy(output,request,sizeof(request));return result(EJODIN4_OK,"",diagnostic,diagnostic_capacity);
    }catch(...){return result(EJODIN4_INTERNAL_ERROR,"Request building raised an exception",diagnostic,diagnostic_capacity);}
}
EJODIN4_Status EJODIN4_CALL EJODIN4_DecodeResponse(const uint8_t* bytes,uint32_t byte_count,int32_t expected,EJODIN4_Response* output,char* diagnostic,uint32_t capacity){
    try{
        if(!bytes||!output||!valid_diagnostic(diagnostic,capacity))return result(EJODIN4_INVALID_ARGUMENT,"Invalid response arguments",diagnostic,capacity);
        if(byte_count!=8)return result(EJODIN4_INVALID_RESPONSE,"Response must contain exactly 8 bytes",diagnostic,capacity);
        EJODIN4_Response value;value.id=int32_t(read32(bytes));value.ack=int32_t(read32(bytes+4));
        if(value.id==-1||value.id==int32_t(UINT32_C(0x80000000))||value.id!=expected||value.ack<0)return result(EJODIN4_INVALID_RESPONSE,"Response id or ACK is invalid",diagnostic,capacity);
        *output=value;return result(EJODIN4_OK,"",diagnostic,capacity);
    }catch(...){return result(EJODIN4_INTERNAL_ERROR,"Response decoding raised an exception",diagnostic,capacity);}
}
EJODIN4_Status EJODIN4_CALL EJODIN4_LocateTarMd5Trailer(const uint8_t* tail,uint32_t bytes,uint64_t offset,EJODIN4_TrailerPosition* output,char* diagnostic,uint32_t capacity){
    if(output){output->md5_offset=-1;output->trailer_start=-1;}
    try{
        const uint64_t maximum=(UINT64_C(1)<<63)-1;
        if(!output||(!tail&&bytes)||bytes>65536u||offset>maximum||bytes>maximum-offset||!valid_diagnostic(diagnostic,capacity))return result(EJODIN4_INVALID_ARGUMENT,"Invalid trailer arguments or offset overflow",diagnostic,capacity);
        const tar_md5::TrailerPos located=tar_md5::locate_trailer(reinterpret_cast<const char*>(tail),bytes,offset);
        if(located.md5_pos<0)return result(EJODIN4_TRAILER_NOT_FOUND,"No MD5 trailer candidate found",diagnostic,capacity);
        output->md5_offset=int64_t(offset+uint64_t(located.md5_pos));output->trailer_start=int64_t(offset+uint64_t(located.trailer_start));return result(EJODIN4_OK,"",diagnostic,capacity);
    }catch(...){return result(EJODIN4_INTERNAL_ERROR,"Trailer location raised an exception",diagnostic,capacity);}
}
