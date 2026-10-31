#ifndef KARU_CLIENT_HPP
#define KARU_CLIENT_HPP

#include "config.hpp"
#include "platform.hpp"
#include "runtime/engine.hpp"

#include <atomic>
#include <memory>
#include <mutex>

struct karu_client {
    explicit karu_client(karu::ConfigSnapshot snapshot)
        : config(std::move(snapshot)), mutex(std::make_unique<std::mutex>()),
          engine(std::make_unique<std::shared_ptr<karu::Engine>>()) {}

    ~karu_client() {
        const int owner = owner_pid.load(std::memory_order_acquire);
        if (owner != 0 && owner != karu::os::pid()) {
            // A locked mutex and a threaded Engine cannot be destroyed safely
            // in a forked child. Their pages disappear when the child exits.
            static_cast<void>(mutex.release());
            static_cast<void>(engine.release());
        }
    }

    std::shared_ptr<karu::Engine> acquire_engine() {
        const int process = karu::os::pid();
        const int owner = owner_pid.load(std::memory_order_acquire);
        if (owner != 0 && owner != process) {
            // The inherited mutex may have been held by a vanished thread.
            // Allocate replacements before abandoning the child-only copies.
            auto fresh_mutex = std::make_unique<std::mutex>();
            auto fresh_engine = std::make_unique<std::shared_ptr<karu::Engine>>();
            static_cast<void>(mutex.release());
            static_cast<void>(engine.release());
            mutex = std::move(fresh_mutex);
            engine = std::move(fresh_engine);
            owner_pid.store(0, std::memory_order_release);
        }
        std::lock_guard lock(*mutex);
        if (!*engine) {
            *engine = std::make_shared<karu::Engine>(config);
            owner_pid.store(process, std::memory_order_release);
        }
        return *engine;
    }

    karu::ConfigSnapshot config;
    std::unique_ptr<std::mutex> mutex;
    std::unique_ptr<std::shared_ptr<karu::Engine>> engine;
    std::atomic<int> owner_pid{0};
};

#endif
