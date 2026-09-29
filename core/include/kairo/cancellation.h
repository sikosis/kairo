#pragma once

#include <atomic>
#include <memory>

namespace kairo {

class CancellationToken {
public:
    CancellationToken() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}
    void Cancel() const { cancelled_->store(true); }
    bool IsCancelled() const { return cancelled_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

}  // namespace kairo
