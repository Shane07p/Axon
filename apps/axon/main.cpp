#include <signal.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
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
#include "axon/net/endpoint.hpp"
#include "axon/net/socket.hpp"
#include "axon/storage/dataset.hpp"
#include "axon/transfer/client.hpp"
#include "axon/transfer/server.hpp"
#include "axon/version.hpp"

namespace {

constexpr std::string_view usage =
    "usage: axon serve --root DIR [--port PORT] [--bind ADDRESS]\n"
    "                  [--fail-prob P [--seed N]]   cut connections on purpose, for testing\n"
    "       axon fetch HOST[:PORT] TOPIC [--output DIR]\n"
    "       axon list HOST[:PORT]\n"
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

double parse_probability(std::string_view s) {
    double p = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), p);
    if (ec != std::errc{} || end != s.data() + s.size() || !(p >= 0 && p <= 1)) {
        throw UsageError(std::format("--fail-prob needs a number from 0 to 1, not '{}'", s));
    }
    return p;
}

std::uint64_t parse_seed(std::string_view s) {
    std::uint64_t seed = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), seed);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        throw UsageError(std::format("--seed needs a whole number, not '{}'", s));
    }
    return seed;
}

std::uint64_t random_seed() {
    std::uint64_t seed = 0;
    if (::getrandom(&seed, sizeof seed, 0) != static_cast<ssize_t>(sizeof seed)) {
        seed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    return seed;
}

// "5.0 MB": decimal units, to match MB/s.
std::string human_bytes(std::uint64_t n) {
    constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};
    auto value = static_cast<double>(n);
    std::size_t unit = 0;
    while (value >= 1000 && unit + 1 < units.size()) {
        value /= 1000;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", n) : std::format("{:.1f} {}", value, units[unit]);
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
    const Options opts =
        parse_options(args, {"--root", "--port", "--bind", "--fail-prob", "--seed"});
    if (!opts.contains("--root")) {
        throw UsageError("serve needs --root DIR");
    }
    if (opts.contains("--seed") && !opts.contains("--fail-prob")) {
        throw UsageError("--seed only makes sense with --fail-prob");
    }
    axon::ServerConfig config;
    if (opts.contains("--fail-prob")) {
        config.fault.probability = parse_probability(opts.at("--fail-prob"));
        config.fault.seed = opts.contains("--seed") ? parse_seed(opts.at("--seed")) : random_seed();
    }
    const std::uint16_t port =
        opts.contains("--port") ? axon::parse_port(opts.at("--port")) : default_port;
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

    axon::Server server{std::move(dataset), axon::Listener::listen(bind, port), config};
    axon::log_msg("serving {} topic(s), {} file(s) on {}:{} (Ctrl+C to stop)", topics, files, bind,
                  server.port());
    if (config.fault.probability > 0) {
        axon::log_msg("fault injection: each connection is cut with probability {} (--seed {})",
                      config.fault.probability, config.fault.seed);
    }
    g_server.store(&server);
    if (g_stop_requested.load()) {
        server.stop();  // Ctrl+C arrived while scanning
    }
    server.run();
    g_server.store(nullptr);
    axon::log_msg("stopped");
    return 0;
}

int fetch(std::span<char* const> args) {
    if (args.size() < 2) {
        throw UsageError("fetch needs HOST[:PORT] and TOPIC");
    }
    const axon::Endpoint server = axon::parse_endpoint(args[0], default_port);
    const std::string topic{args[1]};
    const Options opts = parse_options(args.subspan(2), {"--output"});
    const std::filesystem::path output{opts.contains("--output") ? opts.at("--output") : "."};

    const axon::FetchStats stats = axon::fetch(
        {.host = server.host,
         .port = server.port,
         .topic = topic,
         .output = output,
         .on_retry = [](const std::string& reason, std::chrono::milliseconds wait) {
             std::cerr << std::format("axon: {}; reconnecting in {} ms\n", reason, wait.count());
         }});
    const double secs = stats.elapsed.count();
    const std::size_t reconnects = stats.connections - 1;
    std::cout << std::format("fetched {}: {} file(s), {} in {:.2f} s ({:.1f} MB/s) into {}{}\n",
                             topic, stats.files, human_bytes(stats.bytes), secs,
                             static_cast<double>(stats.bytes) / 1e6 / std::max(secs, 1e-9),
                             (output / topic).string(),
                             reconnects > 0 ? std::format(", {} reconnect(s)", reconnects) : "");
    return 0;
}

int list(std::span<char* const> args) {
    if (args.size() != 1) {
        throw UsageError("list needs HOST[:PORT] and nothing else");
    }
    const axon::Endpoint server = axon::parse_endpoint(args[0], default_port);
    const auto topics = axon::list_topics(server.host, server.port);
    if (topics.empty()) {
        std::cout << "no topics\n";
        return 0;
    }
    std::size_t width = std::string_view{"TOPIC"}.size();
    for (const auto& t : topics) {
        width = std::max(width, t.name.size());
    }
    std::cout << std::format("{:<{}}  {:>7}  {:>9}\n", "TOPIC", width, "FILES", "SIZE");
    for (const auto& t : topics) {  // names were validated: no control characters
        std::cout << std::format("{:<{}}  {:>7}  {:>9}\n", t.name, width, t.files,
                                 human_bytes(t.bytes));
    }
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
        if (command == "fetch") {
            return fetch(args.subspan(1));
        }
        if (command == "list") {
            return list(args.subspan(1));
        }
        throw UsageError(std::format("unknown command '{}'", command));
    } catch (const UsageError& e) {
        std::cerr << "axon: " << e.what() << '\n' << usage;
        return 2;
    } catch (const std::invalid_argument& e) {  // a malformed port, address or topic
        std::cerr << "axon: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "axon: error: " << e.what() << '\n';
        return 1;
    }
}
