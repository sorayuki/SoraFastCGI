#include <memory>
#include <thread>
#include <istream>
#include <iostream>
#include <sstream>
#include <atomic>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include "SoraFastCGI.h"

#include <stdio.h>

namespace asio = boost::asio;
using boost::system::error_code;
using boost::system::system_error;
namespace errc = boost::system::errc;

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
    class IRecordSender
    {
    public:
        virtual bool SendBufferAndDelete(RecordBuf*) = 0;
    };

    class Worker
    {
        asio::io_service& io_service_;
        IRecordSender* sender_;

        std::unordered_map<std::string, std::string> params_;
        asio::streambuf stdin_buf_;

        int reqId_;

        bool requestRunning_;
        bool closeOnComplete_;

        void ResetBuffer()
        {
            params_.clear();
            stdin_buf_.consume(stdin_buf_.size());
        }

        bool OnBeginRequest(RecordBufPtr buf)
        {
            ResetBuffer();
            reqId_ = buf->header.RequestId();

            FCGI_BeginRequestBody* br = (FCGI_BeginRequestBody*)buf->content;

            unsigned short role = (br->roleB1 << 8) | br->roleB0;
            if (role != FCGI_RESPONDER)
                return false;

            requestRunning_ = true;
            closeOnComplete_ = !(br->flags & FCGI_KEEP_CONN);

            return true;
        }

        bool OnParam(RecordBufPtr buf)
        {
            int len = buf->header.ContentLength();
            const char* beginPtr = buf->content;
            const char* endPtr = buf->content + len;

            while (beginPtr < endPtr)
            {
                std::string key, value;
                const char* newBeginPtr = ReadKeyValuePair(beginPtr, key, value);
                if (newBeginPtr == beginPtr) 
                    return false;
                beginPtr = newBeginPtr;
                params_[key] = value;
            }

            return true;
        }

        bool OnAbortRequest(RecordBufPtr buf)
        {
            requestRunning_ = false;
            return true;
        }

        bool OnGetValues(RecordBufPtr buf)
        {
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
            std::stringstream ss;
#if 0
            ss << "Content-type: text/plain\r\n\r\n";
            SendStdout(buf, sizeof(buf) - 1);

            for (auto& x : params_)
            {
                ss << x.first << " = " << x.second << '\r' << '\n';
            }

#else
            ss << "Content-type: text/html\r\n\r\n";
            ss << "<html><body>"
                "<head><title>calculator</title></head>"
                "<form action=? method=POST>"
                "<p>a=<input name='a' type='text' /></p>"
                "<p>b=<input name='b' type='text' /></p>"
                "<p><input type='submit' /></p>"
                "</form>";

            if (params_["REQUEST_METHOD"] == "POST")
            {
                std::ostream(&stdin_buf_).write("", 1);
                std::string tmp;
                std::getline(std::istream(&stdin_buf_), tmp);
                int a, b;
                if (sscanf(tmp.c_str(), "a=%d&b=%d", &a, &b) == 2)
                {
                    ss << "<p>" << a << "+" << b << "=" << a + b << "</p>";
                }
                else
                {
                    ss << "<p>invalid parameters</p>";
                }
            }

            ss << "</body></html>";
#endif
            std::string s = ss.str();
            SendStdout(s.c_str(), s.size());
            SendStdout(0, 0);
            SendEndRequest(0, FCGI_REQUEST_COMPLETE);

            requestRunning_ = false;
            return true;
        }

        bool SendStdout(const char* data, int len)
        {
            while (len > 0 || data == 0)
            {
                int curlen = std::min<int>(len, ushort_max);
                RecordBuf* record = RecordBuf::Create(reqId_, FCGI_STDOUT, curlen);
                if (curlen > 0)
                    memcpy(record->content, data, curlen);
                sender_->SendBufferAndDelete(record);

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

            sender_->SendBufferAndDelete(record);
            return true;
        }

    public:
        Worker(asio::io_service& io_service, IRecordSender* sender)
            : io_service_(io_service)
            , sender_(sender)
            , requestRunning_{}
            , closeOnComplete_{}
        {
        }

        bool FeedPacket(RecordBufPtr buf)
        {
            bool result = true;

            bool notProcessed = false;

            if (!requestRunning_)
            {
                switch (buf->header.type)
                {
                case FCGI_BEGIN_REQUEST:
                    result = OnBeginRequest(std::move(buf));
                    break;
                case FCGI_GET_VALUES:
                    result = OnGetValues(std::move(buf));
                    break;
                default:
                    notProcessed = true;
                    break;
                }
            }
            else
            {
                switch (buf->header.type)
                {
                case FCGI_PARAMS:
                    result = OnParam(std::move(buf));
                    break;

                case FCGI_STDIN:
                    result = OnStdinData(std::move(buf));
                    break;

                case FCGI_ABORT_REQUEST:
                    result = OnAbortRequest(std::move(buf));
                    break;

                default:
                    notProcessed = true;
                    break;
                }
            }

            if (notProcessed)
            {
                LogOutput() << "ReqId:" << buf->header.RequestId() << " Unsupported type " << (int)buf->header.type;

                result = true;
            }

            return result;
        }
    };

    class ProtocolClient : public IRecordSender
    {
        asio::io_service& io_service_;
        asio::ip::tcp::socket socket_;
        asio::streambuf buffer_;
        int workerId_;

        asio::yield_context* yield_;

        SoraFCGIHeader currentHeader_;

        std::array<std::unique_ptr<Worker>, 0xffff> workers_;

        bool RecvHeader()
        {
            if (buffer_.size() < FCGI_HEADER_LEN)
            {
                error_code ec;
                int receivedBytes = asio::async_read(socket_, buffer_, asio::transfer_at_least(FCGI_HEADER_LEN - buffer_.size()), (*yield_)[ec]);

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

        bool RecvRecordContent()
        {
            unsigned int totalLen = currentHeader_.BodyLength();
            if (buffer_.size() < totalLen)
            {
                error_code ec;
                asio::async_read(socket_, buffer_, asio::transfer_at_least(totalLen - buffer_.size()), (*yield_)[ec]);
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
            if (!workers_[reqId])
            {
                workers_[reqId].reset(new Worker(io_service_, this));
            }
            RecordBufPtr buf(RecordBuf::Create(currentHeader_));
            std::istream(&buffer_).read(buf->content, currentHeader_.BodyLength());
            
            bool dispatchResult = workers_[reqId]->FeedPacket(std::move(buf));
            if (!dispatchResult)
            {
                workers_[reqId].reset();
            }

            return dispatchResult;
        }

    public:
        ProtocolClient(asio::io_service& io_service, int workerId = 0)
            : io_service_(io_service)
            , workerId_(workerId)
            , socket_(io_service_)
            , yield_{}
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

        bool SendBufferAndDelete(RecordBuf* record) override
        {
            error_code ec;
            RecordBufPtr p(record);
            asio::async_write(socket_, asio::buffer((char*)record, record->header.TotalLength()), asio::transfer_exactly(record->header.TotalLength()), (*yield_)[ec]);
            return ec == errc::success;
        }

        bool Start(asio::yield_context yield)
        {
            yield_ = &yield;

            for (;;)
            {
                if (!RecvHeader())
                    return false;

                std::istream(&buffer_).read((char*)&currentHeader_, FCGI_HEADER_LEN);

                if (!RecvRecordContent())
                    return false;

                if (!DispatchPacket())
                    return false;
            }

            yield_ = nullptr;
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

            acceptor_.listen(ushort_max, ec);
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
                    io_service_.post([this, worker](){
                        asio::spawn(io_service_, std::bind(&ProtocolClient::Start, worker, std::placeholders::_1));
                    });
                }
            }
        }
    };
};

int main()
{
    using namespace SoraFastCGI;

    asio::io_service io_service;

    Acceptor acceptor(io_service);
    asio::spawn(io_service, [&acceptor](asio::yield_context yield){
        acceptor.Start(yield);
    });

    std::thread([](){ asio::io_service::work keepwork(log_service); log_service.run(); }).detach();

    std::thread([&io_service]() { io_service.run(); }).detach();
    std::thread([&io_service]() { io_service.run(); }).detach();
    std::thread([&io_service]() { io_service.run(); }).detach();
    io_service.run();
    return 0;
}
