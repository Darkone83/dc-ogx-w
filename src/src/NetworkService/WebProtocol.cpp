#include "WebProtocol.h"

#include <cstring>

namespace WebProtocol {
namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equals(const char* text, size_t length, const char* expected) {
    const size_t n = std::strlen(expected);
    if (length != n) return false;
    for (size_t i = 0; i < n; ++i) if (lower(text[i]) != lower(expected[i])) return false;
    return true;
}
bool copy(char* target, size_t capacity, const char* begin, size_t length) {
    if (length >= capacity) return false;
    std::memcpy(target, begin, length);
    target[length] = 0;
    return true;
}
bool token(const char* text, size_t length, const char* wanted) {
    size_t i = 0;
    while (i < length) {
        while (i < length && (text[i] == ' ' || text[i] == '\t' || text[i] == ',')) ++i;
        const size_t begin = i;
        while (i < length && text[i] != ',') ++i;
        size_t end = i;
        while (end > begin && (text[end-1] == ' ' || text[end-1] == '\t')) --end;
        if (equals(text+begin, end-begin, wanted)) return true;
    }
    return false;
}
const char* crlf(const char* begin, const char* end) {
    for (const char* p = begin; p+1 < end; ++p)
        if (p[0] == '\r' && p[1] == '\n') return p;
    return nullptr;
}
uint32_t rotate(uint32_t value, unsigned bits) { return (value << bits) | (value >> (32-bits)); }

// SHA-1 here is solely the mandatory RFC6455 handshake hash, not an update
// authenticity mechanism. A key + GUID is 60 bytes, hence exactly two blocks.
void handshake_sha1(const char input[60], uint8_t digest[20]) {
    uint8_t blocks[128]{};
    std::memcpy(blocks, input, 60);
    blocks[60] = 0x80;
    blocks[126] = 0x01;
    blocks[127] = 0xe0; // 60 * 8 bits
    uint32_t state[5] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    for (unsigned block = 0; block < 2; ++block) {
        uint32_t words[80];
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t* p = blocks + block*64 + i*4;
            words[i] = (uint32_t(p[0])<<24) | (uint32_t(p[1])<<16) | (uint32_t(p[2])<<8) | p[3];
        }
        for (unsigned i = 16; i < 80; ++i)
            words[i] = rotate(words[i-3] ^ words[i-8] ^ words[i-14] ^ words[i-16], 1);
        uint32_t a=state[0], b=state[1], c=state[2], d=state[3], e=state[4];
        for (unsigned i=0; i<80; ++i) {
            uint32_t f, k;
            if (i<20) { f=(b&c)|((~b)&d); k=0x5a827999; }
            else if (i<40) { f=b^c^d; k=0x6ed9eba1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
            else { f=b^c^d; k=0xca62c1d6; }
            const uint32_t next=rotate(a,5)+f+e+k+words[i];
            e=d; d=c; c=rotate(b,30); b=a; a=next;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
    }
    for (unsigned i=0;i<5;++i) for (unsigned j=0;j<4;++j)
        digest[i*4+j]=static_cast<uint8_t>(state[i]>>(24-j*8));
}
} // namespace

ParseResult parse_request(const uint8_t* bytes, size_t size, Request& request) {
    request = Request{};
    const char* data = reinterpret_cast<const char*>(bytes);
    const char* header_end = nullptr;
    for (size_t i=0;i+3<size && i+4<=MAX_HEADER;++i) {
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') {
            header_end=data+i+4;
            break;
        }
    }
    if (!header_end) return size>=MAX_HEADER ? ParseResult::TooLarge : ParseResult::Incomplete;
    request.header_length = static_cast<size_t>(header_end-data);
    // No embedded NUL, bare newlines, or other control characters in headers.
    for (const char* p=data;p<header_end;++p) {
        const unsigned char c=static_cast<unsigned char>(*p);
        if (c==0 || (c<32 && c!='\r' && c!='\n' && c!='\t') || c==127) return ParseResult::Invalid;
        if (c=='\n' && (p==data || p[-1]!='\r')) return ParseResult::Invalid;
        if (c=='\r' && (p+1==header_end || p[1]!='\n')) return ParseResult::Invalid;
    }
    const char* line_end=crlf(data,header_end);
    if (!line_end) return ParseResult::Invalid;
    const char* space=static_cast<const char*>(std::memchr(data,' ',static_cast<size_t>(line_end-data)));
    if (!space || !copy(request.method,sizeof(request.method),data,space-data)) return ParseResult::Invalid;
    const char* path=space+1;
    space=static_cast<const char*>(std::memchr(path,' ',static_cast<size_t>(line_end-path)));
    if (!space || path==space || path[0]!='/' || !copy(request.path,sizeof(request.path),path,space-path)) return ParseResult::Invalid;
    if (!equals(space+1,line_end-space-1,"HTTP/1.1") && !equals(space+1,line_end-space-1,"HTTP/1.0")) return ParseResult::Invalid;

    bool has_length=false, upgrade=false, connection_upgrade=false, version=false, has_host=false;
    for (const char* p=line_end+2;p<header_end-2;) {
        line_end=crlf(p,header_end);
        if (!line_end || p==line_end || p[0]==' ' || p[0]=='\t') return ParseResult::Invalid;
        const char* colon=static_cast<const char*>(std::memchr(p,':',static_cast<size_t>(line_end-p)));
        if (!colon || colon==p) return ParseResult::Invalid;
        const char* value=colon+1;
        while (value<line_end && (*value==' ' || *value=='\t')) ++value;
        const char* value_end=line_end;
        while (value_end>value && (value_end[-1]==' ' || value_end[-1]=='\t')) --value_end;
        const size_t n=static_cast<size_t>(value_end-value);
        if (equals(p,colon-p,"content-length")) {
            if (has_length || n==0) return ParseResult::Invalid;
            has_length=true;
            size_t length=0;
            for (const char* d=value;d<value_end;++d) {
                if (*d<'0' || *d>'9') return ParseResult::Invalid;
                if (length>MAX_BODY/10) return ParseResult::TooLarge;
                length=length*10+static_cast<unsigned>(*d-'0');
                if (length>MAX_BODY) return ParseResult::TooLarge;
            }
            request.body_length=length;
        } else if (equals(p,colon-p,"transfer-encoding")) {
            return ParseResult::Invalid; // chunked request bodies are deliberately unsupported
        } else if (equals(p,colon-p,"host")) {
            if (has_host || !copy(request.host,sizeof(request.host),value,n)) return ParseResult::Invalid;
            has_host=true;
        } else if (equals(p,colon-p,"origin")) {
            if (request.origin[0] || !copy(request.origin,sizeof(request.origin),value,n)) return ParseResult::Invalid;
        } else if (equals(p,colon-p,"content-type")) {
            const char* semi=static_cast<const char*>(std::memchr(value,';',n));
            request.json=equals(value,semi?static_cast<size_t>(semi-value):n,"application/json");
        } else if (equals(p,colon-p,"upgrade")) {
            upgrade=equals(value,n,"websocket");
        } else if (equals(p,colon-p,"connection")) {
            connection_upgrade=token(value,n,"upgrade");
        } else if (equals(p,colon-p,"sec-websocket-version")) {
            version=equals(value,n,"13");
        } else if (equals(p,colon-p,"sec-websocket-key")) {
            if (request.websocket_key[0] || !copy(request.websocket_key,sizeof(request.websocket_key),value,n)) return ParseResult::Invalid;
        }
        p=line_end+2;
    }
    if (!has_host || request.host[0]==0) return ParseResult::Invalid;
    request.websocket=upgrade && connection_upgrade && version && request.websocket_key[0];
    if (upgrade && (!request.websocket || std::strcmp(request.method,"GET") || request.body_length)) return ParseResult::Invalid;
    return size < request.header_length+request.body_length ? ParseResult::Incomplete : ParseResult::Complete;
}

bool same_origin(const Request& request) {
    // Native Python clients omit Origin. Browser writes must come from this
    // device; the localhost companion terminates and proxies its own requests.
    if (request.origin[0]==0) return true;
    constexpr char prefix[]="http://";
    if (std::strncmp(request.origin,prefix,sizeof(prefix)-1)) return false;
    return equals(request.origin+sizeof(prefix)-1,std::strlen(request.origin+sizeof(prefix)-1),request.host);
}

bool websocket_accept(const char* key, char output[29]) {
    if (!key || std::strlen(key)!=24 || key[22]!='=' || key[23]!='=') return false;
    const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i=0;i<22;++i) if (!std::strchr(alphabet,key[i])) return false;
    // A valid 16-byte base64 nonce has only two significant bits in character 22.
    const char* last=std::strchr(alphabet,key[21]);
    if (!last || ((last-alphabet)&15)) return false;
    char input[60];
    std::memcpy(input,key,24);
    std::memcpy(input+24,"258EAFA5-E914-47DA-95CA-C5AB0DC85B11",36);
    uint8_t digest[20];
    handshake_sha1(input,digest);
    size_t out=0;
    for (size_t i=0;i<20;i+=3) {
        const uint32_t v=(uint32_t(digest[i])<<16) | (i+1<20?uint32_t(digest[i+1])<<8:0) | (i+2<20?digest[i+2]:0);
        output[out++]=alphabet[(v>>18)&63];
        output[out++]=alphabet[(v>>12)&63];
        output[out++]=i+1<20?alphabet[(v>>6)&63]:'=';
        output[out++]=i+2<20?alphabet[v&63]:'=';
    }
    output[out]=0;
    return true;
}

ParseResult parse_frame(const uint8_t* data, size_t size, Frame& frame) {
    frame=Frame{};
    if (size<2) return ParseResult::Incomplete;
    if ((data[0]&0x70) || !(data[1]&0x80)) return ParseResult::Invalid;
    frame.final=(data[0]&0x80)!=0;
    frame.opcode=data[0]&15;
    if (frame.opcode!=0 && frame.opcode!=1 && frame.opcode!=2 && frame.opcode!=8 && frame.opcode!=9 && frame.opcode!=10) return ParseResult::Invalid;
    size_t cursor=2;
    uint64_t length=data[1]&127;
    if (length==126) {
        if (size<4) return ParseResult::Incomplete;
        length=(uint16_t(data[2])<<8)|data[3];
        cursor=4;
        if (length<126) return ParseResult::Invalid;
    } else if (length==127) {
        if (size<10) return ParseResult::Incomplete;
        if (data[2]&0x80) return ParseResult::Invalid;
        length=0;
        for (unsigned i=2;i<10;++i) length=(length<<8)|data[i];
        cursor=10;
        if (length<=65535) return ParseResult::Invalid;
    }
    if (frame.opcode>=8 && (!frame.final || length>125 || (frame.opcode==8 && length==1))) return ParseResult::Invalid;
    if (length>MAX_WS_MESSAGE) return ParseResult::TooLarge;
    if (size<cursor+4) return ParseResult::Incomplete;
    std::memcpy(frame.mask,data+cursor,4);
    frame.header_length=cursor+4;
    frame.payload_length=static_cast<size_t>(length);
    return size<frame.header_length+frame.payload_length ? ParseResult::Incomplete : ParseResult::Complete;
}

void unmask(uint8_t* payload, size_t size, const uint8_t mask[4]) {
    for (size_t i=0;i<size;++i) payload[i]^=mask[i&3];
}

bool valid_utf8(const uint8_t* payload, size_t size) {
    for (size_t i=0;i<size;) {
        const uint8_t first=payload[i++];
        if (first<0x80) continue;
        unsigned remaining;
        uint32_t value;
        uint32_t minimum;
        if (first>=0xc2 && first<=0xdf) { remaining=1; value=first&0x1f; minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { remaining=2; value=first&0x0f; minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { remaining=3; value=first&7; minimum=0x10000; }
        else return false;
        if (size-i<remaining) return false;
        while (remaining--) {
            const uint8_t next=payload[i++];
            if ((next&0xc0)!=0x80) return false;
            value=(value<<6)|(next&0x3f);
        }
        if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}

bool valid_close(const uint8_t* payload, size_t size) {
    if (!size) return true;
    if (size<2) return false;
    const unsigned code=(unsigned(payload[0])<<8)|payload[1];
    // 1004/1005/1006 and 1015 are reserved and cannot be sent on the wire.
    if (code<1000 || code>=5000 || code==1004 || code==1005 || code==1006 ||
        (code>=1015 && code<3000)) return false;
    return valid_utf8(payload+2,size-2);
}

size_t frame_header(uint8_t opcode, size_t length, uint8_t* output, size_t capacity) {
    if (length>65535 || capacity<(length<126?2u:4u)) return 0;
    output[0]=0x80|opcode;
    if (length<126) { output[1]=static_cast<uint8_t>(length); return 2; }
    output[1]=126;
    output[2]=static_cast<uint8_t>(length>>8);
    output[3]=static_cast<uint8_t>(length);
    return 4;
}
} // namespace WebProtocol
