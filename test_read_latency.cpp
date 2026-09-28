#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
 
static std::atomic<bool> g_stop{false};
 
struct Options {
    bool server_mode = false;
    std::string host;
    std::string port;
    std::string log_file = "tcp_read_latency.log";
    std::size_t read_size = 512 * 1024;
    std::uint64_t slow_ms = 10;
    int progress_sec = 1;
    std::uint64_t requests = 0;
};
 
struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t responses = 0;
    std::uint64_t bytes = 0;
    std::uint64_t slow_responses = 0;
    std::uint64_t errors = 0;
};
 
static void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}
 
static std::string now_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::tm tmv;
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z", &tmv);
    return buf;
}

static void usage(const char *argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --client HOST:PORT [options]\n"
        << "  " << argv0 << " --server PORT [options]\n\n"
        << "Options:\n"
        << "  --out FILE          Client slow-response log file, default tcp_read_latency.log\n"
        << "  --read-size BYTES   Bytes returned by server per request, default 524288\n"
        << "  --slow-ms N         Client logs responses slower than N ms, default 10\n"
        << "  --progress-sec N    Progress refresh interval, default 1\n"
        << "  --requests N        Client stops after N requests, default 0 means forever\n"
        << "  --help              Show this help\n";
}
 
static bool split_host_port(const std::string &s, std::string &host, std::string &port) {
    auto pos = s.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= s.size()) {
        return false;
    }
    host = s.substr(0, pos);
    port = s.substr(pos + 1);
    return true;
}
 
static Options parse_args(int argc, char **argv) {
    Options opt;
 
    auto need_value = [&](int &i, const std::string &name) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << name << "\n";
            std::exit(2);
        }
        return argv[++i];
    };
 
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--client" || a == "--connect") {
            std::string host;
            std::string port;
            if (!split_host_port(need_value(i, a), host, port)) {
                std::cerr << a << " expects HOST:PORT\n";
                std::exit(2);
            }
            opt.server_mode = false;
            opt.host = host;
            opt.port = port;
        } else if (a == "--server" || a == "--listen") {
            opt.server_mode = true;
            opt.host.clear();
            opt.port = need_value(i, a);
        } else if (a == "--out") {
            opt.log_file = need_value(i, a);
        } else if (a == "--read-size") {
            opt.read_size = static_cast<std::size_t>(std::stoull(need_value(i, a)));
        } else if (a == "--slow-ms") {
            opt.slow_ms = std::stoull(need_value(i, a));
        } else if (a == "--progress-sec") {
            opt.progress_sec = std::stoi(need_value(i, a));
        } else if (a == "--requests") {
            opt.requests = std::stoull(need_value(i, a));
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            usage(argv[0]);
            std::exit(2);
        }
    }
 
    if (opt.port.empty()) {
        std::cerr << "Either --client HOST:PORT or --server PORT is required\n";
        usage(argv[0]);
        std::exit(2);
    }
    if (opt.read_size == 0) {
        std::cerr << "--read-size must be greater than 0\n";
        std::exit(2);
    }
    if (opt.progress_sec <= 0) {
        opt.progress_sec = 1;
    }
    return opt;
}
 
static int connect_tcp(const std::string &host, const std::string &port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
 
    addrinfo *res = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo failed: " << gai_strerror(rc) << "\n";
        return -1;
    }
 
    int fd = -1;
    for (addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
 
    freeaddrinfo(res);
    return fd;
}
 
static int listen_tcp(const std::string &port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
 
    addrinfo *res = nullptr;
    int rc = getaddrinfo(nullptr, port.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo failed: " << gai_strerror(rc) << "\n";
        return -1;
    }
 
    int listen_fd = -1;
    for (addrinfo *p = res; p; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }
 
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == 0 && listen(listen_fd, 16) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);
 
    if (listen_fd < 0) {
        return -1;
    }
 
    std::cerr << "listening on port " << port << ", waiting for one connection...\n";
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    int fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
    close(listen_fd);
    return fd;
}
 
static void set_recv_timeout(int fd) {
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
 
static void tune_tcp(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}
 
static bool send_all(int fd, const void *data, std::size_t len) {
    const char *p = static_cast<const char *>(data);
    std::size_t sent = 0;
    while (sent < len && !g_stop.load(std::memory_order_relaxed)) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return sent == len;
}
 
static bool recv_exact(int fd, void *data, std::size_t len, std::uint64_t &bytes_read) {
    char *p = static_cast<char *>(data);
    bytes_read = 0;
    while (bytes_read < len && !g_stop.load(std::memory_order_relaxed)) {
        ssize_t n = recv(fd, p + bytes_read, len - bytes_read, 0);
        if (n > 0) {
            bytes_read += static_cast<std::uint64_t>(n);
        } else if (n == 0) {
            return false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        } else {
            return false;
        }
    }
    return bytes_read == len;
}
 
static void print_progress(const Stats &s, bool done, bool &first) {
    if (!first) {
        std::cerr << "\033[3A";
    }
    first = false;
    std::cerr << "\r\033[Krequests: total=" << s.requests
              << " responses=" << s.responses
              << " slow=" << s.slow_responses
              << " errors=" << s.errors << '\n'
              << "\r\033[Kbytes: total=" << s.bytes << '\n'
              << "\r\033[Kstatus: " << (done ? "stopped" : "running") << '\n';
    std::cerr << std::flush;
}
 
static int run_client(const Options &opt) {
    std::ofstream log(opt.log_file, std::ios::app);
    if (!log) {
        std::cerr << "failed to open log file: " << opt.log_file << "\n";
        return 1;
    }
 
    log << "==== tcp read latency demo start ====\n"
        << "start_time: " << now_string() << '\n'
        << "mode: client\n"
        << "target: " << opt.host << ":" << opt.port << '\n'
        << "read_size: " << opt.read_size << '\n'
        << "slow_ms: " << opt.slow_ms << '\n'
        << "requests: " << (opt.requests == 0 ? std::string("forever") : std::to_string(opt.requests)) << "\n\n";
    log.flush();
 
    int fd = connect_tcp(opt.host, opt.port);
    if (fd < 0) {
        std::cerr << "failed to establish TCP connection: " << std::strerror(errno) << "\n";
        return 2;
    }
    set_recv_timeout(fd);
    tune_tcp(fd);
 
    std::vector<char> buf(opt.read_size);
    Stats stats;
    bool first_progress_print = true;
    auto last_progress = std::chrono::steady_clock::now();
    const auto progress_interval = std::chrono::seconds(opt.progress_sec);
    const auto slow_threshold = std::chrono::milliseconds(opt.slow_ms);
 
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (opt.requests != 0 && stats.requests >= opt.requests) {
            break;
        }
 
        char req = 'R';
        auto begin = std::chrono::steady_clock::now();
        if (!send_all(fd, &req, sizeof(req))) {
            ++stats.errors;
            log << "time=" << now_string() << " send_request_failed errno=" << errno
                << " message=" << std::strerror(errno) << '\n';
            log.flush();
            break;
        }
        ++stats.requests;
 
        std::uint64_t n = 0;
        bool ok = recv_exact(fd, buf.data(), buf.size(), n);
        auto end = std::chrono::steady_clock::now();
 
        if (ok) {
            ++stats.responses;
            stats.bytes += n;
            auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
            if (latency_us > slow_threshold) {
                ++stats.slow_responses;
                log << "time=" << now_string()
                    << " request_index=" << stats.requests
                    << " bytes=" << n
                    << " latency_us=" << latency_us.count()
                    << " total_bytes=" << stats.bytes << '\n';
                log.flush();
            }
        } else {
            ++stats.errors;
            log << "time=" << now_string() << " recv_response_failed"
                << " request_index=" << stats.requests
                << " bytes_read=" << n
                << " errno=" << errno
                << " message=" << std::strerror(errno) << '\n';
            log.flush();
            break;
        }
 
        auto now = std::chrono::steady_clock::now();
        if (now - last_progress >= progress_interval) {
            print_progress(stats, false, first_progress_print);
            last_progress = now;
        }
    }
 
    close(fd);
    print_progress(stats, true, first_progress_print);
    log << "\n==== tcp read latency demo stop ====\n"
        << "stop_time: " << now_string() << '\n'
        << "requests: " << stats.requests << '\n'
        << "responses: " << stats.responses << '\n'
        << "slow_responses: " << stats.slow_responses << '\n'
        << "bytes: " << stats.bytes << '\n'
        << "errors: " << stats.errors << '\n';
    log.flush();
    return 0;
}
 
static int run_server(const Options &opt) {
    int fd = listen_tcp(opt.port);
    if (fd < 0) {
        std::cerr << "failed to establish TCP connection: " << std::strerror(errno) << "\n";
        return 2;
    }
    set_recv_timeout(fd);
    tune_tcp(fd);
 
    std::vector<char> payload(opt.read_size, 'x');
    Stats stats;
    bool first_progress_print = true;
    auto last_progress = std::chrono::steady_clock::now();
    const auto progress_interval = std::chrono::seconds(opt.progress_sec);
 
    while (!g_stop.load(std::memory_order_relaxed)) {
        char req = 0;
        ssize_t n = recv(fd, &req, sizeof(req), 0);
        if (n == 1) {
            ++stats.requests;
            if (!send_all(fd, payload.data(), payload.size())) {
                ++stats.errors;
                break;
            }
            ++stats.responses;
            stats.bytes += payload.size();
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        } else {
            ++stats.errors;
            break;
        }
 
        auto now = std::chrono::steady_clock::now();
        if (now - last_progress >= progress_interval) {
            print_progress(stats, false, first_progress_print);
            last_progress = now;
        }
    }
 
    close(fd);
    print_progress(stats, true, first_progress_print);
    return 0;
}
 
int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
 
    if (opt.server_mode) {
        return run_server(opt);
    }
    return run_client(opt);
}
