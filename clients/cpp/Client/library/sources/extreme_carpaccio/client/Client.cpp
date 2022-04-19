#include <extreme_carpaccio/client/Client.hpp>
#include <extreme_carpaccio/client/HttpConfig.hpp>

#include <extreme_carpaccio/order_management/Order.hpp>
#include <extreme_carpaccio/order_management/OrderParsing.hpp>
#include <extreme_carpaccio/order_management/TotalAmount.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>


namespace extreme_carpaccio {
namespace client {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

const int version = 11;

} // namespace

http_worker::http_worker(tcp::acceptor& acceptor, const std::string& doc_root) :
   acceptor_(acceptor),
   doc_root_(doc_root)
{
}

void http_worker::start()
{
   accept();
   check_deadline();
}

void http_worker::accept()
{
   // Clean up any previous connection.
   beast::error_code ec;
   socket_.close(ec);
   buffer_.consume(buffer_.size());

   acceptor_.async_accept(
      socket_,
      [this](beast::error_code ec)
   {
      if (ec)
      {
         accept();
      }
      else
      {
         // Request must be fully processed within 60 seconds.
         request_deadline_.expires_after(
            std::chrono::seconds(60));

         read_request();
      }
   });
}

void http_worker::read_request()
{
   // On each read the parser needs to be destroyed and
   // recreated. We store it in a boost::optional to
   // achieve that.
   //
   // Arguments passed to the parser constructor are
   // forwarded to the message object. A single argument
   // is forwarded to the body constructor.
   //
   // We construct the dynamic body with a 1MB limit
   // to prevent vulnerability to buffer attacks.
   //
   parser_.emplace(
      std::piecewise_construct,
      std::make_tuple(),
      std::make_tuple(alloc_));

   http::async_read(
      socket_,
      buffer_,
      *parser_,
      [this](beast::error_code ec, std::size_t)
   {
      if (ec)
         accept();
      else
         process_request(parser_->get());
   });
}

struct Feedback
{
   std::string type;
   std::string content;
};

static Feedback parseFeedback(const std::string& jsonFeedback)
{
   auto parsedFeedback = nlohmann::json::parse(jsonFeedback);
   Feedback feedback = {};

   feedback.type = parsedFeedback["type"].get<std::string>();
   feedback.content = parsedFeedback["content"].get<std::string>();
   return feedback;
}

bool http_worker::handleRequest(http::verb requestType, const std::string & target, const std::string & contentType, const std::string & body)
{
   const bool error = (requestType != http::verb::post || contentType != "application/json");

   if (!error)
   {
      if (target == "/order")
      {
         auto order = order_management::parseOrder(body);
         std::cout << "Order received: " << order << std::endl;

         auto totalAmountResponse = order_management::computeTotalAmount(body);

         nlohmann::json totalAmountJson;

         totalAmountJson["total"] = totalAmountResponse.m_totalAmount;
         send_response(totalAmountResponse.m_status, totalAmountJson.dump());
      }
      else if (target == "/feedback")
      {
         Feedback feedback = parseFeedback(body);

         std::cout << feedback.type << " : " << feedback.content << std::endl;
         send_response(http::status::ok, "Feedback received");
      }
   }
   return error;
}

void http_worker::process_request(http::request<request_body_t, http::basic_fields<alloc_t>> const& req)
{
   const std::string contentType = req[http::field::content_type].to_string();
   const std::string body = req.body();

   if (this->handleRequest(req.method(), req.target().to_string(), contentType, body))
   {
      this->send_response(http::status::not_found, "HTTP code 404\r\n");
   }
}

void http_worker::send_response(http::status status, std::string const& body)
{
   string_response_.emplace(
      std::piecewise_construct,
      std::make_tuple(),
      std::make_tuple(alloc_));

   string_response_->result(status);
   string_response_->keep_alive(false);
   string_response_->set(http::field::server, "Beast");
   string_response_->set(http::field::content_type, "text/plain");
   string_response_->body() = body;
   string_response_->prepare_payload();

   http::async_write(
      socket_,
      *string_response_,
      [this](beast::error_code ec, std::size_t)
   {
      socket_.shutdown(tcp::socket::shutdown_send, ec);
      string_response_.reset();
      accept();
   });
}

void http_worker::check_deadline()
{
   // The deadline may have moved, so check it has really passed.
   if (request_deadline_.expiry() <= std::chrono::steady_clock::now())
   {
      // Close socket to cancel any outstanding operation.
      socket_.close();

      // Sleep indefinitely until we're given a new deadline.
      request_deadline_.expires_at(
         (std::chrono::steady_clock::time_point::max)());
   }

   request_deadline_.async_wait(
      [this](beast::error_code)
   {
      check_deadline();
   });
}

   CarpaccioServer::CarpaccioServer() : CarpaccioServer(HTTP_SERVER_PORT)
   {
	  
   }

   CarpaccioServer::CarpaccioServer(unsigned short port)
      : ioc(1)
      , acceptor(ioc, {boost::asio::ip::make_address(HTTP_SERVER_IP), port } ),
      worker(acceptor, "./feedback")
   {
      
   }

   void CarpaccioServer::start()
   {
      worker.start();
      ioc.run();
   }

   void CarpaccioServer::stop()
   {
      ioc.stop();
   }

   CarpaccioStream::CarpaccioStream(const std::string & host, unsigned short port)
      : m_serverHost(host)
      , m_ioContext()
      , m_resolver(m_ioContext) // These objects perform our I/O
      , m_resolverResults(m_resolver.resolve(host, std::to_string(port))) // Look up the domain name
      , m_stream(m_ioContext)
   {
      // Make the connection on the IP address we get from a lookup
      m_stream.connect(m_resolverResults);
   }

   CarpaccioStream::~CarpaccioStream()
   {
      // Gracefully close the socket
      beast::error_code ec;
      m_stream.socket().shutdown(tcp::socket::shutdown_both, ec);

      //// not_connected happens sometimes
      //// so don't bother reporting it.
      //if (ec && ec != beast::errc::not_connected)
      //   throw beast::system_error{ ec };
   }

   http::response<http::dynamic_body> CarpaccioStream::read(beast::flat_buffer & buffer)
   {
      // Declare a container to hold the response
      http::response<http::dynamic_body> response;

      // Receive the HTTP response
      http::read(m_stream, buffer, response);

      return response;
   }

   void CarpaccioStream::write(boost::beast::http::verb requestType, const std::string & target, const std::string & contentType, const std::string & body)
   {
      // Set up an HTTP GET request message
      http::request<http::string_body> request{ requestType, target, version };
      request.set(http::field::host, m_serverHost);
      request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
      request.set(http::field::content_type, contentType);
      request.body() = body;
      request.prepare_payload();

      // Send the HTTP request to the remote host
      http::write(m_stream, request);
   }
   
} // namespace client
} // namespace extreme_carpaccio