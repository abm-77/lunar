#include <gtest/gtest.h>

#include <common/interner.h>

TEST(Interner, StartsEmpty) { EXPECT_EQ(Interner{}.size(), 0u); }

TEST(Interner, InternDeduplicates) {
  Interner I;
  SymId a = I.intern("hello");
  SymId b = I.intern("hello");
  EXPECT_EQ(a, b);
  EXPECT_EQ(I.size(), 1u);
}

TEST(Interner, DistinctStringsGetDistinctIds) {
  Interner I;
  SymId a = I.intern("foo");
  SymId b = I.intern("bar");
  SymId c = I.intern("baz");
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
  EXPECT_EQ(I.size(), 3u);
}

TEST(Interner, ViewRoundTrips) {
  Interner I;
  SymId id = I.intern("umbral");
  EXPECT_EQ(I.view(id), "umbral");
}

TEST(Interner, EmptyStringInterns) {
  Interner I;
  SymId a = I.intern("");
  SymId b = I.intern("");
  EXPECT_EQ(a, b);
  EXPECT_EQ(I.view(a), "");
}

TEST(Interner, IdsAreSequential) {
  Interner I;
  SymId a = I.intern("x");
  SymId b = I.intern("y");
  SymId c = I.intern("z");
  EXPECT_EQ(a, 0u);
  EXPECT_EQ(b, 1u);
  EXPECT_EQ(c, 2u);
}

TEST(Interner, ManyStringsAllDistinct) {
  Interner I;
  std::vector<std::string> strs;
  for (int i = 0; i < 100; ++i) strs.push_back("sym_" + std::to_string(i));

  std::vector<SymId> ids;
  for (auto &s : strs) ids.push_back(I.intern(s));

  for (size_t i = 0; i < ids.size(); ++i)
    for (size_t j = i + 1; j < ids.size(); ++j)
      EXPECT_NE(ids[i], ids[j]) << "collision at " << i << " and " << j;

  for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(I.view(ids[i]), strs[i]);
}

TEST(Interner, ViewOwnsMemoryIndependently) {
  Interner I;
  std::string_view sv;
  {
    std::string s = "transient";
    sv = I.view(I.intern(s));
  }
  EXPECT_EQ(sv, "transient");
}
