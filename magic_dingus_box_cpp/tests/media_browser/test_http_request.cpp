// tests/media_browser/test_http_request.cpp
//
// media_browser::http::request against a tiny in-process HTTP/1.1 server on
// 127.0.0.1 (ephemeral port, one connection per server). No outside network:
// the clients' own tests mock the transport seam; this file pins what the
// one real transport puts on the wire and how it reports what comes back.
#include <catch2/catch_test_macros.hpp>

#include "media_browser/http/http_request.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>

using media_browser::http::Method;
using media_browser::http::Request;
using media_browser::http::request;

namespace {

// Accepts ONE connection, records the raw request (head + Content-Length
// body), then either writes `response` and closes or (respond=false) holds
// the connection silent until destroyed — the timeout/abort cases.
class LoopbackServer {
public:
    explicit LoopbackServer(std::string response, bool respond = true)
        : response_(std::move(response)), respond_(respond) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(fd_, 4);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackServer() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    // The raw request the server read. Joins the server first.
    std::string received() {
        if (thread_.joinable()) {
            stop_.store(true);
            thread_.join();
        }
        return request_;
    }

private:
    void serve() {
        pollfd p{fd_, POLLIN, 0};
        while (!stop_.load() && ::poll(&p, 1, 50) == 0) {}
        if (stop_.load()) return;
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        read_request(c);
        if (respond_) {
            ::send(c, response_.data(), response_.size(), 0);
        } else {
            while (!stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::close(c);
    }
    void read_request(int c) {
        char buf[4096];
        size_t need = std::string::npos;
        while (true) {
            const auto head_end = request_.find("\r\n\r\n");
            if (head_end != std::string::npos && need == std::string::npos) {
                std::string lower = request_.substr(0, head_end);
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char ch) { return std::tolower(ch); });
                const auto cl = lower.find("content-length:");
                need = head_end + 4 +
                       (cl == std::string::npos ? 0 : std::stoul(lower.substr(cl + 15)));
            }
            if (need != std::string::npos && request_.size() >= need) return;
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 2000) <= 0) return;
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) return;
            request_.append(buf, static_cast<size_t>(n));
        }
    }

    std::string response_;
    bool respond_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::string request_;
};

const char* kOk = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("http::request GET sends headers in order and returns body + status",
          "[http]") {
    LoopbackServer srv(kOk);
    Request r;
    r.url = srv.url("/api/v3/ping");
    r.headers = {"X-Api-Key: k123", "Accept: application/json"};
    r.user_agent = "MagicDingusBox/1.0";
    r.timeout_secs = 5;
    r.connect_timeout_secs = 3;
    const auto resp = request(r);
    CHECK(resp.transport_ok());
    CHECK(resp.status == 200);
    CHECK(resp.body == "hello");
    CHECK(resp.error.empty());
    const std::string got = srv.received();
    CHECK(got.rfind("GET /api/v3/ping HTTP/1.1\r\n", 0) == 0);
    CHECK(contains(got, "\r\nX-Api-Key: k123\r\n"));
    CHECK(contains(got, "\r\nUser-Agent: MagicDingusBox/1.0\r\n"));
    CHECK(got.find("X-Api-Key") < got.find("Accept: application/json"));
}

TEST_CASE("http::request POST, PUT and DELETE carry the body and the verb",
          "[http]") {
    struct Case { Method m; const char* verb; };
    for (const Case c : {Case{Method::Post, "POST"}, Case{Method::Put, "PUT"},
                         Case{Method::Delete, "DELETE"}}) {
        LoopbackServer srv(kOk);
        Request r;
        r.method = c.m;
        r.url = srv.url("/x");
        r.headers = {"Content-Type: application/json"};
        r.body = std::string(R"({"ids":[1,2]})");
        r.timeout_secs = 5;
        const auto resp = request(r);
        CHECK(resp.status == 200);
        const std::string got = srv.received();
        CHECK(got.rfind(std::string(c.verb) + " /x HTTP/1.1\r\n", 0) == 0);
        CHECK(contains(got, "Content-Length: 13\r\n"));
        CHECK(got.substr(got.size() - 13) == R"({"ids":[1,2]})");
    }
}

TEST_CASE("http::request POST with no body sends an empty one", "[http]") {
    LoopbackServer srv(kOk);
    Request r;
    r.method = Method::Post;
    r.url = srv.url("/api/v2/transfer/toggleSpeedLimitsMode");
    r.timeout_secs = 5;
    CHECK(request(r).status == 200);
    const std::string got = srv.received();
    CHECK(got.rfind("POST /api/v2/transfer/toggleSpeedLimitsMode", 0) == 0);
    CHECK(contains(got, "Content-Length: 0\r\n"));
}

TEST_CASE("http::request HEAD sends HEAD and reads no body", "[http]") {
    LoopbackServer srv("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    Request r;
    r.method = Method::Head;
    r.url = srv.url("/ping");
    r.timeout_secs = 3;
    const auto resp = request(r);
    CHECK(resp.transport_ok());
    CHECK(resp.status == 200);
    CHECK(srv.received().rfind("HEAD /ping HTTP/1.1\r\n", 0) == 0);
}

TEST_CASE("http::request reports an error status in-band, transport OK",
          "[http]") {
    LoopbackServer srv("HTTP/1.1 400 Bad Request\r\nContent-Length: 7\r\n"
                       "Connection: close\r\n\r\nalready");
    Request r;
    r.url = srv.url("/movie");
    r.timeout_secs = 5;
    const auto resp = request(r);
    CHECK(resp.transport_ok());
    CHECK(resp.error.empty());   // status mapping is the client's job
    CHECK(resp.status == 400);
    CHECK(resp.body == "already");
}

TEST_CASE("http::request captures response header lines on request", "[http]") {
    LoopbackServer srv("HTTP/1.1 200 OK\r\nset-cookie: SID=abc; HttpOnly; path=/\r\n"
                       "Content-Length: 3\r\nConnection: close\r\n\r\nOk.");
    Request r;
    r.method = Method::Post;
    r.url = srv.url("/api/v2/auth/login");
    r.body = std::string("username=u&password=p");
    r.capture_headers = true;
    r.timeout_secs = 5;
    const auto resp = request(r);
    CHECK(resp.body == "Ok.");
    CHECK(std::find(resp.headers.begin(), resp.headers.end(),
                    "set-cookie: SID=abc; HttpOnly; path=/") != resp.headers.end());
    for (const auto& h : resp.headers) CHECK(!contains(h, "\r"));
}

TEST_CASE("http::request with headers not requested leaves them empty", "[http]") {
    LoopbackServer srv(kOk);
    Request r;
    r.url = srv.url("/");
    r.timeout_secs = 5;
    CHECK(request(r).headers.empty());
}

TEST_CASE("http::request connection refused is a transport error, status 0",
          "[http]") {
    // Take an ephemeral port, then close the listener: nothing is bound
    // there any more, so the kernel answers the SYN with a reset.
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(s, 1);
    socklen_t len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(s);
    Request r;
    r.url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/ping";
    r.timeout_secs = 3;
    r.connect_timeout_secs = 1;
    const auto resp = request(r);
    CHECK_FALSE(resp.transport_ok());
    CHECK(resp.curl_code == CURLE_COULDNT_CONNECT);
    CHECK(resp.status == 0);
    CHECK(resp.error == curl_easy_strerror(CURLE_COULDNT_CONNECT));
}

TEST_CASE("http::request total timeout bounds a silent server", "[http]") {
    LoopbackServer srv("", /*respond=*/false);
    Request r;
    r.url = srv.url("/slow");
    r.timeout_secs = 1;
    const auto t0 = std::chrono::steady_clock::now();
    const auto resp = request(r);
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(resp.curl_code == CURLE_OPERATION_TIMEDOUT);
    CHECK(took < std::chrono::seconds(4));
}

TEST_CASE("http::request abort flag cancels an in-flight transfer", "[http]") {
    LoopbackServer srv("", /*respond=*/false);
    std::atomic<bool> abort{false};
    Request r;
    r.url = srv.url("/search");
    r.timeout_secs = 20;
    r.abort_flag = &abort;
    std::thread flipper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        abort.store(true);
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto resp = request(r);
    flipper.join();
    CHECK(resp.curl_code == CURLE_ABORTED_BY_CALLBACK);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
}

TEST_CASE("http::request on a reused handle works twice and keeps the handle",
          "[http]") {
    CURL* h = curl_easy_init();
    REQUIRE(h != nullptr);
    for (int i = 0; i < 2; ++i) {
        LoopbackServer srv(kOk);
        Request r;
        r.url = srv.url("/poster" + std::to_string(i) + ".jpg");
        r.timeout_secs = 5;
        r.follow_redirects = true;
        const auto resp = request(r, h);
        CHECK(resp.status == 200);
        CHECK(resp.body == "hello");
        CHECK(srv.received().rfind("GET /poster" + std::to_string(i), 0) == 0);
    }
    curl_easy_cleanup(h);
}
