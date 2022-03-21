// Copyright 2022 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <inttypes.h>  // PRIu64
#include <stddef.h>
#include <stdint.h>
#include <string.h>  // memset

#include <array>  // IWYU pragma: keep

#include "hwy/base.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "tests/compress_test.cc"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"
#include "hwy/tests/test_util-inl.h"

HWY_BEFORE_NAMESPACE();
namespace hwy {
namespace HWY_NAMESPACE {

// For regenerating tables used in the implementation
#define HWY_PRINT_TABLES 0

class TestCompress {
  template <class D, class DI, typename T = TFromD<D>, typename TI = TFromD<DI>>
  void CheckStored(D d, DI di, size_t expected_pos, size_t actual_pos,
                   const AlignedFreeUniquePtr<T[]>& in,
                   const AlignedFreeUniquePtr<TI[]>& mask_lanes,
                   const AlignedFreeUniquePtr<T[]>& expected, const T* actual_u,
                   int line) {
    if (expected_pos != actual_pos) {
      hwy::Abort(
          __FILE__, line,
          "Size mismatch for %s: expected %" PRIu64 ", actual %" PRIu64 "\n",
          TypeName(T(), Lanes(d)).c_str(), static_cast<uint64_t>(expected_pos),
          static_cast<uint64_t>(actual_pos));
    }
    // Upper lanes are undefined. Modified from AssertVecEqual.
    for (size_t i = 0; i < expected_pos; ++i) {
      if (!IsEqual(expected[i], actual_u[i])) {
        fprintf(stderr,
                "Mismatch at i=%" PRIu64 " of %" PRIu64 ", line %d:\n\n",
                static_cast<uint64_t>(i), static_cast<uint64_t>(expected_pos),
                line);
        const size_t N = Lanes(d);
        Print(di, "mask", Load(di, mask_lanes.get()), 0, N);
        Print(d, "in", Load(d, in.get()), 0, N);
        Print(d, "expect", Load(d, expected.get()), 0, N);
        Print(d, "actual", Load(d, actual_u), 0, N);
        HWY_ASSERT(false);
      }
    }
  }

 public:
  template <class T, class D>
  HWY_NOINLINE void operator()(T /*unused*/, D d) {
    RandomState rng;

    using TI = MakeSigned<T>;  // For mask > 0 comparison
    const Rebind<TI, D> di;
    const size_t N = Lanes(d);

    const T zero{0};

    for (int frac : {0, 2, 3}) {
      // For CompressStore
      const size_t misalign = static_cast<size_t>(frac) * N / 4;

      auto in_lanes = AllocateAligned<T>(N);
      auto mask_lanes = AllocateAligned<TI>(N);
      auto expected = AllocateAligned<T>(N);
      auto actual_a = AllocateAligned<T>(misalign + N);
      T* actual_u = actual_a.get() + misalign;

      const size_t bits_size = RoundUpTo((N + 7) / 8, 8);
      auto bits = AllocateAligned<uint8_t>(bits_size);
      memset(bits.get(), 0, bits_size);  // for MSAN

      // Each lane should have a chance of having mask=true.
      for (size_t rep = 0; rep < AdjustedReps(200); ++rep) {
        size_t expected_pos = 0;
        for (size_t i = 0; i < N; ++i) {
          const uint64_t bits = Random32(&rng);
          in_lanes[i] = T();  // cannot initialize float16_t directly.
          CopyBytes<sizeof(T)>(&bits, &in_lanes[i]);
          mask_lanes[i] = (Random32(&rng) & 1024) ? TI(1) : TI(0);
          if (mask_lanes[i] > 0) {
            expected[expected_pos++] = in_lanes[i];
          }
        }

        const auto in = Load(d, in_lanes.get());
        const auto mask =
            RebindMask(d, Gt(Load(di, mask_lanes.get()), Zero(di)));
        StoreMaskBits(d, mask, bits.get());

        // Compress
        memset(actual_u, 0, N * sizeof(T));
        StoreU(Compress(in, mask), d, actual_u);
        CheckStored(d, di, expected_pos, expected_pos, in_lanes, mask_lanes,
                    expected, actual_u, __LINE__);

        // CompressStore
        memset(actual_u, 0, N * sizeof(T));
        const size_t size1 = CompressStore(in, mask, d, actual_u);
        CheckStored(d, di, expected_pos, size1, in_lanes, mask_lanes, expected,
                    actual_u, __LINE__);

        // CompressBlendedStore
        memset(actual_u, 0, N * sizeof(T));
        const size_t size2 = CompressBlendedStore(in, mask, d, actual_u);
        CheckStored(d, di, expected_pos, size2, in_lanes, mask_lanes, expected,
                    actual_u, __LINE__);
        // Subsequent lanes are untouched.
        for (size_t i = size2; i < N; ++i) {
          HWY_ASSERT_EQ(zero, actual_u[i]);
        }

        // CompressBits
        memset(actual_u, 0, N * sizeof(T));
        StoreU(CompressBits(in, bits.get()), d, actual_u);
        CheckStored(d, di, expected_pos, expected_pos, in_lanes, mask_lanes,
                    expected, actual_u, __LINE__);

        // CompressBitsStore
        memset(actual_u, 0, N * sizeof(T));
        const size_t size3 = CompressBitsStore(in, bits.get(), d, actual_u);
        CheckStored(d, di, expected_pos, size3, in_lanes, mask_lanes, expected,
                    actual_u, __LINE__);
      }  // rep
    }    // frac
  }      // operator()
};

#if HWY_PRINT_TABLES
namespace detail {  // for code folding
void PrintCompress16x8Tables() {
  printf("======================================= 16x8\n");
  constexpr size_t N = 8;  // 128-bit SIMD
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint8_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    // Doubled (for converting lane to byte indices)
    for (size_t i = 0; i < N; ++i) {
      printf("%d,", 2 * indices[i]);
    }
  }
  printf("\n");
}

// Similar to the above, but uses native 16-bit shuffle instead of bytes.
void PrintCompress16x16HalfTables() {
  printf("======================================= 16x16Half\n");
  constexpr size_t N = 8;
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint8_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    for (size_t i = 0; i < N; ++i) {
      printf("%d,", indices[i]);
    }
    printf("\n");
  }
  printf("\n");
}

// Compressed to nibbles
void PrintCompress32x8Tables() {
  printf("======================================= 32x8\n");
  constexpr size_t N = 8;  // AVX2
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint32_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    // Convert to nibbles
    uint64_t packed = 0;
    for (size_t i = 0; i < N; ++i) {
      HWY_ASSERT(indices[i] < 16);
      packed += indices[i] << (i * 4);
    }

    HWY_ASSERT(packed < (1ull << 32));
    printf("0x%08x,", static_cast<uint32_t>(packed));
  }
  printf("\n");
}

// Pairs of 32-bit lane indices
void PrintCompress64x4Tables() {
  printf("======================================= 64x4\n");
  constexpr size_t N = 4;  // AVX2
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint32_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    for (size_t i = 0; i < N; ++i) {
      printf("%d,%d,", 2 * indices[i], 2 * indices[i] + 1);
    }
  }
  printf("\n");
}

// 4-tuple of byte indices
void PrintCompress32x4Tables() {
  printf("======================================= 32x4\n");
  using T = uint32_t;
  constexpr size_t N = 4;  // SSE4
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint32_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    for (size_t i = 0; i < N; ++i) {
      for (size_t idx_byte = 0; idx_byte < sizeof(T); ++idx_byte) {
        printf("%" PRIu64 ",",
               static_cast<uint64_t>(sizeof(T) * indices[i] + idx_byte));
      }
    }
  }
  printf("\n");
}

// 8-tuple of byte indices
void PrintCompress64x2Tables() {
  printf("======================================= 64x2\n");
  using T = uint64_t;
  constexpr size_t N = 2;  // SSE4
  for (uint64_t code = 0; code < 1ull << N; ++code) {
    std::array<uint32_t, N> indices{0};
    size_t pos = 0;
    for (size_t i = 0; i < N; ++i) {
      if (code & (1ull << i)) {
        indices[pos++] = i;
      }
    }

    for (size_t i = 0; i < N; ++i) {
      for (size_t idx_byte = 0; idx_byte < sizeof(T); ++idx_byte) {
        printf("%" PRIu64 ",",
               static_cast<uint64_t>(sizeof(T) * indices[i] + idx_byte));
      }
    }
  }
  printf("\n");
}
}  // namespace detail
#endif  // HWY_PRINT_TABLES

HWY_NOINLINE void TestAllCompress() {
#if HWY_PRINT_TABLES
  detail::PrintCompress32x8Tables();
  detail::PrintCompress64x4Tables();
  detail::PrintCompress32x4Tables();
  detail::PrintCompress64x2Tables();
  detail::PrintCompress16x8Tables();
  detail::PrintCompress16x16HalfTables();
#endif

  ForUIF163264(ForPartialVectors<TestCompress>());
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace hwy {
HWY_BEFORE_TEST(HwyCompressTest);
HWY_EXPORT_AND_TEST_P(HwyCompressTest, TestAllCompress);
}  // namespace hwy

#endif