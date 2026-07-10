//=============================================================================
// test_ini_reader.cxx — INI 配置文件解析器单元测试
// 涵盖: 基本解析 / 注释 / 默认值 / 节遍历 / 边界情况
//=============================================================================

#include "mini_gtest.h"
#include "ini_reader.h"
#include <fstream>
#include <cstdio>

class IniReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpPath = "test_ini_reader_tmp.ini";
        std::ofstream f(tmpPath);
        f << "[Section1]" << std::endl;
        f << "key1=value1" << std::endl;
        f << "key2=42" << std::endl;
        f << "; comment line" << std::endl;
        f << "# also comment" << std::endl;
        f << "key3=true" << std::endl;
        f << "key4=false" << std::endl;
        f << std::endl;
        f << "[Section2]" << std::endl;
        f << "name=hello world" << std::endl;
        f << "enabled=1" << std::endl;
        f << "  trimmed_key  =  trimmed_value  " << std::endl;
        f << "empty_val=" << std::endl;
        f << "[Section_With_Underscores]" << std::endl;
        f << "foo=bar" << std::endl;
        f.close();

        reader_.Load(tmpPath);
    }

    void TearDown() override {
        reader_.Clear();
        std::remove(tmpPath.c_str());
    }

    IniReader reader_;
    std::string tmpPath;
};

// ==================== 基本读取 ====================

TEST_F(IniReaderTest, BasicGet) {
    EXPECT_EQ(reader_.Get("Section1", "key1"), "value1");
    EXPECT_EQ(reader_.Get("Section1", "key2"), "42");
    EXPECT_EQ(reader_.Get("Section2", "name"), "hello world");
}

TEST_F(IniReaderTest, GetWithDefault) {
    EXPECT_EQ(reader_.Get("Section1", "nonexistent", "default_val"), "default_val");
    EXPECT_EQ(reader_.Get("NonexistentSection", "key1", "default"), "default");
}

// ==================== GetInt / GetBool ====================

TEST_F(IniReaderTest, GetInt) {
    EXPECT_EQ(reader_.GetInt("Section1", "key2"), 42);
    EXPECT_EQ(reader_.GetInt("Section2", "enabled"), 1);
}

TEST_F(IniReaderTest, GetIntDefault) {
    EXPECT_EQ(reader_.GetInt("Section1", "nonexistent", 99), 99);
    EXPECT_EQ(reader_.GetInt("Nonexistent", "key", -1), -1);
}

TEST_F(IniReaderTest, GetBoolTrue) {
    EXPECT_TRUE(reader_.GetBool("Section1", "key3"));  // "true"
    EXPECT_TRUE(reader_.GetBool("Section2", "enabled"));  // "1"
}

TEST_F(IniReaderTest, GetBoolFalse) {
    EXPECT_FALSE(reader_.GetBool("Section1", "key4"));  // "false"
}

TEST_F(IniReaderTest, GetBoolDefault) {
    EXPECT_TRUE(reader_.GetBool("Section1", "nonexistent", true));
    EXPECT_FALSE(reader_.GetBool("Section1", "nonexistent", false));
}

// ==================== HasSection / HasKey ====================

TEST_F(IniReaderTest, HasSection) {
    EXPECT_TRUE(reader_.HasSection("Section1"));
    EXPECT_TRUE(reader_.HasSection("Section2"));
    EXPECT_TRUE(reader_.HasSection("Section_With_Underscores"));
    EXPECT_FALSE(reader_.HasSection("Nonexistent"));
}

TEST_F(IniReaderTest, HasKey) {
    EXPECT_TRUE(reader_.HasKey("Section1", "key1"));
    EXPECT_TRUE(reader_.HasKey("Section2", "name"));
    EXPECT_FALSE(reader_.HasKey("Section1", "nonexistent"));
    EXPECT_FALSE(reader_.HasKey("Nonexistent", "key1"));
}

TEST_F(IniReaderTest, HasKeyEmptyValue) {
    EXPECT_TRUE(reader_.HasKey("Section2", "empty_val"));
}

// ==================== 节遍历 ====================

TEST_F(IniReaderTest, Sections) {
    auto sections = reader_.Sections();
    EXPECT_EQ(sections.size(), 3);
    EXPECT_NE(std::find(sections.begin(), sections.end(), "Section1"),
              sections.end());
    EXPECT_NE(std::find(sections.begin(), sections.end(), "Section2"),
              sections.end());
    EXPECT_NE(std::find(sections.begin(), sections.end(), "Section_With_Underscores"),
              sections.end());
}

TEST_F(IniReaderTest, Keys) {
    auto keys = reader_.Keys("Section1");
    EXPECT_EQ(keys.size(), 4);  // key1, key2, key3, key4 (comments skipped)
    EXPECT_NE(std::find(keys.begin(), keys.end(), "key1"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "key2"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "key3"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "key4"), keys.end());
}

TEST_F(IniReaderTest, KeysEmptySection) {
    auto keys = reader_.Keys("Nonexistent");
    EXPECT_TRUE(keys.empty());
}

TEST_F(IniReaderTest, FindSections) {
    auto found = reader_.FindSections("Section");
    EXPECT_EQ(found.size(), 3);  // All start with "Section"

    auto foundExact = reader_.FindSections("Section1");
    ASSERT_EQ(foundExact.size(), 1);
    EXPECT_EQ(foundExact[0], "Section1");
}

// ==================== 空白处理 ====================

TEST_F(IniReaderTest, WhitespaceInKeysAndValues) {
    EXPECT_EQ(reader_.Get("Section2", "trimmed_key"), "trimmed_value");
}

// ==================== Clear ====================

TEST_F(IniReaderTest, Clear) {
    reader_.Clear();
    EXPECT_TRUE(reader_.Sections().empty());
    EXPECT_EQ(reader_.Get("Section1", "key1", "def"), "def");
}

// ==================== 文件加载 ====================

TEST(IniReaderFileTest, MissingFile) {
    IniReader r;
    EXPECT_FALSE(r.Load("nonexistent_file_12345_test.ini"));
}

TEST(IniReaderFileTest, EmptyFile) {
    const char* path = "test_empty_ini.tmp";
    std::ofstream f(path);
    f.close();

    IniReader r;
    EXPECT_TRUE(r.Load(path));
    EXPECT_TRUE(r.Sections().empty());

    std::remove(path);
}

TEST(IniReaderFileTest, CommentsOnly) {
    const char* path = "test_comments_only_ini.tmp";
    std::ofstream f(path);
    f << "; comment" << std::endl;
    f << "# another comment" << std::endl;
    f << std::endl;
    f.close();

    IniReader r;
    EXPECT_TRUE(r.Load(path));
    EXPECT_TRUE(r.Sections().empty());

    std::remove(path);
}

TEST(IniReaderFileTest, SingleSectionSingleKey) {
    const char* path = "test_single_ini.tmp";
    std::ofstream f(path);
    f << "[Test]" << std::endl;
    f << "x=1" << std::endl;
    f.close();

    IniReader r;
    EXPECT_TRUE(r.Load(path));
    EXPECT_EQ(r.Get("Test", "x"), "1");

    std::remove(path);
}

// ==================== 数据访问 ====================

TEST_F(IniReaderTest, DataConstRef) {
    const auto& data = reader_.Data();
    EXPECT_FALSE(data.empty());

    auto it = data.find("Section1");
    ASSERT_NE(it, data.end());
    EXPECT_EQ(it->second.at("key1"), "value1");
}
