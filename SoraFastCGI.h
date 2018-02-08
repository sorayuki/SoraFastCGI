#ifndef SORA_FASTCGI_H_
#define SORA_FASTCGI_H_

#include "FastCGI.h"

#include <memory>
#include <string>

namespace SoraFastCGI
{
    struct SoraFCGIHeader : FCGI_Header
    {
        unsigned int TotalLength()
        {
            return BodyLength() + sizeof(*this);
        }

        unsigned int BodyLength()
        {
            return ContentLength() + PaddingLength();
        }

        unsigned char PaddingLength()
        {
            return paddingLength;
        }

        unsigned short ContentLength()
        {
            return (contentLengthB1 << 8) | contentLengthB0;
        }

        void ContentLength(unsigned short len)
        {
            contentLengthB1 = len >> 8;
            contentLengthB0 = len & 0xff;
        }

        unsigned short RequestId()
        {
            return (requestIdB1 << 8) | requestIdB0;
        }

        void RequestId(unsigned short id)
        {
            requestIdB1 = id >> 8;
            requestIdB0 = id & 0xff;
        }
    };

    struct RecordBuf {
        SoraFCGIHeader header;
        char content[1];

        static RecordBuf* Create(unsigned short reqId, unsigned char type, int contentLen)
        {
            RecordBuf* result = (RecordBuf*)calloc(contentLen + FCGI_HEADER_LEN, 1);
            result->header.version = FCGI_VERSION_1;
            result->header.RequestId(reqId);
            result->header.ContentLength(contentLen);
            result->header.type = type;
            return result;
        }

        static RecordBuf* Create(SoraFCGIHeader& header)
        {
            RecordBuf* result = (RecordBuf*)calloc(header.BodyLength() + FCGI_HEADER_LEN, 1);
            result->header = header;
            return result;
        }
    };

    struct RecordBufDelete
    {
        void operator()(RecordBuf* val) const
        {
            if (val) free(val);
        }
    };

    using RecordBufPtr = std::unique_ptr < RecordBuf, RecordBufDelete >;

    const char* ReadKeyValuePair(const char* beginPtr, std::string& key, std::string& value);
};

#endif
