#pragma once
// ============================================================================
//  tests/test_framework.hpp —— 150 行的手写测试框架
//
//  为什么不用 GoogleTest/Catch2：本项目要求"零外部依赖、clone 下来就能编"，
//  而单元测试需要的功能其实只有三件事：
//    ① 注册用例   ② 断言并记录失败   ③ 全部跑完统计结果
//  看懂这个文件，你就具备了给自己项目写测试的能力（这比会用框架更重要）。
// ============================================================================

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace tftest {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& check_count() {
    static int n = 0;
    return n;
}

inline int& fail_count() {
    static int n = 0;
    return n;
}

inline std::string& current_name() {
    static std::string name;
    return name;
}

// 全局对象：构造函数在 main 之前把用例塞进 registry，这是最简单的自动注册
struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

template <typename T>
std::string show(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline void report_failure(const char* file, int line, const std::string& message) {
    ++fail_count();
    std::fprintf(stderr, "    [FAIL] %s\n           at %s:%d\n           %s\n",
                 current_name().c_str(), file, line, message.c_str());
}

inline int run_all() {
    int failed_cases = 0;
    for (const auto& tc : registry()) {
        current_name() = tc.name;
        const int before = fail_count();
        std::printf("[ RUN ] %s\n", tc.name.c_str());
        try {
            tc.fn();
        } catch (const std::exception& e) {
            report_failure("<exception>", 0, std::string("uncaught std::exception: ") + e.what());
        } catch (...) {
            report_failure("<exception>", 0, "uncaught unknown exception");
        }
        const bool ok = (fail_count() == before);
        if (!ok) ++failed_cases;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", tc.name.c_str());
    }

    std::printf("\n----------------------------------------\n");
    std::printf("%d cases, %d checks, %d failures\n", static_cast<int>(registry().size()),
                check_count(), fail_count());
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace tftest

// ---- 宏：定义用例 ----
#define TF_TEST(name)                                        \
    static void name();                                      \
    static ::tftest::Registrar tf_reg_##name(#name, &name);  \
    static void name()

// ---- 宏：断言 ----
#define TF_CHECK(cond)                                                              \
    do {                                                                            \
        ++::tftest::check_count();                                                  \
        if (!(cond)) {                                                              \
            ::tftest::report_failure(__FILE__, __LINE__, "expected: " #cond);       \
        }                                                                           \
    } while (false)

#define TF_CHECK_EQ(a, b)                                                           \
    do {                                                                            \
        ++::tftest::check_count();                                                  \
        const auto& tf_a = (a);                                                     \
        const auto& tf_b = (b);                                                     \
        if (!(tf_a == tf_b)) {                                                      \
            ::tftest::report_failure(__FILE__, __LINE__,                            \
                                     std::string("expected ") + #a + " == " + #b +   \
                                         ", got " + ::tftest::show(tf_a) + " vs " +  \
                                         ::tftest::show(tf_b));                      \
        }                                                                           \
    } while (false)
