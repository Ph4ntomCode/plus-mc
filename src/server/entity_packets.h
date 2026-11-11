// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clientbound entity packets emitted by the block-drop path (Milestone F).
// Kept in a header, not a .cpp, so the byte layout can be pinned by a unit test
// without a live client (mirrors the vanilla 26.2 codecs exactly).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <string>

#include <algorithm>
#include <optional>

#include "net/byte_buffer.h"
#include "protocol/ids.h"
#include "protocol/lp_vec.h"
#include "world/effect.h"
#include "world/explosion_effects_table.h"
#include "world/item.h"
#include "world/trade.h"
#include "world/world_light.h"

namespace mc::server {

// Index of ItemEntity.DATA_ITEM in synched-entity-data order: Entity defines 8
// values (flags, air supply, custom name, name visible, silent, no gravity,
// pose, frozen ticks), so the first ItemEntity accessor lands on 8.
constexpr uint8_t kItemEntityItemIndex = 8;

// ITEM_STACK serializer id — EntityDataSerializers registration order is
// BYTE, INT, LONG, FLOAT, STRING, COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK.
constexpr uint32_t kItemStackSerializerId = 7;

// One item entity's item stack, as Set Entity Data carries it:
//
//   0x63 set_entity_data:  varint entity id, then repeated
//                          [u8 data index][varint serializer id][value],
//                          terminated by 0xFF.
//   ITEM_STACK value:      ItemStack.OPTIONAL_STREAM_CODEC = varint count
//                          (0 = empty), then Item.STREAM_CODEC (registry
//                          varint) and the DataComponentPatch (empty patch =
//                          two zero varints).
//
// Note: there is NO byte between the count and the item id — the 1.20.5+ stack
// format dropped the old "present" flag (empty is count == 0). An extra byte
// here desyncs the client's decoder for the rest of the packet.
inline std::vector<uint8_t> make_item_entity_data(int32_t eid, int32_t item_id, int32_t count) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kItemEntityItemIndex);
  w.varint(kItemStackSerializerId);
  w.varint(static_cast<uint32_t>(count));
  w.varint(static_cast<uint32_t>(item_id));
  w.varint(0); // DataComponentPatch: no added components
  w.varint(0); // DataComponentPatch: no removed components
  w.u8(0xFF);  // end of synched data
  return std::move(w).take();
}

// Clientbound Add Entity (0x01). Velocity is the zero LpVec3 (a single 0x00
// byte), which is what vanilla sends for a spawn's first update; there is no
// entity physics yet, so the entity rests where it spawned.
inline std::vector<uint8_t> make_entity_spawn(int32_t eid, const uint8_t uuid[16],
                                              uint32_t entity_type_id, double x, double y, double z,
                                              double vx = 0.0, double vy = 0.0, double vz = 0.0) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySpawnEntityId));
  w.varint(static_cast<uint32_t>(eid));
  w.bytes(uuid, 16);
  w.varint(entity_type_id);
  w.f64(x);
  w.f64(y);
  w.f64(z);
  // ClientboundAddEntityPacket's velocity field is an LpVec3 (the packed
  // 3-short form), not three floats: `writeLpVec3` writes a single 0x00 byte
  // for a zero vector, which is what a stationary spawn sends.
  mc::protocol::write_lp_vec3(w, vx, vy, vz);
  w.u8(0); // xRot
  w.u8(0); // yRot
  w.u8(0); // yHeadRot
  w.varint(0);
  return std::move(w).take();
}

// Add Entity for a dropped item stack. A drop spawns with ItemEntity's own
// launch velocity (`random.nextDouble() * 0.2 - 0.1` on x and z, a flat 0.2 up),
// which the client integrates from until the first authoritative position sync.
inline std::vector<uint8_t> make_item_entity_spawn(int32_t eid, const uint8_t uuid[16], double x,
                                                   double y, double z, double vx = 0.0,
                                                   double vy = 0.0, double vz = 0.0) {
  return make_entity_spawn(eid, uuid, mc::protocol::kItemEntityTypeId, x, y, z, vx, vy, vz);
}

// ---- experience orbs (Phase 3 milestone E) ----------------------------------

// Index of ExperienceOrb.DATA_VALUE in synched-entity-data order. Entity defines
// the same 8 values ItemEntity follows, so the orb's value lands on 8 as well.
constexpr uint8_t kExperienceOrbValueIndex = 8;

// INT serializer id: EntityDataSerializers registration order is BYTE, INT,
// LONG, FLOAT, STRING, COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK, ... (the same
// order that makes ITEM_STACK 7 above).
constexpr uint32_t kIntSerializerId = 1;

// Set Entity Data (0x63) carrying one orb's value.
inline std::vector<uint8_t> make_experience_orb_data(int32_t eid, int32_t value) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kExperienceOrbValueIndex);
  w.varint(kIntSerializerId);
  w.u32(static_cast<uint32_t>(value));
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Add Entity for an experience orb, carrying the launch velocity ExperienceOrb's
// constructor gives it (the client integrates from there).
inline std::vector<uint8_t> make_experience_orb_spawn(int32_t eid, const uint8_t uuid[16], double x,
                                                      double y, double z, double vx = 0.0,
                                                      double vy = 0.0, double vz = 0.0) {
  return make_entity_spawn(eid, uuid, mc::protocol::kExperienceOrbEntityTypeId, x, y, z, vx, vy,
                           vz);
}

// sync_entity_position (0x23, ClientboundEntityPositionSyncPacket): the
// authoritative position of one entity. 26.3 replaced the 26.2
// `PositionMoveRotation` (position + delta movement + rotations) with a
// `PositionPath` dispatch:
//
//   varint entity id, [varint PositionPath type][PositionPath payload],
//   f32 yRot, f32 xRot, bool onGround.
//
// Type LINEAR is 0 and its payload is the end-position Vec3 (three f64).
// There is no delta movement on this packet any more: successive syncs are
// the path, which is how vanilla's `of(entity)` sends an ordinary tracking
// update. `vx`/`vy`/`vz` stay in the signature for the gameplay callers that
// still model velocity; the wire does not carry it.
// `yaw` / `pitch` are absolute rotation in degrees. The ordinary per-tick sync
// leaves them at zero (the entity's rotation did not move); an authoritative
// teleport passes the entity's own, because a zero here would snap the client's
// drawn rotation.
inline std::vector<uint8_t> make_entity_position_sync(int32_t eid, double x, double y, double z,
                                                      double vx, double vy, double vz,
                                                      bool on_ground, float yaw = 0.0f,
                                                      float pitch = 0.0f) {
  (void)vx;
  (void)vy;
  (void)vz;
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayEntityTeleportId));
  w.varint(static_cast<uint32_t>(eid));
  w.varint(0); // PositionPath.Type.LINEAR
  w.f64(x);
  w.f64(y);
  w.f64(z);
  w.f32(yaw);   // yRot
  w.f32(pitch); // xRot
  w.u8(on_ground ? 1 : 0);
  return std::move(w).take();
}

// explode (0x24, ClientboundExplodePacket), the packet `ServerLevel.explode`
// sends to every player within 64 blocks of the centre (`distanceToSqr(center)
// < 4096.0`), after the blast has been applied:
//
//   Vec3 center (three f64), f32 radius, varint block count,
//   Optional<Vec3> playerKnockback (a bool, then three f64 when set),
//   ParticleOptions explosionParticle (a registry id: EXPLOSION for a small
//   blast, EXPLOSION_EMITTER otherwise -- `ServerExplosion.isSmall`),
//   Holder<SoundEvent> explosionSound (SoundEvent.STREAM_CODEC is
//   `ByteBufCodecs.holder`: varint 0 then a direct event, or the registry id
//   *plus one* for a reference, so GENERIC_EXPLODE's id 699 rides as 700),
//   WeightedList<ExplosionParticleInfo> blockParticles: a varint count, then
//   per entry [particle id][f32 scaling][f32 speed][varint weight] --
//   `Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES` is POOF 0.5x and SMOKE 1.0x,
//   both weight 1.
//
// The block count is the ray-cast's own cell count (`toBlow.size()`), not the
// number of cells that survived the air check: vanilla reports the list it
// blasted, and the client uses it for the particle burst's intensity.
//
// `has_knockback` is false for a player the blast pushed nothing at: a dead
// one, or a flying creative one (ServerExplosion.hurtEntities skips the
// `hitPlayers` map for `isSpectator` and for `isCreative() && flying`).
inline std::vector<uint8_t> make_explode(double x, double y, double z, float radius,
                                         int32_t block_count, bool has_knockback, double kx,
                                         double ky, double kz, bool small) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayExplodeId));
  w.f64(x);
  w.f64(y);
  w.f64(z);
  w.f32(radius);
  w.varint(static_cast<uint32_t>(block_count));
  w.u8(has_knockback ? 1 : 0);
  if (has_knockback) {
    w.f64(kx);
    w.f64(ky);
    w.f64(kz);
  }
  w.varint(small ? mc::world::kParticleExplosion : mc::world::kParticleExplosionEmitter);
  w.varint(mc::world::kSoundGenericExplode + 1); // holder reference: id + 1
  w.varint(2); // WeightedList size: Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES
  w.varint(mc::world::kParticlePoof);
  w.f32(0.5f);
  w.f32(1.0f);
  w.varint(1);
  w.varint(mc::world::kParticleSmoke);
  w.f32(1.0f);
  w.f32(1.0f);
  w.varint(1);
  return std::move(w).take();
}

// set_entity_motion (0x65, ClientboundSetEntityMotionPacket): [varint id][LpVec3
// movement]. A single 0x00 byte for a stopped entity.
inline std::vector<uint8_t> make_set_entity_motion(int32_t eid, double vx, double vy, double vz) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityMotionId));
  w.varint(static_cast<uint32_t>(eid));
  mc::protocol::write_lp_vec3(w, vx, vy, vz);
  return std::move(w).take();
}

// take_item_entity (0x7c, ClientboundTakeItemEntityPacket): the pickup
// animation for one entity, taken by one player: three varints — the entity
// picked up, the player that took it, and the amount (always 1 for an orb,
// because ExperienceOrb.playerTouch calls `take(this, 1)`).
inline std::vector<uint8_t> make_take_item_entity(int32_t item_eid, int32_t player_eid,
                                                  int32_t amount) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayTakeItemEntityId));
  w.varint(static_cast<uint32_t>(item_eid));
  w.varint(static_cast<uint32_t>(player_eid));
  w.varint(static_cast<uint32_t>(amount));
  return std::move(w).take();
}

// remove_entities (0x4d): [varint count][varint id]... for entities the session
// spawns itself (an orb that expires or is merged away).
inline std::vector<uint8_t> make_remove_entity_ids(const std::vector<int32_t>& ids) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayRemoveEntitiesId));
  w.varint(static_cast<uint32_t>(ids.size()));
  for (int32_t id : ids)
    w.varint(static_cast<uint32_t>(id));
  return std::move(w).take();
}

// ---- boss bars (Phase 4's raid/boss slice) ----------------------------------
//
// boss_event (ClientboundBossEventPacket) is both the boss HUD and its whole
// lifecycle: every message carries the bar's UUID and one operation, and the
// client keys its bars by that UUID. The operation numbers are the enum's own
// order (ADD, REMOVE, UPDATE_PROGRESS, UPDATE_NAME, UPDATE_STYLE,
// UPDATE_PROPERTIES), and each operation writes only its fields — the client
// reads exactly those, so a stray field desynchronises the packet.
constexpr int kBossBarOpAdd = 0;
constexpr int kBossBarOpRemove = 1;
constexpr int kBossBarOpProgress = 2;

// BossBarColor: PINK, BLUE, RED, GREEN, YELLOW, PURPLE, WHITE.
constexpr int kBossBarColorPink = 0;
constexpr int kBossBarColorRed = 2;
constexpr int kBossBarColorPurple = 5;
// BossBarOverlay: PROGRESS, NOTCHED_6, NOTCHED_10, NOTCHED_12, NOTCHED_20.
constexpr int kBossBarOverlayProgress = 0;

// The ADD's flag byte, from `ClientboundBossEventPacket`'s own constants
// (`FLAG_DARKEN = 1`, `FLAG_MUSIC = 2`, `FLAG_FOG = 4`). They are not optional
// decoration: the wither's event is built with `setDarkenScreen(true)` and the
// dragon's by `EnderDragonFight` with `setPlayBossMusic(true)
// .setCreateWorldFog(true)`, so a bar that sends 0 renders a noticeably
// different encounter.
constexpr uint8_t kBossBarFlagDarkenScreen = 0x01;
constexpr uint8_t kBossBarFlagPlayBossMusic = 0x02;
constexpr uint8_t kBossBarFlagCreateWorldFog = 0x04;

// ADD: [uuid][varint 0][component name][f32 progress][varint color]
// [varint overlay][u8 flags]. The name is a network-NBT component, not a JSON
// string (`FriendlyByteBuf.writeComponent`).
inline std::vector<uint8_t> make_boss_event_add(const uint8_t uuid[16], const std::string& title,
                                                float progress, int color, int overlay,
                                                uint8_t flags = 0) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayBossEventId));
  w.bytes(uuid, 16);
  w.varint(static_cast<uint32_t>(kBossBarOpAdd));
  w.component_text(title);
  w.f32(progress);
  w.varint(static_cast<uint32_t>(color));
  w.varint(static_cast<uint32_t>(overlay));
  w.u8(flags);
  return std::move(w).take();
}

// REMOVE: [uuid][varint 1].
inline std::vector<uint8_t> make_boss_event_remove(const uint8_t uuid[16]) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayBossEventId));
  w.bytes(uuid, 16);
  w.varint(static_cast<uint32_t>(kBossBarOpRemove));
  return std::move(w).take();
}

// UPDATE_PROGRESS: [uuid][varint 2][f32 progress]. Progress is clamped to
// [0, 1] by the client's own setter, so 1.0 is a full bar.
inline std::vector<uint8_t> make_boss_event_progress(const uint8_t uuid[16], float progress) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayBossEventId));
  w.bytes(uuid, 16);
  w.varint(static_cast<uint32_t>(kBossBarOpProgress));
  w.f32(progress);
  return std::move(w).take();
}

// ---- mobs (Phase 3 milestone I) ---------------------------------------------

// Index of LivingEntity.DATA_LIVING_ENTITY_FLAGS in synched-entity-data order:
// Entity defines 8 values, then LivingEntity's own `defineId` order is
// DATA_LIVING_ENTITY_FLAGS (0), DATA_HEALTH (1), DATA_EFFECT_PARTICLES,
// DATA_EFFECT_AMBIENCE_ID, DATA_ARROW_COUNT_ID, DATA_STINGER_COUNT_ID,
// SLEEPING_POS_ID — so the item-use flags are the first one, at 8.
constexpr uint8_t kLivingEntityFlagsIndex = 8;
// Its bits, as `setLivingEntityFlag` writes them (it ORs the *mask* in) and
// vanilla's own readers read them: `isUsingItem` (0x01), `getUsedItemHand`
// (0x02) and `isAutoSpinAttack` (0x04). The last two have no writer here: an
// offhand use is refused (`handle_use_item`) and there is no kinetic weapon.
constexpr uint8_t kLivingEntityFlagUsingItem = 0x01;
constexpr uint8_t kLivingEntityFlagOffHand = 0x02;
constexpr uint8_t kLivingEntityFlagAutoSpinAttack = 0x04;

// Index of LivingEntity.DATA_HEALTH in the same order — the second one, at 8 + 1.
constexpr uint8_t kLivingEntityHealthIndex = 9;
// ...and of LivingEntity.DATA_EFFECT_PARTICLES, the third one, at 8 + 2. It is
// the only carrier of a *living entity's* status-effect swirls to anyone but the
// effect's owner: update_mob_effect goes to the owner alone
// (`ServerPlayer.onEffectAdded` / `onEffectUpdated`), so what every other client
// draws comes from this list.
constexpr uint8_t kLivingEntityEffectParticlesIndex = 10;
// ...and of `LivingEntity.DATA_EFFECT_AMBIENCE_ID`, the fourth one, at 8 + 3:
// the boolean `updateSynchronizedMobEffectParticles` writes beside the list,
// `areAllEffectsAmbient`'s answer (see `effects_all_ambient`). Its codec is
// EntityDataSerializers.BOOLEAN's, `ByteBufCodecs.BOOL` — one byte, and the same
// serializer id an area effect cloud carries its bits with.
constexpr uint8_t kLivingEntityEffectAmbienceIndex = 11;
// BYTE serializer id, and FLOAT's: EntityDataSerializers registers BYTE, INT,
// LONG, FLOAT, ... in that order.
constexpr uint32_t kByteSerializerId = 0;
constexpr uint32_t kFloatSerializerId = 3;

// `LivingEntity.DATA_LIVING_ENTITY_FLAGS` packed from the bits above. One
// builder, for the same reason as the shared flags': the bits share a byte, so a
// caller that carries one and assumes the others clear erases them.
inline uint8_t living_entity_flags_byte(bool using_item, bool off_hand = false,
                                        bool auto_spin_attack = false) {
  return static_cast<uint8_t>((using_item ? kLivingEntityFlagUsingItem : 0x00) |
                              (off_hand ? kLivingEntityFlagOffHand : 0x00) |
                              (auto_spin_attack ? kLivingEntityFlagAutoSpinAttack : 0x00));
}

// Set Entity Data carrying `LivingEntity.DATA_LIVING_ENTITY_FLAGS` — the byte
// that tells every *other* client what this entity is holding up (`startUsingItem`
// and `stopUsingItem` write it through `SynchedEntityData.set`, so it is a diff
// like the shared flags and the caller remembers what it last sent). The client
// animates its *own* use locally; this is the half everyone else sees.
inline std::vector<uint8_t> make_living_entity_flags(int32_t eid, uint8_t flags) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kLivingEntityFlagsIndex);
  w.varint(kByteSerializerId);
  w.u8(flags);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Mth.packDegrees: an angle in degrees as the byte Add Entity carries.
inline uint8_t pack_degrees(float degrees) {
  return static_cast<uint8_t>(static_cast<int8_t>(static_cast<int>(degrees * 256.0f / 360.0f)));
}

// Add Entity for a mob, with the rotation the client needs to draw it facing the
// right way (Entity.setYRot / setXRot / setYHeadRot all travel as packed
// degrees). The velocity is an LpVec3 like the item and orb spawns.
inline std::vector<uint8_t> make_mob_spawn(int32_t eid, const uint8_t uuid[16],
                                           uint32_t entity_type_id, double x, double y, double z,
                                           float yaw, float pitch, float head_yaw, double vx = 0.0,
                                           double vy = 0.0, double vz = 0.0) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySpawnEntityId));
  w.varint(static_cast<uint32_t>(eid));
  w.bytes(uuid, 16);
  w.varint(entity_type_id);
  w.f64(x);
  w.f64(y);
  w.f64(z);
  mc::protocol::write_lp_vec3(w, vx, vy, vz);
  w.u8(pack_degrees(pitch));
  w.u8(pack_degrees(yaw));
  w.u8(pack_degrees(head_yaw));
  w.varint(0); // data (unused for these entities)
  return std::move(w).take();
}

// Set Entity Data carrying one living entity's health word
// (`LivingEntity.DATA_HEALTH_ID`, synched-data index 9). Vanilla sends it at
// spawn because the synched default is 1.0 and a real health is not — the
// `LivingEntity` constructor writes the value, leaving the item dirty for the
// first tracker send — and again on every change (`LivingEntity.setHealth` ->
// `SynchedEntityData.set`), to the players tracking the entity *and* to the
// entity's own client (`ServerEntity.sendDirtyEntityData` ->
// `sendToTrackingPlayersAndSelf`, which has no player-specific arm: a player's
// word reaches the other clients exactly as a mob's does). A mob's readers use
// it for the damage/heal particles they draw on it; for a player it is the same
// answer for every client but the owner, whose bar is the separate set_health.
inline std::vector<uint8_t> make_health_word(int32_t eid, float health) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kLivingEntityHealthIndex);
  w.varint(kFloatSerializerId);
  w.f32(health);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Index of `Player.DATA_PLAYER_ABSORPTION_ID` in synched-entity-data order:
// Entity defines 0..7, LivingEntity 8..14 (health's 9 among them), Avatar the
// main-hand arm at 15 and the skin-layer mask at 16, then Player's own first
// accessor — absorption — at 17. Its default is 0.0
// (`Player.defineSynchedData`), and `Player.setAbsorptionAmount` writes it
// through `SynchedEntityData.set`, so it is a diff like the health word's.
constexpr uint8_t kPlayerAbsorptionIndex = 17;

// Set Entity Data carrying one player's absorption word
// (`Player.DATA_PLAYER_ABSORPTION_ID`). Vanilla sends it on every change
// (`Player.setAbsorptionAmount` -> `SynchedEntityData.set`) through the same
// tracker flush the health word rides (`ServerEntity.sendDirtyEntityData` ->
// `sendToTrackingPlayersAndSelf`), and there is *no* other packet that carries
// it: `ClientboundSetHealthPacket` is health, food and saturation, which is why
// the entity's own client is in the fan-out. The client draws the yellow hearts
// over the bar from this number alone.
inline std::vector<uint8_t> make_absorption_word(int32_t eid, float absorption) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kPlayerAbsorptionIndex);
  w.varint(kFloatSerializerId);
  w.f32(absorption);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Index of `Entity.DATA_SHARED_FLAGS_ID` in synched-entity-data order: it is the
// *first* accessor Entity defines, so 0.
constexpr uint8_t kSharedFlagsIndex = 0;
// The bits of that one byte this server can derive, in `Entity`'s own numbering:
// `FLAG_ONFIRE` 0, `FLAG_SHIFT_KEY_DOWN` 1, `FLAG_SPRINTING` 3, `FLAG_INVISIBLE`
// 5, `FLAG_GLOWING` 6. Each has its own derivation — the burning countdown
// (`setSharedFlagOnFire`), the input packet's sneak
// (`ServerGamePacketListenerImpl.handlePlayerInput` -> `setShiftKeyDown`), the
// sprint command (`handlePlayerCommand`'s START/STOP_SPRINTING ->
// `LivingEntity.setSprinting`), and the effect set
// (`LivingEntity.updateInvisibilityStatus` for bit 5 and `updateGlowingStatus`
// for bit 6) — and each is written by the same `Entity.setSharedFlag`. Bits 4
// (swimming) and 7 (fall flying) have no state to derive from here: nothing in
// this server makes a player swim or glide, so nothing sets them.
constexpr uint8_t kSharedFlagOnFire = 0x01;
constexpr uint8_t kSharedFlagShiftKeyDown = 0x02;
constexpr uint8_t kSharedFlagSprinting = 0x08;
constexpr uint8_t kSharedFlagInvisible = 0x20;
constexpr uint8_t kSharedFlagGlowing = 0x40;

// `Entity.DATA_SHARED_FLAGS_ID` packed from the bits above. One builder for the
// whole byte, so a caller cannot carry one bit and assume another is clear: they
// share the byte, and a sender that guesses erases what it did not carry.
inline uint8_t shared_flags_byte(bool on_fire, bool glowing, bool invisible = false,
                                 bool shift_key_down = false, bool sprinting = false) {
  return static_cast<uint8_t>((on_fire ? kSharedFlagOnFire : 0x00) |
                              (glowing ? kSharedFlagGlowing : 0x00) |
                              (invisible ? kSharedFlagInvisible : 0x00) |
                              (shift_key_down ? kSharedFlagShiftKeyDown : 0x00) |
                              (sprinting ? kSharedFlagSprinting : 0x00));
}

// Set Entity Data carrying the shared flags of any entity. Every derivation
// above writes through `SynchedEntityData.set`, so vanilla only sends this when
// the byte *changes*; the caller is the one that remembers what it last sent.
inline std::vector<uint8_t> make_shared_flags(int32_t eid, uint8_t flags) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kSharedFlagsIndex);
  w.varint(kByteSerializerId);
  w.u8(flags);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// ---- primed TNT (Phase 3 follow-up) -----------------------------------------
//
// A primed TNT block is a `PrimedTnt` entity: the client draws the TNT block
// out of its synched data and counts the fuse down itself, so the two values
// vanilla syncs are the fuse and the block state.

// Index of `PrimedTnt.DATA_FUSE_ID` and `PrimedTnt.DATA_BLOCK_STATE_ID`.
// PrimedTnt extends Entity (whose own eight values end at 7) and declares the
// fuse before the block state, so they land on 8 and 9 — the same slot
// ItemEntity's stack and a throwable's stack occupy, which is why the index
// alone means nothing without the entity it belongs to.
constexpr uint8_t kPrimedTntFuseIndex = 8;
constexpr uint8_t kPrimedTntBlockStateIndex = 9;

// BLOCK_STATE's serializer id: EntityDataSerializers registers BYTE, INT, LONG,
// FLOAT, STRING, COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK, BLOCK_STATE, ... in
// that order, so the block state is the ninth (8) — one past ITEM_STACK's 7.
constexpr uint32_t kBlockStateSerializerId = 8;

// Add Entity for a primed TNT. Vanilla's constructor gives it the launch the
// client integrates from (`Vec3(-sin(rot) * 0.02, 0.2, -cos(rot) * 0.02)`) and
// no rotation of its own, so this writes a zero yaw and pitch around the
// velocity make_mob_spawn's layout already carries.
inline std::vector<uint8_t> make_primed_tnt_spawn(int32_t eid, const uint8_t uuid[16], double x,
                                                  double y, double z, double vx, double vy,
                                                  double vz) {
  return make_mob_spawn(eid, uuid, mc::protocol::kPrimedTntEntityTypeId, x, y, z, 0.0f, 0.0f, 0.0f,
                        vx, vy, vz);
}

// Set Entity Data carrying a primed TNT's fuse and its block state, which is
// how `PrimedTnt.setFuse` and its `defineSynchedData` default travel. Both
// entries ride one packet because vanilla's initial sync sends the non-default
// values together (`ServerEntity.sendPairingData`); a later fuse change is sent
// on its own, which is what the caller's second call to this builder is for.
inline std::vector<uint8_t> make_primed_tnt_data(int32_t eid, int32_t fuse, int32_t block_state_id,
                                                 bool with_block_state = true) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kPrimedTntFuseIndex);
  w.varint(kIntSerializerId);
  w.varint(static_cast<uint32_t>(fuse));
  if (with_block_state) {
    w.u8(kPrimedTntBlockStateIndex);
    w.varint(kBlockStateSerializerId);
    w.varint(static_cast<uint32_t>(block_state_id));
  }
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// ---- thrown potions and their clouds (Phase 3 milestone G follow-up) --------
//
// A thrown potion is an `AbstractThrownPotion` (its Add Entity type is
// splash_potion or lingering_potion) whose synched item stack is the bottle, so
// the client draws the right colour; a lingering potion leaves an
// `AreaEffectCloud`, whose radius, waiting flag and particle are all synched
// data.

// Index of ThrowableItemProjectile.DATA_ITEM_STACK. A throwable item projectile
// extends Entity (eight values: flags, air supply, custom name, name visible,
// silent, no gravity, pose, frozen ticks) exactly as ItemEntity does, so its
// first accessor is 8 as well — the *same* index, a different entity.
constexpr uint8_t kThrowableItemStackIndex = 8;

// Add Entity for a thrown potion. `Projectile.shoot` sets the entity's rotation
// to the direction of flight (yRot from x/z, xRot from y against the horizontal
// distance), and that is what the client orients the bottle with — the layout is
// the one make_mob_spawn writes, so it delegates rather than repeating it.
inline std::vector<uint8_t> make_potion_spawn(int32_t eid, const uint8_t uuid[16],
                                              uint32_t entity_type_id, double x, double y, double z,
                                              float yaw, float pitch, double vx, double vy,
                                              double vz) {
  return make_mob_spawn(eid, uuid, entity_type_id, x, y, z, yaw, pitch, yaw, vx, vy, vz);
}

// Set Entity Data carrying one throwable projectile's item stack. The stack is
// the thrown bottle *with its components* — the `potion_contents` is what the
// client tints the bottle and its trail with — so this writes the real stack
// rather than the count-and-item pair make_item_entity_data uses.
inline std::vector<uint8_t> make_throwable_item_data(int32_t eid,
                                                     const mc::world::ItemStack& stack) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kThrowableItemStackIndex);
  w.varint(kItemStackSerializerId);
  mc::world::write_item_stack(w, stack);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Index of `AbstractArrow`'s three accessors. `Projectile` defines none of its
// own, so an arrow's defineSynchedData order — ID_FLAGS, PIERCE_LEVEL,
// Index of AreaEffectCloud's three accessors. Entity defines the same eight
// values, so the cloud's own defineId order — DATA_RADIUS, DATA_WAITING,
// DATA_PARTICLE — starts at 8.
constexpr uint8_t kAreaEffectCloudRadiusIndex = 8;
constexpr uint8_t kAreaEffectCloudWaitingIndex = 9;
constexpr uint8_t kAreaEffectCloudParticleIndex = 10;

// BOOLEAN and PARTICLE serializer ids. EntityDataSerializers registers BYTE,
// INT, LONG, FLOAT, STRING, COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK (7),
// BOOLEAN (8), ROTATIONS, BLOCK_POS, OPTIONAL_BLOCK_POS, DIRECTION,
// OPTIONAL_LIVING_ENTITY_REFERENCE, BLOCK_STATE, OPTIONAL_BLOCK_STATE, PARTICLE
// (16) — so the two the cloud needs are 8 and 16, not the neighbours of the
// ITEM_STACK id above.
constexpr uint32_t kBooleanSerializerId = 8;
constexpr uint32_t kParticleSerializerId = 16;
// ...and PARTICLES (17), the *list* of them `LivingEntity.DATA_EFFECT_PARTICLES`
// carries. Its codec is `ParticleTypes.STREAM_CODEC.apply(ByteBufCodecs.list())`:
// a varint count, then for each entry the particle type's registry id followed
// by that type's own payload (the ARGB colour for minecraft:entity_effect,
// nothing at all for a bare SimpleParticleType).
constexpr uint32_t kParticlesSerializerId = 17;
// minecraft:entity_effect in the minecraft:particle_type registry, the type
// `ColorParticleOption.create(ParticleTypes.ENTITY_EFFECT, color)` names; its
// stream codec is one INT (the ARGB colour), not a nested particle.
constexpr uint32_t kEntityEffectParticleId = 28;

// Index of `AgeableMob.DATA_BABY_ID` in synched-entity-data order (Phase 4
// milestone I): Entity's own 0..7, LivingEntity's 8..14 (`DATA_HEALTH` is 9 —
// the *field declaration* order assigns these, see the absorption word's note),
// Mob's own `DATA_MOB_FLAGS_ID` at 15, and then AgeableMob's first accessor —
// the baby bit — at 16, with AGE_LOCKED at 17.
constexpr uint8_t kAgeableMobBabyIndex = 16;

// Set Entity Data carrying `AgeableMob.DATA_BABY_ID`. `AgeableMob.setBaby` and
// `setAge` write it through `SynchedEntityData.set`, and it is the *only* thing
// that tells a client a child is a child: Add Entity's trailing `data` int is
// not synched data, and the box it draws comes from the bit alone. It is a diff
// like the health word's, so the caller sends it when it flips — on a newborn's
// spawn, and again when the age crosses 0 and the mob grows up. (The BOOLEAN
// serializer id is the one just above, 8.)
inline std::vector<uint8_t> make_baby_word(int32_t eid, bool baby) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kAgeableMobBabyIndex);
  w.varint(kBooleanSerializerId);
  w.u8(baby ? 1 : 0);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Index of `AbstractArrow`'s three accessors. `Projectile` defines none of its
// own, so an arrow's defineSynchedData order — ID_FLAGS, PIERCE_LEVEL,
// IN_GROUND — starts at the same 8 Entity's eight values end at (the cloud and
// the throwable item stack are the same offset for the same reason).
constexpr uint8_t kArrowFlagsIndex = 8;
constexpr uint8_t kArrowPierceIndex = 9;
constexpr uint8_t kArrowInGroundIndex = 10;
// `AbstractArrow.FLAG_CRIT`, the one bit of ID_FLAGS the client draws (the crit
// particles a fully drawn bow's arrow trails).
constexpr uint8_t kArrowFlagCrit = 0x01;

// Set Entity Data for an arrow: whether it is a critical one and whether it has
// buried itself. Both go through SynchedEntityData.set in vanilla, so the caller
// is the one that decides when to send them; they travel together because each
// is written the moment it changes (`setCritArrow` at the spawn and
// `setInGround` at the bury). PIERCE_LEVEL is deliberately not sent: nothing the
// client draws reads it (see the milestone H limitations).
inline std::vector<uint8_t> make_arrow_data(int32_t eid, bool critical, bool in_ground) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kArrowFlagsIndex);
  w.varint(kByteSerializerId);
  w.u8(critical ? kArrowFlagCrit : 0);
  w.u8(kArrowInGroundIndex);
  w.varint(kBooleanSerializerId);
  w.u8(in_ground ? 1 : 0);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Add Entity for an area effect cloud. Its dimensions come from its radius, so
// the box is whatever the synched-data packet below says; the spawn itself is a
// plain zero-velocity one.
inline std::vector<uint8_t> make_cloud_spawn(int32_t eid, const uint8_t uuid[16], double x,
                                             double y, double z) {
  return make_entity_spawn(eid, uuid, mc::protocol::kAreaEffectCloudEntityTypeId, x, y, z);
}

// Set Entity Data for one cloud: its radius, whether it is still waiting out its
// warm-up, and the colour particle the client draws it with (`updateParticle`
// runs on every setPotionContents, so vanilla always sends all three together at
// least once). The entries are in ascending index order, which is the order
// SynchedEntityData packs them in.
inline std::vector<uint8_t> make_cloud_data(int32_t eid, float radius, bool waiting,
                                            uint32_t color) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kAreaEffectCloudRadiusIndex);
  w.varint(kFloatSerializerId);
  w.f32(radius);
  w.u8(kAreaEffectCloudWaitingIndex);
  w.varint(kBooleanSerializerId);
  w.u8(waiting ? 1 : 0);
  w.u8(kAreaEffectCloudParticleIndex);
  w.varint(kParticleSerializerId);
  w.varint(kEntityEffectParticleId);
  w.u32(color);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// The `LivingEntity.DATA_EFFECT_PARTICLES` entry (index 10) as it appears inside
// a Set Entity Data: the index, the PARTICLES serializer, then the codec — a
// varint count and for each entry the particle type's registry id followed by
// that type's own payload. One writer, because a packet that carries this list
// beside other values (the join burst, or the ambience byte `make_effect_data`
// writes with it) has to write exactly what the list alone would be.
inline void write_effect_particles_entry(mc::net::Writer& w,
                                         const std::vector<mc::world::EffectParticle>& particles) {
  w.u8(kLivingEntityEffectParticlesIndex);
  w.varint(kParticlesSerializerId);
  w.varint(static_cast<uint32_t>(particles.size()));
  for (const mc::world::EffectParticle& p : particles) {
    w.varint(p.particle_type);
    if (p.has_color)
      w.u32(p.color);
  }
}

// The `LivingEntity.DATA_EFFECT_AMBIENCE_ID` entry (index 11) as it appears
// inside a Set Entity Data: the index, the BOOLEAN serializer, then one byte
// (`ByteBufCodecs.BOOL` writes `true` as 1). One writer, for the same reason the
// list's is one: the join burst and the change packet have to write the value
// the same way.
inline void write_effect_ambience_entry(mc::net::Writer& w, bool ambience) {
  w.u8(kLivingEntityEffectAmbienceIndex);
  w.varint(kBooleanSerializerId);
  w.u8(ambience ? 1 : 0);
}

// Set Entity Data carrying the two values
// `LivingEntity.updateSynchronizedMobEffectParticles` writes — the particle list
// (index 10) a client draws a living entity's status-effect swirls from, and the
// ambience byte (index 11) it draws them *sparsely* from. One method writes
// both, so both belong in one packet when both moved: `SynchedEntityData.set`
// marks each value dirty as it changes and `sendDirtyEntityData` flushes them
// together.
//
// Each half is optional because that diff is: `send_particles` is false when the
// client already holds the list, and an *empty* list is a real value (it is how
// the last swirl is taken down: `removeEffectParticles` writes exactly it), so
// the caller sends it on a change rather than only when it is non-empty.
// `send_ambience` is false when vanilla did not write it at all — the list's
// take-down runs on an empty set, and `removeEffectParticles` sets the list
// alone, so a client keeps the last answer a non-empty set gave.
inline std::vector<uint8_t>
make_effect_data(int32_t eid, bool send_particles,
                 const std::vector<mc::world::EffectParticle>& particles, bool send_ambience,
                 bool ambience) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  if (send_particles)
    write_effect_particles_entry(w, particles);
  if (send_ambience)
    write_effect_ambience_entry(w, ambience);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// ---- villagers (Phase 4 milestone L) ---------------------------------------
//
// Three synched-data values and one packet, all defined by `Villager` and
// `MerchantMenu`. The index/order arithmetic, since it is the same trap the
// ambience byte had:
//
//   Entity defines 0..7; LivingEntity 8..14 (health 9, effect particles 10,
//   ambience 11, ...); Mob's DATA_MOB_FLAGS_ID is 15; AgeableMob defines *two* —
//   DATA_BABY_ID 16 and AGE_LOCKED 17; AbstractVillager's DATA_UNHAPPY_COUNTER
//   is 18; and then Villager's own two: DATA_VILLAGER_DATA 19 and
//   DATA_VILLAGER_DATA_FINALIZED 20.
constexpr uint8_t kAbstractVillagerUnhappyIndex = 18;
constexpr uint8_t kVillagerDataIndex = 19;
constexpr uint8_t kVillagerDataFinalizedIndex = 20;
// VILLAGER_DATA's serializer id: the registration order is BYTE, INT, LONG,
// FLOAT, STRING, COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK, BOOLEAN, ROTATIONS,
// BLOCK_POS, OPTIONAL_BLOCK_POS, DIRECTION, OPTIONAL_LIVING_ENTITY_REFERENCE,
// BLOCK_STATE, OPTIONAL_BLOCK_STATE, PARTICLE, PARTICLES, VILLAGER_DATA — the
// nineteenth entry, id 18.
constexpr uint32_t kVillagerDataSerializerId = 18;

// The `VillagerData.STREAM_CODEC` payload: the type's registry id, the
// profession's, and the level, all varints. Both registries are the game's
// static ones (their bootstrap order is `world/trade.h`'s enum), not the
// synchronised kind, so the ids on the wire are their registration indices.
inline void write_villager_data_entry(mc::net::Writer& w, uint8_t type, uint8_t profession,
                                      int level) {
  w.u8(kVillagerDataIndex);
  w.varint(kVillagerDataSerializerId);
  w.varint(static_cast<uint32_t>(type));
  w.varint(static_cast<uint32_t>(profession));
  w.varint(static_cast<uint32_t>(level));
}

// `Villager.DATA_VILLAGER_DATA_FINALIZED` (a BOOLEAN word, index 20).
inline void write_villager_finalized_entry(mc::net::Writer& w, bool finalized) {
  w.u8(kVillagerDataFinalizedIndex);
  w.varint(kBooleanSerializerId);
  w.u8(finalized ? 1 : 0);
}

// Set Entity Data carrying `Villager.DATA_VILLAGER_DATA`. Vanilla writes it at
// spawn (the field's default is a plains level-1 none) and again whenever
// `setVillagerData` runs — a profession assignment, or a level-up.
inline std::vector<uint8_t> make_villager_data_word(int32_t eid, uint8_t type, uint8_t profession,
                                                    int level) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  write_villager_data_entry(w, type, profession, level);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Set Entity Data carrying `Villager.DATA_VILLAGER_DATA_FINALIZED`. A villager
// finalises its data in `finalizeVillagerType`, which every spawn reaches, so a
// spawned villager's bit is true; it exists so a client knows the type/profession
// will not be re-rolled.
inline std::vector<uint8_t> make_villager_finalized_word(int32_t eid, bool finalized) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  write_villager_finalized_entry(w, finalized);
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// Set Entity Data carrying `AbstractVillager.DATA_UNHAPPY_COUNTER` — what
// `Villager.setUnhappy` sets to 40 when a player right-clicks a villager with
// nothing to sell. The word is an INT, i.e. a varint (EntityDataSerializers.INT
// is `forValueType(ByteBufCodecs.VAR_INT)`), not a fixed four bytes. The client
// draws the head shake from it; the server-side one-tick decrement changes no
// drawing and is not sent.
inline std::vector<uint8_t> make_villager_unhappy_word(int32_t eid, int counter) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(kAbstractVillagerUnhappyIndex);
  w.varint(kIntSerializerId);
  w.varint(static_cast<uint32_t>(counter));
  w.u8(0xFF); // end of synched data
  return std::move(w).take();
}

// merchant_offers (0x34, ClientboundMerchantOffersPacket):
//
//   varint container id
//   varint offer count
//     per offer (MerchantOffer.STREAM_CODEC):
//       ItemCost  buy      [varint item id][varint count][DataComponentExactPredicate]
//       ItemStack sell     (the trusted stream codec)
//       optional ItemCost buyB  [bool][ItemCost when true]
//       bool  is exhausted
//       i32   uses
//       i32   max uses
//       i32   xp
//       i32   special price diff
//       f32   price multiplier
//       i32   demand
//   varint villager level
//   varint villager xp
//   bool   show progress (the trade screen's xp bar)
//   bool   can restock
//
// The predicate is `DataComponentExactPredicate.STREAM_CODEC`: a count-prefixed
// list of typed components, which is one `varint 0` for the empty predicate
// every trade in the pack carries. The seven fields from `uses` on are fixed
// big-endian (`FriendlyByteBuf.writeInt`/`writeFloat`), unlike the varints
// around them.
inline void write_item_cost(mc::net::Writer& w, const mc::world::TradeCost& cost) {
  w.varint(static_cast<uint32_t>(cost.item));
  w.varint(static_cast<uint32_t>(cost.count));
  w.varint(0); // DataComponentExactPredicate: no expected components
}

inline std::vector<uint8_t>
make_merchant_offers(int container_id, const std::vector<mc::world::MerchantOffer>& offers,
                     int villager_level, int villager_xp, bool show_progress, bool can_restock) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayMerchantOffersId));
  w.varint(static_cast<uint32_t>(container_id));
  w.varint(static_cast<uint32_t>(offers.size()));
  for (const mc::world::MerchantOffer& offer : offers) {
    write_item_cost(w, offer.cost_a);
    mc::world::write_item_stack(w, offer.result);
    w.u8(offer.has_cost_b ? 1 : 0);
    if (offer.has_cost_b)
      write_item_cost(w, offer.cost_b);
    w.u8(offer.is_out_of_stock() ? 1 : 0);
    w.u32(static_cast<uint32_t>(offer.uses));
    w.u32(static_cast<uint32_t>(offer.max_uses));
    w.u32(static_cast<uint32_t>(offer.xp));
    w.u32(static_cast<uint32_t>(offer.special_price_diff));
    w.f32(offer.price_multiplier);
    w.u32(static_cast<uint32_t>(offer.demand));
  }
  w.varint(static_cast<uint32_t>(villager_level));
  w.varint(static_cast<uint32_t>(villager_xp));
  w.u8(show_progress ? 1 : 0);
  w.u8(can_restock ? 1 : 0);
  return std::move(w).take();
}

// ---- health, damage and death (Phase 3 milestone F) -------------------------

// set_health (0x68): [f32 health][varint food][f32 saturation]. There is no
// entity id — the packet always describes the receiving player. Note the food
// field is a *varint* while health and saturation are floats.
inline std::vector<uint8_t> make_set_health(float health, int32_t food, float saturation) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetHealthId));
  w.f32(health);
  w.varint(static_cast<uint32_t>(food));
  w.f32(saturation);
  return std::move(w).take();
}

// update_mob_effect (0x84), the packet that puts an icon in the client's effect
// list:
//
//   varint entity id
//   varint effect id                 (holderRegistry -> minecraft:mob_effect)
//   varint amplifier
//   varint duration in ticks         (-1 for infinite)
//   u8     flags
//
// The flags are ClientboundUpdateMobEffectPacket's own bits
// (ambient|visible|show_icon|blend). `blend` is the one that is *not* read off
// the instance: vanilla passes true from ServerPlayer.onEffectAdded and false
// from onEffectUpdated and from the join sync (PlayerList.sendActiveEffects),
// which is what makes a fresh effect blend in but a refreshed one not.
inline std::vector<uint8_t> make_update_mob_effect(int32_t eid, int32_t effect_id,
                                                   int32_t amplifier, int32_t duration,
                                                   bool ambient, bool visible, bool show_icon,
                                                   bool blend) {
  uint8_t flags = 0;
  if (ambient)
    flags |= mc::protocol::kMobEffectFlagAmbient;
  if (visible)
    flags |= mc::protocol::kMobEffectFlagVisible;
  if (show_icon)
    flags |= mc::protocol::kMobEffectFlagShowIcon;
  if (blend)
    flags |= mc::protocol::kMobEffectFlagBlend;
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayUpdateMobEffectId));
  w.varint(static_cast<uint32_t>(eid));
  w.varint(static_cast<uint32_t>(effect_id));
  w.varint(static_cast<uint32_t>(amplifier));
  w.varint(static_cast<uint32_t>(duration));
  w.u8(flags);
  return std::move(w).take();
}

// remove_mob_effect (0x4e): [varint entity id][varint effect id]. It clears the
// client's icon, which is why it has to be sent when an effect expires — the
// server's own expiry is invisible to the client otherwise.
inline std::vector<uint8_t> make_remove_mob_effect(int32_t eid, int32_t effect_id) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayRemoveMobEffectId));
  w.varint(static_cast<uint32_t>(eid));
  w.varint(static_cast<uint32_t>(effect_id));
  return std::move(w).take();
}

// cooldown (0x16), the packet that draws the item's cooldown sweep over the
// hotbar icon:
//
//   string cooldown group      (Identifier.STREAM_CODEC: a UTF-8 string)
//   varint duration in ticks
//
// ServerItemCooldowns sends it twice per cooldown: once when it starts, carrying
// the full duration `UseCooldown.ticks()` asked for, and once when the range
// runs out, carrying 0 — the duration is never the remaining time.
inline std::vector<uint8_t> make_cooldown(const std::string& group, int32_t duration) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayCooldownId));
  w.string(group);
  w.varint(static_cast<uint32_t>(duration));
  return std::move(w).take();
}

// damage_event (0x19), which is what plays the hurt sound and the red flash:
//
//   varint entity id
//   varint damage type id            (holderRegistry -> minecraft:damage_type)
//   varint causing entity id + 1     (0 means "no causing entity")
//   varint direct entity id + 1
//   bool   has source position, then 3 doubles when set
//
// The +1 encoding is the packet's own: `writeOptionalEntityId` writes id + 1 so
// that -1 ("none") becomes 0 and the field stays a single byte.
inline std::vector<uint8_t> make_damage_event(int32_t eid, int32_t damage_type_id,
                                              int32_t causing_id, int32_t direct_id,
                                              bool has_position = false, double x = 0.0,
                                              double y = 0.0, double z = 0.0) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayDamageEventId));
  w.varint(static_cast<uint32_t>(eid));
  w.varint(static_cast<uint32_t>(damage_type_id));
  w.varint(static_cast<uint32_t>(causing_id + 1));
  w.varint(static_cast<uint32_t>(direct_id + 1));
  w.u8(has_position ? 1 : 0);
  if (has_position) {
    w.f64(x);
    w.f64(y);
    w.f64(z);
  }
  return std::move(w).take();
}

// hurt_animation (0x2a): [varint entity id][f32 yaw]. The yaw is
// LivingEntity.getHurtDir, which is the direction the hit pushed from — and 0
// for a player, since ServerPlayer.getHurtDir is only ever set by
// indicateDamage.
inline std::vector<uint8_t> make_hurt_animation(int32_t eid, float yaw = 0.0f) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayHurtAnimationId));
  w.varint(static_cast<uint32_t>(eid));
  w.f32(yaw);
  return std::move(w).take();
}

// animate (0x03): [varint entity id][u8 animation]. `LivingEntity.swing` sends
// it to the swinger's tracking players — not the swinger, whose own client
// played the arm locally — with animation 0 for the main hand and 3 for the off
// hand (`kAnimateSwingMainHand` / `kAnimateSwingOffHand`).
inline std::vector<uint8_t> make_animate(int32_t eid, uint8_t animation) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayAnimateId));
  w.varint(static_cast<uint32_t>(eid));
  w.u8(animation);
  return std::move(w).take();
}

// entity_event (0x22): [i32 entity id][u8 event id]. The id is a *fixed* 4-byte
// int here, not a varint — the one packet in this family that is not
// varint-prefixed, which is why it gets its own note.
inline std::vector<uint8_t> make_entity_event(int32_t eid, uint8_t event_id) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayEntityEventId));
  w.u32(static_cast<uint32_t>(eid));
  w.u8(event_id);
  return std::move(w).take();
}

// update_attributes (0x83): [varint entity id][varint count] then, per entry,
// [varint attribute id][f64 base value][varint modifier count] followed by the
// modifiers. One modifier is [string id][f64 amount][varint operation] — the
// operation is a *varint* of the Operation ordinal (add_value 0,
// add_multiplied_base 1, add_multiplied_total 2), not a byte.
//
// Armour is the one attribute this server sends modifiers for: vanilla models
// each equipped piece as a modifier on `minecraft:armor`
// (LivingEntity.detectEquipmentUpdates), and the client draws its armour bar
// from that attribute, so the number has to be pushed as well as used.
struct AttributeModifierValue {
  std::string id; // "minecraft:armor.chestplate"
  double amount = 0.0;
  uint8_t operation = 0; // Operation.ADD_VALUE
};
struct AttributeValue {
  int32_t id;
  double base;
  std::vector<AttributeModifierValue> modifiers;
};

inline std::vector<uint8_t> make_update_attributes(int32_t eid,
                                                   const std::vector<AttributeValue>& values) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayUpdateAttributesId));
  w.varint(static_cast<uint32_t>(eid));
  w.varint(static_cast<uint32_t>(values.size()));
  for (const AttributeValue& value : values) {
    w.varint(static_cast<uint32_t>(value.id));
    w.f64(value.base);
    w.varint(static_cast<uint32_t>(value.modifiers.size()));
    for (const AttributeModifierValue& modifier : value.modifiers) {
      w.string(modifier.id);
      w.f64(modifier.amount);
      w.varint(modifier.operation);
    }
  }
  return std::move(w).take();
}

// player_combat_kill (0x44): [varint player id][component]. This is the packet
// that puts the client on the death screen (ClientPacketListener
// .handlePlayerCombatKill -> DeathScreen), so a death that skips it leaves the
// player alive-looking with zero health.
inline std::vector<uint8_t> make_player_combat_kill(int32_t eid, const std::string& key,
                                                    const std::vector<std::string>& args) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayPlayerCombatKillId));
  w.varint(static_cast<uint32_t>(eid));
  w.component_translate(key, args);
  return std::move(w).take();
}

// change_difficulty (0x0a): [u8 difficulty][bool locked]. The client draws its
// difficulty from this, and the server's FoodData rules read the same value.
inline std::vector<uint8_t> make_change_difficulty(uint8_t difficulty, bool locked) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayChangeDifficultyId));
  w.u8(difficulty);
  w.u8(locked ? 1 : 0);
  return std::move(w).take();
}

// set_equipment (0x66): [varint entity id] then one entry per slot,
// [u8 slot id | 0x80 when another entry follows][ItemStack]. The continuation
// bit is what makes the list self-terminating; the slot ids are EquipmentSlot
// ordinals (mainhand 0, offhand 1, feet 2, legs 3, chest 4, head 5).
struct EquipmentEntry {
  uint8_t slot;
  mc::world::ItemStack stack;
};

// EquipmentSlot ordinals, named so callers stop counting them by hand.
constexpr uint8_t kEquipmentMainHand = 0;
constexpr uint8_t kEquipmentOffHand = 1;
constexpr uint8_t kEquipmentFeet = 2;
constexpr uint8_t kEquipmentLegs = 3;
constexpr uint8_t kEquipmentChest = 4;
constexpr uint8_t kEquipmentHead = 5;

inline std::vector<uint8_t> make_set_equipment(int32_t eid,
                                               const std::vector<EquipmentEntry>& entries) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetEquipmentId));
  w.varint(static_cast<uint32_t>(eid));
  for (size_t i = 0; i < entries.size(); ++i) {
    const bool more = i + 1 != entries.size();
    w.u8(static_cast<uint8_t>(entries[i].slot | (more ? 0x80 : 0x00)));
    mc::world::write_item_stack(w, entries[i].stack);
  }
  return std::move(w).take();
}

// BlockPos.asLong: 26 bits of x, 12 of y, 26 of z, in that order. The default
// spawn position and a death location both carry a block position this way.
inline uint64_t pack_block_pos(int32_t x, int32_t y, int32_t z) {
  return (static_cast<uint64_t>(x & 0x3FFFFFF) << 38) |
         (static_cast<uint64_t>(z & 0x3FFFFFF) << 12) | static_cast<uint64_t>(y & 0xFFF);
}

// level_event (0x2e), which is how a brewing stand tells the client to play its
// brew sound and the bubble particles (BrewingStandBlockEntity.doBrew's
// `level.levelEvent(1035, pos, 0)`):
//
//   i32 event id
//   BlockPos position    (a *packed long*, not three ints — ClientboundLevelEvent
//                         Packet.write calls FriendlyByteBuf.writeBlockPos)
//   i32 event data
//   bool global event
//
// The event id and the data are both fixed 4-byte ints; the data is 0 for every
// brewing sound, and `global` is false so only players close enough hear it.
inline std::vector<uint8_t> make_level_event(int32_t event, int32_t x, int32_t y, int32_t z,
                                             int32_t data, bool global = false) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayLevelEventId));
  w.u32(static_cast<uint32_t>(event));
  w.u64(pack_block_pos(x, y, z));
  w.u32(static_cast<uint32_t>(data));
  w.u8(global ? 1 : 0);
  return std::move(w).take();
}

// CommonPlayerSpawnInfo — the tail of Login and the whole body of Respawn. Both
// writers go through here so the join and the respawn cannot drift apart.
//
//   varint dimension type id, string dimension name, i64 hashed seed,
//   u8 game mode, u8 previous game mode (0xFF = none), bool is debug,
//   bool is flat, optional death location (dimension + packed xyz),
//   varint portal cooldown, varint sea level
struct CommonSpawnInfo {
  uint32_t dimension_type_id = 0;
  std::string dimension = "minecraft:overworld";
  int64_t hashed_seed = 0;
  uint8_t game_mode = 0;
  int32_t previous_game_mode = -1;
  bool is_debug = false;
  bool is_flat = true;
  bool has_death_location = false;
  int32_t death_x = 0, death_y = 0, death_z = 0;
  int32_t portal_cooldown = 0;
  int32_t sea_level = 63;
};

inline void write_common_spawn_info(mc::net::Writer& w, const CommonSpawnInfo& info) {
  w.varint(info.dimension_type_id);
  w.string(info.dimension);
  w.i64(info.hashed_seed);
  w.u8(info.game_mode);
  w.u8(static_cast<uint8_t>(info.previous_game_mode < 0 ? 0xFF : info.previous_game_mode));
  w.u8(info.is_debug ? 1 : 0);
  w.u8(info.is_flat ? 1 : 0);
  w.u8(info.has_death_location ? 1 : 0);
  if (info.has_death_location) {
    w.string(info.dimension);
    w.u64(pack_block_pos(info.death_x, info.death_y, info.death_z));
  }
  w.varint(static_cast<uint32_t>(info.portal_cooldown));
  w.varint(static_cast<uint32_t>(info.sea_level));
}

// light_update (0x30): the sections of one chunk whose light moved.
//
//   varint chunk x, varint chunk z,
//   bitset skyYMask, bitset blockYMask, bitset emptySkyYMask, bitset emptyBlockYMask,
//   collection<byte[2048]> skyUpdates, collection<byte[2048]> blockUpdates
//
// 26.3's `ByteBufCodecs.BIT_SET` is a varint byte length followed by
// `BitSet.toByteArray()` (little-endian bits, trailing zero bytes dropped).
// 26.1/26.2's `FriendlyByteBuf.writeBitSet` is a varint long count followed by
// the longs, big-endian, low word first. An empty bitset is `0` in both. The
// canonical 26.3 builder uses the byte form; `byte_masks = false` is the
// translated 26.1/26.2 form.
//
// Slot `i` is the light section at `first_section_y + i`, and only slots whose
// light actually moved are named: a section the client already has the right
// value for is left out of *both* masks, which is how vanilla's
// `skyChangedLightSectionFilter` works. An all-zero array goes in the empty mask
// instead of the update list — the client paints that as dark either way, and
// vanilla writes absent and empty identically.
inline void write_light_bitset(mc::net::Writer& w, const std::vector<uint64_t>& words,
                               bool byte_masks) {
  size_t used = words.size();
  while (used > 0 && words[used - 1] == 0)
    --used;
  if (byte_masks) {
    size_t bytes = used * 8;
    while (bytes > 0) {
      const size_t word = (bytes - 1) / 8;
      const int shift = 8 * static_cast<int>((bytes - 1) % 8);
      if (((words[word] >> shift) & 0xFF) != 0)
        break;
      --bytes;
    }
    w.varint(static_cast<uint32_t>(bytes));
    for (size_t i = 0; i < bytes; ++i) {
      const size_t word = i / 8;
      const int shift = 8 * static_cast<int>(i % 8);
      w.u8(static_cast<uint8_t>((words[word] >> shift) & 0xFF));
    }
    return;
  }
  w.varint(static_cast<uint32_t>(used));
  for (size_t i = 0; i < used; ++i)
    w.i64(static_cast<int64_t>(words[i]));
}

inline std::vector<uint8_t> make_light_update(int cx, int cz, const mc::world::Chunk& chunk,
                                              const mc::world::LightChange& change,
                                              bool byte_masks = true) {
  std::vector<uint64_t> sky_real(change.sky_changed.size(), 0);
  std::vector<uint64_t> block_real(change.block_changed.size(), 0);
  std::vector<uint64_t> sky_empty(change.sky_changed.size(), 0);
  std::vector<uint64_t> block_empty(change.block_changed.size(), 0);
  std::vector<const mc::world::LightArray*> sky_arrays;
  std::vector<const mc::world::LightArray*> block_arrays;
  const auto all_zero = [](const mc::world::LightArray& data) {
    return std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0; });
  };
  const auto take = [&](bool is_sky, int slot) {
    const int index = change.first_section_y + slot - chunk.min_section_y;
    const std::optional<mc::world::LightArray>* data = nullptr;
    if (index >= 0 && index < static_cast<int>(chunk.sections.size())) {
      const mc::world::Section& section = chunk.sections[static_cast<size_t>(index)];
      data = is_sky ? &section.sky_light : &section.block_light;
    }
    const bool empty = data == nullptr || !*data || all_zero(**data);
    std::vector<uint64_t>& mask =
        is_sky ? (empty ? sky_empty : sky_real) : (empty ? block_empty : block_real);
    mask[static_cast<size_t>(slot / 64)] |= (1ULL << (slot % 64));
    if (!empty)
      (is_sky ? sky_arrays : block_arrays).push_back(&**data);
  };
  for (int slot = 0; slot < change.slot_count; ++slot) {
    if (change.sky_at(slot))
      take(true, slot);
    if (change.block_at(slot))
      take(false, slot);
  }

  mc::net::Writer w;
  w.reserve(2048 * (sky_arrays.size() + block_arrays.size()) + 64);
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayLightUpdateId));
  w.varint(static_cast<uint32_t>(cx));
  w.varint(static_cast<uint32_t>(cz));
  write_light_bitset(w, sky_real, byte_masks);
  write_light_bitset(w, block_real, byte_masks);
  write_light_bitset(w, sky_empty, byte_masks);
  write_light_bitset(w, block_empty, byte_masks);
  const auto write_arrays = [&](const std::vector<const mc::world::LightArray*>& arrays) {
    w.varint(static_cast<uint32_t>(arrays.size()));
    for (const mc::world::LightArray* data : arrays) {
      w.varint(static_cast<uint32_t>(data->size()));
      w.bytes(data->data(), data->size());
    }
  };
  write_arrays(sky_arrays);
  write_arrays(block_arrays);
  return std::move(w).take();
}

// respawn (0x52): the spawn info plus ClientboundRespawnPacket's dataToKeep
// bitmask (1 = attribute modifiers, 2 = entity data, 3 = both).
inline std::vector<uint8_t> make_respawn(const CommonSpawnInfo& info, uint8_t data_to_keep) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayRespawnId));
  write_common_spawn_info(w, info);
  w.u8(data_to_keep);
  return std::move(w).take();
}

} // namespace mc::server
