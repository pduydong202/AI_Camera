#include "source_timeline.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        SourceTimeline healthy;
        for (const auto raw : std::vector<std::int64_t>{0, 50000000, 100011111, 150000000, 1100000000})
            check(healthy.normalize(raw) == raw, "valid VFR/gap timestamp changed");
        SourceTimeline burst;
        check(burst.normalize(1000000000) == 1000000000, "anchor");
        check(burst.normalize(-1) == 1000001000, "missing timestamp");
        check(burst.normalize(1000000000) == 1000002000, "duplicate timestamp");
        check(burst.normalize(999000000) == 1000003000, "backward timestamp");
        check(burst.normalize(1050000000) == 1050000000, "repair cascade did not end on next valid frame");
        SourceTimeline shared;
        std::vector<std::int64_t> canonical;
        for (int i=0;i<1000;++i) canonical.push_back(shared.normalize(i==50 ? -1 : i*50000000LL));
        for (int consumer=0;consumer<8;++consumer) {
            std::int64_t previous=-1;
            for (std::size_t i=consumer;i<canonical.size();i+=consumer+1) {
                check(canonical[i]>previous,"consumer timeline went backward");
                previous=canonical[i];
            }
        }
        SourceTimeline broken;
        broken.normalize(0,0,15000000000LL);
        broken.normalize(0,1000000000LL,15000000000LL);
        bool stopped=false;
        try { broken.normalize(0,16000000000LL,15000000000LL); }
        catch (const std::runtime_error&) { stopped=true; }
        check(stopped,"permanently broken source clock was silently accepted");
        SourceTimeline recovering;
        recovering.normalize(0,0,15000000000LL);
        recovering.normalize(-1,1000000000LL,15000000000LL);
        recovering.normalize(50000000,2000000000LL,15000000000LL);
        check(recovering.normalize(100000000,20000000000LL,15000000000LL)==100000000,
              "recovered source retained stale timeout state");
        std::cout << "PASS: healthy/VFR/gap timestamps unchanged, missing/duplicate/backward burst recovers, shared decisions survive consumer drops\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
