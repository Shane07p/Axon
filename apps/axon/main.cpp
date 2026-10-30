#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "axon/log.hpp"
#include "axon/net/socket.hpp"
#include "axon/storage/dataset.hpp"
#include "axon/transfer/server.hpp"
#include "axon/version.hpp"

namespace {

constexpr std::string_view usage =
    "usage: axon serve --root DIR [--port PORT] [--bind ADDRESS]\n"
    "       axon --version\n";

constexpr std::uint16_t default_port = 9000;

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Options = std::map<std::string_view, std::string_view>;

// Parses "--name value" pairs, allowing only the names in `known`.
Options parse_options(std::span<char* const> args, std::initializer_list<std::string_view> known) {
    Options out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view name = args[i];
        if (std::ranges::find(known, name) == known.end()) {
            throw UsageError(std::format("unknown option '{}'", name));
        }
        if (i + 1 == args.size()) {
            throw UsageError(std::format("{} needs a value", name));
        }
        if (!out.emplace(name, args[++i]).second) {
            throw UsageError(std::format("{} given twice", name));
        }
    }
    return out;
}

std::uint16_t parse_port(std::string_view s) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || value > 65535) {
        throw UsageError(std::format("invalid port '{}'", s));
    }
    return static_cast<std::uint16_t>(value);
}

// --- Ctrl+C ---

std::atomic<axon::Server*> g_server{nullptr};
std::atomic<bool> g_stop_requested{false};

void on_stop_signal(int /*sig*/) {
    if (g_stop_requested.exchange(true)) {
        ::_exit(130);  // second Ctrl+C: give up on a clean shutdown
    }
    if (axon::Server* server = g_server.load()) {
        server->stop();  // async-signal-safe
    }
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // Writing to a closed pipe (e.g. stderr into `head`) should fail, not kill us.
    sa.sa_handler = [](int) {};
    sigaction(SIGPIPE, &sa, nullptr);
}

// --- commands ---

int serve(std::span<char* const> args) {
    const Options opts = parse_options(args, {"--root", "--port", "--bind"});
    if (!opts.contains("--root")) {
        throw UsageError("serve needs --root DIR");
    }
    const std::uint16_t port = opts.contains("--port") ? parse_port(opts.at("--port")) : default_port;
    const std::string bind{opts.contains("--bind") ? opts.at("--bind") : "0.0.0.0"};
    install_signal_handlers();

    auto dataset = axon::Dataset::scan(std::filesystem::path{opts.at("--root")});
    for (const auto& warning : dataset.warnings()) {
        axon::log_msg("warning: {}", warning);
    }
    std::size_t files = 0;
    for (const auto& topic : dataset.topics()) {
        files += topic.files.size();
    }
    const std::size_t topics = dataset.topics().size();

    axon::Server server{std::move(dataset), axon::Listener::listen(bind, port)};
    axon::log_msg("serving {} topic(s), {} file(s) on {}:{} (Ctrl+C to stop)", topics, files, bind,
                  server.port());
    g_server.store(&server);
    if (g_stop_requested.load()) {
        server.stop();  // Ctrl+C arrived while scanning
    }
    server.run();
    g_server.store(nullptr);
    axon::log_msg("stopped");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char* const> args{argv + 1, static_cast<std::size_t>(std::max(argc - 1, 0))};
    if (args.empty()) {
        std::cerr << usage;
        return 2;
    }
    const std::string_view command = args[0];
    try {
        if (command == "--version") {
            std::cout << "axon " << axon::version() << '\n';
            return 0;
        }
        if (command == "--help" || command == "-h") {
            std::cout << usage;
            return 0;
        }
        if (command == "serve") {
            return serve(args.subspan(1));
        }
        throw UsageError(std::format("unknown command '{}'", command));
    } catch (const UsageError& e) {
        std::cerr << "axon: " << e.what() << '\n' << usage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "axon: error: " << e.what() << '\n';
        return 1;
    }
}
