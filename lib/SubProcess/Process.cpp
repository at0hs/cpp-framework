#include "SubProcess/Process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "Exception/SystemException.hpp"
#include "SubProcess/ExecFamily.hpp"
#include "SubProcess/Shell.hpp"

namespace Framework::SubProcess {

	namespace {
		void close_if_open(int &fd) {
			if (fd != -1) {
				::close(fd);
				fd = -1;
			}
		}

		// fd から読めるだけ読み、out に追記する（fd は O_NONBLOCK 前提）。
		// EOF またはエラーに達した場合は false を返す。
		bool drain_pipe(int fd, std::string &out) {
			std::array<char, 4096> buf{};
			while (true) {
				const ssize_t n = ::read(fd, buf.data(), buf.size());
				if (n > 0) {
					out.append(buf.data(), static_cast<size_t>(n));
					continue;
				}
				if (n < 0 && errno == EINTR) {
					continue;
				}
				if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					return true;
				}
				return false; // EOF またはエラー
			}
		}

		// 子プロセスが終了済みか（回収はしない）
		bool has_terminated(pid_t pid) {
			siginfo_t info{};
			if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
				return errno != EINTR; // ECHILD 等: 待つ対象がない
			}
			return info.si_pid == pid;
		}
	} // namespace

	// --- コンストラクタ / デストラクタ ---

	Process::Process(StartInfo start_info) : start_info_(std::move(start_info)) {}

	Process::Process(Process &&other) noexcept
		: start_info_(std::move(other.start_info_)),
		  id_(other.id_),
		  state_(std::move(other.state_)) {
		other.id_ = -1;
	}

	Process &Process::operator=(Process &&other) noexcept {
		if (this != &other) {
			terminate_and_wait();
			start_info_ = std::move(other.start_info_);
			id_ = other.id_;
			state_ = std::move(other.state_);
			other.id_ = -1;
		}
		return *this;
	}

	Process::~Process() {
		terminate_and_wait();
	}

	// --- インスタンスメソッド ---

	std::future<ProcessResult> Process::start_async() {
		if (id_ > 0 && !has_exited()) {
			throw Framework::Exception("Process::start_async: process is already running",
									   Error::Code::InvalidOperation);
		}

		auto start_time = std::chrono::system_clock::now();
		auto state = std::make_shared<SharedState>();

		// リダイレクト用パイプを作成
		std::array<int, 2> stdout_pipe{ -1, -1 };
		std::array<int, 2> stderr_pipe{ -1, -1 };
		auto close_all = [&] {
			close_if_open(stdout_pipe[0]);
			close_if_open(stdout_pipe[1]);
			close_if_open(stderr_pipe[0]);
			close_if_open(stderr_pipe[1]);
		};

		if (start_info_.redirect_standard_output()) {
			if (::pipe(stdout_pipe.data()) != 0) {
				throw Framework::SystemException("pipe() failed for stdout", errno);
			}
		}
		if (start_info_.redirect_standard_error()) {
			if (::pipe(stderr_pipe.data()) != 0) {
				const auto saved_errno = errno;
				close_all();
				throw Framework::SystemException("pipe() failed for stderr", saved_errno);
			}
		}

		pid_t pid = ::fork();
		if (pid < 0) {
			// fork 失敗
			const auto saved_errno = errno;
			close_all();
			throw Framework::SystemException("fork() failed", saved_errno);
		}

		if (pid == 0) {
			// 子プロセス: 読み込み端を閉じる
			close_if_open(stdout_pipe[0]);
			close_if_open(stderr_pipe[0]);
			int ret = child_process(&start_info_, stdout_pipe[1], stderr_pipe[1]);
			::_exit(ret);
		}

		// 親プロセス: 書き込み端を閉じる
		close_if_open(stdout_pipe[1]);
		close_if_open(stderr_pipe[1]);

		// スレッドで待機し、promise 経由で結果を返す。
		// スレッドは Process より長く生存し得るため this は捕捉せず、共有状態のみを保持する。
		std::promise<ProcessResult> promise;
		auto future = promise.get_future();

		try {
			std::thread([p = std::move(promise), pid, start_time, stdout_fd = stdout_pipe[0],
						 stderr_fd = stderr_pipe[0], state]() mutable {
				try {
					p.set_value(collect_result(pid, start_time, stdout_fd, stderr_fd, *state));
				} catch (...) {
					p.set_exception(std::current_exception());
				}
			}).detach();
		} catch (...) {
			// スレッドを起動できなかった: 子プロセスをここで停止・回収する
			::kill(pid, SIGKILL);
			int status{ 0 };
			while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
			close_all();
			throw;
		}

		id_ = pid;
		state_ = std::move(state);
		return future;
	}

	ProcessResult Process::start() {
		return start_async().get();
	}

	void Process::kill() {
		// まず SIGTERM を送り、プロセスが終了する機会を与える
		if (!send_signal(SIGTERM)) {
			return;
		}
		static constexpr auto grace_period = std::chrono::milliseconds(500);
		if (wait_for_exit(grace_period)) {
			return;
		}
		// まだ生きていれば SIGKILL
		send_signal(SIGKILL);
		wait_for_exit(std::chrono::milliseconds(-1));
	}

	bool Process::has_exited() const {
		if (!state_) {
			return false;
		}
		std::scoped_lock const lock(state_->mutex);
		return state_->exited;
	}

	bool Process::send_signal(int signal) {
		if (id_ <= 0 || !state_) {
			return false;
		}
		std::scoped_lock const lock(state_->mutex);
		if (state_->exited) {
			return false;
		}
		::kill(id_, signal);
		return true;
	}

	bool Process::wait_for_exit(std::chrono::milliseconds timeout) {
		if (!state_) {
			return true;
		}
		std::unique_lock lock(state_->mutex);
		if (timeout.count() < 0) {
			state_->condition.wait(lock, [this] { return state_->exited; });
			return true;
		}
		return state_->condition.wait_for(lock, timeout, [this] { return state_->exited; });
	}

	void Process::terminate_and_wait() {
		if (send_signal(SIGKILL)) {
			wait_for_exit(std::chrono::milliseconds(-1));
		}
	}

	// --- プライベート ---

	int Process::child_process(const StartInfo *info, int stdout_fd, int stderr_fd) {
		// stdout リダイレクト
		if (stdout_fd != -1) {
			if (::dup2(stdout_fd, STDOUT_FILENO) == -1) {
				return kProcessFailed;
			}
			::close(stdout_fd);
		}
		// stderr リダイレクト
		if (stderr_fd != -1) {
			if (::dup2(stderr_fd, STDERR_FILENO) == -1) {
				return kProcessFailed;
			}
			::close(stderr_fd);
		}

		// ILauncher 経由でコマンドを exec
		if (info->use_shell()) {
			Shell launcher;
			return launcher.launch(*info);
		}
		ExecFamily launcher;
		return launcher.launch(*info);
	}

	ProcessResult Process::collect_result(pid_t pid,
										  std::chrono::system_clock::time_point start_time,
										  int stdout_read_fd, int stderr_read_fd,
										  SharedState &state) {
		std::string stdout_str;
		std::string stderr_str;

		// stdout / stderr を poll で並行して読む。
		// 片方ずつ読み切る方式だと、子プロセスがもう一方のパイプバッファを埋めた時点で
		// 互いに待ち合ってデッドロックするため。
		std::array<pollfd, 2> fds{ { { stdout_read_fd, POLLIN, 0 },
									 { stderr_read_fd, POLLIN, 0 } } };
		std::array<std::string *, 2> outs{ &stdout_str, &stderr_str };
		for (auto &p : fds) {
			if (p.fd != -1) {
				::fcntl(p.fd, F_SETFL, ::fcntl(p.fd, F_GETFL) | O_NONBLOCK);
			}
		}

		auto drain_all = [&] {
			for (size_t i = 0; i < fds.size(); ++i) {
				if (fds[i].fd != -1 && !drain_pipe(fds[i].fd, *outs[i])) {
					close_if_open(fds[i].fd);
				}
			}
		};

		static constexpr int kPollIntervalMs = 50;
		while (fds[0].fd != -1 || fds[1].fd != -1) {
			// fd == -1 のエントリは poll に無視される
			const int ret = ::poll(fds.data(), fds.size(), kPollIntervalMs);
			if (ret < 0 && errno != EINTR) {
				break;
			}
			drain_all();
			// 子プロセスが終了しても、孫プロセスがパイプを保持していると EOF が来ない。
			// 子プロセス終了後は残りを読み切って打ち切る。
			if (ret == 0 && has_terminated(pid)) {
				drain_all();
				break;
			}
		}
		for (auto &p : fds) {
			close_if_open(p.fd);
		}

		// プロセス終了を待機する（まだ回収しない）
		siginfo_t info{};
		while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) != 0 &&
			   errno == EINTR) {}

		// 回収と exited の更新は mutex 下で行い、回収済み PID へのシグナル送信を防ぐ
		int status{ -1 };
		rusage usage{};
		{
			std::scoped_lock const lock(state.mutex);
			while (::wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {}
			state.exited = true;
		}
		state.condition.notify_all();

		auto exit_time = std::chrono::system_clock::now();

		ProcessResult result;
		result.has_exited_ = true;
		result.exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		result.usage_ = usage;
		result.start_time_ = start_time;
		result.exit_time_ = exit_time;
		result.standard_output_ = std::move(stdout_str);
		result.standard_error_ = std::move(stderr_str);
		return result;
	}

} // namespace Framework::SubProcess
