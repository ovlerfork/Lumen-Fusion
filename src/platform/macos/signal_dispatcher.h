/**
 * @file src/platform/macos/signal_dispatcher.h
 * @brief Dispatch macOS process signals outside the POSIX signal context.
 */
#pragma once

#if defined(__APPLE__) || defined(__MACH__)

  #include <atomic>
  #include <cerrno>
  #include <csignal>
  #include <cstdint>
  #include <fcntl.h>
  #include <functional>
  #include <mutex>
  #include <thread>
  #include <utility>

  #include <unistd.h>

namespace macos_signal {
  /**
   * @brief Delivers SIGINT and SIGTERM callbacks from a dedicated normal thread.
   *
   * The signal handler writes a byte to a non-blocking pipe. All callback work,
   * including logging and shutdown coordination, runs on the worker thread.
   * Registration and stop are serialized by the caller; stop must not be called
   * from the callback. Only one dispatcher may own process dispositions at a time.
   */
  class dispatcher {
  public:
    dispatcher() = default;
    dispatcher(const dispatcher &) = delete;
    dispatcher &operator=(const dispatcher &) = delete;

    ~dispatcher() {
      stop();
    }

    /**
     * @brief Register a callback and then install its POSIX signal handler.
     * @return False when the handoff pipe or signal disposition could not be installed.
     */
    bool register_handler(int signal, std::function<void()> callback) {
      if ((signal != SIGINT && signal != SIGTERM) || stopped_ || !start()) {
        return false;
      }

      {
        std::lock_guard lock(callback_mutex_);
        callback_for(signal) = std::move(callback);
      }

      // Replacing a callback must not replace the original disposition.
      if (signal == SIGINT ? interrupt_registered_ : terminate_registered_) {
        return true;
      }

      struct sigaction action {};
      action.sa_handler = forward;
      sigemptyset(&action.sa_mask);
      action.sa_flags = SA_RESTART;

      struct sigaction previous {};
      if (sigaction(signal, &action, &previous) != 0) {
        return false;
      }

      if (signal == SIGINT) {
        previous_interrupt_ = previous;
        interrupt_registered_ = true;
      } else {
        previous_terminate_ = previous;
        terminate_registered_ = true;
      }
      return true;
    }

    /**
     * @brief Stop callback delivery and restore the signal dispositions.
     */
    void stop() {
      if (!running_.exchange(false)) {
        return;
      }

      stopping_.store(true);
      stopped_ = true;
      if (interrupt_registered_) {
        sigaction(SIGINT, &previous_interrupt_, nullptr);
      }
      if (terminate_registered_) {
        sigaction(SIGTERM, &previous_terminate_, nullptr);
      }

      // Sequential consistency pairs the handler's reader entry and fd load
      // with unpublication and draining here. A late handler sees -1; a handler
      // that saw the live fd must leave before that descriptor can be reused.
      write_fd_.store(-1);
      while (readers_.load() != 0) {
        std::this_thread::yield();
      }
      // Wake explicitly: a just-forked child may still hold a writer until exec.
      // EAGAIN means data is already queued; read() will observe stopping_.
      const std::uint8_t wake = 0;
      (void) write_retry(pipe_.write_fd, &wake);
      close(pipe_.write_fd);
      pipe_.write_fd = -1;
      worker_.join();
      pipe_.reset();
      owner_claimed_.store(false);
    }

  private:
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static inline std::atomic<int> write_fd_ {-1};
    static inline std::atomic<unsigned> readers_ {0};
    static inline std::atomic_bool owner_claimed_ {false};

    struct pipe_handles {
      int read_fd = -1;
      int write_fd = -1;

      ~pipe_handles() {
        reset();
      }

      void reset() noexcept {
        if (write_fd >= 0) {
          close(write_fd);
          write_fd = -1;
        }
        if (read_fd >= 0) {
          close(read_fd);
          read_fd = -1;
        }
      }
    };

    static ssize_t write_retry(int fd, const std::uint8_t *value) noexcept {
      ssize_t written;
      do {
        written = write(fd, value, sizeof(*value));
      } while (written < 0 && errno == EINTR);
      return written;
    }

    static void forward(int signal) noexcept {
      const int saved_errno = errno;
      const std::uint8_t value = static_cast<std::uint8_t>(signal);
      readers_.fetch_add(1);
      const int fd = write_fd_.load();
      if (fd >= 0) {
        (void) write_retry(static_cast<int>(fd), &value);
      }
      readers_.fetch_sub(1);
      errno = saved_errno;
    }

    static bool set_close_on_exec(int fd) {
      const int flags = fcntl(fd, F_GETFD);
      return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
    }

    bool start() {
      if (running_.load()) {
        return true;
      }

      bool unclaimed = false;
      if (!owner_claimed_.compare_exchange_strong(unclaimed, true)) {
        return false;
      }

      int fds[2];
      if (pipe(fds) != 0) {
        owner_claimed_.store(false);
        return false;
      }
      pipe_.read_fd = fds[0];
      pipe_.write_fd = fds[1];
      const int flags = fcntl(fds[1], F_GETFL);
      if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) != 0 || !set_close_on_exec(fds[0]) || !set_close_on_exec(fds[1])) {
        pipe_.reset();
        owner_claimed_.store(false);
        return false;
      }

      stopping_.store(false);
      try {
        worker_ = std::thread([this]() {
          run();
        });
      } catch (...) {
        pipe_.reset();
        owner_claimed_.store(false);
        return false;
      }
      write_fd_.store(pipe_.write_fd);
      running_.store(true);
      return true;
    }

    void run() {
      for (;;) {
        std::uint8_t signal = 0;
        const ssize_t bytes_read = read(pipe_.read_fd, &signal, sizeof(signal));
        if (bytes_read < 0 && errno == EINTR) {
          continue;
        }
        if (bytes_read != static_cast<ssize_t>(sizeof(signal)) || stopping_.load()) {
          return;
        }

        std::function<void()> callback;
        {
          std::lock_guard lock(callback_mutex_);
          callback = callback_for(signal);
        }
        if (callback) {
          callback();
        }
      }
    }

    std::function<void()> &callback_for(int signal) {
      return signal == SIGINT ? interrupt_callback_ : terminate_callback_;
    }

    std::atomic_bool running_ {false};
    std::atomic_bool stopping_ {false};
    bool stopped_ = false;
    pipe_handles pipe_;
    std::thread worker_;
    std::mutex callback_mutex_;
    std::function<void()> interrupt_callback_;
    std::function<void()> terminate_callback_;
    struct sigaction previous_interrupt_ {};
    struct sigaction previous_terminate_ {};
    bool interrupt_registered_ = false;
    bool terminate_registered_ = false;
  };
}  // namespace macos_signal

#endif
