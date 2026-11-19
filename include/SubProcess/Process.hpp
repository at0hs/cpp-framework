#ifndef INCLUDE_SUBPROCESS_PROCESS_HPP
#define INCLUDE_SUBPROCESS_PROCESS_HPP

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <unistd.h>

#include "ProcessResult.hpp"
#include "StartInfo.hpp"

namespace Framework::SubProcess {
	class Process {
	public:
		static constexpr int kProcessFailed = -255;

	private:
		// 回収スレッドと Process 本体で共有する状態。
		// 回収スレッドは Process より長く生存し得るため、this ではなくこれを共有する。
		// exited は mutex 保護下で「子プロセスを回収 (reap) 済み」を表す。
		// シグナル送信も同じ mutex 下で exited を確認してから行うため、
		// 回収済み PID (再利用されている可能性がある) にシグナルを送ることはない。
		struct SharedState {
			std::mutex mutex;
			std::condition_variable condition;
			bool exited{ false };
		};

		StartInfo start_info_;
		pid_t id_{ -1 };
		std::shared_ptr<SharedState> state_;

	public:
		Process() = default;
		explicit Process(StartInfo start_info);

		// コピー不可（プロセスハンドルは一意）
		Process(const Process &) = delete;
		Process &operator=(const Process &) = delete;

		// ムーブ可
		Process(Process &&other) noexcept;
		Process &operator=(Process &&other) noexcept;

		// 実行中の子プロセスがあれば SIGKILL を送り、回収されるまで待機する
		~Process();

		// --- インスタンスメソッド ---

		// 非同期でプロセスを起動し、future<ProcessResult> を返す
		// @throws Framework::Exception (InvalidOperation) 既に実行中の場合
		std::future<ProcessResult> start_async();

		// プロセスを起動して終了まで待機し、ProcessResult を返す
		ProcessResult start();

		// プロセスを強制終了する（SIGTERM 後に SIGKILL）。回収されるまで待機する。
		void kill();

		// --- プロパティ ---

		pid_t id() const { return id_; }

		bool has_exited() const;

	private:
		// 子プロセスが回収済みでなければシグナルを送る。送信した場合 true。
		bool send_signal(int signal);

		// 回収されるまで待機する。timeout が負なら無期限。回収済みなら true。
		bool wait_for_exit(std::chrono::milliseconds timeout);

		// 実行中の子プロセスを SIGKILL で停止し、回収されるまで待機する
		void terminate_and_wait();

		// 子プロセス内で呼び出す。ILauncher 経由でコマンドを exec する。
		// stdout_fd / stderr_fd はパイプの書き込み端（-1 はリダイレクト無効）。
		static int child_process(const StartInfo *info, int stdout_fd, int stderr_fd);

		// 親プロセス側でパイプを読み込みながら子プロセスの終了を待ち、回収して ProcessResult
		// を構築する
		static ProcessResult collect_result(pid_t pid,
											std::chrono::system_clock::time_point start_time,
											int stdout_read_fd, int stderr_read_fd,
											SharedState &state);
	};
} // namespace Framework::SubProcess

#endif // INCLUDE_SUBPROCESS_PROCESS_HPP
