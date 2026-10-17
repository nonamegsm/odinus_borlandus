/*
 * Copyright (c) 2026 Llucs
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ODIN4_BCC_TAR_MD5_TRAILER_H
#define ODIN4_BCC_TAR_MD5_TRAILER_H
#include <stddef.h>
#include <stdint.h>
namespace tar_md5 {
inline bool is_hex_char(unsigned char c) {return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}
struct TrailerPos {int64_t md5_pos,trailer_start;TrailerPos():md5_pos(-1),trailer_start(-1){}};
inline int64_t find_md5(const char* tail,size_t length,uint64_t offset,bool aligned){
    for(int64_t pos=static_cast<int64_t>(length)-32;pos>=0;--pos){
        if(aligned&&(offset+static_cast<uint64_t>(pos))%512!=0)continue;
        bool valid=true;for(int i=0;i<32;++i)if(!is_hex_char(static_cast<unsigned char>(tail[static_cast<size_t>(pos+i)]))){valid=false;break;}
        if(!valid)continue;
        if((pos==0||!is_hex_char(static_cast<unsigned char>(tail[static_cast<size_t>(pos-1)])))&&
           (static_cast<size_t>(pos+32)>=length||!is_hex_char(static_cast<unsigned char>(tail[static_cast<size_t>(pos+32)]))))return pos;
    }
    return -1;
}
// Syntax-only port of pinned Llucs locate_trailer; aligned candidates take
// precedence and fallback newline scans never cross the previous TAR block.
inline TrailerPos locate_trailer(const char* tail,size_t length,uint64_t offset){
    TrailerPos result;if(length<32)return result;
    bool aligned=true;result.md5_pos=find_md5(tail,length,offset,true);
    if(result.md5_pos<0){aligned=false;result.md5_pos=find_md5(tail,length,offset,false);}
    if(result.md5_pos<0)return result;
    result.trailer_start=result.md5_pos;
    if(!aligned){
        const uint64_t absolute=offset+static_cast<uint64_t>(result.md5_pos);
        const uint64_t boundary=absolute-absolute%512;
        const int64_t minimum=boundary>offset?static_cast<int64_t>(boundary-offset):0;
        for(int64_t p=result.md5_pos-1;p>=minimum;--p)if(tail[static_cast<size_t>(p)]=='\n'){result.trailer_start=p+1;break;}
    }
    return result;
}
}
#endif
