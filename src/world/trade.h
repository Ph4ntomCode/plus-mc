// SPDX-License-Identifier: GPL-3.0-or-later
//
// Villagers and trading (Phase 4 milestone L): the merchant model.
//
// Vanilla's classes, ported without the brain: `VillagerTrade` (one entry of
// the data pack's `villager_trade` registry), `MerchantOffer` (a live offer with
// its uses, demand and special-price diff), `MerchantOffers.getRecipeFor`, and
// `AbstractVillager.addOffersFromTradeSet` (the pick). Everything the runtime
// needs that is *data* lives in the generated `world/trade_table.h`; this header
// is the arithmetic on top of it.
//
// The three numbers that are easy to get wrong, spelled out because they are not
// obvious from the pack:
//
//   * `reputation_discount` is not a discount. `VillagerTrade.getOffer` passes it
//     to `MerchantOffer`'s `priceMultiplier`, which scales the demand component
//     of a cost and the reputation discount the villager applies when a trade
//     screen opens — a `MerchantOffer.addToSpecialPriceDiff` value.
//   * a cost's displayed count is `base + max(0, floor(base * demand *
//     priceMultiplier)) + specialPriceDiff`, clamped to [1, max stack size]
//     (`MerchantOffer.getModifiedCostCount`). Cost B is never demand-modified:
//     `take` shrinks it by its base count.
//   * `updateDemand` is `demand + uses - (maxUses - uses)`; it runs on restock,
//     which needs a job site this engine does not have yet (see the milestone's
//     limitations), so a villager's demand only moves when a later milestone
//     calls it.
//
// Registration orders are code, not data, so they are written here with their
// sources — the wire carries registry ids for both:
//
//   * `VillagerType.bootstrap` registers desert, jungle, plains, savanna, snow,
//     swamp, taiga (VillagerData.DEFAULT_TYPE is plains) — ids 0..6;
//   * `VillagerProfession.bootstrap` registers none, armorer, butcher,
//     cartographer, cleric, farmer, fisherman, fletcher, leatherworker,
//     librarian, mason, nitwit, shepherd, toolsmith, weaponsmith — ids 0..14.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/item.h"
#include "world/trade_table.h"

namespace mc::world {

// ---- the two registries, in bootstrap order --------------------------------

enum VillagerTypeId : uint8_t {
  kVillagerDesert = 0,
  kVillagerJungle = 1,
  kVillagerPlains = 2,
  kVillagerSavanna = 3,
  kVillagerSnow = 4,
  kVillagerSwamp = 5,
  kVillagerTaiga = 6,
  kVillagerTypeCount = 7,
};

enum VillagerProfessionId : uint8_t {
  kVillagerNone = 0,
  kVillagerArmorer = 1,
  kVillagerButcher = 2,
  kVillagerCartographer = 3,
  kVillagerCleric = 4,
  kVillagerFarmer = 5,
  kVillagerFisherman = 6,
  kVillagerFletcher = 7,
  kVillagerLeatherworker = 8,
  kVillagerLibrarian = 9,
  kVillagerMason = 10,
  kVillagerNitwit = 11,
  kVillagerShepherd = 12,
  kVillagerToolsmith = 13,
  kVillagerWeaponsmith = 14,
  kVillagerProfessionCount = 15,
};

// The generated rows' "every type may buy this" mask.
constexpr uint8_t kVariantAny = 0x7F;

// `VillagerData.NEXT_LEVEL_XP_THRESHOLDS`, the xp each level *starts* at:
// `getMaxXpPerLevel(level)` is the threshold to rise out of it.
constexpr int kVillagerNextLevelXp[5] = {0, 10, 70, 150, 250};

// `VillagerData.canLevelUp` / `getMaxXpPerLevel`.
inline bool villager_can_level_up(int level) {
  return level >= 1 && level < 5;
}
inline int villager_max_xp_per_level(int level) {
  return villager_can_level_up(level) ? kVillagerNextLevelXp[level] : 0;
}

// ---- one cost, one offer ---------------------------------------------------

// `ItemCost`: an item and a count. No trade in the 26.2 pack carries a
// components predicate (`wants.components` / `additional_wants.components`),
// which would be `DataComponentExactPredicate.EMPTY` for every row here anyway.
struct TradeCost {
  int32_t item = kAirItemId;
  int32_t count = 1;
};

// `MerchantOffer`: the eight fields the wire's `MerchantOffersPacket` carries
// plus the costs and result behind them. Uses, demand, the special price diff
// and the price multiplier are the live state; everything else is the row's.
struct MerchantOffer {
  TradeCost cost_a;
  // `costB`: `Optional<ItemCost>`, absent for a one-cost trade.
  bool has_cost_b = false;
  TradeCost cost_b;
  ItemStack result;
  int uses = 0;
  int max_uses = 4;
  // `rewardExp` (the codec's default, which every pack row gets).
  bool reward_exp = true;
  int special_price_diff = 0;
  int demand = 0;
  // `priceMultiplier` — the pack's `reputation_discount`.
  float price_multiplier = 0.0f;
  int xp = 1;

  // `MerchantOffer.getModifiedCostCount`.
  int modified_cost_count(const TradeCost& cost) const;
  // `getCostA` / `getCostB`: the stacks the trade screen shows.
  ItemStack cost_a_stack() const;
  ItemStack cost_b_stack() const;
  // `MerchantOffer.satisfiedBy`: the payments in the two slots match the costs
  // (item for item, and at least the modified count of A).
  bool satisfied_by(const ItemStack& buy_a, const ItemStack& buy_b) const;
  // `MerchantOffer.take`: shrink both payments. Only shrinks when it matches,
  // so the caller can try `take(a, b) || take(b, a)`, the swap `MerchantResultSlot`
  // performs.
  bool take(ItemStack* buy_a, ItemStack* buy_b);
  // `increaseUses` / `resetUses` / `isOutOfStock` / `needsRestock`.
  void increase_uses();
  void reset_uses();
  bool is_out_of_stock() const;
  bool needs_restock() const;
  // `updateDemand`.
  void update_demand();
  // `addToSpecialPriceDiff` / `resetSpecialPriceDiff`.
  void add_to_special_price_diff(int add);
  void reset_special_price_diff();
  // `assemble`: a copy of the result.
  ItemStack assemble() const;
};

// ---- the generated table ---------------------------------------------------

// The set for a profession and level, or nullptr (none, nitwit, level > 5).
const GeneratedTradeSet* trade_set_for(uint8_t profession, int level);

// "farmer" / "minecraft:farmer" -> id; -1 for an unknown name.
int villager_profession_id(std::string_view name);
// "" for an out-of-range id.
std::string_view villager_profession_name(uint8_t id);
// "plains" / "minecraft:plains" -> id; -1 for an unknown name.
int villager_type_id(std::string_view name);

// `VillagerType.byBiome`: the type a villager born in this biome wears. The map
// is `VillagerType`'s own (28 biomes); anything it does not name is plains,
// which is the code's `.orElse(VillagerData.DEFAULT_TYPE)`.
uint8_t villager_type_for_biome(std::string_view biome);

// `VillagerTrade.getOffer` on one row: the offer, or nullopt for the null paths
// — an unmodelled row, a variant the trade refuses, or a cost that lands below
// one (`toItemCost`'s clamp then the code's `< 1`). An unknown item name is a
// null too: it means the runtime's item table and the pack disagree.
std::optional<MerchantOffer> make_offer(const GeneratedTrade& trade, uint8_t type);

// `AbstractVillager.addOffersFromTradeSet`: append up to the set's `amount`
// offers, picked from its rows **without duplicates** (every 26.2 set is
// `allow_duplicates = false`; a row that cannot produce an offer is dropped
// from the pool, exactly like a null from `getOffer`). `rng` is one uniform
// draw in [0, 1) — the same [0,1) contract the mob tick's environment uses.
void add_offers_from_trade_set(uint8_t profession, int level, uint8_t type,
                               const std::function<double()>& rng,
                               std::vector<MerchantOffer>* offers);

// ---- the villager's merchant state -----------------------------------------

// Everything about a villager a trade screen touches, carried on its `Mob`.
// `offers` is `AbstractVillager.offers`: null until the first `getOffers()`,
// which is what `offers_built` records — a villager's offers are built lazily,
// the first time a player right-clicks it, not at spawn.
struct VillagerState {
  uint8_t type = kVillagerPlains;
  uint8_t profession = kVillagerNone;
  int level = 1;
  int xp = 0;
  bool offers_built = false;
  std::vector<MerchantOffer> offers;
  int64_t last_restock_day = -1;
  // `Villager.updateMerchantTimer` / `increaseProfessionLevelOnUpdate`: armed by
  // `rewardTradeXp` when a trade crosses the level's xp threshold, counted down
  // by the mob tick, applied at zero (`increaseMerchantCareer`).
  int update_merchant_timer = 0;
  bool increase_level_on_update = false;
  // `Villager.setUnhappyCounter` (the head-shake a refused right-click plays).
  // The client reads the value; the one-tick decrement is not modelled.
  int unhappy_counter = 0;
  // `AbstractVillager.tradingPlayer`, as an entity id (-1 = none).
  int32_t trading_player = -1;
  // `AbstractVillager.lastTradedPlayer`: set by `rewardTradeXp` (to whoever is
  // trading) and spent by the next AI step, which broadcasts entity event 14
  // for it. The entity id rather than a pointer, because vanilla's field is the
  // player and this state lives on the mob; the reputation event that same
  // branch raises is not modelled (there is no gossip or reputation container).
  int32_t last_traded_player = -1;
};

// The engine's stand-in for vanilla's `RandomSequences`: one deterministic
// stream per villager and level, hashed from the entity id. Vanilla's stream is
// global per `random_sequence` key and seeded from the world seed; a world seed
// is not something this server carries yet, so the *algorithm* below is exact
// and the numbers it draws are deterministic but not vanilla's. Documented in
// docs/PHASE4.md.
std::function<double()> villager_rng(int32_t entity_id, int level);

// `AbstractVillager.getOffers()`: build level 1's offers on first read.
void ensure_villager_offers(VillagerState* villager, int32_t entity_id);

// `Villager.updateTrades` for one level: append that level's set to the current
// offers. `increaseMerchantCareer` calls it after raising the level.
void add_villager_level_offers(VillagerState* villager, int32_t entity_id, int level);

// `Villager.rewardTradeXp`'s bookkeeping: `xp += offer.xp`, and — when the new
// xp crosses `getMaxXpPerLevel` — arm the 40-tick level-up. Returns whether the
// level-up was armed (the caller pays the extra 5 orb xp and, later, applies the
// level).
bool villager_reward_trade_xp(VillagerState* villager, const MerchantOffer& offer);

// `Villager.shouldIncreaseLevel`.
bool villager_should_increase_level(const VillagerState& villager);

// `Villager.increaseMerchantCareer`: level + 1 (when it can rise) and the new
// level's offers appended. Returns the new level.
int villager_increase_career(VillagerState* villager, int32_t entity_id);

} // namespace mc::world
