//=============================================================================
// test_str_util.cxx — str_util.h 纯函数测试
// 覆盖: Trim, ToLower, StartsWith, EndsWith, Split, ParseKeyValues,
//       SafeStoi, SafeStod, NowMs
//=============================================================================
#include "mini_gtest.h"
#include "str_util.h"

// ==================== Trim ====================

TEST(TrimTest, LeadingSpaces) {
    EXPECT_EQ(Trim("  hello"), "hello");
}

TEST(TrimTest, TrailingSpaces) {
    EXPECT_EQ(Trim("hello  "), "hello");
}

TEST(TrimTest, BothSides) {
    EXPECT_EQ(Trim("  hello world  "), "hello world");
}

TEST(TrimTest, TabsAndNewlines) {
    EXPECT_EQ(Trim("\t\r\n hello \r\n\t"), "hello");
}

TEST(TrimTest, EmptyString) {
    EXPECT_EQ(Trim(""), "");
}

TEST(TrimTest, AllWhitespace) {
    EXPECT_EQ(Trim("   \t\r\n  "), "");
}

TEST(TrimTest, NoTrimNeeded) {
    EXPECT_EQ(Trim("hello"), "hello");
}

// ==================== ToLower ====================

TEST(ToLowerTest, MixedCase) {
    EXPECT_EQ(ToLower("Hello World"), "hello world");
}

TEST(ToLowerTest, AlreadyLower) {
    EXPECT_EQ(ToLower("hello"), "hello");
}

TEST(ToLowerTest, AllUpper) {
    EXPECT_EQ(ToLower("HELLO"), "hello");
}

TEST(ToLowerTest, WithNumbersAndSymbols) {
    EXPECT_EQ(ToLower("Hello123!@#"), "hello123!@#");
}

TEST(ToLowerTest, EmptyString) {
    EXPECT_EQ(ToLower(""), "");
}

// ==================== StartsWith ====================

TEST(StartsWithTest, ExactMatch) {
    EXPECT_TRUE(StartsWith("hello", "hello"));
}

TEST(StartsWithTest, PrefixMatch) {
    EXPECT_TRUE(StartsWith("hello world", "hello"));
}

TEST(StartsWithTest, NoMatch) {
    EXPECT_FALSE(StartsWith("hello", "world"));
}

TEST(StartsWithTest, LongerPrefixReturnsFalse) {
    EXPECT_FALSE(StartsWith("hel", "hello"));
}

TEST(StartsWithTest, EmptyPrefix) {
    EXPECT_TRUE(StartsWith("hello", ""));
}

TEST(StartsWithTest, EmptyStringNoPrefix) {
    EXPECT_FALSE(StartsWith("", "hello"));
}

TEST(StartsWithTest, BothEmpty) {
    EXPECT_TRUE(StartsWith("", ""));
}

// ==================== EndsWith ====================

TEST(EndsWithTest, ExactMatch) {
    EXPECT_TRUE(EndsWith("hello", "hello"));
}

TEST(EndsWithTest, SuffixMatch) {
    EXPECT_TRUE(EndsWith("hello world", "world"));
}

TEST(EndsWithTest, NoMatch) {
    EXPECT_FALSE(EndsWith("hello", "world"));
}

TEST(EndsWithTest, LongerSuffixReturnsFalse) {
    EXPECT_FALSE(EndsWith("world", "hello world"));
}

TEST(EndsWithTest, EmptySuffix) {
    EXPECT_TRUE(EndsWith("hello", ""));
}

TEST(EndsWithTest, EmptyStringNoSuffix) {
    EXPECT_FALSE(EndsWith("", "suffix"));
}

// ==================== Split ====================

TEST(SplitTest, Normal) {
    auto r = Split("a,b,c", ',');
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], "a");
    EXPECT_EQ(r[1], "b");
    EXPECT_EQ(r[2], "c");
}

TEST(SplitTest, MultipleSeparators) {
    auto r = Split("a,,b,,,c", ',');
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], "a");
    EXPECT_EQ(r[1], "b");
    EXPECT_EQ(r[2], "c");
}

TEST(SplitTest, NoSeparator) {
    auto r = Split("abc", ',');
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "abc");
}

TEST(SplitTest, SingleElement) {
    auto r = Split("a", ',');
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "a");
}

TEST(SplitTest, EmptyString) {
    auto r = Split("", ',');
    EXPECT_EQ(r.size(), 0u);
}

TEST(SplitTest, WhitespaceAroundParts) {
    auto r = Split(" a , b , c ", ',');
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], "a");
    EXPECT_EQ(r[1], "b");
    EXPECT_EQ(r[2], "c");
}

TEST(SplitTest, DifferentSeparator) {
    auto r = Split(std::string("x|y|z"), '|');
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], "x");
    EXPECT_EQ(r[1], "y");
    EXPECT_EQ(r[2], "z");
}

// ==================== ParseKeyValues ====================

TEST(ParseKeyValuesTest, SimplePairs) {
    auto r = ParseKeyValues("a=1,b=2,c=3");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "1");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "2");
    EXPECT_EQ(r[2].first, "c");  EXPECT_EQ(r[2].second, "3");
}

TEST(ParseKeyValuesTest, WithParentheses) {
    auto r = ParseKeyValues("a=(1,2),b=3");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "(1,2)");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "3");
}

TEST(ParseKeyValuesTest, WithBrackets) {
    auto r = ParseKeyValues("a=[1,2],b=3");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "[1,2]");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "3");
}

TEST(ParseKeyValuesTest, WithBraces) {
    auto r = ParseKeyValues("a={1,2},b=3");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "{1,2}");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "3");
}

TEST(ParseKeyValuesTest, WhitespaceAroundKey) {
    auto r = ParseKeyValues(" a = 1 , b = 2 ");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "1");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "2");
}

TEST(ParseKeyValuesTest, EmptyKeySkipped) {
    auto r = ParseKeyValues("=1,b=2");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].first, "b");  EXPECT_EQ(r[0].second, "2");
}

TEST(ParseKeyValuesTest, TrailingComma) {
    auto r = ParseKeyValues("a=1,b=2,");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, "a");  EXPECT_EQ(r[0].second, "1");
    EXPECT_EQ(r[1].first, "b");  EXPECT_EQ(r[1].second, "2");
}

TEST(ParseKeyValuesTest, SinglePair) {
    auto r = ParseKeyValues("key=value");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].first, "key");
    EXPECT_EQ(r[0].second, "value");
}

TEST(ParseKeyValuesTest, EmptyInput) {
    auto r = ParseKeyValues("");
    EXPECT_EQ(r.size(), 0u);
}

TEST(ParseKeyValuesTest, NestedBrackets) {
    auto r = ParseKeyValues("a=[[1,2],3],b=4");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].second, "[[1,2],3]");
    EXPECT_EQ(r[1].first, "b");
}

// ==================== SafeStoi ====================

TEST(SafeStoiTest, ValidInteger) {
    EXPECT_EQ(SafeStoi("42"), 42);
}

TEST(SafeStoiTest, NegativeInteger) {
    EXPECT_EQ(SafeStoi("-17"), -17);
}

TEST(SafeStoiTest, InvalidString) {
    EXPECT_EQ(SafeStoi("abc"), 0);
}

TEST(SafeStoiTest, EmptyString) {
    EXPECT_EQ(SafeStoi(""), 0);
}

TEST(SafeStoiTest, CustomDefault) {
    EXPECT_EQ(SafeStoi("abc", -1), -1);
}

TEST(SafeStoiTest, LeadingWhitespace) {
    EXPECT_EQ(SafeStoi("  99"), 99);
}

TEST(SafeStoiTest, TrailingNonNumeric) {
    EXPECT_EQ(SafeStoi("99xyz"), 99);
}

// ==================== SafeStod ====================

TEST(SafeStodTest, ValidDouble) {
    EXPECT_DOUBLE_EQ(SafeStod("3.14"), 3.14);
}

TEST(SafeStodTest, IntegerString) {
    EXPECT_DOUBLE_EQ(SafeStod("42"), 42.0);
}

TEST(SafeStodTest, InvalidString) {
    EXPECT_DOUBLE_EQ(SafeStod("abc"), 0.0);
}

TEST(SafeStodTest, EmptyString) {
    EXPECT_DOUBLE_EQ(SafeStod(""), 0.0);
}

TEST(SafeStodTest, CustomDefault) {
    EXPECT_DOUBLE_EQ(SafeStod("abc", -1.5), -1.5);
}

TEST(SafeStodTest, ScientificNotation) {
    EXPECT_DOUBLE_EQ(SafeStod("1.5e2"), 150.0);
}

TEST(SafeStodTest, NegativeDouble) {
    EXPECT_DOUBLE_EQ(SafeStod("-2.5"), -2.5);
}

// ==================== NowMs ====================

TEST(NowMsTest, IsReasonable) {
    uint64_t now = NowMs();
    // Current epoch milliseconds should be > 1.7e12 (2024+) and < 2e12 (~2033)
    EXPECT_GT(now, 1700000000000ull);
    EXPECT_GT(2000000000000ull, now);
}

TEST(NowMsTest, MonotonicIncrease) {
    uint64_t t1 = NowMs();
    uint64_t t2 = NowMs();
    EXPECT_GE(t2, t1);
}
