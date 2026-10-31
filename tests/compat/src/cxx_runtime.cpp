#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <memory>
#include <cstdlib>
static int depth(int n) { if (n == 0) throw std::runtime_error("deep"); std::string s(64, 'x'); return depth(n - 1) + static_cast<int>(s.size()); }
int main() {
    std::map<std::string, int> m{{"a", 1}, {"b", 2}}; std::vector<int> v(1000, 3);
    int sum = 0; for (int x : v) sum += x; if (sum != 3000 || m["b"] != 2) return 1;
    try { depth(50); return 2; } catch (const std::runtime_error &e) { if (std::string(e.what()) != "deep") return 3; }   /* unwinds via libgcc_s */
    int hits = 0; std::mutex mu; std::vector<std::thread> th;
    for (int i = 0; i < 4; i++) th.emplace_back([&] { for (int j = 0; j < 1000; j++) { std::lock_guard<std::mutex> g(mu); hits++; } });
    for (auto &t : th) { t.join(); }
    if (hits != 4000) return 4;
    auto up = std::make_unique<std::string>("heap"); std::cout << "OK cxx_runtime " << *up << std::endl; return 0;
}
