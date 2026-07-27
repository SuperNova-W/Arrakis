#include "arrakis/runtime/metrics.hpp"
#include <boost/asio.hpp>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>

namespace arrakis::runtime {
struct Metrics::Impl { mutable std::mutex mutex; std::map<std::string,std::int64_t> values; };
Metrics::Metrics():impl_(std::make_unique<Impl>()) {}
Metrics::~Metrics() = default;
void Metrics::increment(const std::string& name, std::uint64_t value) { std::lock_guard lock(impl_->mutex); impl_->values[name] += static_cast<std::int64_t>(value); }
void Metrics::set(const std::string& name, std::int64_t value) { std::lock_guard lock(impl_->mutex); impl_->values[name] = value; }
std::string Metrics::render() const { std::lock_guard lock(impl_->mutex); std::string output; for (const auto& [name,value] : impl_->values) output += name + " " + std::to_string(value) + "\n"; return output; }
struct MetricsServer::Impl { Metrics& metrics; boost::asio::io_context io; boost::asio::ip::tcp::acceptor acceptor; std::atomic_bool running{true}; std::thread thread; Impl(Metrics& m,std::uint16_t port):metrics(m),acceptor(io,{boost::asio::ip::tcp::v4(),port}) { acceptor.non_blocking(true); thread=std::thread([this]{ while(running){ boost::system::error_code error; boost::asio::ip::tcp::socket socket(io); acceptor.accept(socket,error); if (!error) { const std::string body=metrics.render(); const std::string response="HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body; boost::asio::write(socket,boost::asio::buffer(response),error); } std::this_thread::sleep_for(std::chrono::milliseconds(10)); } }); } };
MetricsServer::MetricsServer(Metrics& metrics,std::uint16_t port):impl_(std::make_unique<Impl>(metrics,port)) {}
MetricsServer::~MetricsServer() { impl_->running=false; boost::system::error_code error; impl_->acceptor.close(error); if (impl_->thread.joinable()) impl_->thread.join(); }
}
