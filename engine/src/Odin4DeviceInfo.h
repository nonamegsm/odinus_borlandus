#ifndef EASYJTAG_ODIN4_DEVICE_INFO_H
#define EASYJTAG_ODIN4_DEVICE_INFO_H
#include "EasyJtagOdin4Core.h"
#include <string>
#include <vector>
#include <cstring>
namespace ejodin4 {
struct DeviceInfo { std::string model,carrier; bool carrier_present; DeviceInfo():carrier_present(false){} };
struct FirmwareIdentity { std::string model,blocked_carriers,diagnostic; EJODIN4_Status status; bool present; FirmwareIdentity():status(EJODIN4_DRIVER_UNSUPPORTED),present(false){} };
inline uint32_t device_info_word(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
// Native Odin's 69/* reply has a magic/count header and 12-byte tagged
// records. Offsets are relative to byte 8. The third record word is opaque:
// the inspected native validator does not establish it as a string length.
inline EJODIN4_Status parse_device_info(const std::vector<uint8_t>& bytes,uint32_t protocol,DeviceInfo& output,std::string& error) {
    output=DeviceInfo();
    if(protocol<4||protocol>5||bytes.size()<500||bytes.size()>1000000||bytes.size()%500||device_info_word(&bytes[0])!=0x12345678u){error="Invalid Samsung DEVINFO header or bounded transfer size";return EJODIN4_INVALID_RESPONSE;}
    const uint32_t count=device_info_word(&bytes[4]);
    if(!count||count>256||count>(bytes.size()-8)/12){error="Invalid Samsung DEVINFO record count";return EJODIN4_INVALID_RESPONSE;}
    bool model=false;
    for(uint32_t i=0;i<count;++i){
        const uint8_t* record=&bytes[8+12*i];const uint32_t tag=device_info_word(record);
        if(tag!=0&&tag!=3)continue;
        if((tag==0&&model)||(tag==3&&output.carrier_present)){error="Duplicate Samsung DEVINFO identity tag";return EJODIN4_INVALID_RESPONSE;}
        const uint32_t offset=device_info_word(record+4);
        if(offset<12*count||offset>=bytes.size()-8){error="Samsung DEVINFO identity offset is outside its payload";return EJODIN4_INVALID_RESPONSE;}
        const char* text=reinterpret_cast<const char*>(&bytes[8+offset]);
        const size_t available=bytes.size()-8-offset;
        const char* end=static_cast<const char*>(std::memchr(text,0,available));
        if(!end||size_t(end-text)>127||(tag==0&&end==text)){error="Samsung DEVINFO identity is empty, unbounded or unterminated";return EJODIN4_INVALID_RESPONSE;}
        for(const char* p=text;p!=end;++p)if(static_cast<unsigned char>(*p)<32||static_cast<unsigned char>(*p)>126){error="Samsung DEVINFO identity has unsupported text encoding";return EJODIN4_INVALID_RESPONSE;}
        if(tag==0){output.model.assign(text,end);model=true;}else{output.carrier.assign(text,end);output.carrier_present=true;}
    }
    if(!model||(protocol==5&&!output.carrier_present)){error="Samsung DEVINFO is missing a required model or carrier tag";return EJODIN4_INVALID_RESPONSE;}
    return EJODIN4_OK;
}
inline EJODIN4_Status validate_device_identity(const DeviceInfo& device,const FirmwareIdentity& firmware,uint32_t protocol,std::string& error) {
    if(!firmware.present||firmware.model.empty()){if(firmware.diagnostic.empty())error="Authenticated firmware identity metadata is required for Samsung Download protocol 4/5";else error=firmware.diagnostic;if(firmware.status==EJODIN4_OK)return EJODIN4_DRIVER_UNSUPPORTED;return firmware.status;}
    const size_t separator=firmware.model.find('_');const std::string model=firmware.model.substr(0,separator);
    if(device.model!=model){error="Firmware model does not match Samsung DEVINFO";return EJODIN4_PIT_MAPPING_ERROR;}
    if(protocol!=4&&protocol!=5){error="Unsupported DEVINFO identity policy version";return EJODIN4_INVALID_ARGUMENT;}
    if(device.carrier_present&&!firmware.blocked_carriers.empty()&&!device.carrier.empty()){
        // Carrier policy uses exact underscore-delimited tokens, rather than
        // a substring that could confuse short overlapping carrier names.
        const std::string& list=firmware.blocked_carriers;
        if(list.find("_"+device.carrier+"_")!=std::string::npos){error="Firmware package blocks this device carrier";return EJODIN4_PIT_MAPPING_ERROR;}
    }
    return EJODIN4_OK;
}
}
#endif
