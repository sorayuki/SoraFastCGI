#include <memory>
#include <thread>
#include <istream>
#include <iostream>
#include <sstream>
#include <atomic>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include "FastCGI.h"

namespace asio = boost::asio;
using boost::system::error_code;
using boost::system::system_error;
namespace errc = boost::system::errc;

asio::io_service tcp_io_service;
asio::io_service worker_service;

#if 1
asio::io_service log_service;

class LogOutput : public std::stringstream
{
public:
    ~LogOutput()
    {
        *this << std::endl;
        std::string msg = str();
        log_service.post([msg]() {
            std::cerr << msg;
        });
    }
};
#else
class LogOutput
{
public:
    template<class T>
    LogOutput& operator << (const T& x) { return *this; }
};
#endif

namespace SoraFastCGI
{
    struct SoraFCGIHeader : FCGI_Header
    {
        int TotalLength()
        {
            return BodyLength() + sizeof(*this);
        }

        int BodyLength()
        {
            return ContentLength() + PaddingLength();
        }

        int PaddingLength()
        {
            return paddingLength;
        }

        int ContentLength()
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
            RecordBuf* result = (RecordBuf*) calloc(header.BodyLength() + FCGI_HEADER_LEN, 1);
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

    using RecordBufPtr = std::unique_ptr < RecordBuf, RecordBufDelete > ;


    class Worker
    {
        asio::io_service& io_service_;
        asio::ip::tcp::socket& socket_;

        std::unordered_map<std::string, std::string> params_;
        asio::streambuf stdin_buf_;

        int reqId_;

        bool OnBeginRequest(RecordBufPtr buf)
        {
            params_.clear();
            stdin_buf_.consume(stdin_buf_.size());
            reqId_ = buf->header.RequestId();
            return true;
        }

        bool OnParam(RecordBufPtr buf)
        {
            int len = buf->header.ContentLength();
            char* beginPtr = buf->content;
            char* endPtr = buf->content + len;

            while (beginPtr < endPtr)
            {
                int nameLen = 0, valueLen = 0;
                FCGI_NameValuePair11* p11 = (FCGI_NameValuePair11*)beginPtr;
                FCGI_NameValuePair14* p14 = (FCGI_NameValuePair14*)beginPtr;
                FCGI_NameValuePair41* p41 = (FCGI_NameValuePair41*)beginPtr;
                FCGI_NameValuePair44* p44 = (FCGI_NameValuePair44*)beginPtr;
                char* bodyPtr = 0;

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
                    return false;

                std::string key, value;
                key.resize(nameLen);
                value.resize(valueLen);

                std::copy(bodyPtr, bodyPtr + nameLen, key.begin());
                bodyPtr += nameLen;
                std::copy(bodyPtr, bodyPtr + valueLen, value.begin());
                bodyPtr += valueLen;

                params_[key] = value;

                beginPtr = bodyPtr;
            }

            return true;
        }

        bool OnStdinData(RecordBufPtr buf)
        {
            int contentLen = buf->header.ContentLength();
            if (contentLen > 0)
            {
                std::ostream(&stdin_buf_).write(buf->content, contentLen);
                return true;
            }
            else
            {
                return OnStdinComplete();
            }
        }

        bool OnStdinComplete()
        {
            char buf[] = "Content-type: text/plain\r\n\r\n";
            SendStdout(buf, sizeof(buf) - 1);

            std::stringstream ss;
            for (auto& x : params_)
            {
                ss << x.first << " = " << x.second << '\r' << '\n';
            }

            std::string s = ss.str();
            SendStdout(s.c_str(), s.size());
            SendStdout(0, 0);
            SendEndRequest(0, FCGI_REQUEST_COMPLETE);
            return true;
        }

        bool SendStdout(const char* data, int len)
        {
            while (len > 0 || data == 0)
            {
                int curlen = std::min<int>(len, 65535);
                RecordBuf* record = RecordBuf::Create(reqId_, FCGI_STDOUT, curlen);
                if (curlen > 0)
                    memcpy(record->content, data, curlen);
                SendBufferAndDelete(record);

                if (data == 0)
                    break;

                len -= curlen;
                data += curlen;
            }
            return true;
        }

        bool SendEndRequest(unsigned int exitcode, unsigned int statuscode)
        {
            RecordBuf* record = RecordBuf::Create(reqId_, FCGI_END_REQUEST, sizeof(FCGI_EndRequestBody));

            FCGI_EndRequestBody* body = (FCGI_EndRequestBody*)record->content;
            body->protocolStatus = statuscode;
            body->appStatusB3 = exitcode >> 24;
            body->appStatusB2 = (exitcode >> 16) & 0xff;
            body->appStatusB1 = (exitcode >> 8) & 0xff;
            body->appStatusB0 = exitcode & 0xff;

            SendBufferAndDelete(record);

            return true;
        }

        bool SendBufferAndDelete(RecordBuf* record)
        {
            RecordBufPtr p(record);
            asio::async_write(socket_, asio::buffer((char*)record, record->header.TotalLength()), asio::transfer_exactly(record->header.TotalLength()), [](const error_code& ec, int bytes){});

            return true;
        }

    public:
        Worker(asio::io_service& io_service, asio::ip::tcp::socket& socket)
            : io_service_(io_service)
            , socket_(socket)
        {
        }

        bool FeedPacket(RecordBufPtr buf)
        {
            bool result = true;

            switch (buf->header.type)
            {
            case FCGI_BEGIN_REQUEST:
                result = OnBeginRequest(std::move(buf));
                break;

            case FCGI_PARAMS:
                result = OnParam(std::move(buf));
                break;

            case FCGI_STDIN:
                result = OnStdinData(std::move(buf));
                break;

            default:
                {
                    LogOutput() << "ReqId:" << buf->header.RequestId() << " Unsupported type " << buf->header.type;
                    break;
                }
            }

            return result;
        }
    };

    class ProtocolClient
    {
        asio::io_service& io_service_;
        asio::ip::tcp::socket socket_;
        asio::streambuf buffer_;
        int workerId_;

        SoraFCGIHeader currentHeader_;

        std::array<Worker*, 65536> workers_;

        bool RecvHeader(asio::yield_context& yield)
        {
            if (buffer_.size() < FCGI_HEADER_LEN)
            {
                error_code ec;
                int receivedBytes = asio::async_read(socket_, buffer_, asio::transfer_at_least(FCGI_HEADER_LEN - buffer_.size()), yield[ec]);

                if (ec == asio::error::eof)
                    return false;

                if (ec != errc::success)
                {
                    LogOutput() << "[" << workerId_ << "] : fail to receive header data - " << ec.message();
                    return false;
                }
            }
            return true;
        }

        bool RecvRecordContent(asio::yield_context& yield)
        {
            int totalLen = currentHeader_.BodyLength();
            if (buffer_.size() < totalLen)
            {
                error_code ec;
                asio::async_read(socket_, buffer_, asio::transfer_at_least(totalLen - buffer_.size()), yield[ec]);
                if (ec != errc::success)
                {
                    LogOutput() << "[" << workerId_ << "] : fail to receive content data - " << ec.message();
                    return false;
                }
            }
            return true;
        }

        bool DispatchPacket()
        {
            int reqId = currentHeader_.RequestId();
            if (workers_[reqId] == 0)
            {
                workers_[reqId] = new Worker(io_service_, socket_);
            }
            RecordBufPtr buf(RecordBuf::Create(currentHeader_));
            std::istream(&buffer_).read(buf->content, currentHeader_.BodyLength());
            return workers_[reqId]->FeedPacket(std::move(buf));
        }

    public:
        ProtocolClient(asio::io_service& io_service, int workerId = 0)
            : io_service_(io_service)
            , workerId_(workerId)
            , socket_(io_service_)
            , workers_{}
        {
        }
        
        ~ProtocolClient()
        {
            if (socket_.is_open())
                socket_.close();
        }

        asio::ip::tcp::socket& Socket()
        {
            return socket_;
        }

        bool Start(asio::yield_context yield)
        {
            for (;;)
            {
                if (!RecvHeader(yield))
                    return false;

                std::istream(&buffer_).read((char*)&currentHeader_, FCGI_HEADER_LEN);

                if (!RecvRecordContent(yield))
                    return false;

                if (!DispatchPacket())
                    return false;
            }
        }
    };

    class Acceptor
    {
        asio::io_service& io_service_;
        asio::ip::tcp::acceptor acceptor_;
        std::atomic_int workerId_;

    public:
        Acceptor(asio::io_service& io_service)
            : io_service_(io_service)
            , acceptor_(io_service_)
        {
        }

        bool Start(asio::yield_context yield)
        {
            error_code ec;
            acceptor_.open(asio::ip::tcp::v4(), ec);
            if (ec != errc::success)
            {
                LogOutput() << "fail to create socket : " << ec;
                return false;
            }
            
            acceptor_.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 6666), ec);
            if (ec != errc::success)
            {
                LogOutput() << "fail to bind port : " << ec.message();
                return false;
            }

            acceptor_.listen(50, ec);
            if (ec != errc::success)
            {
                LogOutput() << "fail to listen on port : " << ec.message();
                return false;
            }

            for (;;)
            {
                std::shared_ptr<ProtocolClient> worker{ new ProtocolClient(io_service_, workerId_++) };
                asio::ip::tcp::endpoint fcgi_remote_endpoint;
                acceptor_.async_accept(worker->Socket(), fcgi_remote_endpoint, yield[ec]);
                if (ec != errc::success)
                {
                    LogOutput() << "fail to accept client : " << ec.message();
                }
                else
                {
                    asio::spawn(io_service_, std::bind(&ProtocolClient::Start, worker, std::placeholders::_1));
                }
            }
        }
    };
};

int main()
{
    using namespace SoraFastCGI;

    Acceptor acceptor(tcp_io_service);
    asio::spawn(tcp_io_service, [&acceptor](asio::yield_context yield){
        acceptor.Start(yield);
    });

    std::thread([](){ asio::io_service::work keepwork(log_service); log_service.run(); }).detach();

    std::thread([]() { tcp_io_service.run(); }).detach();
    std::thread([]() { tcp_io_service.run(); }).detach();
    std::thread([]() { tcp_io_service.run(); }).detach();
    tcp_io_service.run();
    return 0;
}