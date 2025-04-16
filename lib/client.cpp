#include "client.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <functional>
#include <iostream>
#include <string>

std::ostream& operator<<(std::ostream& out, const ResponseHeader& header) {
  return out << "[" << "header_type: " << static_cast<int>(header.header_type)
             << ", "
             << "camera_id: " << header.camera_id << ", "
             << "payload_size: " << header.payload_size << ", "
             << "width: " << header.width << ", " << "height: " << header.height
             << ", "
             << "frame_id: " << header.frame_id << "]";
}

Client::Client(boost::asio::io_context& io_context, VideoWindow& video_win,
               const char* host, const char* port)
    : io_context_(io_context),
      socket_(io_context),
      host_(host),
      port_(port),
      video_win_(video_win),
      stopped_(false),
      read_buffer_(kMaxReadLength),
      read_frame_buffer_cursor_(0) {}

void Client::connect() {
  boost::asio::ip::tcp::resolver r(io_context_);

  std::cout << "socket is open " << socket_.is_open() << std::endl;
  // not sure why async_resolve always fails
  boost::asio::ip::tcp::resolver::results_type endpoints =
      r.resolve(host_, port_);
  socket_.async_connect(
      endpoints.begin()->endpoint(),
      std::bind(&Client::handle_connect, this, std::placeholders::_1));
}

// This function terminates all the actors to shut down the connection. It
// may be called by the user of the Client class, or by the class itself in
// response to graceful termination or an unrecoverable error.
void Client::disconnect() {
  stopped_ = true;
  boost::system::error_code ignored_error;
  socket_.close(ignored_error);
}

void Client::stop() {
  write_request(Request{.command = Command::stop}, nullptr);
}

void Client::handle_connect(const boost::system::error_code& error) {
  if (error) {
    std::cout << "connection failed with error " << error.message()
              << std::endl;
    socket_.close();
    return;
  }
  std::cout << "socket is open " << socket_.is_open() << std::endl;
  read_frame_header(0, 0, 0);
}

void Client::write_request(
    Request request,
    std::function<void(boost::system::error_code const&)> callback) {
  socket_.async_write_some(
      boost::asio::buffer(&request, sizeof(Request)),
      [this, callback](boost::system::error_code error, std::size_t length) {
        if (error) {
          std::cout << "Failed to send request with error: " << error.message()
                    << std::endl;
        }
        if (callback) {
          callback(error);
        }
      });
}

bool Client::is_socket_open() const { return socket_.is_open(); }

void Client::configure() {
  write_request(Request{.command = Command::configure}, nullptr);
}

void Client::start() {
  write_request(Request{.command = Command::start}, nullptr);
}

// void Client::start(){
// }

void Client::read_frame_content(uint32_t offset, uint32_t buffer_offset,
                                uint32_t excess) {
  if (excess > 0) {
    uint32_t remainder = current_header_.payload_size - offset;
    bool finishes_content = (excess >= remainder);
    memcpy(reinterpret_cast<uint8_t*>(
               read_frame_buffers_[read_frame_buffer_cursor_].data()) +
               offset,
           read_buffer_.data() + buffer_offset,
           finishes_content ? remainder : excess);
    if (finishes_content) {
      handle_response(current_header_);
      read_frame_header(0, buffer_offset + remainder, excess - remainder);
      return;
    } else {
      offset += excess;
      // guranteed that buffer_offset can be reset to zero
      // guranteed that excess is all absorbed
    }
  }
  socket_.async_read_some(
      boost::asio::buffer(read_buffer_),
      [this, offset](const boost::system::error_code& error,
                     std::size_t length) {
        if (error) {
          std::cout << "read_frame_content(): error: " << error << ": "
                    << error.message() << std::endl;
        }
        // std::cout << "read_frame_content(): received " << length <<
        // std::endl;

        uint32_t remainder = current_header_.payload_size - offset;
        // std::cout << "read_frame_content(): read asio with length " << length
        //           << " remainder=" << remainder << std::endl;
        bool finishes_content = (length >= remainder);
        // bool has_excess = (length > remainder);
        memcpy(reinterpret_cast<uint8_t*>(
                   read_frame_buffers_[read_frame_buffer_cursor_].data()) +
                   offset,
               read_buffer_.data(), finishes_content ? remainder : length);
        if (finishes_content) {
          handle_response(current_header_);
          read_frame_header(0, remainder, length - remainder);
        } else {
          read_frame_content(offset + length, length, 0);
        }
      });
}

void Client::read_frame_header(uint32_t offset, uint32_t buffer_offset,
                               uint32_t excess) {
  if (excess > 0) {
    uint32_t remainder = sizeof(ResponseHeader) - offset;
    bool finishes_header = (excess >= remainder);
    memcpy(reinterpret_cast<uint8_t*>(&current_header_) + offset,
           read_buffer_.data() + buffer_offset,
           finishes_header ? remainder : excess);
    if (finishes_header) {
      std::cout << current_header_ << std::endl;
      if (current_header_.payload_size == 0) {
        handle_response(current_header_);
        read_frame_header(0, buffer_offset + remainder, excess - remainder);
      } else {
        read_frame_buffers_[read_frame_buffer_cursor_].resize(
            current_header_.payload_size);
        read_frame_content(0, buffer_offset + remainder, excess - remainder);
      }
      return;
    } else {
      offset += excess;
      // guranteed that buffer_offset can be reset to zero
      // guranteed that excess is all absorbed
    }
  }
  std::cout << "reading  header.. " << std::endl;
  socket_.async_read_some(
      boost::asio::buffer(read_buffer_),
      [this, offset](const boost::system::error_code& error,
                     std::size_t length) {
        if (error) {
          std::cout << "error in read_frame_header(): " << error.message()
                    << std::endl;
        }
        std::cout << "read_frame_header(): received " << length << std::endl;

        uint32_t remainder = sizeof(ResponseHeader) - offset;
        bool finishes_header = (length >= remainder);
        // bool has_excess = (length > remainder);
        memcpy(reinterpret_cast<uint8_t*>(&current_header_) + offset,
               read_buffer_.data(), finishes_header ? remainder : length);
        if (finishes_header) {
          std::cout << current_header_ << std::endl;
          if (current_header_.payload_size == 0) {
            handle_response(current_header_);
            read_frame_header(0, remainder, length - remainder);
          } else {
            std::cout << "read_frame_buffer_cursor_ is "
                      << read_frame_buffer_cursor_ << std::endl;
            std::cout << "The size of read_frame_buffers_="
                      << read_frame_buffers_.size() << std::endl;
            read_frame_buffers_[read_frame_buffer_cursor_].resize(
                current_header_.payload_size);
            read_frame_content(0, remainder, length - remainder);
          }
        } else {
          read_frame_header(offset + length, length, 0);
        }
      });
}

void Client::handle_response(ResponseHeader header) {
  if (header.header_type == HeaderType::ack) {
    std::cout << "received ack header" << std::endl;
    std::cout << header << std::endl;
    video_win_.notify_image_dim(header.width, header.height);
  } else if (header.header_type == HeaderType::bayer) {
    handle_frame(header, advance_cursor());
  }
}

void Client::handle_frame(ResponseHeader header,
                          unsigned int frame_buffer_cursor) {
  video_win_.update_pbo(header,
                        read_frame_buffers_[frame_buffer_cursor].data());
}

unsigned int Client::advance_cursor() {
  unsigned int old_cursor = read_frame_buffer_cursor_;
  read_frame_buffer_cursor_ =
      (read_frame_buffer_cursor_ + 1) % kNumReadFrameBuffers;
  return old_cursor;
}
