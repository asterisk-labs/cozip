#include "contract.hpp"

namespace karu::backends {

const CloudProvider* cloud_provider(Backend backend) noexcept {
    switch (backend) {
    case Backend::S3:
        return &s3_provider();
    case Backend::Gcs:
        return &gcs_provider();
    case Backend::Azure:
        return &azure_provider();
    case Backend::Source:
        return &source_provider();
    default:
        return nullptr;
    }
}

} // namespace karu::backends
