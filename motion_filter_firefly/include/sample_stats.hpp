#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// One reporting window only: memory does not grow with stream uptime.
class SampleStats {
  public:
    void add(double value) {
        if (!std::isfinite(value) || value < 0.0)
            return;
        values_.push_back(value);
        sum_ += value;
        max_ = std::max(max_, value);
    }

    std::size_t count() const {
        return values_.size();
    }

    double mean() const {
        return values_.empty() ? unavailable() : sum_ / static_cast<double>(values_.size());
    }

    double maximum() const {
        return values_.empty() ? unavailable() : max_;
    }

    double p95() const {
        if (values_.empty())
            return unavailable();
        auto sorted = values_;
        const std::size_t rank = (sorted.size() * 95U + 99U) / 100U - 1U;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank),
                         sorted.end());
        return sorted[rank];
    }

    void clear() {
        values_.clear();
        sum_ = 0.0;
        max_ = 0.0;
    }

  private:
    static double unavailable() {
        return std::numeric_limits<double>::quiet_NaN();
    }

    std::vector<double> values_;
    double sum_ = 0.0;
    double max_ = 0.0;
};
