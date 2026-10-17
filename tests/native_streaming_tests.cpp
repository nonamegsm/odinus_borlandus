/* Classic BCC32 engine regression tests. Synthetic files only: no USB I/O. */
#define NOMINMAX
#include "Odin4FirmwareStream.h"
#include "Odin4LayoutProfile.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "lz4.h"
#include "xxhash.h"
using namespace ejodin4;
typedef std::vector<uint8_t> Bytes;
static unsigned checks=0;
static void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
static void le32(Bytes& bytes,size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=uint8_t(value>>(8*i));}
static Bytes bytes(const char* text){return Bytes(text,text+std::strlen(text));}
static void append(Bytes& tar,const char* name,const Bytes& data){
    Bytes header(512,0);std::memcpy(&header[0],name,std::strlen(name));
    uint64_t size=data.size();for(int i=134;i>=124;--i){header[size_t(i)]=uint8_t('0'+(size&7));size>>=3;}
    header[156]='0';std::memcpy(&header[257],"ustar",5);std::fill(header.begin()+148,header.begin()+156,uint8_t(' '));
    unsigned sum=0;for(size_t i=0;i<header.size();++i)sum+=header[i];char octal[9]={0};std::sprintf(octal,"%06o",sum);std::memcpy(&header[148],octal,6);header[154]=0;header[155]=' ';
    tar.insert(tar.end(),header.begin(),header.end());tar.insert(tar.end(),data.begin(),data.end());tar.resize((tar.size()+511)/512*512,0);
}
struct Temp {
    std::vector<std::string> files;
    ~Temp(){for(size_t i=0;i<files.size();++i)DeleteFileA(files[i].c_str());}
    std::string write(Bytes data){
        data.resize(data.size()+1024,0);char folder[MAX_PATH+1]={0},name[MAX_PATH+1]={0};
        check(GetTempPathA(MAX_PATH,folder)!=0&&GetTempFileNameA(folder,"obc",0,name)!=0,"temporary TAR path");
        HANDLE file=CreateFileA(name,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);check(file!=INVALID_HANDLE_VALUE,"open temporary TAR");
        DWORD actual=0;BOOL ok=WriteFile(file,&data[0],DWORD(data.size()),&actual,0);CloseHandle(file);files.push_back(name);check(ok&&actual==data.size(),"write temporary TAR");return name;
    }
};
static Bytes pit(){
    const char* names[]={"boot.img","userdata.img","misc.bin","omr.img"};const char* parts[]={"BOOT","USERDATA","MISC","OMR"};
    Bytes p(28+4*132,0);le32(p,0,0x12349876);le32(p,4,4);
    for(size_t i=0;i<4;++i){size_t at=28+i*132;le32(p,at+4,8);le32(p,at+8,uint32_t(i+1));le32(p,at+24,16384);std::memcpy(&p[at+36],parts[i],std::strlen(parts[i]));std::memcpy(&p[at+68],names[i],std::strlen(names[i]));}return p;
}
static void sparse_tests(){
    std::string error;uint64_t expanded=0;Bytes raw=bytes("abc");SparseGeometry g;
    for(size_t i=0;i<raw.size();++i)check(g.consume(&raw[i],1,error)==EJODIN4_OK,"fragmented raw image");check(g.finish(3,expanded,error)==EJODIN4_OK&&expanded==3,"raw byte count");
    Bytes sparse(44,0);le32(sparse,0,0xed26ff3a);sparse[4]=1;sparse[8]=28;sparse[10]=12;le32(sparse,12,4096);le32(sparse,16,2097152);le32(sparse,20,1);sparse[28]=0xc2;sparse[29]=0xca;le32(sparse,32,2097152);le32(sparse,36,16);le32(sparse,40,0xaabbccdd);
    SparseGeometry fill;for(size_t i=0;i<sparse.size();++i)check(fill.consume(&sparse[i],1,error)==EJODIN4_OK,"fragmented FILL sparse");check(fill.finish(sparse.size(),expanded,error)==EJODIN4_OK&&expanded==(uint64_t(8)<<30),"64-bit sparse expanded geometry");
    uint8_t extra=1;check(fill.consume(&extra,1,error)==EJODIN4_INVALID_COMPRESSED_DATA,"sparse trailing byte rejected");
    SparseGeometry short_image;check(short_image.consume(&sparse[0],43,error)==EJODIN4_OK,"truncated fill prefix");check(short_image.finish(43,expanded,error)==EJODIN4_INVALID_COMPRESSED_DATA,"truncated sparse rejected");
    sparse[24]=1;SparseGeometry crc;check(crc.consume(&sparse[0],sparse.size(),error)==EJODIN4_INVALID_COMPRESSED_DATA,"unsupported expanded sparse checksum rejected");
}
struct Progress {unsigned calls;uint64_t done,total;Progress():calls(0),done(0),total(0){}static void report(void* context,const VerificationProgress& value){Progress* p=static_cast<Progress*>(context);++p->calls;p->done=value.completed_bytes;p->total=value.total_bytes;check(value.completed_bytes<=value.total_bytes,"verification progress bounds");}};
static int32_t EJODIN4_CALL stop(void*){return 1;}
static void planner_tests(){
    Temp temp;Bytes ap;append(ap,"boot.img",bytes("abc"));append(ap,"userdata.img",bytes("private"));append(ap,"misc.bin",bytes("erase"));append(ap,"omr.img",bytes("protected"));append(ap,"meta-data/fota.zip",bytes("metadata"));
    Bytes reference;append(reference,"layout.pit",pit());append(reference,"ignored.img.lz4",bytes("not a valid lz4 frame"));
    Bytes repeat;append(repeat,"boot.img",bytes("xyz"));std::string a=temp.write(ap),r=temp.write(reference),u=temp.write(repeat);
    EJODIN4_FirmwareFile files[3];files[0].role=EJODIN4_PACKAGE_USERDATA;files[0].path=u.c_str();files[0].path_bytes=uint32_t(u.size()+1);files[1].role=EJODIN4_PACKAGE_PIT_REFERENCE;files[1].path=r.c_str();files[1].path_bytes=uint32_t(r.size()+1);files[2].role=EJODIN4_PACKAGE_AP;files[2].path=a.c_str();files[2].path_bytes=uint32_t(a.size()+1);
    Cancellation cancel={0,0};std::unique_ptr<FirmwareSet> set;std::string error;Progress progress;
    EJODIN4_Status status=prepare_firmware_set(files,3,EJODIN4_UPDATE_SELECTED,1048576,cancel,set,error,VerificationCallback(&Progress::report,&progress));if(status!=EJODIN4_OK)std::cerr<<error<<std::endl;
    check(status==EJODIN4_OK&&set.get()!=0,"prepare actual held archives with reference-only PIT");check(set->images.size()==2&&set->total_bytes==6,"ordered repeated writes included in byte total");check(set->images[0].role==EJODIN4_PACKAGE_AP&&set->images[1].role==EJODIN4_PACKAGE_USERDATA,"canonical role order independent of caller order");
    check(set->images[0].sha256=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","CryptoAPI SHA256 known answer");check(set->images[0].sha256!=set->images[1].sha256,"overwrites retain distinct digests");check(progress.calls>3&&progress.done==progress.total,"synchronous callbacks remain alive and finish");
    unsigned protected_skips=0,reference_skips=0;for(size_t i=0;i<set->skips.size();++i){protected_skips+=set->skips[i].reason==EJODIN4_SKIP_USERDATA;reference_skips+=set->skips[i].reason==EJODIN4_SKIP_PIT_REFERENCE;}check(protected_skips==3&&reference_skips==1,"selected protections and reference exclusion retained");
    check(set->validate_device_pit(set->package_pit,4096,error)==EJODIN4_OK,"exact live PIT route and capacity validation");set->images[1].route.attributes^=1;check(set->validate_device_pit(set->package_pit,4096,error)==EJODIN4_PIT_MAPPING_ERROR,"repeated route mutation fails closed");set->images[1].route.attributes^=1;
    std::unique_ptr<ImageReader> reader;check(set->open_image(0,reader,error)==EJODIN4_OK,"open second pass reader");uint8_t out[8]={0};uint32_t actual=0;check(reader->read(out,8,actual,cancel,error)==EJODIN4_OK&&actual==3&&std::memcmp(out,"abc",3)==0,"second pass payload");check(reader->read(out,8,actual,cancel,error)==EJODIN4_OK&&actual==0,"second pass length and SHA validated at EOF");reader.reset();set.reset();
    Cancellation stopped={stop,0};check(prepare_firmware_set(files,3,EJODIN4_UPDATE_SELECTED,1048576,stopped,set,error)==EJODIN4_CANCELLED&&set.get()==0,"preflight cancellation releases held files");
    check(prepare_firmware_set(files,3,EJODIN4_UPDATE_SELECTED,1,cancel,set,error)==EJODIN4_DECODE_LIMIT_EXCEEDED&&set.get()==0,"decoded byte cap fails closed");
}
static std::vector<EJODIN4_PitEntry> parse_test_pit(const Bytes& table){std::vector<EJODIN4_PitEntry> entries(512);uint32_t count=0;char diagnostic[512]={0};check(EJODIN4_ParsePit(&table[0],uint32_t(table.size()),&entries[0],512,&count,diagnostic,sizeof(diagnostic))==EJODIN4_OK,"parse synthetic/optional logical PIT");entries.resize(count);return entries;}
static void layout_tests(const char* path){
    FirmwareSet firmware;LayoutResolution result;std::string error;Bytes empty;
    check(result.sector_bytes==0&&!result.per_partition_units&&result.source==0,"layout defaults retain no inferred unit");
    check(resolve_layout_profile(firmware,empty,0,0,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"automatic units require a real plan and matching logical PIT");
    const uint32_t units[]={512,1024,2048,4096};for(size_t i=0;i<4;++i)check(resolve_layout_profile(firmware,empty,0,0,units[i],result,error)==EJODIN4_OK&&result.sector_bytes==units[i]&&!result.per_partition_units,"positive caller units override automatic route rules");
    check(resolve_layout_profile(firmware,empty,0,0,8192,result,error)==EJODIN4_INVALID_ARGUMENT&&result.sector_bytes==0&&result.source==0,"invalid caller units never fall back to auto");
    Bytes table=pit();firmware.package_pit_bytes=table;firmware.package_pit=parse_test_pit(table);firmware.data_policy=EJODIN4_UPDATE_SELECTED;
    PlanImage image;image.role=EJODIN4_PACKAGE_AP;image.route=firmware.package_pit[0];image.name="boot.img";image.sha256="unrelated-new-vendor-boot-hash";image.decoded_bytes=1;image.expanded_bytes=67108864;firmware.images.push_back(image);
    const char* name="AP_SM-G991B_OTHER_BUILD_random.tar.md5";EJODIN4_FirmwareFile file;file.role=EJODIN4_PACKAGE_AP;file.path=name;file.path_bytes=uint32_t(std::strlen(name)+1);
    check(resolve_layout_profile(firmware,table,&file,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==4096&&!result.per_partition_units&&result.source,"generic UFS plan resolves4096 by PIT storage type without model gate");
    file.path="completely-renamed-firmware.tar";file.path_bytes=uint32_t(std::strlen(file.path)+1);firmware.images[0].sha256="another-vendor-boot-hash";
    check(resolve_layout_profile(firmware,table,&file,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==4096,"different filename/build/hash has no effect on PIT units");
    Bytes live=table;live.resize(live.size()+12,0xa5);check(same_pit_layout(table,live)&&resolve_layout_profile(firmware,live,&file,1,0,result,error)==EJODIN4_OK,"logical PIT matching excludes transport padding");
    live=table;live[8]^=1;check(resolve_layout_profile(firmware,live,&file,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR&&result.source==0,"logical header mismatch rejected");
    live=table;live[28+24]^=1;check(resolve_layout_profile(firmware,live,&file,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"logical entry mismatch rejected");live=table;live.resize(28+131);check(!same_pit_layout(table,live),"truncated logical PIT rejected");
    EJODIN4_FirmwareFile duplicates[2];duplicates[0]=file;duplicates[1]=file;check(resolve_layout_profile(firmware,table,duplicates,2,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"duplicate package roles rejected");check(resolve_layout_profile(firmware,table,0,0,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"automatic units need image packages");
    firmware.images.clear();check(resolve_layout_profile(firmware,table,&file,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"automatic units require images");firmware.images.push_back(image);
    Bytes mmc=table;for(size_t i=0;i<4;++i)le32(mmc,28+i*132+4,2);firmware.package_pit_bytes=mmc;firmware.package_pit=parse_test_pit(mmc);firmware.images[0].route=firmware.package_pit[0];
    check(resolve_layout_profile(firmware,mmc,&file,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==512&&!result.per_partition_units,"homogeneous eMMC plan uses512 units");
    Bytes mixed=table;le32(mixed,28+132+4,2);firmware.package_pit_bytes=mixed;firmware.package_pit=parse_test_pit(mixed);firmware.images[0].route=firmware.package_pit[0];PlanImage second=image;second.route=firmware.package_pit[1];second.expanded_bytes=512;firmware.images.push_back(second);
    check(resolve_layout_profile(firmware,mixed,&file,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==0&&result.per_partition_units,"mixed selected UFS/eMMC plan reports per-partition units and no false common unit");
    check(resolve_layout_profile(firmware,mixed,&file,1,4096,result,error)==EJODIN4_OK&&result.sector_bytes==4096&&!result.per_partition_units,"caller unit overrides mixed automatic units");
    firmware.images[1].route.device_type=1;check(resolve_layout_profile(firmware,mixed,&file,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"unknown selected storage type cannot resolve allocation units");firmware.images.resize(1);
    if(path&&*path){std::FILE* input=std::fopen(path,"rb");check(input!=0,"open optional real PIT");Bytes real;uint8_t block[1024];size_t actual=0;while((actual=std::fread(block,1,sizeof(block),input))!=0)real.insert(real.end(),block,block+actual);std::fclose(input);std::vector<EJODIN4_PitEntry> entries=parse_test_pit(real);firmware.package_pit_bytes=real;bool found=false;for(size_t i=0;i<entries.size();++i)if(entries[i].device_type==2||entries[i].device_type==8){firmware.images[0].route=entries[i];found=true;break;}check(found,"real PIT has supported storage route");check(resolve_layout_profile(firmware,real,&file,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==(firmware.images[0].route.device_type==8?4096u:512u),"real PIT uses storage-type rule without model fingerprint");}
}
static void generic_capacity_tests(){
    std::string error;FirmwareSet set;set.data_policy=EJODIN4_CLEAN_INSTALL;PlanImage image;image.route.identifier=1;image.route.device_type=8;image.route.block_count=2;std::strcpy(image.route.partition_name,"BOOT");std::strcpy(image.route.file_name,"boot.img");image.decoded_bytes=1;image.expanded_bytes=4096;set.images.push_back(image);std::vector<EJODIN4_PitEntry> live(1,image.route);
    check(set.validate_device_pit(live,0,error)==EJODIN4_OK,"UFS automatic4096 unit validates full sparse expanded bytes");
    check(set.validate_device_pit(live,512,error)==EJODIN4_PIT_MAPPING_ERROR,"explicit512 override rejects expanded image without auto fallback");check(set.validate_device_pit(live,4096,error)==EJODIN4_OK,"explicit4096 override remains strict");
    live[0].device_type=2;set.images[0].route=live[0];check(set.validate_device_pit(live,0,error)==EJODIN4_PIT_MAPPING_ERROR,"same expanded image exceeds automatic eMMC512 capacity");set.images[0].expanded_bytes=1024;check(set.validate_device_pit(live,0,error)==EJODIN4_OK,"eMMC exact512-unit capacity accepted");
    PlanImage ufs=image;ufs.route.identifier=2;ufs.route.block_count=1;std::strcpy(ufs.route.partition_name,"VENDOR_BOOT");std::strcpy(ufs.route.file_name,"vendor_boot.img");set.images.push_back(ufs);live.push_back(ufs.route);check(set.validate_device_pit(live,0,error)==EJODIN4_OK,"mixed image capacities use each route's own allocation unit");set.images[0].expanded_bytes=1025;check(set.validate_device_pit(live,0,error)==EJODIN4_PIT_MAPPING_ERROR,"one-byte eMMC overflow fails whole mixed plan despite UFS neighbor");set.images.resize(1);live.resize(1);set.images[0].expanded_bytes=1024;
    live[0].attributes^=1;check(set.validate_device_pit(live,0,error)==EJODIN4_PIT_MAPPING_ERROR,"automatic units retain exact selected route equality");live[0].attributes^=1;
    for(unsigned type=0;type<10;++type){live[0].device_type=type;set.images[0].route=live[0];const EJODIN4_Status status=set.validate_device_pit(live,0,error);check(status==((type==2||type==8)?EJODIN4_OK:EJODIN4_PIT_MAPPING_ERROR),"only MMC/UFS addressing remains supported");}
    live[0]=image.route;live[0].block_count=0;set.images[0].route=live[0];check(set.validate_device_pit(live,0,error)==EJODIN4_PIT_MAPPING_ERROR,"zero-count non-USERDATA always rejected");
    std::strcpy(live[0].partition_name,"USERDATA");std::strcpy(live[0].file_name,"userdata.img");set.images[0].route=live[0];set.images[0].expanded_bytes=8192;
    check(set.validate_device_pit(live,0,error)==EJODIN4_PIT_MAPPING_ERROR,"zero-count USERDATA has no auto or model-derived capacity floor");
    check(set.validate_device_pit(live,0,error,8192)==EJODIN4_OK,"zero-count USERDATA accepts independent capacity with automatic UFS units");check(set.validate_device_pit(live,0,error,4096)==EJODIN4_PIT_MAPPING_ERROR,"measured USERDATA capacity still bounds expanded sparse bytes");check(set.validate_device_pit(live,0,error,8193)==EJODIN4_PIT_MAPPING_ERROR,"measured USERDATA capacity must align to resolved UFS units");
    check(set.validate_device_pit(live,4096,error,8192)==EJODIN4_OK,"explicit units plus verified USERDATA capacity remains strict");
    std::strcpy(live[0].partition_name,"USER_DATA");set.images[0].route=live[0];check(set.validate_device_pit(live,0,error,8192)==EJODIN4_OK,"USER_DATA alias supports independently measured capacity");check(set.validate_device_pit(live,0,error,UINT64_MAX)==EJODIN4_PIT_MAPPING_ERROR,"unbounded external capacity rejected");
    live[0].block_count=2;set.images[0].route=live[0];check(set.validate_device_pit(live,0,error,12288)==EJODIN4_PIT_MAPPING_ERROR,"measured capacity cannot contradict positive PIT extent");set.data_policy=EJODIN4_UPDATE_SELECTED;check(set.validate_device_pit(live,0,error,8192)==EJODIN4_PIT_MAPPING_ERROR,"automatic units do not bypass selected USERDATA protection");set.data_policy=EJODIN4_CLEAN_INSTALL;set.images[0].expanded_bytes=0;check(set.validate_device_pit(live,0,error,8192)==EJODIN4_PIT_MAPPING_ERROR,"zero logical image extent remains rejected");
}
static void userdata_capacity_identity_tests(){
    const uint32_t units[]={0,4096};
    for(size_t i=0;i<2;++i){
        FirmwareSet set;set.data_policy=EJODIN4_CLEAN_INSTALL;PlanImage first;first.decoded_bytes=1;first.expanded_bytes=8192;first.route.identifier=1;first.route.device_type=8;std::strcpy(first.route.partition_name,"USERDATA");std::strcpy(first.route.file_name,"userdata.img");
        PlanImage second=first;second.route.identifier=2;std::strcpy(second.route.partition_name,"USER_DATA");std::strcpy(second.route.file_name,"user_data.img");set.images.push_back(first);set.images.push_back(second);std::vector<EJODIN4_PitEntry> live;live.push_back(first.route);live.push_back(second.route);std::string error;
        check(set.validate_device_pit(live,units[i],error,8192)==EJODIN4_PIT_MAPPING_ERROR&&error.find("ambiguous")!=std::string::npos,"one supplied capacity cannot approve distinct zero-count USERDATA alias IDs during whole-plan preflight");
        set.images[1]=first;error.clear();check(set.validate_device_pit(live,units[i],error,8192)==EJODIN4_OK,"repeated transfers to one USERDATA ID may reuse its verified capacity even with an unselected alias in PIT");
        set.images[1]=second;set.images[0].route.block_count=2;set.images[1].route.block_count=2;live[0]=set.images[0].route;live[1]=set.images[1].route;error.clear();check(set.validate_device_pit(live,units[i],error)==EJODIN4_OK,"distinct positive alias extents keep independent PIT capacity checks when no scalar capacity is supplied");
        check(set.validate_device_pit(live,units[i],error,8192)==EJODIN4_PIT_MAPPING_ERROR&&error.find("ambiguous")!=std::string::npos,"a supplied scalar capacity remains identity-bound for positive alias extents");
    }
}
static unsigned destroyed=0;
class ChangedReader:public ImageReader {bool sent_;public:ChangedReader():sent_(false){}EJODIN4_Status read(uint8_t* out,uint32_t capacity,uint32_t& actual,const Cancellation&,std::string&){actual=0;if(!sent_&&capacity>=3){std::memcpy(out,"xyz",3);actual=3;sent_=true;}return EJODIN4_OK;}};
class ChangedArchive:public HeldArchive {public:~ChangedArchive(){++destroyed;}EJODIN4_Status verify_unchanged(std::string&)const{return EJODIN4_OK;}EJODIN4_Status open_member(size_t,std::unique_ptr<ImageReader>& reader,std::string&){reader.reset(new ChangedReader);return EJODIN4_OK;}};
static void second_pass_tests(){
    std::string error;Cancellation cancel={0,0};
    {FirmwareSet set;set.archives.push_back(new ChangedArchive);PlanImage image;image.decoded_bytes=3;const uint8_t expected[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};std::copy(expected,expected+32,image.digest.begin());set.images.push_back(image);std::unique_ptr<ImageReader> reader;check(set.open_image(0,reader,error)==EJODIN4_OK,"open changed second pass source");uint8_t out[4];uint32_t actual=0;check(reader->read(out,4,actual,cancel,error)==EJODIN4_OK&&actual==3,"changed source delivers same length");check(reader->read(out,4,actual,cancel,error)==EJODIN4_CHECKSUM_MISMATCH,"changed source SHA fails before validated EOF");reader.reset();}
    check(destroyed==1,"FirmwareSet owns and destroys held archives exactly once");
}
Bytes encoded_frame(const std::vector<Bytes>& blocks,bool checksums=true,unsigned block_id=4,bool independent=true,bool sized=true,bool dictionary=false,unsigned checksum_mode=3){
    uint64_t bytes=0;Bytes content;for(size_t bi=0;bi<blocks.size();++bi){const Bytes& block=blocks[bi];bytes+=block.size();content.insert(content.end(),block.begin(),block.end());}
    const uint8_t header[]={4,0x22,0x4d,0x18,static_cast<unsigned char>(0x40|(independent?0x20:0)|(sized?8:0)|(checksums?((checksum_mode&1?4:0)|(checksum_mode&2?16:0)):0)|(dictionary?1:0)),static_cast<unsigned char>(block_id<<4)};Bytes frame(header,header+6);
    if(sized)for(unsigned i=0;i<8;++i)frame.push_back(static_cast<unsigned char>(bytes>>(8*i)));
    if(dictionary)frame.insert(frame.end(),4,0);
    frame.push_back(static_cast<unsigned char>(XXH32(&frame[0]+4,frame.size()-4,0)>>8));
    for(size_t i=0;i<blocks.size();++i){
        Bytes payload(LZ4_compressBound(static_cast<int>(blocks[i].size())));
        int n=LZ4_compress_default(reinterpret_cast<const char*>(&blocks[i][0]),reinterpret_cast<char*>(&payload[0]),static_cast<int>(blocks[i].size()),static_cast<int>(payload.size()));
        check(n>0,"record fixture compress");payload.resize(static_cast<size_t>(n));
        const bool raw=(i%2)==0;if(raw)payload=blocks[i];size_t at=frame.size();frame.resize(at+4);le32(frame,at,static_cast<uint32_t>(payload.size())|(raw?UINT32_C(0x80000000):0));
        frame.insert(frame.end(),payload.begin(),payload.end());if(checksums&&(checksum_mode&2)){at=frame.size();frame.resize(at+4);le32(frame,at,XXH32(&payload[0],payload.size(),0));}
    }
    frame.insert(frame.end(),4,0);if(checksums&&(checksum_mode&1)){size_t at=frame.size();frame.resize(at+4);le32(frame,at,XXH32(&content[0],content.size(),0));}return frame;
}
static EJODIN4_Status prepare_encoded(Temp& temp,const Bytes& frame,std::unique_ptr<FirmwareSet>& set,std::string& error){
 Bytes tar;append(tar,"boot.img.lz4",frame);append(tar,"layout.pit",pit());std::string p=temp.write(tar);
 EJODIN4_FirmwareFile file={EJODIN4_PACKAGE_AP,p.c_str(),uint32_t(p.size()+1)};Cancellation c={0,0};
 return prepare_firmware_set(&file,1,EJODIN4_UPDATE_SELECTED,UINT64_C(64)*1024*1024,c,set,error);
}
struct EncodedProgress{uint64_t value;EncodedProgress():value(0){}static void report(void* context,uint64_t n,uint64_t total){check(n<=total,"encoded preflight bounded progress");static_cast<EncodedProgress*>(context)->value=n;}};
static void encoded_reader_tests(){
 Temp temp;std::string error;Cancellation none={0,0},cancel={stop,0};std::vector<Bytes> blocks(482,Bytes(65536,'A'));blocks.back().resize(13);Bytes frame=encoded_frame(blocks);std::unique_ptr<FirmwareSet> set;std::unique_ptr<EncodedImageReader> reader;
 check(prepare_encoded(temp,frame,set,error)==EJODIN4_OK,error.c_str());EncodedProgress progress;
 check(set->open_encoded_image(0,reader,error,none,ArchiveScanCallback(&EncodedProgress::report,&progress))==EJODIN4_OK,error.c_str());check(progress.value==set->images[0].decoded_bytes,"complete SHA preflight precedes reader publication");
 Bytes joined,records;uint32_t count=0;uint64_t decoded=0;unsigned sequences=0;
 do{check(reader->read_sequence(30*1024*1024,records,count,none,error)==EJODIN4_OK,error.c_str());check(count<=30*1024*1024&&records.size()<=30*1024*1024+4096,"bounded encoded sequence");if(count){++sequences;decoded+=count;joined.insert(joined.end(),records.begin(),records.end());}}while(count);
 check(sequences==2&&decoded==set->images[0].decoded_bytes,"two decoded sequences preserve totals");check(joined==Bytes(frame.begin()+15,frame.end()-8),"original mixed raw/compressed records include checksums and exclude frame envelope");
 check(reader->read_sequence(30*1024*1024,records,count,none,error)==EJODIN4_OK&&count==0&&records.empty(),"repeat validated encoded EOF");reader.reset();
 set->images[0].digest[0]^=1;check(set->open_encoded_image(0,reader,error)==EJODIN4_CHECKSUM_MISMATCH&&!reader.get(),"encoded preflight rejects changed SHA");set->images[0].digest[0]^=1;
 ++set->images[0].decoded_bytes;check(set->open_encoded_image(0,reader,error)==EJODIN4_CHECKSUM_MISMATCH&&!reader.get(),"encoded preflight rejects mismatched planned size");--set->images[0].decoded_bytes;
 check(set->open_encoded_image(0,reader,error,cancel)==EJODIN4_CANCELLED&&!reader.get(),"encoded preflight cancellation");check(set->open_encoded_image(0,reader,error)==EJODIN4_OK,"reopen encoded reader");
 check(reader->read_sequence(30*1024*1024,records,count,cancel,error)==EJODIN4_CANCELLED&&records.empty()&&count==0,"cancelled encoded sequence publishes no partial records");reader.reset();set.reset();
 for(unsigned mode=0;mode<4;++mode){std::vector<Bytes> small(1,Bytes(65536,'x'));small.push_back(Bytes(10,'z'));check(prepare_encoded(temp,encoded_frame(small,true,4,true,true,false,mode),set,error)==EJODIN4_OK,"prepare optional checksum profile");check(set->open_encoded_image(0,reader,error)==EJODIN4_OK,"optional checksums encoded reader");check(reader->read_sequence(30*1024*1024,records,count,none,error)==EJODIN4_OK&&count==65546,"optional checksum decoded totals");reader.reset();set.reset();}
 for(unsigned profile=0;profile<5;++profile){std::vector<Bytes> small(1,Bytes(3,'x'));if(profile==4)small.push_back(Bytes(3,'y'));Bytes unsupported=encoded_frame(small,true,profile==0?7:4,profile!=1,profile!=2,profile==3);check(prepare_encoded(temp,unsupported,set,error)==EJODIN4_OK,error.c_str());check(set->open_encoded_image(0,reader,error)==EJODIN4_DRIVER_UNSUPPORTED&&!reader.get(),"fully valid unsupported profile requests host decode before writes");set.reset();}
 for(unsigned id=4;id<=6;++id){std::vector<Bytes> small(1,Bytes(1u<<(8+2*id),'x'));check(prepare_encoded(temp,encoded_frame(small,true,id),set,error)==EJODIN4_OK,"prepare native block profile");check(set->open_encoded_image(0,reader,error)==EJODIN4_OK,"64/256/1024KiB native profiles");check(reader->read_sequence(1,records,count,none,error)==EJODIN4_INVALID_ARGUMENT&&count==0&&records.empty(),"too-small sequence publishes no records");reader.reset();set.reset();}
 for(unsigned profile=4;profile<=7;profile+=3){std::vector<Bytes> small(1,Bytes(65536,'x'));small.push_back(Bytes(3,'y'));Bytes valid=encoded_frame(small,true,profile);
 for(unsigned damage=0;damage<5;++damage){Bytes broken=valid;if(damage==0)broken[14]^=1;else if(damage==1)broken[20]^=1;else if(damage==2)broken.back()^=1;else if(damage==3)broken.pop_back();else broken.push_back(0);Bytes tar;append(tar,"boot.img.lz4",broken);std::string p=temp.write(tar);EJODIN4_FirmwareFile file={2,p.c_str(),uint32_t(p.size()+1)};std::unique_ptr<HeldArchive> held;check(open_held_archive(file,none,held,error)==EJODIN4_OK,"held corrupt frame fixture");PlanImage planned;planned.decoded_bytes=65539;EJODIN4_Status result=held->open_encoded_member(0,planned,reader,error);check(result!=EJODIN4_OK&&result!=EJODIN4_DRIVER_UNSUPPORTED&&!reader.get(),"corrupt header/block/footer/truncated/trailing frame cannot request fallback");}}
}


static void authenticated_archive_tests(){
 Temp temp;std::string error;Cancellation none={0,0},cancel={stop,0};
 for(unsigned mode=0;mode<5;++mode){Bytes tar;append(tar,"boot.img",bytes("abc"));append(tar,"layout.pit",pit());
 if(mode==1){append(tar,"meta-data/one.mf",Bytes(300,0));append(tar,"meta-data/two.mf",Bytes(300,0));}
 if(mode==2)append(tar,"meta-data/signed.mf",Bytes(256,0));
 if(mode==3)append(tar,"meta-data/signed.mf",Bytes(300,0));
 if(mode==4)append(tar,"meta-data/signed.mf.lz4",encoded_frame(std::vector<Bytes>(1,bytes("opaque metadata"))));
 std::string p=temp.write(tar);EJODIN4_FirmwareFile file={EJODIN4_PACKAGE_AP,p.c_str(),uint32_t(p.size()+1)};std::unique_ptr<FirmwareSet> set;
 check(prepare_firmware_set(&file,1,EJODIN4_UPDATE_SELECTED,1048576,none,set,error)==EJODIN4_OK,"prepare signed metadata negative archive");
 FirmwareIdentity identity;EJODIN4_Status s=set->authenticate_identity(identity,none);check(s!=EJODIN4_OK&&!identity.present&&!identity.diagnostic.empty()&&identity.status==s,"missing ambiguous malformed or invalid signature metadata cannot authenticate archive");
 if(mode==0)check(s==EJODIN4_DRIVER_UNSUPPORTED,"legacy unsigned package retains missing proof status");if(mode==1)check(s==EJODIN4_UNSUPPORTED_ARCHIVE,"multiple signed manifests reject ambiguity");if(mode==2)check(s==EJODIN4_INVALID_ARCHIVE,"signed manifest must exceed RSA signature prefix");if(mode==3)check(s==EJODIN4_CHECKSUM_MISMATCH,"invalid RSA signature cannot authenticate archive");
 }
 std::vector<Bytes> blocks(1,Bytes(1000,'a'));Bytes frame=encoded_frame(blocks);std::unique_ptr<FirmwareSet> set;check(prepare_encoded(temp,frame,set,error)==EJODIN4_OK,"prepare stored compressed member audit");std::unique_ptr<ImageReader> stored;check(set->archives[0]->open_stored_member(set->images[0].member_index,stored,error)==EJODIN4_OK,"open exact stored bytes independent of decode flag");uint8_t header[4]={0};uint32_t actual=0;check(stored->read(header,4,actual,none,error)==EJODIN4_OK&&actual==4&&header[0]==4&&header[1]==0x22&&header[2]==0x4d&&header[3]==0x18,"signed archive audit hashes compressed frame envelope rather than decoded image");check(stored->read(header,4,actual,cancel,error)==EJODIN4_CANCELLED&&actual==0,"stored identity reader cancellation exposes no bytes");
}

int main(int argc,char** argv){try{authenticated_archive_tests();encoded_reader_tests();sparse_tests();planner_tests();second_pass_tests();layout_tests(argc>1?argv[1]:0);generic_capacity_tests();userdata_capacity_identity_tests();std::cout<<"native streaming: "<<checks<<" checks passed"<<std::endl;return 0;}catch(const std::exception& error){std::cerr<<"native streaming failed after "<<checks<<" checks: "<<error.what()<<std::endl;return 1;}}
