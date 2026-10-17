/* Classic BCC32 engine regression tests. Synthetic files only: no USB I/O. */
#define NOMINMAX
#include "Odin4FirmwareStream.h"
#include "Odin4LayoutProfile.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
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
static void layout_tests(const char* path){
    FirmwareSet firmware;LayoutResolution result;std::string error;Bytes empty;
    check(result.sector_bytes==0&&result.userdata_lower_bound_bytes==0&&result.source==0,"layout defaults remain zero/null");
    check(resolve_layout_profile(firmware,empty,0,0,0,result,error)==EJODIN4_PIT_MAPPING_ERROR&&result.sector_bytes==0,"unknown layout cannot infer geometry");
    const uint32_t units[]={512,1024,2048,4096};for(size_t i=0;i<4;++i){error.clear();check(resolve_layout_profile(firmware,empty,0,0,units[i],result,error)==EJODIN4_OK&&result.sector_bytes==units[i]&&result.userdata_lower_bound_bytes==0,"explicit independently verified sector unit");}
    check(resolve_layout_profile(firmware,empty,0,0,8192,result,error)==EJODIN4_INVALID_ARGUMENT&&result.sector_bytes==0&&result.source==0,"invalid caller geometry clears previous resolution");
    if(!path||!*path){std::cout<<"native layout: reviewed PIT fixture not supplied; synthetic/default cases only"<<std::endl;return;}
    std::FILE* file=std::fopen(path,"rb");check(file!=0,"open reviewed real PIT fixture");Bytes table;uint8_t block[1024];size_t actual=0;while((actual=std::fread(block,1,sizeof(block),file))!=0)table.insert(table.end(),block,block+actual);std::fclose(file);check(table.size()==7280,"reviewed PIT byte size");
    std::vector<EJODIN4_PitEntry> rows(512);uint32_t count=0;char diagnostic[512]={0};check(EJODIN4_ParsePit(&table[0],uint32_t(table.size()),&rows[0],512,&count,diagnostic,sizeof(diagnostic))==EJODIN4_OK,"parse reviewed PIT");rows.resize(count);
    firmware.data_policy=EJODIN4_UPDATE_SELECTED;firmware.package_pit_bytes=table;PlanImage image;bool found=false;for(size_t i=0;i<rows.size();++i)if(std::strncmp(rows[i].partition_name,"BOOT",32)==0){image.route=rows[i];found=true;break;}check(found,"reviewed BOOT anchor");image.role=EJODIN4_PACKAGE_AP;image.name="boot.img";image.decoded_bytes=67108864;image.expanded_bytes=uint64_t(image.route.block_count)*4096;firmware.images.push_back(image);
    const char* names[]={"BL_S921BXXS9BYH2_test.tar.md5","AP_S921BXXS9BYH2_test.tar.md5","CP_S921BXXS9BYH2_test.tar.md5","CSC_OXM_S921BOXM9BYH2_test.tar.md5"};EJODIN4_FirmwareFile files[4];for(size_t i=0;i<4;++i){files[i].role=uint32_t(i+1);files[i].path=names[i];files[i].path_bytes=uint32_t(std::strlen(names[i])+1);}
    error.clear();check(resolve_layout_profile(firmware,table,files+1,1,0,result,error)==EJODIN4_OK&&result.sector_bytes==4096&&result.userdata_lower_bound_bytes==0,"pinned real PIT selected layout resolves to 4096 without USERDATA inference");
    Bytes live=table;live.resize(live.size()+12,0xa5);error.clear();check(same_pit_layout(table,live)&&resolve_layout_profile(firmware,live,files+1,1,0,result,error)==EJODIN4_OK,"opaque live PIT suffix excluded from layout identity");
    live[28+24]^=1;error.clear();check(resolve_layout_profile(firmware,live,files+1,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR&&result.sector_bytes==0,"tampered live partition geometry rejected");
    firmware.package_pit_bytes[firmware.package_pit_bytes.size()-1]^=1;error.clear();check(resolve_layout_profile(firmware,table,files+1,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"entire reviewed package PIT fingerprint retained");firmware.package_pit_bytes=table;
    firmware.images[0].expanded_bytes+=4096;error.clear();check(resolve_layout_profile(firmware,table,files+1,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"anchor logical extent mismatch rejected");firmware.images[0].expanded_bytes-=4096;
    EJODIN4_FirmwareFile wrong=files[1];wrong.path="AP_OTHER.tar.md5";wrong.path_bytes=uint32_t(std::strlen(wrong.path)+1);error.clear();check(resolve_layout_profile(firmware,table,&wrong,1,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"unreviewed firmware build rejected");
    const char* anchors[]={"SUPER","BOOT","CACHE","PRISM","OPTICS","VENDOR_BOOT"};firmware.images.clear();for(size_t anchor=0;anchor<6;++anchor){found=false;for(size_t i=0;i<rows.size();++i)if(std::strncmp(rows[i].partition_name,anchors[anchor],32)==0){image=PlanImage();image.route=rows[i];image.role=EJODIN4_PACKAGE_AP;image.name=rows[i].file_name;image.expanded_bytes=uint64_t(rows[i].block_count)*4096;image.decoded_bytes=1;if(anchor==5)image.sha256="9fb4b7b4c0e132d9a95cca9641c8f34434612838e6427eb5e04f35077c2a975d";firmware.images.push_back(image);found=true;break;}check(found,"reviewed full layout anchor found");}
    firmware.data_policy=EJODIN4_PRESERVE_DATA;error.clear();check(resolve_layout_profile(firmware,table,files,4,0,result,error)==EJODIN4_OK&&result.sector_bytes==4096&&result.userdata_lower_bound_bytes==(uint64_t(64)<<30),"full reviewed layout keeps exact 64GiB USERDATA lower bound on BCC");
    firmware.images[5].sha256="wrong";error.clear();check(resolve_layout_profile(firmware,table,files,4,0,result,error)==EJODIN4_PIT_MAPPING_ERROR,"reviewed VENDOR_BOOT mount configuration digest required");
}
static unsigned destroyed=0;
class ChangedReader:public ImageReader {bool sent_;public:ChangedReader():sent_(false){}EJODIN4_Status read(uint8_t* out,uint32_t capacity,uint32_t& actual,const Cancellation&,std::string&){actual=0;if(!sent_&&capacity>=3){std::memcpy(out,"xyz",3);actual=3;sent_=true;}return EJODIN4_OK;}};
class ChangedArchive:public HeldArchive {public:~ChangedArchive(){++destroyed;}EJODIN4_Status verify_unchanged(std::string&)const{return EJODIN4_OK;}EJODIN4_Status open_member(size_t,std::unique_ptr<ImageReader>& reader,std::string&){reader.reset(new ChangedReader);return EJODIN4_OK;}};
static void second_pass_tests(){
    std::string error;Cancellation cancel={0,0};
    {FirmwareSet set;set.archives.push_back(new ChangedArchive);PlanImage image;image.decoded_bytes=3;const uint8_t expected[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};std::copy(expected,expected+32,image.digest.begin());set.images.push_back(image);std::unique_ptr<ImageReader> reader;check(set.open_image(0,reader,error)==EJODIN4_OK,"open changed second pass source");uint8_t out[4];uint32_t actual=0;check(reader->read(out,4,actual,cancel,error)==EJODIN4_OK&&actual==3,"changed source delivers same length");check(reader->read(out,4,actual,cancel,error)==EJODIN4_CHECKSUM_MISMATCH,"changed source SHA fails before validated EOF");reader.reset();}
    check(destroyed==1,"FirmwareSet owns and destroys held archives exactly once");
}
int main(int argc,char** argv){try{sparse_tests();planner_tests();second_pass_tests();layout_tests(argc>1?argv[1]:0);std::cout<<"native streaming: "<<checks<<" checks passed"<<std::endl;return 0;}catch(const std::exception& error){std::cerr<<"native streaming failed after "<<checks<<" checks: "<<error.what()<<std::endl;return 1;}}
