
#pragma once

template <typename F>
class ScopeGuard {
    F func;
public:
    explicit ScopeGuard(F&& f) : func(std::forward<F>(f)) {}
    ~ScopeGuard() { func(); }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};