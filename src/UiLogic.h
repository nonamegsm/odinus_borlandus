#ifndef ODIN_BCC_UI_LOGIC_H
#define ODIN_BCC_UI_LOGIC_H
#include "../include/EasyJtagOdin4Core.h"
#include <string>
#include <cstring>

// Pure C++03 UI decisions; neither this helper nor its tests perform USB I/O.
namespace OdinUi {
inline bool supported(uint32_t flags) { return flags==EJODIN4_DOWNLOAD_WINUSB||flags==EJODIN4_DOWNLOAD_SAMSUNG_SERIAL; }
inline bool textValid(const char* value,size_t size) { return std::memchr(value,0,size)!=0; }
inline bool hashValid(const char* hash) {
    if(!textValid(hash,65))return false;
    if(!hash[0])return true;
    if(std::strlen(hash)!=64)return false;
    for(size_t i=0;i<64;++i)if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')||(hash[i]>='A'&&hash[i]<='F')))return false;
    return true;
}
struct ProgressState {
    bool invalid,rebootPending;unsigned terminalPhases;uint64_t acknowledged;
    uint32_t operation,packageCount,roleMask,verifyPhase,verifyIndex;bool reboot;uint64_t verifyCompleted,verifyTotal;std::string verifyName;
    uint32_t logicalSectorBytes;
    bool loggedImages[3072];
    ProgressState():invalid(false),rebootPending(false),terminalPhases(0),acknowledged(0),operation(EJODIN4_FLASH_WRITE),packageCount(0),roleMask(0),verifyPhase(0),verifyIndex(0),reboot(false),verifyCompleted(0),verifyTotal(0),logicalSectorBytes(0){std::memset(loggedImages,0,sizeof(loggedImages));}
    bool observe(const EJODIN4_FlashProgress* p) {
        if(invalid)return false;
        if(!p||p->struct_size!=sizeof(*p)||p->reserved||
            !((p->phase>=1&&p->phase<=17)||p->phase==20||p->phase==21)||
            p->image_count>3072||p->image_index>p->image_count||p->package_role>7||p->acknowledged_bytes>p->total_bytes||p->acknowledged_bytes<acknowledged||p->image_acknowledged_bytes>p->image_bytes||
            !textValid(p->image_name,sizeof(p->image_name))||!hashValid(p->sha256)) { invalid=true;return false; }
        const bool verification=p->phase==20||p->phase==21;
        const std::string name(p->image_name);
        const bool same=verifyPhase==p->phase&&verifyIndex==p->image_index&&verifyName==name;
        if(p->phase==17&&(p->skip_reason<EJODIN4_SKIP_METADATA||p->skip_reason>EJODIN4_SKIP_PIT_REFERENCE)){invalid=true;return false;}
        if(verification&&(!p->image_count||p->image_count>packageCount||p->image_index>=p->image_count||!p->package_role||
            !(roleMask&(1u<<p->package_role))||name.empty()||p->sha256[0]||p->expanded_bytes||p->skip_reason||
            (same&&(p->source_offset<verifyCompleted||p->stored_bytes!=verifyTotal))||p->acknowledged_bytes||p->total_bytes||p->image_acknowledged_bytes||p->image_bytes||
            p->source_offset>p->stored_bytes)) { invalid=true;return false; }
        if(verification){verifyPhase=p->phase;verifyIndex=p->image_index;verifyName=name;verifyCompleted=p->source_offset;verifyTotal=p->stored_bytes;return true;}
        const bool offline=operation==EJODIN4_FLASH_PLAN_ONLY,pit=operation==EJODIN4_FLASH_READ_PIT;
        if((offline&&((p->phase>=3&&p->phase<=7)||(p->phase>=9&&p->phase<=13)))||((offline||pit)&&p->acknowledged_bytes)||
            (pit&&((p->phase>=9&&p->phase<=11)||p->phase==13))){invalid=true;return false;}
        if((p->phase==2||p->phase==9||p->phase==10||p->phase==11)&&
            (!p->image_count||p->image_index>=p->image_count||!p->package_role||p->package_role>6||
             ((p->phase==2||p->phase==9)&&!(roleMask&(1u<<p->package_role))))) { invalid=true;return false; }
        if(p->phase==EJODIN4_PHASE_PREFLIGHT){
            verifyPhase=verifyIndex=0;verifyName.clear();verifyCompleted=verifyTotal=0;
        }
        if(!verification) {
            if(p->acknowledged_bytes<acknowledged){invalid=true;return false;}
            acknowledged=p->acknowledged_bytes;
        }
        if(p->phase==16)terminalPhases|=2;
        if(p->phase==15)terminalPhases|=1;
        if(p->phase==13&&operation==EJODIN4_FLASH_WRITE&&reboot)rebootPending=true;
        return true;
    }
    bool firstImage(uint32_t index) { if(index>=3072||loggedImages[index])return false;loggedImages[index]=true;return true; }
};
struct Completion { bool valid,success,rebootWarning;Completion():valid(false),success(false),rebootWarning(false){} };
inline Completion completion(const EJODIN4_FlashResult& r,EJODIN4_Status status,uint32_t operation,bool reboot,bool cancelled,const ProgressState& state) {
    Completion out;const bool write=operation==EJODIN4_FLASH_WRITE,pit=operation==EJODIN4_FLASH_READ_PIT;
    const bool any=(r.flags&EJODIN4_FLASH_ANY_WRITE)!=0,finalized=(r.flags&EJODIN4_FLASH_FINALIZED)!=0,sent=(r.flags&EJODIN4_FLASH_REBOOT_SENT)!=0;
    if(state.invalid||r.struct_size!=sizeof(r)||r.operation!=operation||r.phase<1||r.phase>17||r.last_proven_phase>17||
        (r.logical_sector_bytes&&r.logical_sector_bytes!=512&&r.logical_sector_bytes!=1024&&r.logical_sector_bytes!=2048&&r.logical_sector_bytes!=4096)||
        (r.flags&~63u)||(r.transport&&!supported(r.transport))||r.image_count>3072||r.images_completed>r.image_count||r.pit_entry_count>512||
        r.acknowledged_bytes>r.total_bytes||r.acknowledged_bytes<state.acknowledged||
        (!write&&(any||finalized||sent||r.acknowledged_bytes))||(sent&&(!finalized||!reboot))||
        (operation==EJODIN4_FLASH_PLAN_ONLY&&(r.transport||r.protocol_version||(r.flags&~EJODIN4_FLASH_PLAN_READY)||r.images_completed))||
        (pit&&r.flags)||
        ((r.flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS)&&(!write||state.logicalSectorBytes||r.logical_sector_bytes||
            !(r.flags&EJODIN4_FLASH_PIT_MATCHED)||!(r.flags&EJODIN4_FLASH_PLAN_READY)))||
        (write&&state.logicalSectorBytes&&r.logical_sector_bytes&&state.logicalSectorBytes!=r.logical_sector_bytes))return out;
    out.valid=true;
    out.rebootWarning=write&&reboot&&!cancelled&&!(state.terminalPhases&2)&&state.rebootPending&&
        status==EJODIN4_USB_IO_ERROR&&r.phase==EJODIN4_PHASE_FAILED&&r.last_proven_phase==EJODIN4_PHASE_REBOOT&&
        r.transport==EJODIN4_DOWNLOAD_SAMSUNG_SERIAL&&r.protocol_version>=1&&r.protocol_version<=5&&r.last_response_id==0x67&&!r.last_ack&&
        any&&finalized&&!sent&&(r.flags&EJODIN4_FLASH_PIT_MATCHED)&&(r.flags&EJODIN4_FLASH_PLAN_READY)&&r.image_count&&r.total_bytes&&
        r.images_completed==r.image_count&&r.acknowledged_bytes==r.total_bytes;
    if(status!=EJODIN4_OK&&!out.rebootWarning)return out;
    if((!out.rebootWarning&&r.phase!=EJODIN4_PHASE_COMPLETE)||((pit||write)&&(!r.transport||!r.protocol_version||r.protocol_version>5))||
        (write&&(!any||!finalized||!(r.flags&EJODIN4_FLASH_PIT_MATCHED)||!(r.flags&EJODIN4_FLASH_PLAN_READY)||!r.image_count||!r.total_bytes||
            (!out.rebootWarning&&sent!=reboot)||r.images_completed!=r.image_count||r.acknowledged_bytes!=r.total_bytes||
            (!r.logical_sector_bytes&&!(r.flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS))||
            (state.logicalSectorBytes&&r.logical_sector_bytes!=state.logicalSectorBytes)))||
        (!pit&&(!(r.flags&EJODIN4_FLASH_PLAN_READY)||!r.image_count||!r.total_bytes))||(pit&&!r.pit_entry_count))return out;
    out.success=true;return out;
}
enum Tone { Plain,Phase,Image,Good,Warning,Error,Configuration,Summary };
inline Tone tone(const std::wstring& s) {
    if(s.find(L"Warning:")==0||s.find(L"Cancellation requested")==0||s.find(L"Operation cancelled")==0)return Warning;
    if(s.find(L"FAILED:")==0||s==L"Operation failed")return Error;
    if(s.find(L"SUCCESS:")==0||s==L"Flash completed successfully.")return Good;
    if(s.find(L"Writing [")==0||s.find(L"Plan [")==0)return Image;
    if(s.find(L"Policy:")==0||s.find(L"Slots:")==0)return Configuration;
    if(s.find(L"Transferred:")==0||s.find(L"Plan:")==0||s.find(L"Partition map:")==0)return Summary;
    if(s.find(L"--- ")==0||s.find(L"Verifying ")==0||s.find(L"Connecting ")==0||s.find(L"Finalizing ")==0||s.find(L"Reboot ")==0)return Phase;
    return Plain;
}
}
#endif
