#include "Odin4LayoutProfile.h"
#include <cstring>
namespace ejodin4 {
namespace {
uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool valid_packages(const FirmwareSet& firmware,const EJODIN4_FirmwareFile* files,uint32_t count){
    if(!files||!count||count>6||firmware.images.empty())return false;
    uint32_t roles=0;
    for(uint32_t i=0;i<count;++i){const EJODIN4_FirmwareFile& file=files[i];
        if(file.role<EJODIN4_PACKAGE_BL||file.role>EJODIN4_PACKAGE_PIT_REFERENCE||(roles&(1u<<file.role))||!file.path||file.path_bytes<2)return false;
        roles|=1u<<file.role;
    }
    return (roles&0x7eu)!=0;
}
}
bool same_pit_layout(const std::vector<uint8_t>& package,const std::vector<uint8_t>& live){
    if(package.size()<28||live.size()<28||le32(&package[0])!=0x12349876||le32(&live[0])!=0x12349876)return false;
    const uint32_t count=le32(&package[0]+4);
    if(!count||count>512||count!=le32(&live[0]+4))return false;
    const size_t bytes=28u+static_cast<size_t>(count)*sizeof(EJODIN4_PitEntry);
    return package.size()>=bytes&&live.size()>=bytes&&std::memcmp(&package[0],&live[0],bytes)==0;
}
EJODIN4_Status resolve_layout_profile(const FirmwareSet& firmware,const std::vector<uint8_t>& live_pit,
    const EJODIN4_FirmwareFile* files,uint32_t count,uint32_t caller_sector_bytes,
    LayoutResolution& output,std::string& error){
    output=LayoutResolution();
    if(caller_sector_bytes){
        if(caller_sector_bytes!=512&&caller_sector_bytes!=1024&&caller_sector_bytes!=2048&&caller_sector_bytes!=4096){error="Caller PIT allocation unit is unsupported";return EJODIN4_INVALID_ARGUMENT;}
        output.sector_bytes=caller_sector_bytes;output.source="Caller-verified PIT allocation unit";return EJODIN4_OK;
    }
    if(!valid_packages(firmware,files,count)||!same_pit_layout(firmware.package_pit_bytes,live_pit)){
        error="Automatic PIT units require valid selected packages and matching logical package/live PIT";return EJODIN4_PIT_MAPPING_ERROR;
    }
    uint32_t common=0;bool mixed=false;
    for(size_t i=0;i<firmware.images.size();++i){
        const uint32_t type=firmware.images[i].route.device_type;
        if(type!=2&&type!=8){error="Unknown partition addressing for automatic PIT allocation units";return EJODIN4_PIT_MAPPING_ERROR;}
        // Samsung PIT allocation convention, not a model/image fingerprint.
        // Independent multi-device evidence and limits: docs/PIT_ROUTING.md.
        const uint32_t unit=type==8?4096u:512u;
        if(!common)common=unit;else if(common!=unit)mixed=true;
    }
    output.sector_bytes=mixed?0:common;output.per_partition_units=mixed;
    output.source=mixed?"Samsung PIT units per selected partition: eMMC512/UFS4096":common==4096?"Samsung UFS PIT allocation unit:4096 bytes":"Samsung eMMC PIT allocation unit:512 bytes";
    return EJODIN4_OK;
}
}
