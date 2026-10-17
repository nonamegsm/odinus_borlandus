#include "Odin4DeviceInfo.h"
#include <cstdio>
#include <stdexcept>
using namespace ejodin4;
namespace {
unsigned checks=0;void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
void put(std::vector<uint8_t>& b,size_t n,uint32_t v){for(unsigned i=0;i<4;++i)b[n+i]=uint8_t(v>>(8*i));}
std::vector<uint8_t> info(bool carrier=true){std::vector<uint8_t>b(500,0);put(b,0,0x12345678);put(b,4,carrier?2:1);put(b,8,0);put(b,12,48);std::memcpy(&b[56],"SM-X123B",8);if(carrier){put(b,20,3);put(b,24,80);std::memcpy(&b[88],"PLS",3);}return b;}
}
int main(){try{DeviceInfo d;std::string error;std::vector<uint8_t>b=info();
 check(parse_device_info(b,4,d,error)==EJODIN4_OK&&d.model=="SM-X123B"&&d.carrier=="PLS","v4 bounded model/carrier payload");
 check(parse_device_info(b,5,d,error)==EJODIN4_OK&&d.carrier_present,"v5 required tags");
 put(b,16,0xdeadbeef);check(parse_device_info(b,5,d,error)==EJODIN4_OK,"third record word remains opaque");
 b=info(false);check(parse_device_info(b,4,d,error)==EJODIN4_OK,"v4 model alone");check(parse_device_info(b,5,d,error)==EJODIN4_INVALID_RESPONSE,"v5 missing carrier rejects");
 b=info();put(b,8,3);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"missing model/duplicate carrier rejects");
 b=info();put(b,4,257);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"bounded record count");
 b=info();put(b,4,0);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"empty table rejects");
 b=info();put(b,0,1);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"magic mismatch");
 b=info();b.pop_back();check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"size must preserve 500-byte records");
 b=info();put(b,12,0);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"identity cannot point into its table");
 b=info();put(b,12,0xffffffff);check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"offset overflow rejects");
 b=info();put(b,12,490);for(size_t n=498;n<b.size();++n)b[n]='A';check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"unterminated model rejects");
 b=info();b[56]=0;check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"empty model rejects");
 b=info();b[56]=0x80;check(parse_device_info(b,4,d,error)==EJODIN4_INVALID_RESPONSE,"model encoding is explicit");
 b=info();put(b,4,3);put(b,32,99);put(b,36,0xffffffff);put(b,40,0xdeadbeef);check(parse_device_info(b,5,d,error)==EJODIN4_OK,"unknown tags stay opaque and are not identity authority");
 FirmwareIdentity f;check(validate_device_identity(d,f,5,error)==EJODIN4_DRIVER_UNSUPPORTED,"unknown package identity cannot authorize writes");
 f.present=true;f.model="SM-X123B_BUILD";check(validate_device_identity(d,f,5,error)==EJODIN4_OK,"matching model uses native underscore suffix convention");
 f.model="SM-OTHER";check(validate_device_identity(d,f,5,error)==EJODIN4_PIT_MAPPING_ERROR,"model mismatch rejects");f.model="SM-X123B";
 f.blocked_carriers="_ABC_PLS_EUX_";check(validate_device_identity(d,f,5,error)==EJODIN4_PIT_MAPPING_ERROR,"carrier block policy rejects");
 f.blocked_carriers="_ABC_PL_EUX_";check(validate_device_identity(d,f,5,error)==EJODIN4_OK,"carrier tokens do not partially match");
 std::printf("PASS DEVINFO parser/policy: %u checks; no device I/O\n",checks);return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
