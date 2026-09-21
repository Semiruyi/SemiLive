#include <semilive/receiver/infrastructure/output/ffplay/ffplay_video_output_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace semilive::receiver::infra::output {
namespace {

using contracts::output::LiveVideoOutputIssue;
using contracts::output::LiveVideoOutputOperation;

[[nodiscard]] LiveVideoOutputIssue issue(
    const LiveVideoOutputOperation operation,
    const std::int64_t native_code,
    std::string message) {
    return {operation, native_code, std::move(message)};
}

struct ProcessIssue {
    std::int64_t native_code = 0;
    std::string message;
};

using ProcessResult = std::expected<void, ProcessIssue>;

#ifdef _WIN32

[[nodiscard]] std::expected<std::wstring, ProcessIssue> utf8_to_wide(
    const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (required == 0) {
        const auto error = static_cast<std::int64_t>(GetLastError());
        return std::unexpected{ProcessIssue{
            error, "failed to convert an ffplay argument to UTF-16"}};
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), converted.data(),
                            required) == 0) {
        const auto error = static_cast<std::int64_t>(GetLastError());
        return std::unexpected{ProcessIssue{
            error, "failed to convert an ffplay argument to UTF-16"}};
    }
    return converted;
}

[[nodiscard]] std::wstring quote_windows_argument(
    const std::wstring& argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted{L'\"'};
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

class ChildProcess final {
public:
    ChildProcess() = default;
    ~ChildProcess() {
        abort();
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] ProcessResult start(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments) {
        if (process_ != nullptr || stdin_write_ != nullptr) {
            return std::unexpected{
                ProcessIssue{0, "ffplay process is already running"}};
        }

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE stdin_read = nullptr;
        HANDLE stdin_write = nullptr;
        if (CreatePipe(&stdin_read, &stdin_write, &attributes, 0) == 0) {
            const auto error = static_cast<std::int64_t>(GetLastError());
            return std::unexpected{
                ProcessIssue{error, "failed to create the ffplay input pipe"}};
        }
        if (SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) == 0) {
            const auto error = static_cast<std::int64_t>(GetLastError());
            CloseHandle(stdin_read);
            CloseHandle(stdin_write);
            return std::unexpected{ProcessIssue{
                error, "failed to configure the ffplay input pipe"}};
        }

        std::wstring command_line =
            quote_windows_argument(executable.native());
        for (const auto& argument : arguments) {
            const auto wide = utf8_to_wide(argument);
            if (!wide) {
                CloseHandle(stdin_read);
                CloseHandle(stdin_write);
                return std::unexpected{wide.error()};
            }
            command_line.push_back(L' ');
            command_line.append(quote_windows_argument(*wide));
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process_info{};
        auto mutable_command_line = command_line;
        const auto created = CreateProcessW(
            nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0,
            nullptr, nullptr, &startup, &process_info);
        const auto create_error =
            created == 0 ? static_cast<std::int64_t>(GetLastError()) : 0;
        CloseHandle(stdin_read);
        if (created == 0) {
            CloseHandle(stdin_write);
            return std::unexpected{
                ProcessIssue{create_error, "failed to start ffplay"}};
        }

        CloseHandle(process_info.hThread);
        process_ = process_info.hProcess;
        stdin_write_ = stdin_write;
        return {};
    }

    [[nodiscard]] ProcessResult write_all(
        const std::span<const std::byte> bytes) {
        if (stdin_write_ == nullptr) {
            return std::unexpected{
                ProcessIssue{0, "ffplay input pipe is not open"}};
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (WriteFile(stdin_write_, bytes.data() + offset, chunk, &written,
                          nullptr) == 0) {
                const auto error = static_cast<std::int64_t>(GetLastError());
                return std::unexpected{ProcessIssue{
                    error, "failed to write an H.264 access unit to ffplay"}};
            }
            if (written == 0) {
                return std::unexpected{ProcessIssue{
                    0, "ffplay input pipe accepted zero bytes"}};
            }
            offset += static_cast<std::size_t>(written);
        }
        return {};
    }

    void finish() noexcept {
        close_stdin();
        if (process_ == nullptr) {
            return;
        }
        constexpr DWORD graceful_timeout_ms = 5'000;
        if (WaitForSingleObject(process_, graceful_timeout_ms) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_, 1);
            (void)WaitForSingleObject(process_, INFINITE);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }

    void abort() noexcept {
        terminate();
        close_stdin();
    }

    void terminate() noexcept {
        if (process_ == nullptr) {
            return;
        }
        (void)TerminateProcess(process_, 1);
        (void)WaitForSingleObject(process_, INFINITE);
        CloseHandle(process_);
        process_ = nullptr;
    }

private:
    void close_stdin() noexcept {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
    }

    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
};

#else

class ChildProcess final {
public:
    ChildProcess() = default;
    ~ChildProcess() {
        abort();
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] ProcessResult start(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments) {
        if (pid_ > 0 || stdin_write_ >= 0) {
            return std::unexpected{
                ProcessIssue{0, "ffplay process is already running"}};
        }

        int input_socket[2]{};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, input_socket) != 0) {
            return std::unexpected{ProcessIssue{
                errno, "failed to create the ffplay input channel"}};
        }

        posix_spawn_file_actions_t actions{};
        auto action_error = posix_spawn_file_actions_init(&actions);
        if (action_error == 0) {
            action_error =
                posix_spawn_file_actions_adddup2(&actions, input_socket[0],
                                                  STDIN_FILENO);
        }
        if (action_error == 0 && input_socket[0] != STDIN_FILENO) {
            action_error =
                posix_spawn_file_actions_addclose(&actions, input_socket[0]);
        }
        if (action_error == 0) {
            action_error =
                posix_spawn_file_actions_addclose(&actions, input_socket[1]);
        }
        if (action_error != 0) {
            (void)posix_spawn_file_actions_destroy(&actions);
            (void)close(input_socket[0]);
            (void)close(input_socket[1]);
            return std::unexpected{ProcessIssue{
                action_error, "failed to configure the ffplay process"}};
        }

        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1U);
        storage.push_back(executable.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1U);
        for (auto& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        pid_t child = -1;
        const auto spawn_error = posix_spawnp(
            &child, storage.front().c_str(), &actions, nullptr, argv.data(),
            environ);
        (void)posix_spawn_file_actions_destroy(&actions);
        (void)close(input_socket[0]);
        if (spawn_error != 0) {
            (void)close(input_socket[1]);
            return std::unexpected{
                ProcessIssue{spawn_error, "failed to start ffplay"}};
        }

        pid_ = child;
        stdin_write_ = input_socket[1];
        return {};
    }

    [[nodiscard]] ProcessResult write_all(
        const std::span<const std::byte> bytes) {
        if (stdin_write_ < 0) {
            return std::unexpected{
                ProcessIssue{0, "ffplay input pipe is not open"}};
        }

        ProcessResult result{};
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(
                    std::numeric_limits<ssize_t>::max()));
            const auto written = ::send(stdin_write_, bytes.data() + offset,
                                        chunk, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                result = std::unexpected{ProcessIssue{
                    errno, "failed to write an H.264 access unit to ffplay"}};
                break;
            }
            if (written == 0) {
                result = std::unexpected{ProcessIssue{
                    0, "ffplay input pipe accepted zero bytes"}};
                break;
            }
            offset += static_cast<std::size_t>(written);
        }

        return result;
    }

    void finish() noexcept {
        close_stdin();
        wait_for_exit(false);
    }

    void abort() noexcept {
        terminate();
        close_stdin();
    }

    void terminate() noexcept {
        wait_for_exit(true);
    }

private:
    void close_stdin() noexcept {
        if (stdin_write_ >= 0) {
            (void)close(stdin_write_);
            stdin_write_ = -1;
        }
    }

    void wait_for_exit(const bool terminate) noexcept {
        if (pid_ <= 0) {
            return;
        }
        if (terminate) {
            (void)kill(pid_, SIGTERM);
        }
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    pid_t pid_ = -1;
    int stdin_write_ = -1;
};

#endif

}  // namespace

struct FfplayVideoOutputBackend::Impl {
    enum class State : std::uint8_t {
        Closed,
        Open,
        Closing,
        Failed,
    };

    explicit Impl(FfplayVideoOutputConfig config)
        : config_{std::move(config)} {}

    ~Impl() {
        close(contracts::output::LiveVideoOutputCloseMode::Abort);
    }

    [[nodiscard]] contracts::output::LiveVideoOutputOpenResult open();
    [[nodiscard]] contracts::output::LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit);
    void close(contracts::output::LiveVideoOutputCloseMode mode) noexcept;
    void write_loop() noexcept;

    FfplayVideoOutputConfig config_;
    ChildProcess process_;
    std::mutex mutex_;
    std::condition_variable state_changed_;
    std::deque<model::TimedH264AccessUnit> queue_;
    std::size_t buffered_access_units_ = 0;
    std::size_t buffered_bytes_ = 0;
    bool abort_requested_ = false;
    State state_ = State::Closed;
    std::optional<LiveVideoOutputIssue> asynchronous_issue_;
    std::thread writer_;
};

contracts::output::LiveVideoOutputOpenResult
FfplayVideoOutputBackend::Impl::open() {
    std::lock_guard lock{mutex_};
    if (state_ != State::Closed) {
        return std::unexpected{issue(
            LiveVideoOutputOperation::State, 0,
            "ffplay output backend can only open from the closed state")};
    }
    if (config_.executable_path.empty()) {
        return std::unexpected{issue(LiveVideoOutputOperation::Open, 0,
                                     "ffplay executable path must not be empty")};
    }
    if (config_.maximum_buffered_access_units == 0 ||
        config_.maximum_buffered_bytes == 0) {
        return std::unexpected{issue(
            LiveVideoOutputOperation::Open, 0,
            "ffplay output queue limits must be positive")};
    }

    const auto started =
        process_.start(config_.executable_path, config_.arguments);
    if (!started) {
        return std::unexpected{issue(LiveVideoOutputOperation::Open,
                                     started.error().native_code,
                                     started.error().message)};
    }

    queue_.clear();
    buffered_access_units_ = 0;
    buffered_bytes_ = 0;
    abort_requested_ = false;
    asynchronous_issue_.reset();
    state_ = State::Open;
    try {
        writer_ = std::thread{[this] { write_loop(); }};
    } catch (...) {
        state_ = State::Closed;
        process_.abort();
        throw;
    }
    return contracts::output::LiveVideoOutputInfo{
        "ffplay:" + config_.executable_path.string()};
}

contracts::output::LiveVideoOutputSubmitResult
FfplayVideoOutputBackend::Impl::submit(
    model::TimedH264AccessUnit access_unit) {
    const auto bytes = access_unit.access_unit().annex_b().size();
    std::lock_guard lock{mutex_};
    if (state_ == State::Failed && asynchronous_issue_) {
        return std::unexpected{*asynchronous_issue_};
    }
    if (state_ != State::Open) {
        return std::unexpected{issue(
            LiveVideoOutputOperation::State, 0,
            "ffplay output backend must be open before submitting")};
    }
    if (bytes == 0) {
        return std::unexpected{issue(LiveVideoOutputOperation::Submit, 0,
                                     "H.264 access unit must not be empty")};
    }

    const auto bytes_would_overflow =
        bytes > config_.maximum_buffered_bytes -
                    std::min(buffered_bytes_,
                             config_.maximum_buffered_bytes);
    if (buffered_access_units_ >= config_.maximum_buffered_access_units ||
        bytes_would_overflow) {
        return contracts::output::LiveVideoSubmitReceipt{
            contracts::output::LiveVideoSubmitStatus::DroppedBackpressure, 0};
    }

    queue_.push_back(std::move(access_unit));
    ++buffered_access_units_;
    buffered_bytes_ += bytes;
    state_changed_.notify_one();
    return contracts::output::LiveVideoSubmitReceipt{
        contracts::output::LiveVideoSubmitStatus::Accepted, bytes};
}

void FfplayVideoOutputBackend::Impl::close(
    const contracts::output::LiveVideoOutputCloseMode mode) noexcept {
    bool abort_process = false;
    {
        std::lock_guard lock{mutex_};
        if (state_ == State::Closed) {
            return;
        }
        abort_process =
            mode == contracts::output::LiveVideoOutputCloseMode::Abort ||
            state_ == State::Failed;
        abort_requested_ = abort_process;
        state_ = State::Closing;
        if (abort_process) {
            queue_.clear();
            buffered_access_units_ = 0;
            buffered_bytes_ = 0;
        }
    }
    state_changed_.notify_all();

    if (abort_process) {
        process_.terminate();
    }
    if (writer_.joinable()) {
        writer_.join();
    }
    if (abort_process) {
        process_.abort();
    } else {
        process_.finish();
    }

    std::lock_guard lock{mutex_};
    queue_.clear();
    buffered_access_units_ = 0;
    buffered_bytes_ = 0;
    abort_requested_ = false;
    asynchronous_issue_.reset();
    state_ = State::Closed;
}

void FfplayVideoOutputBackend::Impl::write_loop() noexcept {
    for (;;) {
        std::optional<model::TimedH264AccessUnit> access_unit;
        std::size_t bytes = 0;
        {
            std::unique_lock lock{mutex_};
            state_changed_.wait(lock, [this] {
                return abort_requested_ || !queue_.empty() ||
                       state_ == State::Closing;
            });
            if (abort_requested_) {
                return;
            }
            if (queue_.empty()) {
                return;
            }
            bytes = queue_.front().access_unit().annex_b().size();
            access_unit.emplace(std::move(queue_.front()));
            queue_.pop_front();
        }

        const auto written =
            process_.write_all(access_unit->access_unit().annex_b());
        {
            std::lock_guard lock{mutex_};
            if (buffered_access_units_ > 0) {
                --buffered_access_units_;
            }
            buffered_bytes_ =
                bytes > buffered_bytes_ ? 0 : buffered_bytes_ - bytes;
            if (!written) {
                asynchronous_issue_ = issue(
                    LiveVideoOutputOperation::Submit,
                    written.error().native_code, written.error().message);
                state_ = State::Failed;
                queue_.clear();
                buffered_access_units_ = 0;
                buffered_bytes_ = 0;
                state_changed_.notify_all();
                return;
            }
        }
    }
}

FfplayVideoOutputBackend::FfplayVideoOutputBackend(
    FfplayVideoOutputConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

FfplayVideoOutputBackend::~FfplayVideoOutputBackend() = default;

contracts::output::LiveVideoOutputOpenResult
FfplayVideoOutputBackend::open() {
    return impl_->open();
}

contracts::output::LiveVideoOutputSubmitResult
FfplayVideoOutputBackend::submit(model::TimedH264AccessUnit access_unit) {
    return impl_->submit(std::move(access_unit));
}

void FfplayVideoOutputBackend::close(
    const contracts::output::LiveVideoOutputCloseMode mode) noexcept {
    impl_->close(mode);
}

}  // namespace semilive::receiver::infra::output
