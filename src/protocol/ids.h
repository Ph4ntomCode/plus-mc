// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clientbound packet ids for protocol 777 (Minecraft 26.3), the canonical
// version, derived from the generated tables in
// protocol/packet_ids.generated.h (docs/PHASE1.md, T0 packet-table dispatch).
//
// No id here is transcribed by hand any more: each constant is
// `checked_packet_id(clientbound_id(State::X, "name"))`, so it is evaluated at
// compile time from the table the generator produced out of vanilla's own
// protocol registration order, and a name that no longer exists is a build
// error. Refreshing for a new release is `python3 tools/gen_protocol_table.py`
// plus the renames the compiler reports.
//
// Serverbound packet ids are NOT here at all: dispatch keys off canonical names
// resolved by protocol/packet_table.cpp, and the generated serverbound template
// (protocol/packet_ids.h) is the ground truth a test checks that table against.
//
// Ids that are not packet ids (entity-type indices, game-event reasons, effect
// flags, ...) stay literal below; they are registry or enum data, not packet
// registration order.
#pragma once

#include <cstdint>

#include "protocol/packet_ids.h"

namespace mc::protocol {

// --- Status (clientbound) ---
constexpr int kStatusResponseId =
    checked_packet_id(clientbound_id(State::Status, "status_response"));
constexpr int kStatusPongId = checked_packet_id(clientbound_id(State::Status, "pong_response"));

// --- Login (clientbound) ---
constexpr int kLoginDisconnectId =
    checked_packet_id(clientbound_id(State::Login, "login_disconnect"));
constexpr int kLoginEncryptionRequestId =
    checked_packet_id(clientbound_id(State::Login, "hello")); // encryption_begin (server)
constexpr int kLoginSuccessId = checked_packet_id(clientbound_id(State::Login, "login_finished"));
constexpr int kLoginSetCompressionId =
    checked_packet_id(clientbound_id(State::Login, "login_compression"));
constexpr int kLoginPluginRequestId = checked_packet_id(
    clientbound_id(State::Login, "custom_query")); // login plugin request (server)

// --- Configuration (clientbound) ---
constexpr int kConfigPluginMessageId =
    checked_packet_id(clientbound_id(State::Configuration, "custom_payload"));
constexpr int kConfigFinishId =
    checked_packet_id(clientbound_id(State::Configuration, "finish_configuration"));
constexpr int kConfigRegistryDataId =
    checked_packet_id(clientbound_id(State::Configuration, "registry_data"));
constexpr int kConfigFeatureFlagsId =
    checked_packet_id(clientbound_id(State::Configuration, "update_enabled_features"));
constexpr int kConfigTagsId =
    checked_packet_id(clientbound_id(State::Configuration, "update_tags"));
constexpr int kConfigKnownPacksCbId =
    checked_packet_id(clientbound_id(State::Configuration, "select_known_packs"));

// --- Play (clientbound) ---
constexpr int kPlayBundleDelimiterId = checked_packet_id(clientbound_id(State::Play, "bundle"));
constexpr int kPlaySpawnEntityId =
    checked_packet_id(clientbound_id(State::Play, "add_entity")); // Add Entity
constexpr int kPlayAnimateId =
    checked_packet_id(clientbound_id(State::Play, "animate")); // arm swing
constexpr int kPlayBlockChangedAckId =
    checked_packet_id(clientbound_id(State::Play, "block_changed_ack"));
constexpr int kPlayBlockDestructionId =
    checked_packet_id(clientbound_id(State::Play, "block_destruction"));
constexpr int kPlayBlockUpdateId = checked_packet_id(clientbound_id(State::Play, "block_update"));
constexpr int kPlaySetEntityDataId =
    checked_packet_id(clientbound_id(State::Play, "set_entity_data"));
constexpr int kPlaySetEntityMotionId =
    checked_packet_id(clientbound_id(State::Play, "set_entity_motion"));
constexpr int kPlayTakeItemEntityId =
    checked_packet_id(clientbound_id(State::Play, "take_item_entity"));
// --- Play (clientbound), window/menu protocol ---
constexpr int kPlayContainerCloseId =
    checked_packet_id(clientbound_id(State::Play, "container_close"));
constexpr int kPlayContainerSetContentId =
    checked_packet_id(clientbound_id(State::Play, "container_set_content"));
constexpr int kPlayContainerSetDataId =
    checked_packet_id(clientbound_id(State::Play, "container_set_data"));
constexpr int kPlayContainerSetSlotId =
    checked_packet_id(clientbound_id(State::Play, "container_set_slot"));
constexpr int kPlayOpenScreenId = checked_packet_id(clientbound_id(State::Play, "open_screen"));
// merchant_offers: the trade list MerchantMenu's screen draws, sent when the
// villager's window opens (and on a restock, which is not modelled yet).
constexpr int kPlayMerchantOffersId =
    checked_packet_id(clientbound_id(State::Play, "merchant_offers"));
// --- Play (clientbound), statistics and scoreboard (Phase 4 milestone M) ---
// award_stats: the client's statistics screen asks for the deltas through
// `client_command` REQUEST_STATS and this answers.
constexpr int kPlayAwardStatsId = checked_packet_id(clientbound_id(State::Play, "award_stats"));
constexpr int kPlaySetObjectiveId = checked_packet_id(clientbound_id(State::Play, "set_objective"));
constexpr int kPlaySetScoreId = checked_packet_id(clientbound_id(State::Play, "set_score"));
constexpr int kPlayResetScoreId = checked_packet_id(clientbound_id(State::Play, "reset_score"));
constexpr int kPlaySetDisplayObjectiveId =
    checked_packet_id(clientbound_id(State::Play, "set_display_objective"));
constexpr int kPlaySetPlayerTeamId =
    checked_packet_id(clientbound_id(State::Play, "set_player_team"));
// --- Play (clientbound), advancements (Phase 4 milestone N) ---
// update_advancements: the tree's additions, removals and progress, one packet
// per flush; the `reset` bit drops the client's stale copy on the first one.
constexpr int kPlayUpdateAdvancementsId =
    checked_packet_id(clientbound_id(State::Play, "update_advancements"));
// select_advancements_tab: which tab a `seen_advancements` click selected
// (absent = the client's screen closed).
constexpr int kPlaySelectAdvancementsTabId =
    checked_packet_id(clientbound_id(State::Play, "select_advancements_tab"));
// ServerboundClientCommandPacket.Action: PERFORM_RESPAWN 0, REQUEST_STATS 1,
// REQUEST_GAMERULE_VALUES 2.
constexpr int kClientCommandRequestStats = 1;
// Recipe book (Phase 3 milestone D2).
constexpr int kPlayPlaceGhostRecipeId =
    checked_packet_id(clientbound_id(State::Play, "place_ghost_recipe"));
constexpr int kPlayRecipeBookAddId =
    checked_packet_id(clientbound_id(State::Play, "recipe_book_add"));
constexpr int kPlayRecipeBookSettingsId =
    checked_packet_id(clientbound_id(State::Play, "recipe_book_settings"));
constexpr int kPlayUpdateRecipesId =
    checked_packet_id(clientbound_id(State::Play, "update_recipes"));
constexpr int kPlaySetCursorItemId =
    checked_packet_id(clientbound_id(State::Play, "set_cursor_item"));
constexpr int kPlaySetExperienceId =
    checked_packet_id(clientbound_id(State::Play, "set_experience"));
// sync_entity_position (ClientboundEntityPositionSyncPacket).
constexpr int kPlayEntityTeleportId =
    checked_packet_id(clientbound_id(State::Play, "entity_position_sync"));
// explode (ClientboundExplodePacket): the blast's own packet -- centre, radius,
// block count, the viewer's knockback, the particle, the sound and the block
// particles, all by registry id (world/explosion_effects_table.h).
constexpr int kPlayExplodeId = checked_packet_id(clientbound_id(State::Play, "explode"));
constexpr int kPlayChunkBatchStartId =
    checked_packet_id(clientbound_id(State::Play, "chunk_batch_start"));
constexpr int kPlayChunkBatchFinishedId =
    checked_packet_id(clientbound_id(State::Play, "chunk_batch_finished"));
constexpr int kPlayGameEventId = checked_packet_id(clientbound_id(State::Play, "game_event"));
// level_event: the block-animation packet that carries the brewing stand's brew
// sound and particles.
constexpr int kPlayLevelEventId = checked_packet_id(clientbound_id(State::Play, "level_event"));
// LevelEvent.BREWING_STAND_BREW: what BrewingStandBlockEntity.doBrew raises.
constexpr int kLevelEventBrewingStandBrew = 1035;
// ClientboundGameEventPacket reason ids. The packet body is [u8 reason][f32
// param] (ClientboundGameEventPacket.write). LEVEL_CHUNKS_LOAD_START is what
// releases the client's "Loading terrain" screen: ClientPacketListener
// .handleGameEvent calls LevelLoadTracker.loadingPacketsReceived() for it and
// for nothing else, moving the tracker out of WaitingForServer. That state has
// no tick() transition, so without this packet the client can never reach
// ClientLevelReady and sits on the loading screen until its 30 s join timeout
// drops the connection.
constexpr int kGameEventChangeGameMode = 3;
constexpr int kGameEventLevelChunksLoadStart = 13;
// The weather's four reasons, in vanilla's numbering: the two flips carry 0.0
// and the two level changes carry the new level (0..1). `advanceWeatherCycle`
// sends them level-wide, and `PlayerList.sendLevelInfo` sends the three rain
// ones at join when it is already raining.
constexpr int kGameEventStartRaining = 1;
constexpr int kGameEventStopRaining = 2;
constexpr int kGameEventRainLevelChange = 7;
constexpr int kGameEventThunderLevelChange = 8;
constexpr int kPlayJoinGameId = checked_packet_id(clientbound_id(State::Play, "login"));
constexpr int kPlayKeepAliveCbId = checked_packet_id(clientbound_id(State::Play, "keep_alive"));
constexpr int kPlayChunkDataId =
    checked_packet_id(clientbound_id(State::Play, "level_chunk_with_light"));
constexpr int kPlayLightUpdateId = checked_packet_id(clientbound_id(State::Play, "light_update"));
constexpr int kPlayPlayerAbilitiesId =
    checked_packet_id(clientbound_id(State::Play, "player_abilities"));
constexpr int kPlayPlayerChatId = checked_packet_id(clientbound_id(State::Play, "player_chat"));
constexpr int kPlayPlayerInfoRemoveId =
    checked_packet_id(clientbound_id(State::Play, "player_info_remove"));
constexpr int kPlayPlayerInfoUpdateId =
    checked_packet_id(clientbound_id(State::Play, "player_info_update"));
constexpr int kPlayRemoveEntitiesId =
    checked_packet_id(clientbound_id(State::Play, "remove_entities"));
// synchronize player position.
constexpr int kPlayPlayerPositionId =
    checked_packet_id(clientbound_id(State::Play, "player_position"));
constexpr int kPlaySystemChatId = checked_packet_id(clientbound_id(State::Play, "system_chat"));
// commands: the Brigadier tree the client uses for input highlighting and
// completions. Every translated play profile carries the same packet name at
// wire id 0x10 as well, so one encoder serves all supported clients.
constexpr int kPlayCommandsId = checked_packet_id(clientbound_id(State::Play, "commands"));
// boss_event (ClientboundBossEventPacket): the boss HUD's add, remove and
// progress messages, keyed by the bar's UUID.
constexpr int kPlayBossEventId = checked_packet_id(clientbound_id(State::Play, "boss_event"));
constexpr int kPlaySetDefaultSpawnId =
    checked_packet_id(clientbound_id(State::Play, "set_default_spawn_position"));
constexpr int kPlaySetCenterChunkId =
    checked_packet_id(clientbound_id(State::Play, "set_chunk_cache_center"));
// set_chunk_cache_radius (view distance).
constexpr int kPlaySetChunkCacheRadiusId =
    checked_packet_id(clientbound_id(State::Play, "set_chunk_cache_radius"));
constexpr int kPlayRelEntityMoveId =
    checked_packet_id(clientbound_id(State::Play, "move_entity_pos"));
constexpr int kPlayEntityMoveLookId =
    checked_packet_id(clientbound_id(State::Play, "move_entity_pos_rot"));
constexpr int kPlayEntityLookId = checked_packet_id(clientbound_id(State::Play, "move_entity_rot"));
constexpr int kPlaySetHeldSlotId = checked_packet_id(clientbound_id(State::Play, "set_held_slot"));
constexpr int kPlaySetTimeId = checked_packet_id(clientbound_id(State::Play, "set_time"));
constexpr int kPlayUpdateTagsId = checked_packet_id(clientbound_id(State::Play, "update_tags"));
constexpr int kPlayDisconnectId = checked_packet_id(clientbound_id(State::Play, "disconnect"));
constexpr int kPlayPingCbId = checked_packet_id(clientbound_id(State::Play, "ping"));
constexpr int kPlayPongCbId = checked_packet_id(clientbound_id(State::Play, "pong_response"));

// --- Play (clientbound), survival / combat (Phase 3 milestone F) ---
constexpr int kPlayChangeDifficultyId =
    checked_packet_id(clientbound_id(State::Play, "change_difficulty"));
constexpr int kPlayDamageEventId = checked_packet_id(clientbound_id(State::Play, "damage_event"));
constexpr int kPlayEntityEventId = checked_packet_id(clientbound_id(State::Play, "entity_event"));
constexpr int kPlayHurtAnimationId =
    checked_packet_id(clientbound_id(State::Play, "hurt_animation"));
constexpr int kPlayPlayerCombatKillId =
    checked_packet_id(clientbound_id(State::Play, "player_combat_kill"));
constexpr int kPlayRespawnId = checked_packet_id(clientbound_id(State::Play, "respawn"));
constexpr int kPlaySetEquipmentId = checked_packet_id(clientbound_id(State::Play, "set_equipment"));
constexpr int kPlaySetHealthId = checked_packet_id(clientbound_id(State::Play, "set_health"));
constexpr int kPlayUpdateAttributesId =
    checked_packet_id(clientbound_id(State::Play, "update_attributes"));
// --- Play (clientbound), status effects (Phase 3 milestone G) ---
constexpr int kPlayRemoveMobEffectId =
    checked_packet_id(clientbound_id(State::Play, "remove_mob_effect"));
constexpr int kPlayUpdateMobEffectId =
    checked_packet_id(clientbound_id(State::Play, "update_mob_effect"));
// --- Play (clientbound), item cooldowns (Phase 3 milestone G follow-up) ---
// The server sends it from ServerItemCooldowns: the full duration when a
// cooldown starts, and a duration of 0 the tick the range runs out.
constexpr int kPlayCooldownId = checked_packet_id(clientbound_id(State::Play, "cooldown"));

// ClientboundUpdateMobEffectPacket's flag bits.
constexpr uint8_t kMobEffectFlagAmbient = 1;
constexpr uint8_t kMobEffectFlagVisible = 2;
constexpr uint8_t kMobEffectFlagShowIcon = 4;
constexpr uint8_t kMobEffectFlagBlend = 8;

// `ClientboundAnimatePacket`'s animation ids. `LivingEntity.swing(hand)`
// broadcasts 0 for the main hand and 3 for the off hand to the swinger's
// tracking players (`ChunkMap.broadcast`, which excludes the swinger: its own
// client animated the arm locally).
constexpr uint8_t kAnimateSwingMainHand = 0;
constexpr uint8_t kAnimateSwingOffHand = 3;

// EntityEvent ids that matter here (EntityEvent: the byte of entity_event).
constexpr uint8_t kEntityEventHurt = 2;
constexpr uint8_t kEntityEventDeath = 3;
// `Animal.setInLove` and `Animal.finalizeSpawnChildFromBreeding` both raise this
// on the animal: the client answers with the heart burst above it.
constexpr uint8_t kEntityEventLove = 18;
// `Villager.customServerAiStep`: one happy-villager burst per trade, raised on
// the tick that spends its `lastTradedPlayer` (the client answers event 14 with
// ParticleTypes.HAPPY_VILLAGER). 13 is the angry burst that same class raises
// when a *player* hits it, which this server never sends: the mob hurt paths are
// not a villager's to touch.
constexpr uint8_t kEntityEventHappyVillager = 14;
// LivingEntity.randomTeleport's own event on a successful teleport (the client
// bursts portal particles): broadcastEntityEvent(this, (byte)46).
constexpr uint8_t kEntityEventTeleportParticles = 46;
constexpr uint8_t kEntityEventDrownParticles = 67; // "air bubbles" burst

// ClientboundRespawnPacket.dataToKeep bits.
constexpr uint8_t kRespawnKeepAttributeModifiers = 1;
constexpr uint8_t kRespawnKeepEntityData = 2;
constexpr uint8_t kRespawnKeepAllData = 3;

// The player inventory menu always uses container id 0 (InventoryMenu has the
// reserved id 0; no server-opened container ever reuses it).
constexpr int kPlayerContainerId = 0;

// Index of minecraft:player in the 777 minecraft:entity_type registry (the
// Add Entity type varint). From the 26.3 server's own registries report.
// Client-baked registry, so it is not part of the synchronized config
// registries.
constexpr uint32_t kPlayerEntityTypeId = 159;

// Index of minecraft:item in the same registry (the Add Entity type varint
// for a dropped item stack). From the 26.3 server's registries report.
constexpr uint32_t kItemEntityTypeId = 72;

// Index of minecraft:experience_orb in the same registry (milestone E's XP
// orbs). Also from the 26.3 registries report, which agrees with the observed
// wire ids for item (72) and player (159).
constexpr uint32_t kExperienceOrbEntityTypeId = 50;

// Index of minecraft:area_effect_cloud, minecraft:splash_potion and
// minecraft:lingering_potion in the same registry (the throwable potions and the
// cloud a lingering one leaves). Same source as the three above.
constexpr uint32_t kAreaEffectCloudEntityTypeId = 3;
constexpr uint32_t kSplashPotionEntityTypeId = 108;
constexpr uint32_t kLingeringPotionEntityTypeId = 109;

// The projectiles of Phase 4 milestone H in the same registry: the arrow a bow
// fires, the spectral arrow (its own type, identical in flight here), and the
// three thrown items. From the same `build/_dl/boot/generated/reports/
// registries.json` the six above come from, and cross-checked against the ids
// that report agrees with *on the wire* (player 156, item 71, tnt 133, splash
// 105, lingering 106, area_effect_cloud 3 and experience_orb 49 are all pinned
// elsewhere in this file and all match it exactly).
constexpr uint32_t kArrowEntityTypeId = 6;
constexpr uint32_t kSpectralArrowEntityTypeId = 126;
constexpr uint32_t kSnowballEntityTypeId = 123;
constexpr uint32_t kEggEntityTypeId = 40;
constexpr uint32_t kEnderPearlEntityTypeId = 45;
// `minecraft:wither_skull`, the entity the wither's volley spawns. The generated
// entity-type order puts it between the wither (149) and the wolf (152).
constexpr uint32_t kWitherSkullEntityTypeId = 151;
// `minecraft:end_crystal`, the entity a dragon's healing beam comes from. The
// 26.3 `EntityTypes` registration order gives 46, and the same report produces
// the dragon 44, wither skull 151, item 72, arrow 6 and player 159 pinned here.
constexpr uint32_t kEndCrystalEntityTypeId = 46;

// Index of minecraft:tnt in the same registry (the entity a primed TNT block
// becomes). Cross-checked rather than quoted: the tail of the registry is
// alphabetical from `tnt` to `zombified_piglin`, and that run holds exactly 18
// entries before `zombie` — which this server pins at 151 above — so
// `tnt = 154 - 18 = 136` for any 26.x registry that does not add an entity
// whose name sorts between them.
constexpr uint32_t kPrimedTntEntityTypeId = 136;

} // namespace mc::protocol
