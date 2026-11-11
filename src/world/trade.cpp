// SPDX-License-Identifier: GPL-3.0-or-later
//
// Villagers and trading (Phase 4 milestone L): the merchant arithmetic.
// See world/trade.h for the vanilla classes each function is and the three
// numbers that are easy to get wrong.
#include "world/trade.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <unordered_map>

namespace mc::world {
namespace {

constexpr const char* kProfessionNames[] = {
    "none",   "armorer",   "butcher",  "cartographer",  "cleric",
    "farmer", "fisherman", "fletcher", "leatherworker", "librarian",
    "mason",  "nitwit",    "shepherd", "toolsmith",     "weaponsmith",
};

constexpr const char* kTypeNames[] = {"desert", "jungle", "plains", "savanna",
                                      "snow",   "swamp",  "taiga"};

// `VillagerType.BY_BIOME`, transcribed whole: the biomes that answer a type
// other than the plains default.
const std::unordered_map<std::string_view, uint8_t>& by_biome() {
  static const std::unordered_map<std::string_view, uint8_t> map = {
      {"badlands", kVillagerDesert},
      {"desert", kVillagerDesert},
      {"eroded_badlands", kVillagerDesert},
      {"wooded_badlands", kVillagerDesert},
      {"bamboo_jungle", kVillagerJungle},
      {"jungle", kVillagerJungle},
      {"sparse_jungle", kVillagerJungle},
      {"savanna_plateau", kVillagerSavanna},
      {"savanna", kVillagerSavanna},
      {"windswept_savanna", kVillagerSavanna},
      {"deep_frozen_ocean", kVillagerSnow},
      {"frozen_ocean", kVillagerSnow},
      {"frozen_river", kVillagerSnow},
      {"ice_spikes", kVillagerSnow},
      {"snowy_beach", kVillagerSnow},
      {"snowy_taiga", kVillagerSnow},
      {"snowy_plains", kVillagerSnow},
      {"grove", kVillagerSnow},
      {"snowy_slopes", kVillagerSnow},
      {"frozen_peaks", kVillagerSnow},
      {"jagged_peaks", kVillagerSnow},
      {"swamp", kVillagerSwamp},
      {"mangrove_swamp", kVillagerSwamp},
      {"old_growth_spruce_taiga", kVillagerTaiga},
      {"old_growth_pine_taiga", kVillagerTaiga},
      {"windswept_gravelly_hills", kVillagerTaiga},
      {"windswept_hills", kVillagerTaiga},
      {"taiga", kVillagerTaiga},
      {"windswept_forest", kVillagerTaiga},
  };
  return map;
}

// A namespaced name's path: "minecraft:farmer" -> "farmer", "farmer" ->
// "farmer". Any other namespace is not one of these registries and answers the
// empty string, so "mythic:farmer" is an unknown name rather than a farmer.
std::string_view path_of(std::string_view name) {
  const size_t colon = name.find(':');
  if (colon == std::string_view::npos)
    return name;
  if (name.substr(0, colon) != "minecraft")
    return {};
  return name.substr(colon + 1);
}

int32_t item_of(std::string_view name) {
  const std::optional<int32_t> id = ItemRegistry::instance().id_of(name);
  return id.value_or(kAirItemId);
}

} // namespace

// ---- MerchantOffer ---------------------------------------------------------

int MerchantOffer::modified_cost_count(const TradeCost& cost) const {
  // `Mth.floor((float)(basePrice * this.demand) * this.priceMultiplier)`: the
  // int product is cast to float first, then scaled.
  const int base = cost.count;
  const int demand_diff = std::max(
      0, static_cast<int>(std::floor(static_cast<float>(base * demand) * price_multiplier)));
  const int max_stack = ItemRegistry::instance().max_stack_size(cost.item);
  return std::clamp(base + demand_diff + special_price_diff, 1, max_stack);
}

ItemStack MerchantOffer::cost_a_stack() const {
  return ItemStack::of_id(cost_a.item, modified_cost_count(cost_a));
}

ItemStack MerchantOffer::cost_b_stack() const {
  if (!has_cost_b)
    return ItemStack{};
  return ItemStack::of_id(cost_b.item, cost_b.count);
}

bool MerchantOffer::satisfied_by(const ItemStack& buy_a, const ItemStack& buy_b) const {
  // `ItemCost.test` compares the item and the components predicate; the
  // predicate is empty for every row here, so item equality is the whole test.
  if (buy_a.item != cost_a.item || buy_a.empty() || buy_a.count < modified_cost_count(cost_a))
    return false;
  if (has_cost_b)
    return buy_b.item == cost_b.item && buy_b.count >= cost_b.count;
  return buy_b.empty();
}

bool MerchantOffer::take(ItemStack* buy_a, ItemStack* buy_b) {
  if (!satisfied_by(*buy_a, *buy_b))
    return false;
  buy_a->count -= modified_cost_count(cost_a);
  if (!cost_b_stack().empty())
    buy_b->count -= cost_b.count;
  return true;
}

void MerchantOffer::increase_uses() {
  ++uses;
}

void MerchantOffer::reset_uses() {
  uses = 0;
}

bool MerchantOffer::is_out_of_stock() const {
  return uses >= max_uses;
}

bool MerchantOffer::needs_restock() const {
  return uses > 0;
}

void MerchantOffer::update_demand() {
  demand = demand + uses - (max_uses - uses);
}

void MerchantOffer::add_to_special_price_diff(int add) {
  special_price_diff += add;
}

void MerchantOffer::reset_special_price_diff() {
  special_price_diff = 0;
}

ItemStack MerchantOffer::assemble() const {
  return result;
}

// ---- the generated table ---------------------------------------------------

const GeneratedTradeSet* trade_set_for(uint8_t profession, int level) {
  for (const GeneratedTradeSet& set : kGeneratedTradeSets) {
    if (set.level != level)
      continue;
    if (villager_profession_id(set.profession) == static_cast<int>(profession))
      return &set;
  }
  return nullptr;
}

int villager_profession_id(std::string_view name) {
  const std::string_view path = path_of(name);
  for (size_t i = 0; i < sizeof(kProfessionNames) / sizeof(kProfessionNames[0]); ++i)
    if (path == kProfessionNames[i])
      return static_cast<int>(i);
  return -1;
}

std::string_view villager_profession_name(uint8_t id) {
  if (id >= kVillagerProfessionCount)
    return {};
  return kProfessionNames[id];
}

int villager_type_id(std::string_view name) {
  const std::string_view path = path_of(name);
  for (size_t i = 0; i < sizeof(kTypeNames) / sizeof(kTypeNames[0]); ++i)
    if (path == kTypeNames[i])
      return static_cast<int>(i);
  return -1;
}

uint8_t villager_type_for_biome(std::string_view biome) {
  const auto it = by_biome().find(path_of(biome));
  return it == by_biome().end() ? kVillagerPlains : it->second;
}

// ---- VillagerTrade.getOffer ------------------------------------------------

std::optional<MerchantOffer> make_offer(const GeneratedTrade& trade, uint8_t type) {
  // The predicate half of `getOffer`: the variant mask the generator built from
  // the row's `merchant_predicate`, and the `modelled` flag for the dynamic
  // pieces this milestone cannot evaluate (both land on vanilla's null).
  if (!trade.modelled || (trade.variant_mask & (1u << type)) == 0)
    return std::nullopt;
  MerchantOffer offer;
  const int32_t want = item_of(trade.want);
  const int32_t give = item_of(trade.give);
  if (want == kAirItemId || give == kAirItemId)
    return std::nullopt;
  // `TradeCost.toItemCost(lootContext, 0)`: the count clamped to the item's own
  // stack size, then `getOffer`'s `< 1` refusal.
  const int max_stack = ItemRegistry::instance().max_stack_size(want);
  const int want_count = std::clamp(trade.want_count, 0, max_stack);
  if (want_count < 1)
    return std::nullopt;
  offer.cost_a = TradeCost{want, want_count};
  if (trade.additional != nullptr) {
    const int32_t additional = item_of(trade.additional);
    if (additional == kAirItemId)
      return std::nullopt;
    const int additional_count =
        std::clamp(trade.additional_count, 0, ItemRegistry::instance().max_stack_size(additional));
    if (additional_count < 1)
      return std::nullopt;
    offer.has_cost_b = true;
    offer.cost_b = TradeCost{additional, additional_count};
  }
  // `ItemStackTemplate.create()`: the result stack, whose count is the
  // template's own (default 1).
  const int give_stack = ItemRegistry::instance().max_stack_size(give);
  offer.result = ItemStack::of_id(give, std::max(1, std::min(trade.give_count, give_stack)));
  // The three `Math.max` clamps `getOffer` applies on its way into MerchantOffer.
  offer.max_uses = std::max(trade.max_uses, 1);
  offer.xp = std::max(trade.xp, 0);
  offer.price_multiplier = std::max(trade.reputation_discount, 0.0f);
  return offer;
}

void add_offers_from_trade_set(uint8_t profession, int level, uint8_t type,
                               const std::function<double()>& rng,
                               std::vector<MerchantOffer>* offers) {
  const GeneratedTradeSet* set = trade_set_for(profession, level);
  if (set == nullptr)
    return;
  // `addOffersFromItemListingsWithoutDuplicates`: a `Lists.newArrayList` of the
  // set's entries, `remove(random.nextInt(size))`, then the offer — a null is
  // dropped, and a row is never re-added however it went.
  std::vector<int> leftover;
  leftover.reserve(static_cast<size_t>(set->count));
  for (int i = 0; i < set->count; ++i)
    leftover.push_back(set->first + i);
  int found = 0;
  while (found < set->amount && !leftover.empty()) {
    int pick = static_cast<int>(rng() * static_cast<double>(leftover.size()));
    pick = std::clamp(pick, 0, static_cast<int>(leftover.size()) - 1);
    const int index = leftover[static_cast<size_t>(pick)];
    leftover.erase(leftover.begin() + pick);
    std::optional<MerchantOffer> offer = make_offer(kGeneratedTrades[index], type);
    if (!offer)
      continue;
    offers->push_back(std::move(*offer));
    ++found;
  }
}

// ---- the villager's merchant state -----------------------------------------

// The engine's stand-in for `MinecraftServer.getRandomSequence`. The site is
// `LootContext.Builder.create(randomSequence)`: with a sequence, vanilla's
// random is that sequence's stream, not the level's. Those streams are global
// per key and seeded from the world seed (`RandomSequences.get(key, seed)`), and
// this server has no world seed, so this hashes instead: one `mt19937_64` per
// (villager, level), advanceable and reproducible in a test.
std::function<double()> villager_rng(int32_t entity_id, int level) {
  const uint64_t mixed = (static_cast<uint64_t>(static_cast<uint32_t>(entity_id)) << 32) ^
                         static_cast<uint64_t>(static_cast<uint32_t>(level)) ^
                         0x9E3779B97F4A7C15ull;
  auto engine = std::make_shared<std::mt19937_64>(mixed);
  return [engine] { return static_cast<double>((*engine)() >> 11) * (1.0 / 9007199254740992.0); };
}

void ensure_villager_offers(VillagerState* villager, int32_t entity_id) {
  if (villager->offers_built)
    return;
  villager->offers_built = true;
  if (villager->profession == kVillagerNone)
    return;
  add_villager_level_offers(villager, entity_id, villager->level);
}

void add_villager_level_offers(VillagerState* villager, int32_t entity_id, int level) {
  if (villager->profession == kVillagerNone)
    return;
  add_offers_from_trade_set(villager->profession, level, villager->type,
                            villager_rng(entity_id, level), &villager->offers);
}

bool villager_reward_trade_xp(VillagerState* villager, const MerchantOffer& offer) {
  villager->xp += offer.xp;
  // `rewardTradeXp`'s `this.lastTradedPlayer = this.getTradingPlayer()` — the
  // next AI step is what spends it (see `villager_traded` in MobTickReport).
  villager->last_traded_player = villager->trading_player;
  if (!villager_should_increase_level(*villager))
    return false;
  // `rewardTradeXp` arms the 40-tick update (`updateMerchantTimer = 40`,
  // `increaseProfessionLevelOnUpdate = true`) rather than levelling here; the
  // mob tick applies it. It is re-armed on every later trade, like vanilla's.
  villager->update_merchant_timer = 40;
  villager->increase_level_on_update = true;
  return true;
}

bool villager_should_increase_level(const VillagerState& villager) {
  const int level = std::clamp(villager.level, 1, 5);
  return villager_can_level_up(level) && villager.xp >= villager_max_xp_per_level(level);
}

int villager_increase_career(VillagerState* villager, int32_t entity_id) {
  if (!villager_can_level_up(villager->level))
    return villager->level;
  villager->level += 1;
  // `Villager.setVillagerData` nulls the offers when the *profession* changes;
  // a level change keeps them, which is why this appends rather than rebuilds.
  add_villager_level_offers(villager, entity_id, villager->level);
  return villager->level;
}

} // namespace mc::world
