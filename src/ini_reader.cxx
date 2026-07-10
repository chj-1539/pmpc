#include "ini_reader.h"
#include "str_util.h"

bool IniReader::Load(const std::string& path) {
    std::ifstream fin(path);
    if (!fin.is_open()) return false;

    data_.clear();
    std::string line, curSection;

    while (std::getline(fin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[') {
            curSection = Trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos || curSection.empty()) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        data_[curSection][key] = val;
    }
    return true;
}

std::string IniReader::Get(const std::string& section, const std::string& key,
                           const std::string& def) const {
    auto sit = data_.find(section);
    if (sit == data_.end()) return def;
    auto kit = sit->second.find(key);
    return kit != sit->second.end() ? kit->second : def;
}

int IniReader::GetInt(const std::string& section, const std::string& key,
                      int def) const {
    auto v = Get(section, key, "");
    return v.empty() ? def : SafeStoi(v, def);
}

bool IniReader::GetBool(const std::string& section, const std::string& key,
                        bool def) const {
    auto v = ToLower(Get(section, key, ""));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes";
}

bool IniReader::HasSection(const std::string& section) const {
    return data_.find(section) != data_.end();
}

bool IniReader::HasKey(const std::string& section, const std::string& key) const {
    auto sit = data_.find(section);
    if (sit == data_.end()) return false;
    return sit->second.find(key) != sit->second.end();
}

std::vector<std::string> IniReader::Sections() const {
    std::vector<std::string> res;
    for (const auto& [k, _] : data_) res.push_back(k);
    return res;
}

std::vector<std::string> IniReader::Keys(const std::string& section) const {
    std::vector<std::string> res;
    auto sit = data_.find(section);
    if (sit != data_.end())
        for (const auto& [k, _] : sit->second) res.push_back(k);
    return res;
}

std::vector<std::string> IniReader::FindSections(const std::string& prefix) const {
    std::vector<std::string> res;
    auto lp = ToLower(prefix);
    for (const auto& [k, _] : data_)
        if (StartsWith(ToLower(k), lp)) res.push_back(k);
    return res;
}

void IniReader::Clear() { data_.clear(); }
