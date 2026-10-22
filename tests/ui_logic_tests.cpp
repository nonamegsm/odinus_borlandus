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
static EJODIN4_FlashProgress pitProgress(uint32_t phase){EJODIN4_FlashProgress p;std::memset(&p,0,sizeof(p));p.struct_size=sizeof(p);p.phase=phase;p.image_count=4;p.total_bytes=100;return p;}
int main(){
    EJODIN4_FlashResult r=finalized();OdinUi::ProgressState s=context();
    check(OdinUi::supported(EJODIN4_DOWNLOAD_TCP),"TCP is a supported completion transport");
    EJODIN4_FlashResult tcp=r;tcp.transport=EJODIN4_DOWNLOAD_TCP;check(!warning(tcp,s),"TCP reboot failure cannot use serial-only success warning");
    tcp.phase=EJODIN4_PHASE_COMPLETE;tcp.flags|=EJODIN4_FLASH_REBOOT_SENT;check(OdinUi::completion(tcp,EJODIN4_OK,EJODIN4_FLASH_WRITE,true,false,s).success,"acknowledged TCP flash completes");
    check(warning(r,s),"only finalized full transfer with serial reboot disconnect is successful warning");
    check(!(r.flags&EJODIN4_FLASH_REBOOT_SENT),"warning preserves reboot not confirmed");
    EJODIN4_FlashResult mixed=r;mixed.logical_sector_bytes=0;mixed.flags|=EJODIN4_FLASH_PER_PARTITION_PIT_UNITS;
    check(warning(mixed,s),"mixed known PIT units retain successful flash and unconfirmed-reboot warning");
    OdinUi::ProgressState strictState=s;strictState.logicalSectorBytes=512;
    check(!warning(mixed,strictState),"strict caller unit cannot accept mixed PIT units flag");
    EJODIN4_FlashResult homogeneous=mixed;homogeneous.logical_sector_bytes=512;
    check(!warning(homogeneous,s),"homogeneous reported unit cannot accompany mixed-unit flag");
    EJODIN4_FlashResult unplanned=mixed;unplanned.flags&=~EJODIN4_FLASH_PLAN_READY;check(!warning(unplanned,s),"mixed PIT units require planned package proof");
    EJODIN4_FlashResult unmapped=mixed;unmapped.flags&=~EJODIN4_FLASH_PIT_MATCHED;check(!warning(unmapped,s),"mixed PIT units require exact live-PIT route proof");
    EJODIN4_FlashResult missing=mixed;missing.flags&=~EJODIN4_FLASH_PER_PARTITION_PIT_UNITS;check(!warning(missing,s),"zero homogeneous unit without mixed-unit proof cannot succeed");
    EJODIN4_FlashResult strict=r;strict.phase=EJODIN4_PHASE_COMPLETE;strict.logical_sector_bytes=512;strict.flags|=EJODIN4_FLASH_REBOOT_SENT;
    OdinUi::Completion strictDone=OdinUi::completion(strict,EJODIN4_OK,EJODIN4_FLASH_WRITE,true,false,strictState);
    check(strictDone.valid&&strictDone.success,"strict caller unit remains homogeneous and successful");
    EJODIN4_FlashResult plan;std::memset(&plan,0,sizeof(plan));plan.struct_size=sizeof(plan);plan.operation=EJODIN4_FLASH_PLAN_ONLY;plan.phase=EJODIN4_PHASE_COMPLETE;
    plan.image_count=2;plan.total_bytes=100;plan.flags=EJODIN4_FLASH_PLAN_READY;OdinUi::ProgressState planState;planState.operation=EJODIN4_FLASH_PLAN_ONLY;
    check(OdinUi::completion(plan,EJODIN4_OK,EJODIN4_FLASH_PLAN_ONLY,false,false,planState).success,"valid offline plan control completes");
    plan.flags|=EJODIN4_FLASH_PER_PARTITION_PIT_UNITS;check(!OdinUi::completion(plan,EJODIN4_OK,EJODIN4_FLASH_PLAN_ONLY,false,false,planState).valid,"offline plan rejects write-only per-partition result flag");
    plan.operation=EJODIN4_FLASH_READ_PIT;plan.flags=EJODIN4_FLASH_PER_PARTITION_PIT_UNITS;plan.image_count=0;plan.total_bytes=0;
    plan.transport=EJODIN4_DOWNLOAD_SAMSUNG_SERIAL;plan.protocol_version=3;plan.pit_entry_count=1;
    check(!OdinUi::completion(plan,EJODIN4_OK,EJODIN4_FLASH_READ_PIT,false,false,planState).valid,"read-PIT result cannot carry a write-only mixed-unit flag");
    for(unsigned protocol=1;protocol<=5;++protocol){EJODIN4_FlashResult old=r;old.protocol_version=protocol;check(warning(old,s),"supported stock protocol reboot disconnect warning accepted");}
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
    bad=r;bad.protocol_version=6;check(!warning(bad,s),"warning only supported protocol 1 through 5");
    bad=r;bad.last_proven_phase=EJODIN4_PHASE_FINALIZING;check(!warning(bad,s),"finalize failure is not reboot failure");
    bad=r;bad.last_response_id=0x66;check(!warning(bad,s),"requires proven accepted finalization response");
    bad=r;bad.last_ack=1;check(!warning(bad,s),"device NACK never warning success");
    bad=r;bad.flags|=512;check(!warning(bad,s),"unknown result flags rejected");
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
    s=OdinUi::ProgressState();s.packageCount=1;s.roleMask=1u<<2;p=image();p.phase=EJODIN4_PHASE_VERIFY_IMAGE;p.image_count=1;p.image_index=0;
    p.total_bytes=p.image_bytes=0;p.source_offset=p.stored_bytes=100;check(s.observe(&p),"first verification pass completes");
    EJODIN4_FlashProgress preflight;std::memset(&preflight,0,sizeof(preflight));preflight.struct_size=sizeof(preflight);preflight.phase=EJODIN4_PHASE_PREFLIGHT;
    check(s.observe(&preflight),"fresh preflight starts another verification pass");
    check(!s.verifyPhase&&!s.verifyIndex&&s.verifyName.empty()&&!s.verifyCompleted&&!s.verifyTotal,"preflight resets only verification cursor");
    p.source_offset=0;check(s.observe(&p),"same verification name can restart from zero after preflight");
    p.source_offset=50;check(s.observe(&p),"new pass advances independently");p.source_offset=49;check(!s.observe(&p),"regression inside second pass still rejected");
    s=context();s.verifyPhase=EJODIN4_PHASE_VERIFY_IMAGE;s.verifyIndex=1;s.verifyName="same.img";s.verifyCompleted=s.verifyTotal=100;s.loggedImages[19]=true;
    preflight.acknowledged_bytes=preflight.total_bytes=s.acknowledged;check(s.observe(&preflight),"preflight with consistent ACK counter accepted");
    check(s.acknowledged==11882352976ULL&&s.terminalPhases==0&&s.rebootPending&&s.loggedImages[19],"preflight preserves ACK and safety state");
    s.terminalPhases=2;check(s.observe(&preflight)&&s.terminalPhases==2,"preflight does not clear cancellation proof");
    check(OdinUi::supported(1)&&OdinUi::supported(2)&&!OdinUi::supported(0)&&OdinUi::supported(3)&&!OdinUi::supported(4)&&!OdinUi::supported(256),"device choices support only exact known transport flags");
    check(OdinUi::tone(L"Writing [19/26] super_error.img (AP)")==OdinUi::Image,"filename error token is not severity");
    check(OdinUi::tone(L"Warning: reboot error unconfirmed")==OdinUi::Warning,"warning with error word stays amber");
    check(OdinUi::tone(L"FAILED: Image rejected")==OdinUi::Error,"real failure red");
    check(OdinUi::tone(L"SUCCESS: Flash completed successfully.")==OdinUi::Good,"success green");
    check(OdinUi::tone(L"Policy: preserve data")==OdinUi::Configuration,"configuration distinct colour");
    check(OdinUi::tone(L"literal 100% %s %n <b> & text")==OdinUi::Plain,"literal text has no markup or format interpretation");
    const uint32_t full=(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4);
    check(OdinUi::repartitionSelection(EJODIN4_CLEAN_INSTALL,full),"embedded target permits clean full image selection");
    check(OdinUi::repartitionSelection(EJODIN4_CLEAN_INSTALL,full|(1u<<6)|(1u<<8)),"raw target permits optional USERDATA with clean full images");
    for(unsigned role=1;role<=4;++role)check(!OdinUi::repartitionSelection(EJODIN4_CLEAN_INSTALL,full&~(1u<<role)),"each BL AP CP CSC slot required for repartition");
    check(!OdinUi::repartitionSelection(EJODIN4_PRESERVE_DATA,full)&&!OdinUi::repartitionSelection(EJODIN4_UPDATE_SELECTED,full),"repartition never uses preserve or selected-update policy");
    check(!OdinUi::repartitionSelection(EJODIN4_CLEAN_INSTALL,full|(1u<<7))&&!OdinUi::repartitionSelection(EJODIN4_CLEAN_INSTALL,full|(1u<<5)),"reference and HOME_CSC cannot authorize repartition");
    check(OdinUi::rawPitName("C:/firmware/device.PIT")&&!OdinUi::rawPitName("device.pit.tar")&&!OdinUi::rawPitName(".xml")&&!OdinUi::rawPitName(""),"raw target accepts only explicit PIT extension");
    for(unsigned phase=18;phase<=24;++phase){
        s=OdinUi::ProgressState();p=pitProgress(phase);
        check(!s.observe(&p),"ordinary operation rejects repartition and malformed verification phases");
    }
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;
    p=pitProgress(18);check(s.observe(&p)&&s.pitStage==1,"PIT upload intent starts ordered proof");
    p.phase=19;check(s.observe(&p)&&s.pitStage==2,"PIT upload complete follows intent");
    p.phase=22;check(s.observe(&p)&&s.pitStage==3,"PIT readback proof follows upload");
    s.roleMask=1u<<2;p=image();check(s.observe(&p),"firmware begins only after PIT readback proof");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;s.roleMask=1u<<2;p=image();check(!s.observe(&p),"image before PIT proof rejected");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;p=pitProgress(19);check(!s.observe(&p),"PIT upload cannot skip intent");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;p=pitProgress(18);check(s.observe(&p)&&!s.observe(&p),"duplicate PIT stage rejected");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;p=pitProgress(18);p.acknowledged_bytes=1;check(!s.observe(&p),"PIT bytes excluded from firmware acknowledgement counter");
    s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;p=pitProgress(18);p.package_role=8;check(!s.observe(&p),"PIT upload is not a firmware image role");
    for(unsigned phase=23;phase<=24;++phase){s=OdinUi::ProgressState();s.operation=EJODIN4_FLASH_REPARTITION;p=pitProgress(phase);check(!s.observe(&p),"obsolete repartition phase rejected");}
    EJODIN4_FlashResult rep=finalized();rep.operation=EJODIN4_FLASH_REPARTITION;rep.phase=EJODIN4_PHASE_COMPLETE;rep.flags|=EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_PIT_UPLOADED|EJODIN4_FLASH_PIT_VERIFIED|EJODIN4_FLASH_REBOOT_SENT;
    OdinUi::ProgressState repState=context();repState.operation=EJODIN4_FLASH_REPARTITION;repState.pitStage=3;
    check(OdinUi::completion(rep,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repState).success,"repartition success requires complete PIT and firmware proof");
    repState.pitStage=2;check(!OdinUi::completion(rep,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repState).success,"result alone cannot replace missing PIT readback callback");repState.pitStage=3;
    rep.phase=EJODIN4_PHASE_FAILED;rep.last_proven_phase=18;rep.image_count=4;rep.images_completed=0;rep.acknowledged_bytes=0;rep.total_bytes=100;repState.acknowledged=0;repState.pitStage=1;
    for(unsigned mask=0;mask<16;++mask){
        rep.flags=((mask&1)?EJODIN4_FLASH_ANY_WRITE:0)|((mask&2)?EJODIN4_FLASH_PIT_WRITE_STARTED:0)|((mask&4)?EJODIN4_FLASH_PIT_UPLOADED:0)|((mask&8)?EJODIN4_FLASH_PIT_VERIFIED:0);
        const bool expected=mask==0||mask==3||mask==7||mask==15;
        const OdinUi::Completion failed=OdinUi::completion(rep,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_REPARTITION,true,false,repState);
        check(failed.valid==expected&&!failed.success,"PIT provenance chain exhaustively validates zero-image failures");
    }
    rep.flags=EJODIN4_FLASH_ANY_WRITE|EJODIN4_FLASH_PIT_WRITE_STARTED;rep.transport=EJODIN4_DOWNLOAD_WINUSB;
    check(!OdinUi::completion(rep,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_REPARTITION,true,false,repState).valid,"repartition rejects unsupported WinUSB result");
    rep.transport=EJODIN4_DOWNLOAD_SAMSUNG_SERIAL;rep.operation=EJODIN4_FLASH_WRITE;rep.last_proven_phase=EJODIN4_PHASE_NEGOTIATING;
    check(!OdinUi::completion(rep,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_WRITE,true,false,repState).valid,"ordinary write rejects PIT side-effect flags");
    check(OdinUi::tone(L"Re-partition: replace PIT before images")==OdinUi::Warning&&OdinUi::tone(L"PIT readback verified.")==OdinUi::Good,"repartition warning and verified proof use distinct log tones");
    // PIT writes are explicit and their separate proof cannot be inferred from image totals.
    OdinUi::ProgressState rp;rp.operation=EJODIN4_FLASH_REPARTITION;rp.roleMask=1u<<2;rp.packageCount=2;
    EJODIN4_FlashProgress pitProgress;std::memset(&pitProgress,0,sizeof(pitProgress));pitProgress.struct_size=sizeof(pitProgress);
    pitProgress.image_count=26;pitProgress.total_bytes=100;pitProgress.phase=EJODIN4_PHASE_PIT_UPLOAD_BEGIN;
    OdinUi::ProgressState normal=rp;normal.operation=EJODIN4_FLASH_WRITE;check(!normal.observe(&pitProgress),"normal flash rejects PIT-changing callback");
    OdinUi::ProgressState offlinePit=rp;offlinePit.operation=EJODIN4_FLASH_PLAN_ONLY;check(!offlinePit.observe(&pitProgress),"offline plan never uploads PIT");
    OdinUi::ProgressState readPit=rp;readPit.operation=EJODIN4_FLASH_READ_PIT;check(!readPit.observe(&pitProgress),"read-PIT operation never uploads PIT");
    OdinUi::ProgressState earlyImage=rp;EJODIN4_FlashProgress early=image();check(!earlyImage.observe(&early),"repartition rejects firmware before verified readback");
    OdinUi::ProgressState skippedPit=rp;pitProgress.phase=EJODIN4_PHASE_PIT_VERIFIED;check(!skippedPit.observe(&pitProgress),"cannot skip upload proof phases");
    pitProgress.phase=EJODIN4_PHASE_PIT_UPLOAD_BEGIN;check(rp.observe(&pitProgress)&&rp.pitStage==1,"explicit PIT begin accepted without firmware ACK");
    OdinUi::ProgressState malformedPit=rp;EJODIN4_FlashProgress malformed=pitProgress;malformed.image_bytes=1;check(!malformedPit.observe(&malformed),"PIT bytes never count as firmware image bytes");
    malformedPit=rp;malformed=pitProgress;malformed.package_role=8;check(!malformedPit.observe(&malformed),"PIT stage uses global callback context not archive role");
    malformedPit=rp;malformed=pitProgress;malformed.partition.identifier=1;check(!malformedPit.observe(&malformed),"PIT stage cannot smuggle image partition metadata");
    pitProgress.phase=EJODIN4_PHASE_PIT_UPLOAD_COMPLETE;check(rp.observe(&pitProgress)&&rp.pitStage==2,"PIT upload acknowledgment separate from readback");
    earlyImage=rp;check(!earlyImage.observe(&early),"uploaded PIT alone cannot authorize image progress");
    pitProgress.phase=EJODIN4_PHASE_PIT_VERIFIED;check(rp.observe(&pitProgress)&&rp.pitStage==3,"PIT readback accepted after upload");
    check(rp.observe(&early),"ordinary image phases allowed after PIT verification");
    OdinUi::ProgressState regressionPit=rp;pitProgress.phase=EJODIN4_PHASE_PIT_UPLOAD_COMPLETE;check(!regressionPit.observe(&pitProgress),"PIT proof phases cannot regress");
    EJODIN4_FlashResult repartDone=finalized();repartDone.operation=EJODIN4_FLASH_REPARTITION;repartDone.phase=EJODIN4_PHASE_COMPLETE;repartDone.flags|=EJODIN4_FLASH_REBOOT_SENT|EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_PIT_UPLOADED|EJODIN4_FLASH_PIT_VERIFIED;
    OdinUi::ProgressState repartState=context();repartState.operation=EJODIN4_FLASH_REPARTITION;repartState.pitStage=3;
    check(OdinUi::completion(repartDone,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).success,"full PIT proof plus completed firmware succeeds");
    repartState.pitStage=2;check(!OdinUi::completion(repartDone,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).success,"result flags cannot replace missing observed readback");repartState.pitStage=3;
    for(unsigned bit=64;bit<=256;bit*=2){EJODIN4_FlashResult missingProof=repartDone;missingProof.flags&=~bit;check(!OdinUi::completion(missingProof,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).success,"every explicit PIT result proof is required");}
    EJODIN4_FlashResult tcpRepart=repartDone;tcpRepart.transport=EJODIN4_DOWNLOAD_TCP;
    check(OdinUi::completion(tcpRepart,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).success,"TCP repartition succeeds with complete PIT and firmware proof");
    for(unsigned bit=64;bit<=256;bit*=2){EJODIN4_FlashResult missingProof=tcpRepart;missingProof.flags&=~bit;check(!OdinUi::completion(missingProof,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).success,"TCP repartition requires every independent PIT proof");}
    OdinUi::ProgressState tcpRepartState=repartState;tcpRepartState.pitStage=2;
    check(!OdinUi::completion(tcpRepart,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,tcpRepartState).success,"TCP result flags cannot replace missing readback callback");
    EJODIN4_FlashResult tcpPartial=tcpRepart;--tcpPartial.acknowledged_bytes;tcpRepartState=repartState;tcpRepartState.acknowledged=tcpPartial.acknowledged_bytes;
    check(!OdinUi::completion(tcpPartial,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,tcpRepartState).success,"TCP repartition cannot succeed with partial firmware byte acknowledgement");
    EJODIN4_FlashResult tcpCancelled=tcpPartial;tcpCancelled.phase=EJODIN4_PHASE_CANCELLED;tcpCancelled.last_proven_phase=EJODIN4_PHASE_DATA_ACK;--tcpCancelled.images_completed;
    tcpCancelled.flags&=~(EJODIN4_FLASH_FINALIZED|EJODIN4_FLASH_REBOOT_SENT);tcpRepartState.rebootPending=false;
    const OdinUi::Completion tcpCancelledResult=OdinUi::completion(tcpCancelled,EJODIN4_CANCELLED,EJODIN4_FLASH_REPARTITION,true,true,tcpRepartState);
    check(tcpCancelledResult.valid&&!tcpCancelledResult.success&&!tcpCancelledResult.rebootWarning,"TCP repartition cancellation retains PIT proof and acknowledged prefix without success");
    tcpRepartState.terminalPhases=2;
    const OdinUi::Completion tcpCancelCallbackResult=OdinUi::completion(tcpCancelled,EJODIN4_CANCELLED,EJODIN4_FLASH_REPARTITION,true,false,tcpRepartState);
    check(tcpCancelCallbackResult.valid&&!tcpCancelCallbackResult.success&&!tcpCancelCallbackResult.rebootWarning,"TCP repartition cancelled status and callback cannot produce success");
    EJODIN4_FlashResult tcpReboot=tcpRepart;tcpReboot.phase=EJODIN4_PHASE_FAILED;tcpReboot.flags&=~EJODIN4_FLASH_REBOOT_SENT;
    const OdinUi::Completion tcpRebootResult=OdinUi::completion(tcpReboot,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_REPARTITION,true,false,repartState);
    check(tcpRebootResult.valid&&!tcpRebootResult.success&&!tcpRebootResult.rebootWarning,"TCP repartition reboot error cannot use serial-only warning success");
    EJODIN4_FlashResult usbRepart=tcpRepart;usbRepart.transport=EJODIN4_DOWNLOAD_WINUSB;
    check(!OdinUi::completion(usbRepart,EJODIN4_OK,EJODIN4_FLASH_REPARTITION,true,false,repartState).valid,"WinUSB repartition remains unsupported even with complete proof");
    OdinUi::ProgressState ordinaryState=repartState;ordinaryState.operation=EJODIN4_FLASH_WRITE;
    check(!OdinUi::completion(repartDone,EJODIN4_OK,EJODIN4_FLASH_WRITE,true,false,ordinaryState).valid,"unrequested PIT result flags invalid");
    EJODIN4_FlashResult repartWarning=repartDone;repartWarning.phase=EJODIN4_PHASE_FAILED;repartWarning.flags&=~EJODIN4_FLASH_REBOOT_SENT;
    check(OdinUi::completion(repartWarning,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_REPARTITION,true,false,repartState).rebootWarning,"fully verified PIT and firmware permit narrow reboot warning");
    repartWarning.flags&=~EJODIN4_FLASH_PIT_VERIFIED;check(!OdinUi::completion(repartWarning,EJODIN4_USB_IO_ERROR,EJODIN4_FLASH_REPARTITION,true,false,repartState).rebootWarning,"reboot warning cannot hide missing PIT proof");
    EJODIN4_FlashResult pitFailure;std::memset(&pitFailure,0,sizeof(pitFailure));pitFailure.struct_size=sizeof(pitFailure);pitFailure.operation=EJODIN4_FLASH_REPARTITION;pitFailure.phase=EJODIN4_PHASE_FAILED;pitFailure.last_proven_phase=EJODIN4_PHASE_PIT_UPLOAD_COMPLETE;
    pitFailure.flags=EJODIN4_FLASH_ANY_WRITE|EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_PIT_UPLOADED;pitFailure.transport=EJODIN4_DOWNLOAD_SAMSUNG_SERIAL;pitFailure.protocol_version=3;
    OdinUi::ProgressState failedState;failedState.operation=EJODIN4_FLASH_REPARTITION;failedState.pitStage=2;
    OdinUi::Completion failedDone=OdinUi::completion(pitFailure,EJODIN4_PIT_MAPPING_ERROR,EJODIN4_FLASH_REPARTITION,false,false,failedState);
    check(failedDone.valid&&!failedDone.success,"PIT upload readback failure remains valid failed result without firmware ACK");
    pitFailure.flags&=~EJODIN4_FLASH_ANY_WRITE;check(!OdinUi::completion(pitFailure,EJODIN4_PIT_MAPPING_ERROR,EJODIN4_FLASH_REPARTITION,false,false,failedState).valid,"PIT started requires destructive-write warning bit");
    plan=EJODIN4_FlashResult();plan.struct_size=sizeof(plan);plan.operation=EJODIN4_FLASH_PLAN_ONLY;plan.phase=EJODIN4_PHASE_COMPLETE;plan.flags=EJODIN4_FLASH_PLAN_READY;plan.image_count=2;plan.total_bytes=100;
    OdinUi::ProgressState repartPlan;repartPlan.operation=EJODIN4_FLASH_PLAN_ONLY;
    check(OdinUi::completion(plan,EJODIN4_OK,EJODIN4_FLASH_PLAN_ONLY,false,false,repartPlan).success,"offline target plan needs no device-write proof");
    check(OdinUi::tone(L"Re-partition: write selected PIT")==OdinUi::Warning,"destructive configuration stays amber");
    std::printf("%s BCC UI logic: %u checks; no device I/O\n",failures?"FAIL":"PASS",checks);return failures?1:0;
}
