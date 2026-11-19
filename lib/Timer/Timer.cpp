#include "Timer.hpp"
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

Timer::~Timer() {
	wait_for_stop();
}

void Timer::start() {
	if (active_) {
		return;
	}
	if (thread_.joinable()) {
		thread_.join();
	}
	active_ = true;
	thread_ = std::thread{ [this] {
		std::vector<std::function<void()>> handlers;
		{
			std::lock_guard lock(handlers_mutex_);
			handlers = event_handlers_;
		}
		while (true) {
			{
				// stop() で即座に起こせるよう、sleep_for ではなく条件変数で待つ
				std::unique_lock lock(state_mutex_);
				if (state_cv_.wait_for(lock, time_, [this] { return !active_.load(); })) {
					break;
				}
			}
			for (const auto &f : handlers) {
				std::invoke(f);
			}
			if (!this->restart_) {
				break;
			}
		}
		active_ = false;
	} };
}

void Timer::stop() {
	{
		std::scoped_lock const lock(state_mutex_);
		active_ = false;
	}
	state_cv_.notify_all();
}

void Timer::wait_for_stop() {
	stop();
	if (thread_.joinable()) {
		thread_.join();
	}
}
