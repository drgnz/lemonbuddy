#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <deque>

#include "eventloop.hpp"
#include "services/command.hpp"
#include "utils/io.hpp"
#include "utils/macros.hpp"

EventLoop::EventLoop(std::string input_pipe)
  : bar(get_bar()),
    registry(std::make_shared<Registry>()),
    logger(get_logger()),
    state(STATE_STOPPED),
    pipe_filename(input_pipe)
{
  if (this->pipe_filename.empty())
    return;
  if (io::file::is_fifo(this->pipe_filename))
    return;
  if (io::file::exists(this->pipe_filename))
    unlink(this->pipe_filename.c_str());
  if (mkfifo(this->pipe_filename.c_str(), 0600) == -1)
    throw EventLoopTerminate(StrErrno());
}

bool EventLoop::running()
{
  return this->state() == STATE_STARTED;
}

void EventLoop::start()
{
  if (this->state() == STATE_STARTED)
    return;

  this->logger->debug("Starting event loop...");

  this->bar->load(registry);

  this->registry->load([&](std::string module_name){
    this->logger->debug("Adding stdin subscriber: "+ module_name);
    this->stdin_subs.emplace_back(module_name);
  });

  this->state = STATE_STARTED;

  this->t_write = std::thread(&EventLoop::loop_write, this);
  this->t_read = std::thread(&EventLoop::loop_read, this);

  this->logger->debug("Event loop started...");
}

void EventLoop::stop()
{
  if (this->state() == STATE_STOPPED)
    return;

  this->logger->debug("Stopping event loop...");

  this->state = STATE_STOPPED;

  // break the input read block - totally how it's meant to be done!
  if (!this->pipe_filename.empty()) {
    int status  = std::system(("echo >"+this->pipe_filename).c_str());
    log_trace(std::to_string(status));
  }

  this->registry->unload();

  this->logger->debug("Event loop stopped...");
}

void EventLoop::wait()
{
  if (!this->running())
    return;

  while (!this->registry->ready())
    std::this_thread::sleep_for(100ms);

  int sig = 0;

  sigset_t wait_mask;
  sigemptyset(&wait_mask);
  sigaddset(&wait_mask, SIGINT);
  sigaddset(&wait_mask, SIGQUIT);
  sigaddset(&wait_mask, SIGTERM);

  if (pthread_sigmask(SIG_BLOCK, &wait_mask, nullptr) == -1)
    logger->fatal(StrErrno());

  // Ignore SIGPIPE since we'll handle it manually
  signal(SIGPIPE, SIG_IGN);

  // Wait for termination signal
  sigwait(&wait_mask, &sig);

  this->logger->info("Termination signal received... Shutting down");
}

void EventLoop::loop_write()
{
  std::deque<std::chrono::high_resolution_clock::time_point> ticks;

  // Allow <throttle_limit>  ticks within <throttle_ms> timeframe
  const auto throttle_limit = config::get<unsigned int>("settings", "throttle_limit", 5);
  const auto throttle_ms = std::chrono::duration<double, std::milli>(config::get<unsigned int>("settings", "throttle_ms", 50));

  while (this->running()) {
    try {
      if (!this->registry->wait())
        continue;

      auto now = std::chrono::high_resolution_clock::now();

      // Expire previous ticks
      while (ticks.size() > 0) {
        if ((now - ticks.front()) < throttle_ms)
          break;

        ticks.pop_front();
      }

      // Place the new tick in the bottom of the deck
      ticks.emplace_back(std::chrono::high_resolution_clock::now());

      // Have we reached the limit?
      if (ticks.size() >= throttle_limit) {
        log_debug("Throttling write to stdout");

        std::this_thread::sleep_for(throttle_ms * ticks.size());

        if (ticks.size() - 1 >= throttle_limit)
          continue;
      }

      this->write_stdout();
    } catch (Exception &e) {
      this->logger->error(e.what());

      auto pid = proc::get_process_id();
      proc::kill(pid, SIGTERM);
      proc::wait_for_completion(pid);

      return;
    }
  }
}

void EventLoop::loop_read()
{
  while (!this->pipe_filename.empty() && this->running()) {
    try {
      if ((this->fd_stdin = ::open(this->pipe_filename.c_str(), O_RDONLY)) == -1)
        throw EventLoopTerminate(StrErrno());

      this->read_stdin();
    } catch (Exception &e) {
      this->logger->error(e.what());
      return;
    }
  }

  if (!this->pipe_filename.empty()) {
    close(this->fd_stdin);
    unlink(this->pipe_filename.c_str());
  }
}

void EventLoop::read_stdin()
{
  std::string input;

  while ((input = io::readline(this->fd_stdin)).empty() == false) {
    this->logger->debug("Input value: \""+ input +"\"");

    bool input_processed = false;

    for (auto &module_name : this->stdin_subs) {
      if (this->registry->find(module_name)->module->handle_command(input)) {
        input_processed = true;
        break;
      }
    }

    if (!input_processed) {
      this->logger->debug("Unrecognized input value");
      this->logger->debug("Forwarding input to shell");

      auto command = std::make_unique<Command>("/usr/bin/env\nsh\n-c\n"+ input);

      try {
        command->exec(false);
        command->tail([](std::string cmd_output){
          get_logger()->debug("| "+ cmd_output);
        });
        command->wait();
      } catch (CommandException &e) {
        this->logger->error(e.what());
      }
    }

    return;
  }
}

void EventLoop::write_stdout()
{
  if (!this->registry->ready())
    return;

  try {
    auto data = this->bar->get_output();

    if (!this->running())
      return;

    if (dprintf(this->fd_stdout, "%s\n", data.c_str()) == -1)
      throw EventLoopTerminate("Failed to write to stdout");
  } catch (RegistryError &e) {
    this->logger->error(e.what());
  }
}

void EventLoop::cleanup(int timeout_ms)
{
  this->logger->debug("Cleaning up...");

  std::atomic<bool> t_read_joined(false);
  std::atomic<bool> t_write_joined(false);

  std::thread t_timeout([&]{
    auto start = std::chrono::system_clock::now();

    while (true) {
      std::this_thread::sleep_for(20ms);

      if (t_read_joined && t_write_joined)
        break;

      auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now() - start);

      if (dur.count() > timeout_ms)
        throw EventLoopTerminateTimeout();
    }
  });

  this->logger->debug("Joining input thread");
  if (this->t_read.joinable())
    this->t_read.join();
  else
    this->logger->debug("Input thread not joinable");
  t_read_joined = true;

  this->logger->debug("Joining output thread");
  if (this->t_write.joinable())
    this->t_write.join();
  else
    this->logger->debug("Output thread not joinable");
  t_write_joined = true;

  this->logger->debug("Joining timeout thread");
  if (t_timeout.joinable())
    t_timeout.join();
  else
    this->logger->debug("Timeout thread not joinable");
}
