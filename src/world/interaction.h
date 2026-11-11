// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interaction ranges: how far a player's packet is allowed to reach (Phase 3
// follow-up — the serverbound packet audit).
//
// Every vanilla handler that names a block or an entity validates the packet by
// measuring from the player's *eye* to the target's bounding box, against an
// attribute plus a fixed lag buffer, and drops the packet when it is outside:
//
//   * Attributes.BLOCK_INTERACTION_RANGE, 4.5 (Attributes.java:23) —
//     `ServerGamePacketListenerImpl.handleUseItemOn` (line 1316) and
//     `ServerPlayerGameMode.handleBlockBreakAction` (line 155) both call
//     `player.isWithinBlockInteractionRange(pos, 1.0)`, so a placement or a dig
//     reaches 5.5 blocks from the eye to the block's unit box;
//   * Attributes.ENTITY_INTERACTION_RANGE, 3.0 (Attributes.java:28) —
//     `handleInteract` uses it with the same 3.0 buffer, and `handleAttack` uses
//     `isWithinAttackRange(item, box, 3.0)`, whose range with no ATTACK_RANGE
//     component is `AttackRange.defaultFor(player)` = ENTITY_INTERACTION_RANGE,
//     so a swing reaches 6.0. (`Mob.DEFAULT_ATTACK_REACH` is the same 3.0 written
//     as `sqrt(2.04) - 0.6` for a mob's own melee, see world/mob.h.)
//
// The measurement itself is `AABB(pos).distanceToSqr(eye) < (range + buffer)^2`
// (Player.java:1895), i.e. the distance is to the *nearest point of the block's
// unit cube*, never to its centre: a block under the player's feet is 1.6 blocks
// away from the eye, while its centre is 2.1.
//
// What no range can catch is a client that lies about *where* on the block it hit,
// which is why `handleUseItemOn` also checks the cursor the packet carries against
// the block it claims to have hit.
#pragma once

#include <algorithm>
#include <cmath>

namespace mc::world {

// Attributes.BLOCK_INTERACTION_RANGE / Attributes.ENTITY_INTERACTION_RANGE.
constexpr double kPlayerBlockInteractionRange = 4.5;
constexpr double kPlayerEntityInteractionRange = 3.0;
// The buffers the handlers pass for lag: 1.0 on both block paths, 3.0 on the
// entity ones (handleInteract and handleAttack).
constexpr double kBlockInteractionBuffer = 1.0;
constexpr double kPlayerInteractionLagBuffer = 3.0;
// The menus' own, much looser buffer: `AbstractContainerMenu.stillValid`
// (line 119) is `isWithinBlockInteractionRange(pos, 4.0)`, i.e. 8.5 blocks from
// the eye to the block. It is re-evaluated on every player tick, not on a
// packet (ServerPlayer.doTick line 700, and the tick at line 644), because that
// is what vanilla uses to close a screen the player has walked away from.
constexpr double kContainerValidityBuffer = 4.0;

// Player.isWithinBlockInteractionRange(pos, buffer): the gap from the eye to the
// block's unit cube, against BLOCK_INTERACTION_RANGE plus `buffer`. One block
// beside a standing player is 0 (the eye is over it), so the range is generous in
// exactly the way vanilla's is.
inline bool block_within_interaction_range(int bx, int by, int bz, double eye_x, double eye_y,
                                           double eye_z, double buffer = kBlockInteractionBuffer) {
  const double limit = kPlayerBlockInteractionRange + buffer;
  const double dx = std::max(0.0, std::max(static_cast<double>(bx) - eye_x, eye_x - (bx + 1.0)));
  const double dy = std::max(0.0, std::max(static_cast<double>(by) - eye_y, eye_y - (by + 1.0)));
  const double dz = std::max(0.0, std::max(static_cast<double>(bz) - eye_z, eye_z - (bz + 1.0)));
  return dx * dx + dy * dy + dz * dz < limit * limit;
}

// handleUseItemOn's cursor sanity check: the hit location a `use_item_on` carries
// has to sit within a hair of the block it claims (`|location - atCenterOf(pos)| <
// 1.0000001` per axis, line 1321). The packet encodes the location relative to the
// block's minimum corner, so vanilla's absolute comparison becomes `|cursor - 0.5|
// < 1.0000001` per axis: every point of the block's own box passes (the tolerance
// is deliberately a full half-block wider than the box on each side, which is
// slack for the float packing of a hit on a face), as does anything within a block
// of it. That is vanilla's own slack, kept rather than tightened, and it is worth
// being honest about what it does and does not buy: it does reject a coordinate
// that names a block the packet is not about — a client sending a position in the
// middle of nowhere, or the block it *actually* wants to touch, hoping the server
// reads the hit position instead of the block position — and it does not reject a
// client that lies within a block of the truth, which no range check can catch
// either (the range is measured from the block, not from the cursor).
inline bool cursor_within_block(double cursor_x, double cursor_y, double cursor_z) {
  constexpr double kLimit = 1.0000001;
  return std::abs(cursor_x - 0.5) < kLimit && std::abs(cursor_y - 0.5) < kLimit &&
         std::abs(cursor_z - 0.5) < kLimit;
}

} // namespace mc::world
