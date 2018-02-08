#include "SoraFastCGI.h"

namespace SoraFastCGI
{
    const char* ReadKeyValuePair(const char* beginPtr, std::string& key, std::string& value)
    {
        int nameLen = 0, valueLen = 0;
        const FCGI_NameValuePair11* p11 = (const FCGI_NameValuePair11*)beginPtr;
        const FCGI_NameValuePair14* p14 = (const FCGI_NameValuePair14*)beginPtr;
        const FCGI_NameValuePair41* p41 = (const FCGI_NameValuePair41*)beginPtr;
        const FCGI_NameValuePair44* p44 = (const FCGI_NameValuePair44*)beginPtr;
        const char* bodyPtr = 0;

        if ((p11->nameLengthB0 >> 7) == 0 && (p11->nameLengthB0 >> 7) == 0)
        {
            nameLen = p11->nameLengthB0;
            valueLen = p11->valueLengthB0;
            bodyPtr = p11->data;
        }
        else if ((p11->nameLengthB0 >> 7) == 0 && (p11->valueLengthB0 >> 7) == 1)
        {
            nameLen = p14->nameLengthB0;
            valueLen = ((p14->valueLengthB3 & 0x7f) << 24)
                | (p14->valueLengthB2 << 16)
                | (p14->valueLengthB1 << 8)
                | (p14->valueLengthB0);
            bodyPtr = p14->data;
        }
        else if ((p41->nameLengthB3 >> 7) == 1 && (p41->valueLengthB0 >> 7) == 0)
        {
            nameLen = ((p41->nameLengthB3 & 0x7f) << 24)
                | (p41->nameLengthB2 << 16)
                | (p41->nameLengthB1 << 8)
                | (p41->nameLengthB0);
            valueLen = p41->valueLengthB0;
            bodyPtr = p41->data;
        }
        else if ((p44->nameLengthB3 >> 7) == 1 && (p44->valueLengthB3 >> 7) == 1)
        {
            nameLen = ((p44->nameLengthB3 & 0x7f) << 24)
                | (p44->nameLengthB2 << 16)
                | (p44->nameLengthB1 << 8)
                | (p44->nameLengthB0);
            valueLen = ((p44->valueLengthB3 & 0x7f) << 24)
                | (p44->valueLengthB2 << 16)
                | (p44->valueLengthB1 << 8)
                | (p44->valueLengthB0);
            bodyPtr = p44->data;
        }
        else
            return beginPtr;

        if (nameLen > ushort_max || valueLen > ushort_max)
            return beginPtr;

        if (nameLen)
        {
            key.resize(nameLen);
            std::copy(bodyPtr, bodyPtr + nameLen, key.begin());
            bodyPtr += nameLen;
        }
        else
            key.clear();

        if (valueLen)
        {
            value.resize(valueLen);
            std::copy(bodyPtr, bodyPtr + valueLen, value.begin());
            bodyPtr += valueLen;
        }
        else
            value.clear();

        return bodyPtr;
    }
};
