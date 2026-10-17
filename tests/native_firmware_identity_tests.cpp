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
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace ejodin4;
namespace {
unsigned checks=0;
void check(bool good,const char* what){++checks;if(!good)throw std::runtime_error(what);}
std::string payload(){return "2\nSM-ANY-MODEL\nboot.img "+std::string(64,'a')+"\nsub/vendor_boot.img.lz4 "+std::string(64,'B')+"\n";}
SignedFirmwareManifest sentinel(){SignedFirmwareManifest out;out.model="unchanged";out.blocked_carriers="_KEEP_";out.member_sha256["keep"]="keep";return out;}
bool untouched(const SignedFirmwareManifest& out){return out.model=="unchanged"&&out.blocked_carriers=="_KEEP_"&&out.member_sha256.size()==1&&out.member_sha256.count("keep")==1;}
void reject(const std::string& text){SignedFirmwareManifest out=sentinel();std::string error;check(firmware_identity_detail::parse_authenticated_manifest(text,out,error)==EJODIN4_INVALID_ARCHIVE,"malformed authenticated payload rejected");check(untouched(out)&&!error.empty(),"malformed payload cannot partially assign identity");}
struct TestKey {
    HCRYPTPROV provider;HCRYPTKEY key;std::vector<uint8_t> public_blob;
    TestKey():provider(0),key(0){
        check(CryptAcquireContextW(&provider,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=0,"test crypto provider");
        check(CryptGenKey(provider,AT_SIGNATURE,(2048u<<16)|CRYPT_EXPORTABLE,&key)!=0,"generate independent RSA-2048 test key");
        DWORD size=0;check(CryptExportKey(key,0,PUBLICKEYBLOB,0,0,&size)!=0,"public blob size");public_blob.resize(size);
        check(CryptExportKey(key,0,PUBLICKEYBLOB,0,&public_blob[0],&size)!=0,"export independent public key");
    }
    ~TestKey(){if(key)CryptDestroyKey(key);if(provider)CryptReleaseContext(provider,0);}
    std::vector<uint8_t> sign(const std::string& text){
        HCRYPTHASH hash=0;check(CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=0,"test SHA-256");
        check(CryptHashData(hash,reinterpret_cast<BYTE*>(const_cast<char*>(text.data())),DWORD(text.size()),0)!=0,"hash exact test payload");
        DWORD size=0;check(CryptSignHashA(hash,AT_SIGNATURE,0,0,0,&size)!=0,"test signature size");std::vector<uint8_t> signature(size);
        check(CryptSignHashA(hash,AT_SIGNATURE,0,0,&signature[0],&size)!=0,"sign test payload");CryptDestroyHash(hash);check(size==256,"RSA-2048 prefix size");std::reverse(signature.begin(),signature.end());return signature;
    }
    EJODIN4_Status verify(const std::vector<uint8_t>& sig,const std::string& data,std::string& error){return firmware_identity_detail::verify_rsa_pkcs1_sha256(public_blob,sig.empty()?0:&sig[0],sig.size(),reinterpret_cast<const uint8_t*>(data.data()),data.size(),error);}
};
}
int main(){try{
    SignedFirmwareManifest out;std::string error;std::string text=payload();
    check(firmware_identity_detail::parse_authenticated_manifest(text,out,error)==EJODIN4_OK,"valid version-one implicit manifest");
    check(out.model=="SM-ANY-MODEL"&&out.member_sha256.size()==2&&out.member_sha256["sub/vendor_boot.img.lz4"]==std::string(64,'b')&&out.blocked_carriers.empty(),"model paths and digest normalization");
    check(firmware_identity_detail::parse_authenticated_manifest(text+"VERSION:2\nBLKLIST:_PLS_EUX_\n",out,error)==EJODIN4_OK&&out.blocked_carriers=="_PLS_EUX_","native raw carrier blocklist preserved");
    check(firmware_identity_detail::parse_authenticated_manifest(text+"VERSION:3\n",out,error)==EJODIN4_OK&&out.blocked_carriers.empty(),"optional blocklist and replacement clear stale state");
    check(firmware_identity_detail::parse_authenticated_manifest(text+"VERSION:1",out,error)==EJODIN4_OK,"final line need not have newline");
    std::string crlf;for(size_t i=0;i<text.size();++i){char c=text[i];if(c=='\n')crlf+='\r';crlf+=c;}check(firmware_identity_detail::parse_authenticated_manifest(crlf,out,error)==EJODIN4_OK,"CRLF accepted without altering authenticated bytes");
    reject("");reject("0\nM\n");reject("101\nM\n");reject("4294967296\nM\n");reject("-1\nM\n");reject("1 extra\nM\n");reject("1\n\n");reject("1\n"+std::string(30,'M')+"\n");reject("1\n M\n");reject("1\nM\tX\n");
    reject("2\nM\nboot.img "+std::string(64,'a')+"\n");
    const std::string hash(64,'a');
    const char* unsafe[]={"/boot.img","../boot.img","a/../boot.img","./boot.img","a//boot.img","a/","C:/boot.img","a\\boot.img","a:b","a*b","a?b","a|b"};
    for(size_t i=0;i<sizeof(unsafe)/sizeof(unsafe[0]);++i)reject("1\nM\n"+std::string(unsafe[i])+" "+hash+"\n");reject("1\nM\n"+std::string(256,'x')+" "+hash+"\n");
    reject("1\nM\nboot.img "+std::string(63,'a')+"\n");reject("1\nM\nboot.img "+std::string(64,'g')+"\n");reject("1\nM\nboot.img "+hash+" unexpected\n");
    reject("2\nM\nboot.img "+hash+"\nboot.img "+hash+"\n");
    const char* tails[]={"\n","UNKNOWN:x\n","VERSION:0\n","VERSION:4294967296\n","VERSION:2x\n","VERSION:1\nBLKLIST:_A_\n","VERSION:2\nUNKNOWN:x\n","VERSION:2\nBLKLIST:_A_\nEXTRA:x\n","VERSION:2\nBLKLIST:A\tB\n"};
    for(size_t i=0;i<sizeof(tails)/sizeof(tails[0]);++i)reject(text+tails[i]);reject(text+"VERSION:2\nBLKLIST:"+std::string(4097,'A')+"\n");
    std::string damaged=text;damaged[4]='\0';reject(damaged);damaged=text;std::memset(&damaged[4],0x80,1);reject(damaged);damaged=text;damaged[4]='\r';reject(damaged);reject(std::string(128*1024,'x'));
    std::string hundred="100\nM\n";for(unsigned i=0;i<100;++i){std::ostringstream n;n<<i;hundred+="image"+n.str()+".bin "+hash+"\n";}
    check(firmware_identity_detail::parse_authenticated_manifest(hundred,out,error)==EJODIN4_OK&&out.member_sha256.size()==100,"maximum member count accepted");
    TestKey key;std::vector<uint8_t> signature=key.sign(text);
    check(key.verify(signature,text,error)==EJODIN4_OK,"real CryptoAPI verification of big-endian RSA/SHA256 signature");
    std::vector<uint8_t> changed=signature;changed[0]^=1;check(key.verify(changed,text,error)==EJODIN4_CHECKSUM_MISMATCH,"tampered signature rejected");
    damaged=text;damaged[damaged.size()-3]^=1;check(key.verify(signature,damaged,error)==EJODIN4_CHECKSUM_MISMATCH,"tampered authenticated payload rejected");
    check(key.verify(signature,crlf,error)==EJODIN4_CHECKSUM_MISMATCH,"newline normalization before verification is forbidden");
    check(key.verify(signature,text+"\n",error)==EJODIN4_CHECKSUM_MISMATCH,"trailing byte is authenticated");
    changed=signature;std::reverse(changed.begin(),changed.end());check(key.verify(changed,text,error)==EJODIN4_CHECKSUM_MISMATCH,"little-endian package signature rejected");
    changed.pop_back();check(key.verify(changed,text,error)==EJODIN4_INVALID_ARGUMENT,"short RSA prefix rejected");
    std::vector<uint8_t> envelope=signature;envelope.insert(envelope.end(),text.begin(),text.end());out=sentinel();
    check(verify_signed_manifest(envelope,out,error)==EJODIN4_CHECKSUM_MISMATCH&&untouched(out),"production trust rejects independently generated test key despite valid generic verification");
    const size_t sizes[]={0,255,256,128*1024};for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){envelope.assign(sizes[i],0);out=sentinel();check(verify_signed_manifest(envelope,out,error)==EJODIN4_INVALID_ARCHIVE&&untouched(out),"invalid envelope size rejected without output mutation");}
    std::cout<<"Signed manifest tests passed: "<<checks<<" checks; no manufacturer-private-key fixture is available.\n";return 0;
}catch(const std::exception& e){std::cerr<<"Signed manifest test failure: "<<e.what()<<"\n";return 1;}}
