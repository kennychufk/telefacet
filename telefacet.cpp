#include <boost/asio.hpp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "client.hpp"
#include "video_window.hpp"

int main(int argc, char **argv) {
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);

  std::thread asio_thread([&io_context]() { io_context.run(); });

  Fl::visual(FL_RGB8 | FL_DOUBLE);
  Fl_Window *win = new Fl_Window(340, 180);
  Fl_Grid *grid = new Fl_Grid(0, 0, win->w(), win->h());
  grid->layout(2, 1, 10, 10);
  grid->color(FL_WHITE);
  Fl_Button *button = new Fl_Button(0, 0, 100, 30, "connect");
  VideoWindow *video_win = new VideoWindow(0, 0, 300, 300);
  Client client(io_context, *video_win);
  button->callback(
      [](Fl_Widget *widget, void *data) {
        Client *client = reinterpret_cast<Client *>(data);
        std::cout << "button pressed" << std::endl;
        if (!client->is_socket_open()) {
          client->connect("rpi5-dic0.local", "8080");
          widget->label("configure");
        } else if (strcmp(widget->label(), "configure") == 0) {
          std::cout << "configure" << std::endl;
          client->configure();
          widget->label("start");
        } else if (strcmp(widget->label(), "start") == 0) {
          client->start();
          widget->label("stop");
        } else if (strcmp(widget->label(), "stop") == 0) {
          client->stop();
          widget->label("start");
        }
      },
      &client);
  video_win->end();
  grid->widget(video_win, 0, 0);
  grid->widget(button, 1, 0);
  grid->row_weight(0, 100);
  grid->row_weight(1, 1);
  grid->margin(0);
  grid->gap(0);
  grid->end();
  win->end();
  win->resizable(grid);
  win->size_range(300, 100);

  win->show();
  Fl::add_timeout(1.0 / 30.0, timer_callback, video_win);
  Fl::run();
  work_guard.reset();
  io_context.stop();
  asio_thread.join();
}
