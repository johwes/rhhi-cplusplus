#include <curl/curl.h>
#include <iostream>
#include <memory>
#include <string>

constexpr size_t MAX_RESPONSE_BYTES = 10 * 1024 * 1024; // 10 MB

struct CurlGlobalGuard {
    CurlGlobalGuard() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalGuard() { curl_global_cleanup(); }
};

struct CurlDeleter {
    void operator()(CURL* curl) const {
        if (curl) curl_easy_cleanup(curl);
    }
};
using ScopedCurl = std::unique_ptr<CURL, CurlDeleter>;

static size_t WriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_bytes = size * nmemb;
    auto* body = static_cast<std::string*>(userp);

    if (body->size() + total_bytes > MAX_RESPONSE_BYTES) {
        std::cerr << "Response exceeded size limit (" << MAX_RESPONSE_BYTES << " bytes)\n";
        return 0; // Returning short count aborts the transfer (CURLE_WRITE_ERROR)
    }

    try {
        body->append(contents, total_bytes);
        return total_bytes;
    } catch (const std::exception& e) {
        std::cerr << "Memory allocation failed: " << e.what() << "\n";
        return 0;
    }
}

int main(int argc, char* argv[]) {
    CurlGlobalGuard global_guard;

    std::string url = "https://example.com";
    if (argc > 1) {
        url = argv[1];
    }

    ScopedCurl curl(curl_easy_init());
    if (!curl) {
        std::cerr << "curl_easy_init failed" << std::endl;
        return 1;
    }

    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "rhhi-cplusplus/1.0");

    CURLcode res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) {
        std::cerr << "request failed: " << curl_easy_strerror(res) << std::endl;
        return 1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    std::cout << "URL: " << url << "\nHTTP status: " << http_code
              << "\nBody bytes: " << body.size() << std::endl;

    return 0;
}
