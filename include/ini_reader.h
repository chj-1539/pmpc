#ifndef INI_READER_H
#define INI_READER_H

#include <string>
#include <vector>
#include <map>
#include <fstream>

/// 统一 INI 配置文件解析器
/// 支持: [Section] / key=value / ;注释 / #注释 / 行首尾空白裁剪
class IniReader {
public:
    IniReader() = default;

    /// 加载文件，返回是否成功
    bool Load(const std::string& path);

    /// 获取字符串值（section 不存在或 key 不存在返回默认值）
    std::string Get(const std::string& section, const std::string& key,
                    const std::string& def = "") const;

    /// 获取整数值
    int GetInt(const std::string& section, const std::string& key,
               int def = 0) const;

    /// 获取布尔值（1/true/yes → true，其余 false）
    bool GetBool(const std::string& section, const std::string& key,
                 bool def = false) const;

    /// 判断 section 是否存在
    bool HasSection(const std::string& section) const;

    /// 判断 section 下是否有某个 key
    bool HasKey(const std::string& section, const std::string& key) const;

    /// 获取所有 section 名
    std::vector<std::string> Sections() const;

    /// 获取某 section 下所有 key
    std::vector<std::string> Keys(const std::string& section) const;

    /// 查找以 prefix 开头的 sections（略去大小写）
    std::vector<std::string> FindSections(const std::string& prefix) const;

    /// 获取原始数据（整个文件）
    const std::map<std::string, std::map<std::string, std::string>>& Data() const { return data_; }

    /// 清空
    void Clear();

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

#endif // INI_READER_H
