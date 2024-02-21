// Copyright 2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0
// SPDX-License-Identifier: BSD-3-Clause

#include "hwy/abort.h"

#include "gtest/gtest.h"
#include "hwy/base.h"

namespace hwy {

#ifdef GTEST_HAS_DEATH_TEST
TEST(AbortDeathTest, AbortDefault) {
  std::string expected = std::string("Abort at ") + __FILE__ + ":" +
                         std::to_string(__LINE__ + 1) + ": Test Abort";
  ASSERT_DEATH(HWY_ABORT("Test %s", "Abort"), expected);
}

TEST(AbortDeathTest, AbortOverride) {
  std::string expected =
      std::string("Test Abort from [0-9]+ of ") + __FILE__;

  ASSERT_DEATH(
      {
        AbortFunc CustomAbortHandler = [](const char* file, int line,
                                          const char* formatted_err) -> void {
          fprintf(stderr, "%s from %d of %s", formatted_err, line, file);
        };

        SetAbortFunc(CustomAbortHandler);
        HWY_ABORT("Test %s", "Abort");
      },
      expected);
}
#endif

TEST(AbortTest, AbortOverrideChain) {
  AbortFunc FirstHandler = [](const char* file, int line,
                              const char* formatted_err) -> void {
    fprintf(stderr, "%s from %d of %s", formatted_err, line, file);
  };
  AbortFunc SecondHandler = [](const char* file, int line,
                               const char* formatted_err) -> void {
    fprintf(stderr, "%s from %d of %s", formatted_err, line, file);
  };

  ASSERT_EQ(SetAbortFunc(FirstHandler), nullptr);
  ASSERT_EQ(GetAbortFunc(), FirstHandler);
  ASSERT_EQ(SetAbortFunc(SecondHandler), FirstHandler);
  ASSERT_EQ(GetAbortFunc(), SecondHandler);
  ASSERT_EQ(SetAbortFunc(nullptr), SecondHandler);
  ASSERT_EQ(GetAbortFunc(), nullptr);
}

}  // namespace hwy
