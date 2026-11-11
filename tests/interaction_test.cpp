// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the interaction ranges (the serverbound packet audit):
//   * the two attribute values and the two lag buffers, pinned to vanilla,
//   * `block_within_interaction_range` measuring from the eye to the block's
//     *unit cube* rather than its centre, and the exact edge of the sphere,
//   * the buffer's contribution (the sphere is range + buffer, 5.5 by default),
//   * `cursor_within_block`'s per-axis tolerance and where it actually bites.
//
// The wire tests (tests/block_interact_test.py) assert the same rules against a
// real client; these cases are what pin the geometry without a socket.
#include <cmath>
#include <cstdio>

#include "world/interaction.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A standing player at (0.5, 64.0, 0.5): the eye is PLAYER_DIMENSIONS eyeHeight
// (1.62) above the feet, so every check below measures from (0.5, 65.62, 0.5).
constexpr double kEyeX = 0.5;
constexpr double kEyeY = 64.0 + 1.62;
constexpr double kEyeZ = 0.5;

bool reach(int bx, int by, int bz, double buffer = kBlockInteractionBuffer) {
  return block_within_interaction_range(bx, by, bz, kEyeX, kEyeY, kEyeZ, buffer);
}

void test_constants() {
  // Attributes.BLOCK_INTERACTION_RANGE / ENTITY_INTERACTION_RANGE, and the
  // buffers ServerGamePacketListenerImpl passes (1.0 on the block paths, 3.0 on
  // the entity ones), so a block reaches 5.5 from the eye and an entity 6.0.
  CHECK_NEAR(kPlayerBlockInteractionRange, 4.5, 1e-12);
  CHECK_NEAR(kPlayerEntityInteractionRange, 3.0, 1e-12);
  CHECK_NEAR(kBlockInteractionBuffer, 1.0, 1e-12);
  CHECK_NEAR(kPlayerInteractionLagBuffer, 3.0, 1e-12);
  // handleAttack's reach with no ATTACK_RANGE component is exactly the sum.
  CHECK_NEAR(kPlayerEntityInteractionRange + kPlayerInteractionLagBuffer, 6.0, 1e-12);
  CHECK_NEAR(kPlayerBlockInteractionRange + kBlockInteractionBuffer, 5.5, 1e-12);
  // AbstractContainerMenu.stillValid's buffer: four blocks on top of the
  // attribute, the loosest of the ranges.
  CHECK_NEAR(kContainerValidityBuffer, 4.0, 1e-12);
  CHECK_NEAR(kPlayerBlockInteractionRange + kContainerValidityBuffer, 8.5, 1e-12);
}

void test_container_validity_is_the_loosest_range() {
  // The menu range exists because a screen is not a click: the player may walk
  // around in front of a chest and keep using it. Six blocks away is past the
  // 5.5-block placement limit and well inside the 8.5-block menu one, which is
  // exactly the gap `stillValid` covers — a chest you can no longer place
  // against but can still have open.
  CHECK(!reach(6, 65, 0));
  CHECK(reach(6, 65, 0, kContainerValidityBuffer));
  CHECK(reach(8, 65, 0, kContainerValidityBuffer));  // gap 7.5
  CHECK(!reach(9, 65, 0, kContainerValidityBuffer)); // gap 8.5: strict, so out
}

void test_own_blocks_are_in_reach() {
  // The block the player stands on (its top face is the feet's y), the block at
  // the feet's own level and the block the head is inside are all reachable:
  // one block beside a standing player is a distance of 0 to its own cube.
  CHECK(reach(0, 63, 0));  // underfoot: 1.62 straight down from the eye
  CHECK(reach(0, 64, 0));  // the feet's own level: 0 (the eye is above it)
  CHECK(reach(0, 65, 0));  // the eye's own block: 0
  CHECK(reach(1, 64, 0));  // one block beside: 0
  CHECK(!reach(0, 59, 0)); // six blocks below the feet: 65.62 - 60 = 5.62 > 5.5
  CHECK(reach(0, 60, 0));  // five below: 65.62 - 61 = 4.62
}

void test_reach_edge() {
  // Straight along +x at the eye's own block level: the near face of the block
  // at bx is bx from the origin, 0.5 of which the eye already occupies, so the
  // gap is bx - 0.5. The sphere is 5.5, so bx = 6 (gap exactly 5.5) is the first
  // block refused and bx = 5 (gap 4.5) the last accepted.
  CHECK(reach(5, 65, 0));
  CHECK(!reach(6, 65, 0));
  // The comparison is strict (`< limit^2`), so the boundary block is out.
  CHECK(!block_within_interaction_range(6, 65, 0, kEyeX, kEyeY, kEyeZ));
  // Vertically the same: (0, 71, 0) starts 71 - 65.62 = 5.38 below the limit and
  // (0, 72, 0) starts 6.38 past it.
  CHECK(reach(0, 71, 0));
  CHECK(!reach(0, 72, 0));
  // The buffer is what makes the sphere 5.5 rather than the raw attribute 4.5:
  // with the lag buffer removed, the block at bx = 5 is exactly on the edge.
  CHECK(!reach(5, 65, 0, 0.0));
  CHECK(reach(4, 65, 0, 0.0));
}

void test_measured_to_the_box_not_the_centre() {
  // Player.isWithinBlockInteractionRange measures `AABB(pos).distanceToSqr(eye)`
  // — the nearest point of the block's *unit cube*. The block diagonally one up
  // and four across is the case that tells the two apart: its nearest corner is
  // 4.9497 away, inside the 5.5 sphere, while its centre is 5.6575 away, outside.
  const int bx = 4, by = 65, bz = 4;
  CHECK(reach(bx, by, bz));

  const double near_dx = bx - kEyeX, near_dz = bz - kEyeZ; // dy = 0: same level
  const double nearest = std::sqrt(near_dx * near_dx + near_dz * near_dz);
  const double centre_dx = bx + 0.5 - kEyeX;
  const double centre_dy = by + 0.5 - kEyeY;
  const double centre_dz = bz + 0.5 - kEyeZ;
  const double centre =
      std::sqrt(centre_dx * centre_dx + centre_dy * centre_dy + centre_dz * centre_dz);
  CHECK_NEAR(nearest, 4.949747468, 1e-9);
  CHECK_NEAR(centre, 5.658127, 1e-6);
  CHECK(nearest < kPlayerBlockInteractionRange + kBlockInteractionBuffer);
  CHECK(centre > kPlayerBlockInteractionRange + kBlockInteractionBuffer);

  // ...and the same block one further out is refused on either measure.
  CHECK(!reach(bx + 1, by, bz + 1));
}

void test_cursor_tolerance() {
  // The packet carries the hit location relative to the block's minimum corner,
  // and vanilla compares it to the centre with a 1.0000001 per-axis tolerance.
  CHECK(cursor_within_block(0.5, 0.5, 0.5)); // dead centre
  CHECK(cursor_within_block(0.0, 0.0, 0.0)); // any corner of the block
  CHECK(cursor_within_block(1.0, 1.0, 1.0));
  CHECK(cursor_within_block(0.25, 0.75, 0.5)); // anywhere on a face
  // The tolerance is wider than the box: |cursor - 0.5| < 1.0000001 accepts
  // everything in (-0.5000001, 1.5000001) per axis.
  CHECK(cursor_within_block(1.5, 0.5, 0.5));
  CHECK(cursor_within_block(-0.5, 0.5, 0.5));
  CHECK(!cursor_within_block(1.5000002, 0.5, 0.5));
  CHECK(!cursor_within_block(-0.5000002, 0.5, 0.5));
  // What it does reject: a location that is not on (or within a block of) the
  // block the packet names.
  CHECK(!cursor_within_block(5.0, 0.5, 0.5));
  CHECK(!cursor_within_block(0.5, 0.5, 64.0));
  CHECK(!cursor_within_block(-3.0, 0.5, 0.5));
  // Every axis has to pass, not just one.
  CHECK(!cursor_within_block(0.5, 0.5, 2.0));
}

} // namespace

int main() {
  test_constants();
  test_container_validity_is_the_loosest_range();
  test_own_blocks_are_in_reach();
  test_reach_edge();
  test_measured_to_the_box_not_the_centre();
  test_cursor_tolerance();
  if (failures == 0)
    std::printf("interaction: all checks passed\n");
  else
    std::printf("interaction: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
