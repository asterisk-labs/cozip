#ifndef KARU_CURL_TYPES_HPP
#define KARU_CURL_TYPES_HPP

#include <curl/curl.h>
#include <memory>

namespace karu {

struct EasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};

struct MultiDeleter {
    void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
};

struct ShareDeleter {
    void operator()(CURLSH* handle) const noexcept { curl_share_cleanup(handle); }
};

struct SlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

using Easy = std::unique_ptr<CURL, EasyDeleter>;
using Multi = std::unique_ptr<CURLM, MultiDeleter>;
using Share = std::unique_ptr<CURLSH, ShareDeleter>;
using Slist = std::unique_ptr<curl_slist, SlistDeleter>;

} // namespace karu

#endif
