#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <iostream>
#include <video_window.hpp>

using boost::asio::ip::tcp;

enum class Command : uint32_t { configure, control, start, stop };
struct Request {
  Command command;
  uint32_t key;
  uint8_t value[32];
};

std::ostream& operator<<(std::ostream& out, const ResponseHeader& header);

class Client {
 public:
  static constexpr int kMaxReadLength = 32768;
  static constexpr int kNumReadFrameBuffers = 2;
  Client(boost::asio::io_context& io_context, VideoWindow&);
  void connect(std::string const& host, std::string const& service);
  void configure();
  void start();
  void stop();
  void disconnect();

  bool is_socket_open() const;

 private:
  void handle_connect(const boost::system::error_code& error);
  void read_frame_header(uint32_t offset, uint32_t buffer_offset,
                         uint32_t excess);
  void read_frame_content(uint32_t offset, uint32_t buffer_offset,
                          uint32_t excess);
  void handle_frame(ResponseHeader header, unsigned int frame_buffer_cursor);
  void handle_response(ResponseHeader header);
  void write_request(
      Request request,
      std::function<void(boost::system::error_code const&)> callback);
  unsigned int advance_cursor();

 private:
  boost::asio::io_context& io_context_;
  tcp::socket socket_;
  VideoWindow& video_win_;
  bool stopped_;
  std::vector<uint8_t> read_buffer_;
  ResponseHeader current_header_;
  unsigned int read_frame_buffer_cursor_;
  // std::array<std::vector<uint8_t>, kNumReadFrameBuffers> failed
  std::vector<std::vector<uint8_t>> read_frame_buffers_;
};
