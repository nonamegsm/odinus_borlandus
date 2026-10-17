#define EJ_ODIN4_CORE_STATIC
#include "../src/UiLogic.h"
#include <cstdio>
#include <cstring>
static unsigned checks=0,failures=0;
static void check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL %s\n",name);}}
static EJODIN4_FlashResult finalized(){EJODIN4_FlashResult r;std::memset(&r,0,sizeof(r));r.struct_size=sizeof(r);r.operation=EJODIN4_FLASH_WRITE;r.phase=EJODIN4_PHASE_FAILED;r.protocol_version=3;r.transport=EJODIN4_DOWNLOAD_SAMSUNG_SERIAL;r.image_count=r.images_completed=26;r.pit_entry_count=44;r.flags=EJODIN4_FLASH_ANY_WRITE|EJODIN4_FLASH_FINALIZED|EJODIN4_FLASH_PIT_MATCHED|EJODIN4_FLASH_PLAN_READY;r.total_bytes=r.acknowledged_bytes=11882352976ULL;r.last_response_id=0x67;r.last_proven_phase=EJODIN4_PHASE_REBOOT;r.logical_sector_bytes=4096;return r;}
static OdinUi::ProgressState context(){OdinUi::ProgressState s;s.reboot=true;s.packageCount=4;s.roleMask=(1u<<1)|(1u<<2)|(1u<<3)|(1u<<5);s.rebootPending=true;s.acknowledged=11882352976ULL;return s;}
static bool warning(const EJODIN4_FlashResult& r,const OdinUi::ProgressState& s,EJODIN4_Status status=EJODIN4_USB_IO_ERROR,bool cancel=false){OdinUi::Completion c=OdinUi::completion(r,status,EJODIN4_FLASH_WRITE,true,cancel,s);return c.valid&&c.success&&c.rebootWarning;}
static EJODIN4_FlashProgress image(){EJODIN4_FlashProgress p;std::memset(&p,0,sizeof(p));p.struct_size=sizeof(p);p.phase=EJODIN4_PHASE_IMAGE_BEGIN;p.image_count=26;p.image_index=19;p.package_role=2;p.total_bytes=100;p.image_bytes=60;std::strcpy(p.image_name,"vbmeta.img");return p;}
int main(){
    EJODIN4_FlashResult r=finalized();OdinUi::ProgressState s=context();
    check(warning(r,s),"only finalized full transfer with serial reboot disconnect is successful warning");
    check(!(r.flags&EJODIN4_FLASH_REBOOT_SENT),"warning preserves reboot not confirmed");
    for(unsigned protocol=1;protocol<=2;++protocol){EJODIN4_FlashResult old=r;old.protocol_version=protocol;check(warning(old,s),"supported legacy protocol reboot disconnect warning accepted");}
    EJODIN4_FlashResult partial=r;partial.acknowledged_bytes--;OdinUi::ProgressState partialState=s;partialState.acknowledged=partial.acknowledged_bytes;
    check(!warning(partial,partialState),"partial ACK rejected by full-transfer proof even with consistent progress");
    EJODIN4_FlashResult empty=r;empty.image_count=empty.images_completed=0;check(!warning(empty,s),"empty image set never warning success");
    EJODIN4_FlashResult protocolZero=r;protocolZero.protocol_version=0;check(!warning(protocolZero,s),"missing negotiated protocol never warning success");
    EJODIN4_FlashResult wrongPhase=r;wrongPhase.phase=EJODIN4_PHASE_COMPLETE;check(!warning(wrongPhase,s),"USB error with COMPLETE phase is not narrow reboot case");
    EJODIN4_FlashResult sent=r;sent.flags|=EJODIN4_FLASH_REBOOT_SENT;check(!warning(sent,s),"already confirmed reboot cleanup failure is not warning case");
    OdinUi::Completion noReboot=OdinUi::completion(r,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_WRITE,false,false,s);
    check(!noReboot.success&&!noReboot.rebootWarning,"unrequested reboot cannot produce successful warning");
    check(!warning(r,s,EJODIN4_USB_IO_ERROR,true),"user cancellation never becomes warning success");
    check(!warning(r,s,EJODIN4_CANCELLED),"cancel status never becomes warning success");
    check(!warning(r,s,EJODIN4_TIMEOUT),"timeout status never becomes warning success");
    check(!warning(r,s,EJODIN4_INVALID_RESPONSE),"invalid response never becomes warning success");
    check(!warning(r,s,EJODIN4_INTERNAL_ERROR),"internal engine failure never becomes warning success");
    check(!warning(r,s,EJODIN4_BAD_HANDSHAKE),"protocol rejection never becomes warning success");
    s.invalid=true;check(!warning(r,s),"malformed callback blocks green result");s=context();
    s.terminalPhases=2;check(!warning(r,s),"cancel callback blocks warning");s=context();
    s.rebootPending=false;check(!warning(r,s),"must observe actual reboot phase");s=context();
    for(unsigned flag=1;flag<=16;flag*=2){if(flag==4)continue;EJODIN4_FlashResult bad=r;bad.flags&=~flag;check(!warning(bad,s),"every write finalized PIT plan flag required");}
    EJODIN4_FlashResult bad=r;bad.struct_size=1;check(!warning(bad,s),"wrong packed ABI size rejected");
    bad=r;bad.operation=EJODIN4_FLASH_PLAN_ONLY;check(!warning(bad,s),"wrong operation rejected");
    bad=r;bad.images_completed--;check(!warning(bad,s),"all image acknowledgements required");
    bad=r;bad.acknowledged_bytes--;check(!warning(bad,s),"all bytes required");
    bad=r;bad.total_bytes=bad.acknowledged_bytes=0;check(!warning(bad,s),"nonzero transfer total required");
    bad=r;bad.transport=EJODIN4_DOWNLOAD_WINUSB;check(!warning(bad,s),"warning narrowly limited to stock serial driver");
    bad=r;bad.protocol_version=4;check(!warning(bad,s),"warning only supported protocol 1 through 3");
    bad=r;bad.last_proven_phase=EJODIN4_PHASE_FINALIZING;check(!warning(bad,s),"finalize failure is not reboot failure");
    bad=r;bad.last_response_id=0x66;check(!warning(bad,s),"requires proven accepted finalization response");
    bad=r;bad.last_ack=1;check(!warning(bad,s),"device NACK never warning success");
    bad=r;bad.flags|=32;check(!warning(bad,s),"unknown result flags rejected");
    bad=r;bad.logical_sector_bytes=123;check(!warning(bad,s),"unknown geometry rejected");
    r.phase=EJODIN4_PHASE_COMPLETE;r.flags|=EJODIN4_FLASH_REBOOT_SENT;
    OdinUi::Completion c=OdinUi::completion(r,EJODIN4_OK,EJODIN4_FLASH_WRITE,true,false,s);
    check(c.valid&&c.success&&!c.rebootWarning,"confirmed complete flash remains successful without warning");
    s=OdinUi::ProgressState();s.roleMask=1u<<2;s.packageCount=1;s.reboot=true;
    EJODIN4_FlashProgress p=image();check(s.observe(&p),"valid image callback accepted");check(s.firstImage(19)&&!s.firstImage(19)&&s.firstImage(20),"same index logs once but repeated VBMETA at distinct index retained");
    p.reserved=1;check(!s.observe(&p)&&s.invalid,"malformed repeated callback validated before duplicate suppression");
    s=OdinUi::ProgressState();p=image();p.struct_size=0;check(!s.observe(&p),"callback ABI size rejected");
    s=OdinUi::ProgressState();p=image();p.phase=19;check(!s.observe(&p),"unknown callback phase rejected");
    s=OdinUi::ProgressState();s.roleMask=1u<<2;p=image();p.image_index=26;check(!s.observe(&p),"out of range ordered index rejected");
    s=OdinUi::ProgressState();s.roleMask=1u<<2;p=image();std::strcpy(p.sha256,"bad");check(!s.observe(&p),"malformed hash rejected");
    s=OdinUi::ProgressState();s.roleMask=1u<<2;p=image();p.acknowledged_bytes=101;check(!s.observe(&p),"counter exceeds total rejected");
    s=OdinUi::ProgressState();s.roleMask=1u<<2;p=image();p.acknowledged_bytes=50;check(s.observe(&p),"initial ACK accepted");p.acknowledged_bytes=49;check(!s.observe(&p),"regressing ACK rejected");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_PLAN_ONLY;s.roleMask=1u<<2;p=image();check(!s.observe(&p),"offline plan cannot emit write callback");
    s=OdinUi::ProgressState();s.packageCount=1;s.roleMask=1u<<2;p=image();p.phase=EJODIN4_PHASE_VERIFY_IMAGE;p.image_count=1;p.image_index=0;p.total_bytes=p.image_bytes=0;p.source_offset=50;p.stored_bytes=100;check(s.observe(&p),"valid verification accepted");p.source_offset=49;check(!s.observe(&p),"verification counter regression rejected");
    check(OdinUi::supported(1)&&OdinUi::supported(2)&&!OdinUi::supported(0)&&!OdinUi::supported(3)&&!OdinUi::supported(256),"device choices support only exact known transport flags");
    check(OdinUi::tone(L"Writing [19/26] super_error.img (AP)")==OdinUi::Image,"filename error token is not severity");
    check(OdinUi::tone(L"Warning: reboot error unconfirmed")==OdinUi::Warning,"warning with error word stays amber");
    check(OdinUi::tone(L"FAILED: Image rejected")==OdinUi::Error,"real failure red");
    check(OdinUi::tone(L"SUCCESS: Flash completed successfully.")==OdinUi::Good,"success green");
    check(OdinUi::tone(L"Policy: preserve data")==OdinUi::Configuration,"configuration distinct colour");
    check(OdinUi::tone(L"literal 100% %s %n <b> & text")==OdinUi::Plain,"literal text has no markup or format interpretation");
    std::printf("%s BCC UI logic: %u checks; no device I/O\n",failures?"FAIL":"PASS",checks);return failures?1:0;
}
