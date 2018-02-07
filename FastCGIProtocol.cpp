#include <memory>
#include <thread>
#include <istream>
#include <iostream>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/coroutine/asymmetric_coroutine.hpp>

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

        int RequestId()
        {
            return (requestIdB1 << 8) | requestIdB0;
        }
    };

    struct DataBuf {
        SoraFCGIHeader header;
        char content[1];

        static DataBuf* Create(SoraFCGIHeader& header)
        {
            DataBuf* result = (DataBuf*) calloc(header.TotalLength() + FCGI_HEADER_LEN, 1);
            result->header = header;
            return result;
        }
    };

    using DataBufPtr = std::unique_ptr < DataBuf, DataBufDelete > ;

    struct DataBufDelete
    {
        void operator()(DataBuf* val) const
        {
            if (val) free(val);
        }
    };

    class Worker
    {
        asio::io_service& io_service_;
        asio::strand& strand_;

        std::unordered_map<std::string, std::string> params_;

    public:
        Worker(asio::io_service& io_service, asio::strand& strand)
            : io_service_(io_service)
            , strand_(strand)
        {
        }

        void FeedPacket(DataBufPtr buf)
        {
            switch (buf->header.type)
            {

            }
        }
    };

    class ProtocolClient
    {
        asio::io_service& io_service_;
        asio::strand tcp_strand_;
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
                asio::async_read(socket_, buffer_, asio::transfer_at_least(FCGI_HEADER_LEN - buffer_.size()), yield[ec]);
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
            int totalLen = currentHeader_.TotalLength();
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

        void DispatchPacket()
        {
            int reqId = currentHeader_.RequestId();
            if (workers_[reqId] == 0)
            {
                workers_[reqId] = new Worker(worker_service, tcp_strand_);
            }
            DataBufPtr buf(DataBuf::Create(currentHeader_));
            std::istream(&buffer_).read(buf->content, currentHeader_.TotalLength());
            workers_[reqId]->FeedPacket(std::move(buf));
        }

    public:
        ProtocolClient(asio::io_service& io_service, int workerId = 0)
            : io_service_(io_service)
            , workerId_(workerId)
            , socket_(io_service_)
            , tcp_strand_(io_service_)
            , workers_{}
        {
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

                DispatchPacket();
            }
        }
    };

    class Acceptor
    {
        asio::io_service& io_service_;
        asio::ip::tcp::acceptor acceptor_;

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

            acceptor_.listen(std::numeric_limits<int>::max(), ec);
            if (ec != errc::success)
            {
                LogOutput() << "fail to listen on port : " << ec.message();
                return false;
            }

            for (;;)
            {
                std::shared_ptr<ProtocolClient> worker{ new ProtocolClient(io_service_) };
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

    tcp_io_service.run();
    return 0;
}