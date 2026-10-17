#include "Odin4FirmwareIdentity.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
namespace ejodin4 {
namespace {
const size_t signature_size=256, maximum_manifest_size=128*1024;
// RSA moduli decoded from the two SPKI public keys in native Odin 3.14.4
// constructor 0x00409D50. Both observed public exponents are 65537.
const char* const trusted_moduli[] = {
    "ac065c6170524528540f618839c63405e3ec44b9a62a0452f4795f237e1b762479ee353e17628f549e2910f3ff0ac7f5f2bbbe8cee7aec1761c57d8e5f59a4be899a2483a1fe5490f70da84ab3f253a160d7aa3568b78ceeb798dd57df727197b3dd1fd8089f743d887cebb92282516dc96fa16e0bc7b165a063f4fcad26fe34280bc648780b183edaa53358b69bbc16c2c2b982ff98632512f813b14f659893ea1c06265074d743a985315c72a73073051f5347954a121e7dc4fed7573e2fc7e5f61748686bf5b3b2c34f151e653e710e3433952aaec0a8dd9e2d7748344eb4a017eec1d507dd0d442dffe4e24b2cddb1c543dc03d22d068046ae760ce8daef",
    "b944920ef7a2a7ffe76e20aa8d4240d45a09d3b9b5a90b9c779153448bde7a6cd9642d0e1a27a9144837feb936e9716684537b77b905efef30a2df70613869c03eb061842e9ab0717eced686a7bc8315d64f10a55ba3532b6e42da889630da9a0d298947a3fdc122676dbef572505a0b32da149b0cc6208be0d99a6519c9aef9ad4f52cfdcbe381732d7351eeb5565311a3fcfc98cb75cd8bb8afc3673f8926b0479d468aeec0da8c4a60eaa16816d187e498c069e55a941d11d89b0ff6f7c3163820d119d18aa75774caf0e182c1f56e3e063574ed1a4452ca488ac11c0457d5501dbce9337d948ac6de2c8e230feee100d81cc1533ae38745df89891abffb9"
};
unsigned nibble(char c) { return c>='0'&&c<='9'?unsigned(c-'0'):unsigned(c-'a'+10); }
std::vector<uint8_t> trusted_blob(const char* modulus) {
    std::vector<uint8_t> bytes(sizeof(BLOBHEADER)+sizeof(RSAPUBKEY)+signature_size,0);
    BLOBHEADER header={PUBLICKEYBLOB,CUR_BLOB_VERSION,0,CALG_RSA_SIGN};
    RSAPUBKEY rsa={0x31415352,2048,65537};
    std::memcpy(&bytes[0],&header,sizeof(header));
    std::memcpy(&bytes[0]+sizeof(header),&rsa,sizeof(rsa));
    for(size_t i=0;i<signature_size;++i) {
        const size_t j=(signature_size-1-i)*2;
        bytes[sizeof(header)+sizeof(rsa)+i]=uint8_t(nibble(modulus[j])*16+nibble(modulus[j+1]));
    }
    return bytes;
}
bool number(const std::string& text,uint32_t& value) {
    if(text.empty())return false;
    uint32_t parsed=0;
    for(size_t i=0;i<text.size();++i) {
        const char c=text[i];
        if(c<'0'||c>'9'||parsed>(std::numeric_limits<uint32_t>::max()-unsigned(c-'0'))/10)return false;
        parsed=parsed*10+unsigned(c-'0');
    }
    value=parsed;return true;
}
std::string trim(const std::string& text) {
    const size_t begin=text.find_first_not_of(" \t");
    return begin==std::string::npos?std::string():text.substr(begin,text.find_last_not_of(" \t")-begin+1);
}
bool safe_name(const std::string& name) {
    if(name.empty()||name.size()>255||name[0]=='/'||name[name.size()-1]=='/')return false;
    size_t begin=0;
    for(size_t i=0;i<=name.size();++i) {
        if(i==name.size()||name[i]=='/') {
            const std::string part=name.substr(begin,i-begin);
            if(part.empty()||part=="."||part=="..")return false;
            begin=i+1;
        } else {
            const unsigned char c=static_cast<unsigned char>(name[i]);
            if(c<=32||c>=127||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
        }
    }
    return true;
}
bool digest(std::string& hash) {
    if(hash.size()!=64)return false;
    for(size_t i=0;i<hash.size();++i) {
        char& c=hash[i];
        if(c>='A'&&c<='F')c=char(c-'A'+'a');
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    }
    return true;
}
bool next_line(std::istringstream& input,std::string& line) {
    if(!std::getline(static_cast<std::istream&>(input),line))return false;
    if(!line.empty()&&line[line.size()-1]=='\r')line.resize(line.size()-1);
    return true;
}
EJODIN4_Status malformed(std::string& error,const char* text) {
    error=text;return EJODIN4_INVALID_ARCHIVE;
}
}
namespace firmware_identity_detail {
EJODIN4_Status verify_rsa_pkcs1_sha256(const std::vector<uint8_t>& public_key_blob,
    const uint8_t* signature_be,size_t signature_bytes,const uint8_t* payload,
    size_t payload_bytes,std::string& error) {
    if(!signature_be||signature_bytes!=signature_size||!payload||!payload_bytes||
       payload_bytes>=maximum_manifest_size||public_key_blob.empty()||
       public_key_blob.size()>std::numeric_limits<DWORD>::max()) {
        error="Invalid signed manifest verification input";return EJODIN4_INVALID_ARGUMENT;
    }
    HCRYPTPROV provider=0;HCRYPTKEY key=0;HCRYPTHASH hash=0;
    EJODIN4_Status result=EJODIN4_INTERNAL_ERROR;
    if(!CryptAcquireContextW(&provider,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)) {
        error="Cannot acquire signed manifest SHA-256 provider";return result;
    }
    if(!CryptImportKey(provider,const_cast<BYTE*>(&public_key_blob[0]),DWORD(public_key_blob.size()),0,0,&key))
        error="Cannot import signed manifest RSA public key";
    else if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)||
            !CryptHashData(hash,const_cast<BYTE*>(payload),DWORD(payload_bytes),0))
        error="Cannot hash signed manifest payload";
    else {
        // CryptoAPI expects little-endian RSA signatures; the package prefix is big-endian.
        std::vector<uint8_t> signature(signature_be,signature_be+signature_bytes);
        std::reverse(signature.begin(),signature.end());
        if(CryptVerifySignatureA(hash,&signature[0],DWORD(signature.size()),key,0,0)) {
            error.clear();result=EJODIN4_OK;
        } else {error="Manufacturer signed manifest RSA/SHA-256 signature mismatch";result=EJODIN4_CHECKSUM_MISMATCH;}
    }
    if(hash)CryptDestroyHash(hash);
    if(key)CryptDestroyKey(key);
    CryptReleaseContext(provider,0);
    return result;
}
EJODIN4_Status parse_authenticated_manifest(const std::string& payload,
    SignedFirmwareManifest& output,std::string& error) {
    if(payload.empty()||payload.size()>=maximum_manifest_size-signature_size)
        return malformed(error,"Signed manifest payload size is invalid");
    for(size_t i=0;i<payload.size();++i) {
        const unsigned char c=static_cast<unsigned char>(payload[i]);
        if(c=='\r') {if(i+1==payload.size()||payload[i+1]!='\n')return malformed(error,"Signed manifest has a bare carriage return");}
        else if(c!='\n'&&c!='\t'&&(c<32||c>126))return malformed(error,"Signed manifest text must be bounded ASCII");
    }
    std::istringstream input(payload);std::string line;
    uint32_t count=0;
    if(!next_line(input,line)||!number(trim(line),count)||!count||count>100)
        return malformed(error,"Signed manifest member count must be 1 through 100");
    SignedFirmwareManifest parsed;
    if(!next_line(input,line)||line.empty()||line.size()>29||trim(line)!=line||line.find('\t')!=std::string::npos)
        return malformed(error,"Signed manifest model is invalid");
    parsed.model=line;
    for(uint32_t i=0;i<count;++i) {
        if(!next_line(input,line))return malformed(error,"Signed manifest member list is truncated");
        std::istringstream record(line);std::string name,hash,extra;
        if(!(record>>name>>hash)||(record>>extra)||!safe_name(name)||!digest(hash))
            return malformed(error,"Signed manifest member or SHA-256 is invalid");
        if(!parsed.member_sha256.insert(std::make_pair(name,hash)).second)
            return malformed(error,"Signed manifest repeats a member name");
    }
    if(next_line(input,line)) {
        uint32_t version=0;
        if(line.compare(0,8,"VERSION:")||!number(trim(line.substr(8)),version)||!version)
            return malformed(error,"Signed manifest version field is invalid");
        if(next_line(input,line)) {
            if(version<2||line.compare(0,8,"BLKLIST:")||line.size()-8>4096||line.find('\t')!=std::string::npos)
                return malformed(error,"Signed manifest carrier blocklist is invalid");
            // Keep the authenticated raw value: native Odin searches _CARRIER_ without padding it.
            parsed.blocked_carriers=line.substr(8);
            if(next_line(input,line))return malformed(error,"Signed manifest has unexpected trailing fields");
        }
    }
    output=parsed;error.clear();return EJODIN4_OK;
}
}
EJODIN4_Status verify_signed_manifest(const std::vector<uint8_t>& bytes,
    SignedFirmwareManifest& output,std::string& error) {
    if(bytes.size()<=signature_size||bytes.size()>=maximum_manifest_size)
        return malformed(error,"Signed manifest must contain a 256-byte signature and bounded payload");
    EJODIN4_Status status=EJODIN4_CHECKSUM_MISMATCH;
    for(size_t trusted=0;trusted<sizeof(trusted_moduli)/sizeof(trusted_moduli[0]);++trusted) {
        const char* modulus=trusted_moduli[trusted];
        status=firmware_identity_detail::verify_rsa_pkcs1_sha256(trusted_blob(modulus),
            &bytes[0],signature_size,&bytes[0]+signature_size,bytes.size()-signature_size,error);
        if(status==EJODIN4_OK) {
            const std::string payload(reinterpret_cast<const char*>(&bytes[0]+signature_size),bytes.size()-signature_size);
            return firmware_identity_detail::parse_authenticated_manifest(payload,output,error);
        }
        if(status!=EJODIN4_CHECKSUM_MISMATCH)return status;
    }
    return status;
}
}
