#ifndef STR_UTIL_H
#define STR_UTIL_H

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

/// 去除首尾空白
inline std::string Trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// 转小写
inline std::string ToLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return r;
}

/// 判断前缀
inline bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

/// 判断后缀
inline bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

/// 按分隔符拆分
inline std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> res;
    size_t start = 0, pos;
    while ((pos = s.find(sep, start)) != std::string::npos) {
        auto part = Trim(s.substr(start, pos - start));
        if (!part.empty()) res.push_back(part);
        start = pos + 1;
    }
    auto last = Trim(s.substr(start));
    if (!last.empty()) res.push_back(last);
    return res;
}

/// 按分隔符拆分 key=value 对 (支持逗号分隔, 括号内的逗号忽略)
inline std::vector<std::pair<std::string, std::string>> ParseKeyValues(const std::string& input) {
    std::vector<std::pair<std::string, std::string>> result;
    size_t pos = 0;
    while (pos < input.size()) {
        size_t eq = input.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = Trim(input.substr(pos, eq - pos));
        if (key.empty()) {
            // 空 key 时, 跳过对应的值 (找到下一个逗号)
            int depth = 0;
            size_t comma = std::string::npos;
            for (size_t i = eq + 1; i < input.size(); i++) {
                if (input[i] == '(' || input[i] == '[' || input[i] == '{') depth++;
                else if (input[i] == ')' || input[i] == ']' || input[i] == '}') depth--;
                else if (input[i] == ',' && depth == 0) { comma = i; break; }
            }
            pos = (comma == std::string::npos) ? input.size() : comma + 1;
            continue;
        }
        int depth = 0;
        size_t comma = std::string::npos;
        for (size_t i = eq + 1; i < input.size(); i++) {
            if (input[i] == '(' || input[i] == '[' || input[i] == '{') depth++;
            else if (input[i] == ')' || input[i] == ']' || input[i] == '}') depth--;
            else if (input[i] == ',' && depth == 0) { comma = i; break; }
        }
        std::string val;
        if (comma == std::string::npos) {
            val = Trim(input.substr(eq + 1));
            pos = input.size();
        } else {
            val = Trim(input.substr(eq + 1, comma - eq - 1));
            pos = comma + 1;
        }
        result.push_back({key, val});
    }
    return result;
}

// ─── 时间工具 ────────────────────────────────────────────────────────────────

/// 获取当前系统时间戳（毫秒，Epoch）
inline uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// ─── 安全数值解析 ────────────────────────────────────────────────────────────

/// stoi 的异常安全版本，解析失败返回默认值
inline int SafeStoi(const std::string& s, int def = 0) {
    try { return std::stoi(s); } catch (...) { return def; }
}

/// stod 的异常安全版本，解析失败返回默认值
inline double SafeStod(const std::string& s, double def = 0.0) {
    try { return std::stod(s); } catch (...) { return def; }
}

#endif // STR_UTIL_H
