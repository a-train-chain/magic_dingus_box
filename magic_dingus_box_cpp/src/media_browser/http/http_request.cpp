#include "media_browser/http/http_request.h"

namespace media_browser::http {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t header_cb(char* ptr, size_t size, size_t nitems, void* ud) {
    const size_t n = size * nitems;
    size_t end = n;
    while (end > 0 && (ptr[end - 1] == '\r' || ptr[end - 1] == '\n')) --end;
    static_cast<std::vector<std::string>*>(ud)->emplace_back(ptr, end);
    return n;
}

int abort_cb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(ud);
    return flag && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

}  // namespace

Response request(const Request& req, CURL* reuse) {
    Response out;
    CURL* curl = reuse;
    if (curl) {
        curl_easy_reset(curl);
    } else {
        curl = curl_easy_init();
        if (!curl) {
            out.curl_code = CURLE_FAILED_INIT;
            out.error = "curl init failed";
            return out;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    switch (req.method) {
        case Method::Get:    break;
        case Method::Head:   curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); break;
        case Method::Post:   curl_easy_setopt(curl, CURLOPT_POST, 1L); break;
        case Method::Put:    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT"); break;
        case Method::Delete: curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE"); break;
    }
    if (req.body && req.method != Method::Get && req.method != Method::Head) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body->size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body->c_str());
    } else if (req.method == Method::Post) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    if (req.timeout_secs > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_secs);
    if (req.connect_timeout_secs > 0)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_secs);
    if (!req.user_agent.empty())
        curl_easy_setopt(curl, CURLOPT_USERAGENT, req.user_agent.c_str());
    if (req.follow_redirects) curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (req.force_http_1_1)
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    if (req.tcp_keepalive) curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (req.abort_flag) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,
                         const_cast<std::atomic<bool>*>(req.abort_flag));
    }
    if (req.capture_headers) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
    }
    curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    out.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    if (out.curl_code != CURLE_OK) out.error = curl_easy_strerror(out.curl_code);

    if (hdrs) curl_slist_free_all(hdrs);
    // A reused handle must not keep pointers into this call's locals; the
    // reset keeps its connections, which is the point of reusing it.
    if (reuse) curl_easy_reset(curl);
    else       curl_easy_cleanup(curl);
    return out;
}

}  // namespace media_browser::http
