// STL containers, algorithms, strings, iostreams, <format>, <regex>, <random>.
#include <algorithm>
#include <any>
#include <array>
#include <bitset>
#include <cctype>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::string name() const = 0;
};
struct Rect : Shape {
    double w, h;
    Rect(double w_, double h_) : w(w_), h(h_) {}
    double area() const override { return w * h; }
    std::string name() const override { return "rect"; }
};
struct Circle : Shape {
    double r;
    explicit Circle(double r_) : r(r_) {}
    double area() const override { return 3.25 * r * r; }
    std::string name() const override { return "circle"; }
};

static void containers() {
    std::vector<int> v;
    for (int i = 0; i < 1000; i++) v.push_back((i * 7919) % 1009);
    std::sort(v.begin(), v.end());
    auto last = std::unique(v.begin(), v.end());
    v.erase(last, v.end());
    std::cout << "vector unique=" << v.size() << " sum=" << std::accumulate(v.begin(), v.end(), 0LL)
              << " lower_bound(500)=" << (std::lower_bound(v.begin(), v.end(), 500) - v.begin()) << "\n";

    std::list<std::string> l{"delta", "alpha", "charlie", "bravo"};
    l.sort();
    l.push_front("zulu");
    l.reverse();
    std::cout << "list:";
    for (const auto& s : l) std::cout << ' ' << s;
    std::cout << "\n";

    std::deque<int> d;
    for (int i = 0; i < 100; i++) {
        if (i % 2) d.push_back(i);
        else d.push_front(i);
    }
    std::cout << "deque front=" << d.front() << " back=" << d.back() << " [50]=" << d[50] << "\n";

    std::map<std::string, int> m;
    std::string text = "the quick brown fox jumps over the lazy dog the end";
    std::istringstream words(text);
    for (std::string w; words >> w;) m[w]++;
    std::cout << "map:";
    for (const auto& [k, c] : m) std::cout << ' ' << k << '=' << c;
    std::cout << "\n";

    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 500; i++) um[i * 31 % 997] = std::to_string(i);
    std::vector<int> keys;
    for (const auto& kv : um) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    long long ksum = 0;
    for (int k : keys) ksum += k * static_cast<long long>(um[k].size());
    std::cout << "unordered_map size=" << um.size() << " weighted=" << ksum << " find(31)=" << um.find(31)->second
              << "\n";

    std::set<int> s1{1, 3, 5, 7, 9, 11}, s2{3, 6, 9, 12};
    std::vector<int> inter, uni;
    std::set_intersection(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(inter));
    std::set_union(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(uni));
    std::cout << "set intersection=" << inter.size() << " union=" << uni.size() << "\n";
    std::unordered_set<std::string> us{"a", "b", "c", "a"};
    std::cout << "unordered_set size=" << us.size() << " count(b)=" << us.count("b") << "\n";

    std::array<int, 8> arr{8, 3, 5, 1, 9, 2, 7, 4};
    std::stable_sort(arr.begin(), arr.end(), std::greater<>());
    std::nth_element(v.begin(), v.begin() + 10, v.end());
    std::cout << "array:";
    for (int x : arr) std::cout << ' ' << x;
    std::cout << " nth=" << v[10] << "\n";

    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Rect>(2.0, 3.5));
    shapes.push_back(std::make_unique<Circle>(1.5));
    shapes.push_back(std::make_unique<Rect>(0.5, 0.5));
    double total = 0;
    for (const auto& sh : shapes) total += sh->area();
    std::cout << "shapes total=" << total << " first=" << shapes[0]->name() << "\n";

    auto shared = std::make_shared<std::vector<int>>(5, 3);
    std::weak_ptr<std::vector<int>> weak = shared;
    auto copy = shared;
    std::cout << "shared use_count=" << shared.use_count() << " expired=" << weak.expired();
    copy.reset();
    shared.reset();
    std::cout << " after reset expired=" << weak.expired() << "\n";

    std::bitset<64> bits(0xF0F0F0F0F0F0F0F0ull);
    std::cout << "bitset count=" << bits.count() << " str=" << (bits >> 60).to_string().substr(56) << "\n";
}

static void strings_and_streams() {
    std::string s = "Hello, World";
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string_view sv(s);
    std::cout << "upper=" << s << " find=" << s.find("WORLD") << " rfind=" << s.rfind('L') << " sub=" << sv.substr(7, 3)
              << " starts=" << sv.starts_with("HELLO") << "\n";
    std::string joined;
    for (int i = 0; i < 5; i++) joined += std::to_string(i * i) + (i < 4 ? "," : "");
    std::cout << "joined=" << joined << " stoi=" << std::stoi("-1234") << " stoull=" << std::stoull("ffff", nullptr, 16)
              << " stod=" << std::stod("6.25e2") << "\n";
    std::cout << "to_string(double)=" << std::to_string(1.0 / 3.0) << " to_string(-0.5f)=" << std::to_string(-0.5f)
              << "\n";

    std::cout << std::fixed << std::setprecision(3) << "fixed=" << 3.14159265 << ' ' << 2.0 / 3.0;
    std::cout << std::scientific << std::setprecision(2) << " sci=" << 123456.789;
    std::cout << std::defaultfloat << std::setprecision(6) << " default=" << 0.1 + 0.2 << "\n";
    std::cout << std::hex << std::showbase << "hex=" << 255 << ' ' << std::uppercase << 0xabcdef << std::nouppercase
              << std::dec << std::noshowbase << " dec=" << 255 << "\n";
    std::cout << '[' << std::setw(8) << std::setfill('*') << 42 << "][" << std::left << std::setw(6) << "ab" << ']'
              << std::right << std::setfill(' ') << std::boolalpha << " bool=" << true << std::noboolalpha << "\n";

    std::ostringstream out;
    out << "x=" << 10 << " y=" << 2.5 << " z=" << 'c';
    std::istringstream in("17 3.75 word 0x1f");
    int i = 0;
    double dd = 0;
    std::string w;
    int hx = 0;
    in >> i >> dd >> w >> std::hex >> hx;
    std::cout << "ostringstream=" << out.str() << " istringstream=" << i << '/' << dd << '/' << w << '/' << hx << "\n";

    std::wstring ws = L"wide";
    ws += L"-string";
    std::cout << "wstring length=" << ws.size() << "\n";

    std::cout << std::format("format: {} {:>6} {:<6}| {:08.3f} {:x} {:#b} {:e}", 42, "r", "l", 3.14159, 255, 5, 1234.5)
              << "\n";
    std::cout << std::format("format: {1} {0} {2:.0f}", "first", "second", 2.5) << "\n";
}

static void regex_and_random() {
    std::regex date(R"((\d{4})-(\d{2})-(\d{2}))");
    std::string log = "start 2024-01-15 middle 2025-12-31 end 1999-07-04";
    std::cout << "regex:";
    for (std::sregex_iterator it(log.begin(), log.end(), date), end; it != end; ++it) {
        std::cout << ' ' << (*it)[1] << '/' << (*it)[2] << '/' << (*it)[3];
    }
    std::cout << " replaced=" << std::regex_replace(std::string("a1b22c333"), std::regex("[0-9]+"), "#") << "\n";

    std::mt19937 gen(12345);
    std::cout << "mt19937:";
    for (int i = 0; i < 4; i++) std::cout << ' ' << gen();
    std::mt19937_64 gen64(987654321);
    std::cout << " mt19937_64=" << gen64() << "\n";
    std::uniform_int_distribution<int> dist(1, 6);
    std::mt19937 dice(7);
    int counts[7] = {};
    for (int i = 0; i < 6000; i++) counts[dist(dice)]++;
    std::cout << "dice:";
    for (int f = 1; f <= 6; f++) std::cout << ' ' << counts[f];
    std::cout << "\n";
}

static void vocabulary_types() {
    std::optional<int> o;
    std::cout << "optional has=" << o.has_value() << " value_or=" << o.value_or(-1);
    o = 5;
    std::cout << " now=" << *o << "\n";
    std::variant<int, double, std::string> var = 3.5;
    std::cout << "variant index=" << var.index();
    var = std::string("text");
    std::cout << " visit="
              << std::visit(
                     [](const auto& x) -> std::string {
                         std::ostringstream os;
                         os << x;
                         return os.str();
                     },
                     var)
              << "\n";
    std::any a = 10;
    std::cout << "any type int=" << (a.type() == typeid(int)) << " cast=" << std::any_cast<int>(a) << "\n";

    std::function<int(int)> fact = [&](int n) { return n <= 1 ? 1 : n * fact(n - 1); };
    auto bound = std::bind(std::plus<int>(), std::placeholders::_1, 100);
    std::cout << "function fact(10)=" << fact(10) << " bind=" << bound(23) << "\n";

    using namespace std::chrono;
    auto dur = hours(1) + minutes(30) + seconds(15);
    std::cout << "chrono seconds=" << duration_cast<seconds>(dur).count()
              << " ms=" << duration_cast<milliseconds>(dur).count() << "\n";
    auto t0 = steady_clock::now();
    auto t1 = steady_clock::now();
    std::cout << "steady monotonic=" << (t1 >= t0) << "\n";
}

int main() {
    containers();
    strings_and_streams();
    regex_and_random();
    vocabulary_types();
    std::cout << std::flush;
    return 0;
}
