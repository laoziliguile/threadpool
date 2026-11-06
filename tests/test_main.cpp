// tests/test_main.cpp —— 测试入口：把所有注册的用例跑一遍
#include "test_framework.hpp"

int main() {
    return tftest::run_all();
}
