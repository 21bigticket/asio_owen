#pragma once
#include <functional>

class ScopeGuard {
public:
    explicit ScopeGuard(std::function<void()> f) : func_(std::move(f)) {}
    ~ScopeGuard() { if (active_) func_(); }
    void dismiss() { active_ = false; }
private:
    std::function<void()> func_;
    bool active_ = true;
};
