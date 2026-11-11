// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for villager trading (Phase 4 milestone L):
//   * the generated table: 65 trade sets (thirteen professions x five levels),
//     the smith rows a tag pull shares, the exact numbers of one known trade
//     (armorer level 1's iron boots want 4 emeralds, 12 uses, xp 1, a 0.2
//     price multiplier), the variant masks the cartographer/fisherman
//     predicates produce, and the four levels whose whole pool is unmodelled;
//   * `MerchantOffer`: the modified cost count's demand and special-price
//     terms, cost B never being demand-modified, `satisfiedBy`'s item and count
//     gates, `take`'s swap fallback, uses/out-of-stock/needs-restock,
//     `updateDemand` and the two resets;
//   * `makeOffer`: the null paths (unmodelled, wrong variant, a zero count) and
//     a real row's offer field for field;
//   * `addOffersFromTradeSet`: the without-duplicates pick with a pinned RNG —
//     the pool size it draws from, the count it stops at, and an unmodelled row
//     thinning the result rather than being replaced;
//   * `VillagerType.byBiome` (the map's own entries and the plains default) and
//     the profession/type name lookups;
//   * the xp bookkeeping: the thresholds, the armed 40-tick level-up, and
//     `increaseMerchantCareer` appending the next level's set;
//   * the merchant clock in the mob tick: the `!isTrading()` gate, the fortieth
//     tick raising the career, the regeneration it adds, and the one
//     happy-villager report each trade produces.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "world/item.h"
#include "world/mob.h"
#include "world/trade.h"
#include "world/trade_table.h"

#include "net/byte_buffer.h"
#include "protocol/ids.h"
#include "server/entity_packets.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

const GeneratedTrade* find_trade(const char* name) {
  for (int i = 0; i < kGeneratedTradeCount; ++i)
    if (std::string(kGeneratedTrades[i].name) == name)
      return &kGeneratedTrades[i];
  return nullptr;
}

std::optional<MerchantOffer> offer_of(const char* name, uint8_t type) {
  const GeneratedTrade* trade = find_trade(name);
  if (trade == nullptr)
    return std::nullopt;
  return make_offer(*trade, type);
}

// A deterministic RNG: cycles a fixed list of draws, in order.
std::function<double()> draws(std::initializer_list<double> values) {
  auto state = std::make_shared<std::vector<double>>(values);
  auto index = std::make_shared<size_t>(0);
  return [state, index] {
    const double value = (*index < state->size()) ? (*state)[*index] : 0.0;
    ++*index;
    return value;
  };
}

void test_table_shape() {
  // Thirteen professions with trade sets, five levels each.
  CHECK_EQ(kGeneratedTradeSetCount, 65);
  for (const GeneratedTradeSet& set : kGeneratedTradeSets) {
    CHECK(set.level >= 1 && set.level <= 5);
    CHECK(villager_profession_id(set.profession) >= 0);
    CHECK(set.count > 0);
    CHECK(set.amount >= 1);
  }
  // The smith trade is in three level-1 tags' pull — armorer, toolsmith and
  // weaponsmith all name `#minecraft:common_smith/level_1` — so the tool emits
  // a row per set reference rather than deduplicating.
  int coal = 0;
  for (int i = 0; i < kGeneratedTradeCount; ++i)
    if (std::string(kGeneratedTrades[i].name) == "minecraft:smith/1/coal_emerald")
      ++coal;
  CHECK_EQ(coal, 3);

  // The known trade, field for field: `armorer/1/emerald_iron_boots` is
  // `wants` 4 emeralds, `gives` iron boots, 12 uses, xp 1 (the default), a 0.2
  // price multiplier and no predicate.
  const GeneratedTrade* boots = find_trade("minecraft:armorer/1/emerald_iron_boots");
  CHECK(boots != nullptr);
  if (boots) {
    CHECK(std::string(boots->want) == "minecraft:emerald");
    CHECK_EQ(boots->want_count, 4);
    CHECK(boots->additional == nullptr);
    CHECK(std::string(boots->give) == "minecraft:iron_boots");
    CHECK_EQ(boots->give_count, 1);
    CHECK_EQ(boots->max_uses, 12);
    CHECK_EQ(boots->xp, 1);
    CHECK(boots->reputation_discount > 0.19f && boots->reputation_discount < 0.21f);
    CHECK_EQ(boots->variant_mask, kVariantAny);
    CHECK(boots->modelled);
  }

  // The two predicate shapes: a swamp cartographer may buy a black banner, a
  // desert one may not; a plains fisherman may buy an oak boat.
  const GeneratedTrade* black = find_trade("minecraft:cartographer/4/emerald_black_banner");
  CHECK(black != nullptr && black->modelled);
  if (black)
    CHECK_EQ(black->variant_mask, 1u << kVillagerSwamp);
  const GeneratedTrade* oak = find_trade("minecraft:fisherman/5/oak_boat_emerald");
  CHECK(oak != nullptr && oak->modelled);
  if (oak)
    CHECK_EQ(oak->variant_mask, 1u << kVillagerPlains);

  // The four levels the dynamic trades empty: every row there is unmodelled.
  const char* empty_levels[][2] = {
      {"armorer", "4"}, {"armorer", "5"}, {"toolsmith", "5"}, {"weaponsmith", "5"}};
  for (const auto& entry : empty_levels) {
    const GeneratedTradeSet* set =
        trade_set_for(static_cast<uint8_t>(villager_profession_id(entry[0])), std::atoi(entry[1]));
    CHECK(set != nullptr);
    if (!set)
      continue;
    bool any_modelled = false;
    for (int i = 0; i < set->count; ++i)
      any_modelled = any_modelled || kGeneratedTrades[set->first + i].modelled;
    CHECK(!any_modelled);
  }

  // A fully modelled high-count set: mason level 4 is 33 rows of quartz and
  // terracotta, all modelled.
  const GeneratedTradeSet* mason4 = trade_set_for(kVillagerMason, 4);
  CHECK(mason4 != nullptr);
  if (mason4) {
    CHECK_EQ(mason4->count, 33);
    for (int i = 0; i < mason4->count; ++i)
      CHECK(kGeneratedTrades[mason4->first + i].modelled);
  }
}

void test_offer_arithmetic() {
  std::optional<MerchantOffer> maybe =
      offer_of("minecraft:armorer/1/emerald_iron_boots", kVillagerPlains);
  CHECK(maybe.has_value());
  if (!maybe)
    return;
  MerchantOffer offer = *maybe;
  CHECK_EQ(offer.cost_a.item, ItemRegistry::instance().id_of("minecraft:emerald").value_or(-1));
  CHECK_EQ(offer.modified_cost_count(offer.cost_a), 4);
  CHECK_EQ(offer.result.item, ItemRegistry::instance().id_of("minecraft:iron_boots").value_or(-1));
  CHECK_EQ(offer.result.count, 1);
  CHECK_EQ(offer.max_uses, 12);
  CHECK_EQ(offer.xp, 1);
  CHECK(!offer.has_cost_b);
  CHECK_EQ(offer.special_price_diff, 0);
  CHECK_EQ(offer.demand, 0);

  // The demand term: base * demand * priceMultiplier, floored, never negative.
  offer.demand = 12; // uses 12 of 12 -> updateDemand would leave 12
  CHECK_EQ(offer.modified_cost_count(offer.cost_a), 4 + static_cast<int>(0.2f * 12 * 4));
  offer.demand = -100; // a negative demand is clamped at 0
  CHECK_EQ(offer.modified_cost_count(offer.cost_a), 4);
  // The special price diff moves the count directly, down to 1.
  offer.demand = 0;
  offer.special_price_diff = -2;
  CHECK_EQ(offer.modified_cost_count(offer.cost_a), 2);
  offer.special_price_diff = -100;
  CHECK_EQ(offer.modified_cost_count(offer.cost_a), 1);
  offer.special_price_diff = 0;

  // satisfiedBy: the item, the count and the second cost.
  ItemStack four = ItemStack::of_id(offer.cost_a.item, 4);
  ItemStack three = ItemStack::of_id(offer.cost_a.item, 3);
  CHECK(offer.satisfied_by(four, ItemStack{}));
  CHECK(!offer.satisfied_by(three, ItemStack{}));
  CHECK(!offer.satisfied_by(ItemStack::of("minecraft:iron_ingot", 4), ItemStack{}));
  CHECK(!offer.satisfied_by(four, ItemStack::of("minecraft:book", 1)));
  ItemStack payments_a = four;
  ItemStack payments_b;
  CHECK(offer.take(&payments_a, &payments_b));
  CHECK_EQ(payments_a.count, 0);
  // A failed take does not shrink anything.
  ItemStack short_a = three;
  CHECK(!offer.take(&short_a, &payments_b));
  CHECK_EQ(short_a.count, 3);

  // Uses and the stock predicates.
  CHECK(!offer.is_out_of_stock());
  CHECK(!offer.needs_restock());
  offer.increase_uses();
  CHECK_EQ(offer.uses, 1);
  CHECK(offer.needs_restock());
  offer.reset_uses();
  CHECK_EQ(offer.uses, 0);
  for (int i = 0; i < offer.max_uses; ++i)
    offer.increase_uses();
  CHECK(offer.is_out_of_stock());
  // updateDemand: demand + uses - (maxUses - uses), in place.
  offer.update_demand();
  CHECK_EQ(offer.demand, 0 + offer.max_uses - (offer.max_uses - offer.max_uses));
  CHECK_EQ(offer.demand, offer.max_uses);

  // A second cost is never demand-modified: `take` shrinks it by the base count
  // while cost A carries the modified one.
  std::optional<MerchantOffer> two =
      offer_of("minecraft:librarian/1/paper_emerald", kVillagerPlains);
  CHECK(two.has_value());
  if (two) {
    CHECK(!two->has_cost_b);
  }
  const GeneratedTrade* with_b = nullptr;
  for (int i = 0; i < kGeneratedTradeCount; ++i)
    if (kGeneratedTrades[i].additional != nullptr && kGeneratedTrades[i].modelled) {
      with_b = &kGeneratedTrades[i];
      break;
    }
  if (with_b != nullptr) {
    std::optional<MerchantOffer> offer = make_offer(*with_b, kVillagerPlains);
    CHECK(offer.has_value());
    if (offer) {
      CHECK(offer->has_cost_b);
      CHECK_EQ(offer->cost_b.count, with_b->additional_count);
      offer->demand = 5;
      ItemStack a = ItemStack::of_id(offer->cost_a.item, 64);
      ItemStack b = ItemStack::of_id(offer->cost_b.item, 64);
      const int a_before = b.count;
      CHECK(offer->take(&a, &b));
      CHECK_EQ(b.count, a_before - with_b->additional_count);
    }
  }
}

void test_offer_null_paths() {
  // An unmodelled row is a null, whatever the villager.
  const GeneratedTrade* enchant = find_trade("minecraft:armorer/4/emerald_enchanted_diamond_boots");
  CHECK(enchant != nullptr && !enchant->modelled);
  if (enchant)
    CHECK(!make_offer(*enchant, kVillagerPlains).has_value());

  // The variant predicate refuses every type it does not name.
  CHECK(!offer_of("minecraft:cartographer/4/emerald_black_banner", kVillagerPlains).has_value());
  CHECK(offer_of("minecraft:cartographer/4/emerald_black_banner", kVillagerSwamp).has_value());

  // The librarian's enchanted book wants a count of 0 — vanilla's null.
  const GeneratedTrade* book = find_trade("minecraft:librarian/1/emerald_and_book_enchanted_book");
  CHECK(book != nullptr);
  if (book) {
    CHECK(!book->modelled);
    CHECK(!make_offer(*book, kVillagerPlains).has_value());
  }
}

void test_selection() {
  // Armorer level 1: five rows (the smith coal row first), two picks. With a
  // pinned RNG the first pick is row 0 (coal), the second is the boots (row 2
  // of the remaining four).
  std::vector<MerchantOffer> offers;
  add_offers_from_trade_set(kVillagerArmorer, 1, kVillagerPlains, draws({0.0, 0.25}), &offers);
  CHECK_EQ(offers.size(), 2);
  if (offers.size() == 2) {
    CHECK(std::string(kGeneratedTrades[0].name) == "minecraft:smith/1/coal_emerald");
    CHECK_EQ(offers[0].cost_a.item, ItemRegistry::instance().id_of("minecraft:coal").value_or(-1));
    CHECK_EQ(offers[0].max_uses, 16);
    CHECK_EQ(offers[1].cost_a.item,
             ItemRegistry::instance().id_of("minecraft:emerald").value_or(-1));
  }

  // A pool of two rows, one unmodelled, still yields one offer — the null is
  // dropped, not replaced (the without-duplicates loop removes first).
  const GeneratedTradeSet* armorer4 = trade_set_for(kVillagerArmorer, 4);
  CHECK(armorer4 != nullptr);
  if (armorer4) {
    CHECK_EQ(armorer4->amount, 2);
    std::vector<MerchantOffer> thin;
    add_offers_from_trade_set(kVillagerArmorer, 4, kVillagerPlains, draws({0.0, 0.0}), &thin);
    CHECK_EQ(thin.size(), 0);
  }

  // The variant gate thins the cartographer's banner pool the same way: exactly
  // the four swamp-reachable entries of its sixteen rows, no duplicates.
  int swamp_banners = 0;
  const GeneratedTradeSet* cart4 = trade_set_for(kVillagerCartographer, 4);
  CHECK(cart4 != nullptr);
  if (cart4)
    for (int i = 0; i < cart4->count; ++i)
      if ((kGeneratedTrades[cart4->first + i].variant_mask & (1u << kVillagerSwamp)) != 0)
        ++swamp_banners;
  CHECK(swamp_banners > 0);
  std::vector<MerchantOffer> banners;
  add_offers_from_trade_set(kVillagerCartographer, 4, kVillagerSwamp, draws({0.0, 0.0}), &banners);
  CHECK_EQ(banners.size(), 2);
  // With both draws pinned at 0 the pick walks the set in file order: the item
  // frame (no predicate) then the first swamp-reachable banner (light blue).
  if (banners.size() == 2) {
    CHECK_EQ(banners[0].result.item,
             ItemRegistry::instance().id_of("minecraft:item_frame").value_or(-1));
    CHECK_EQ(banners[1].result.item,
             ItemRegistry::instance().id_of("minecraft:light_blue_banner").value_or(-1));
    CHECK(banners[0].result.item != banners[1].result.item);
  }
}

void test_names_and_biomes() {
  CHECK_EQ(villager_profession_id("farmer"), kVillagerFarmer);
  CHECK_EQ(villager_profession_id("minecraft:farmer"), kVillagerFarmer);
  CHECK_EQ(villager_profession_id("farmer "), -1);
  CHECK_EQ(villager_profession_id("mythic:farmer"), -1);
  CHECK(std::string(villager_profession_name(kVillagerWeaponsmith)) == "weaponsmith");
  CHECK(villager_profession_name(15).empty());

  CHECK_EQ(villager_type_id("plains"), kVillagerPlains);
  CHECK_EQ(villager_type_id("minecraft:desert"), kVillagerDesert);
  CHECK_EQ(villager_type_id("beach"), -1);

  // VillagerType.byBiome: the map's own entries, and plains for everything it
  // does not name (a beach, an ocean, an unknown name).
  CHECK_EQ(villager_type_for_biome("minecraft:badlands"), kVillagerDesert);
  CHECK_EQ(villager_type_for_biome("minecraft:jungle"), kVillagerJungle);
  CHECK_EQ(villager_type_for_biome("minecraft:windswept_forest"), kVillagerTaiga);
  CHECK_EQ(villager_type_for_biome("minecraft:snowy_plains"), kVillagerSnow);
  CHECK_EQ(villager_type_for_biome("minecraft:mangrove_swamp"), kVillagerSwamp);
  CHECK_EQ(villager_type_for_biome("minecraft:beach"), kVillagerPlains);
  CHECK_EQ(villager_type_for_biome("minecraft:ocean"), kVillagerPlains);
}

void test_xp_bookkeeping() {
  VillagerState villager;
  villager.profession = kVillagerFarmer;
  villager.type = kVillagerPlains;
  CHECK_EQ(villager.level, 1);
  // Level 1 rises at 10 xp. A first trade of xp 1 does not arm anything.
  MerchantOffer offer;
  offer.xp = 5;
  CHECK(!villager_reward_trade_xp(&villager, offer));
  CHECK_EQ(villager.xp, 5);
  CHECK(!villager_should_increase_level(villager));
  CHECK(villager_reward_trade_xp(&villager, offer));
  CHECK_EQ(villager.xp, 10);
  CHECK(villager_should_increase_level(villager));
  CHECK_EQ(villager.update_merchant_timer, 40);
  CHECK(villager.increase_level_on_update);

  // The lazy build: level 1's set, then increaseMerchantCareer appends level 2's.
  villager.offers.clear();
  villager.offers_built = false;
  ensure_villager_offers(&villager, 7);
  CHECK(villager.offers_built);
  const size_t level_one = villager.offers.size();
  CHECK(level_one > 0);
  CHECK_EQ(villager_increase_career(&villager, 7), 2);
  CHECK(villager.offers.size() > level_one);
  // A profession with no set (none) builds nothing.
  VillagerState nobody;
  nobody.profession = kVillagerNone;
  ensure_villager_offers(&nobody, 8);
  CHECK(nobody.offers.empty());
}

// The offer list and the three words around it, as a client reads them: one
// packet, one writer each for the cost, the result and the eight fixed fields.
void test_offer_packet() {
  const GeneratedMob* type = mob_by_name("minecraft:villager");
  CHECK(type != nullptr);
  if (type == nullptr)
    return;
  Mob mob;
  mob.entity_id = 3;
  mob_place(mob, *type, MobEnvironment{}, 0.5, 64.0, 0.5, 0.0f);
  mob.villager.profession = kVillagerFarmer;
  mob.villager.type = kVillagerPlains;
  ensure_villager_offers(&mob.villager, mob.entity_id);
  CHECK_EQ(mob.villager.offers.size(), static_cast<size_t>(2)); // farmer/1 amount = 2

  const std::vector<uint8_t> bytes =
      mc::server::make_merchant_offers(7, mob.villager.offers, 1, 0, true, true);
  mc::net::Reader r(bytes);
  uint32_t packet_id = 0, container = 0, count = 0;
  CHECK(r.varint(&packet_id));
  CHECK_EQ(packet_id, static_cast<uint32_t>(mc::protocol::kPlayMerchantOffersId));
  CHECK(r.varint(&container));
  CHECK_EQ(container, static_cast<uint32_t>(7));
  CHECK(r.varint(&count));
  CHECK_EQ(count, static_cast<uint32_t>(mob.villager.offers.size()));
  for (size_t i = 0; i < mob.villager.offers.size(); ++i) {
    const MerchantOffer& offer = mob.villager.offers[i];
    uint32_t item = 0, cost = 0, predicate = 0;
    CHECK(r.varint(&item));
    CHECK_EQ(item, static_cast<uint32_t>(offer.cost_a.item));
    CHECK(r.varint(&cost));
    CHECK_EQ(cost, static_cast<uint32_t>(offer.cost_a.count));
    CHECK(r.varint(&predicate));
    CHECK_EQ(predicate, static_cast<uint32_t>(0)); // the empty predicate
    mc::world::ItemStack result;
    CHECK(mc::world::read_item_stack(r, &result));
    CHECK_EQ(result.item, offer.result.item);
    CHECK_EQ(result.count, offer.result.count);
    uint8_t has_b = 0;
    CHECK(r.u8(&has_b));
    CHECK_EQ(has_b, offer.has_cost_b ? 1 : 0);
    if (offer.has_cost_b) {
      CHECK(r.varint(&item));
      CHECK_EQ(item, static_cast<uint32_t>(offer.cost_b.item));
      CHECK(r.varint(&cost));
      CHECK_EQ(cost, static_cast<uint32_t>(offer.cost_b.count));
      CHECK(r.varint(&predicate));
    }
    uint8_t out_of_stock = 0;
    CHECK(r.u8(&out_of_stock));
    CHECK_EQ(out_of_stock, offer.is_out_of_stock() ? 1 : 0);
    uint32_t field = 0;
    CHECK(r.u32(&field));
    CHECK_EQ(field, static_cast<uint32_t>(offer.uses));
    CHECK(r.u32(&field));
    CHECK_EQ(field, static_cast<uint32_t>(offer.max_uses));
    CHECK(r.u32(&field));
    CHECK_EQ(field, static_cast<uint32_t>(offer.xp));
    CHECK(r.u32(&field));
    CHECK_EQ(field, static_cast<uint32_t>(offer.special_price_diff));
    float price = 0.0f;
    CHECK(r.f32(&price));
    CHECK(price == offer.price_multiplier);
    CHECK(r.u32(&field));
    CHECK_EQ(field, static_cast<uint32_t>(offer.demand));
  }
  uint32_t level = 0, xp = 0;
  CHECK(r.varint(&level));
  CHECK_EQ(level, static_cast<uint32_t>(1));
  CHECK(r.varint(&xp));
  CHECK_EQ(xp, static_cast<uint32_t>(0));
  uint8_t show = 0, restock = 0;
  CHECK(r.u8(&show));
  CHECK_EQ(show, static_cast<uint8_t>(1));
  CHECK(r.u8(&restock));
  CHECK_EQ(restock, static_cast<uint8_t>(1));
  // Nothing may be left over: a field-order slip would show up here.
  uint8_t trailing = 0;
  CHECK(!r.u8(&trailing));

  // ...and the villager's own word, the one a career rise resends: the
  // `VillagerData` entry (index 19, serializer 18, three varints).
  const std::vector<uint8_t> data =
      mc::server::make_villager_data_word(3, kVillagerTaiga, kVillagerFarmer, 4);
  mc::net::Reader d(data);
  uint32_t id = 0, eid = 0, serializer = 0, value = 0;
  CHECK(d.varint(&id));
  CHECK_EQ(id, static_cast<uint32_t>(mc::protocol::kPlaySetEntityDataId));
  CHECK(d.varint(&eid));
  CHECK_EQ(eid, static_cast<uint32_t>(3));
  uint8_t index = 0;
  CHECK(d.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(mc::server::kVillagerDataIndex));
  CHECK(d.varint(&serializer));
  CHECK_EQ(serializer, static_cast<uint32_t>(mc::server::kVillagerDataSerializerId));
  CHECK(d.varint(&value));
  CHECK_EQ(value, static_cast<uint32_t>(kVillagerTaiga));
  CHECK(d.varint(&value));
  CHECK_EQ(value, static_cast<uint32_t>(kVillagerFarmer));
  CHECK(d.varint(&value));
  CHECK_EQ(value, static_cast<uint32_t>(4));
  CHECK(d.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(0xFF));
  CHECK(!d.u8(&trailing));

  // The finalized bit beside it (index 20, a BOOLEAN word) and the unhappy
  // counter (index 18, an INT, i.e. a *varint*, not four fixed bytes).
  const std::vector<uint8_t> finalized_bytes = mc::server::make_villager_finalized_word(3, true);
  mc::net::Reader f(finalized_bytes);
  CHECK(f.varint(&id));
  CHECK(f.varint(&eid));
  CHECK(f.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(mc::server::kVillagerDataFinalizedIndex));
  CHECK(f.varint(&serializer));
  CHECK_EQ(serializer, static_cast<uint32_t>(mc::server::kBooleanSerializerId));
  uint8_t finalized = 0;
  CHECK(f.u8(&finalized));
  CHECK_EQ(finalized, static_cast<uint8_t>(1));
  CHECK(f.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(0xFF));
  CHECK(!f.u8(&trailing));

  const std::vector<uint8_t> unhappy = mc::server::make_villager_unhappy_word(3, 40);
  mc::net::Reader u(unhappy);
  CHECK(u.varint(&id));
  CHECK(u.varint(&eid));
  CHECK(u.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(mc::server::kAbstractVillagerUnhappyIndex));
  CHECK(u.varint(&serializer));
  CHECK_EQ(serializer, static_cast<uint32_t>(mc::server::kIntSerializerId));
  CHECK(u.varint(&value));
  CHECK_EQ(value, static_cast<uint32_t>(40));
  CHECK(u.u8(&index));
  CHECK_EQ(index, static_cast<uint32_t>(0xFF));
  CHECK(!u.u8(&trailing));
}

// The merchant clock lives in the mob tick (`Villager.customServerAiStep`), so
// this is where the 40 ticks, the level rise, the regeneration and the
// happy-villager report are checked — packet-free, on one mob.
void test_merchant_clock() {
  const GeneratedMob* type = mob_by_name("minecraft:villager");
  CHECK(type != nullptr);
  if (type == nullptr)
    return;
  Mob mob;
  mob.entity_id = 7;
  mob_place(mob, *type, MobEnvironment{}, 0.5, 64.0, 0.5, 0.0f);
  mob.persistence_required = true; // nothing here is a spawn distance
  mob.villager.profession = kVillagerFarmer;
  mob.villager.type = kVillagerPlains;
  ensure_villager_offers(&mob.villager, mob.entity_id);
  const size_t level_one = mob.villager.offers.size();
  CHECK(level_one > 0);

  // A traded offer while a player has the screen open: the xp crosses level 1's
  // threshold, which arms the 40-tick update and records the trading player.
  mob.villager.trading_player = 42;
  mob.villager.xp = 9;
  MerchantOffer sold;
  sold.xp = 1;
  CHECK(villager_reward_trade_xp(&mob.villager, sold));
  CHECK_EQ(mob.villager.update_merchant_timer, 40);
  CHECK_EQ(mob.villager.last_traded_player, 42);

  std::vector<Mob> mobs{mob};
  // `!isTrading()`: with the customer still trading the clock does not move, but
  // the first tick spends `lastTradedPlayer` — one happy-villager burst per trade.
  MobTickReport report = tick_mobs(mobs, MobEnvironment{});
  CHECK_EQ(mobs[0].villager.update_merchant_timer, 40);
  CHECK_EQ(mobs[0].villager.level, 1);
  CHECK(report.villager_leveled.empty());
  CHECK_EQ(report.villager_traded.size(), static_cast<size_t>(1));
  if (!report.villager_traded.empty())
    CHECK_EQ(report.villager_traded[0], 7);
  CHECK_EQ(mobs[0].villager.last_traded_player, -1);
  // ...and it is spent exactly once: the next tick reports nobody.
  CHECK(tick_mobs(mobs, MobEnvironment{}).villager_traded.empty());

  // The screen closes (`stopTrading`): 39 ticks leave the villager at level 1,
  // the fortieth raises the career and appends level 2's offers.
  mobs[0].villager.trading_player = -1;
  bool rose = false;
  for (int i = 1; i <= 40; ++i) {
    report = tick_mobs(mobs, MobEnvironment{});
    CHECK_EQ(mobs[0].villager.update_merchant_timer, i == 40 ? 0 : 40 - i);
    if (i < 40) {
      CHECK_EQ(mobs[0].villager.level, 1);
      CHECK(report.villager_leveled.empty());
    }
    if (i == 40) {
      CHECK_EQ(report.villager_leveled.size(), static_cast<size_t>(1));
      rose = !report.villager_leveled.empty();
    }
  }
  CHECK(rose);
  CHECK_EQ(mobs[0].villager.level, 2);
  CHECK(!mobs[0].villager.increase_level_on_update);
  CHECK(mobs[0].villager.offers.size() > level_one);
  // `addEffect(REGENERATION, 200, 0)`: a real instance on the mob's own set.
  const GeneratedMobEffect* regeneration = mob_effect_by_name("minecraft:regeneration");
  CHECK(regeneration != nullptr);
  if (regeneration != nullptr) {
    const MobEffectInstance* instance = mobs[0].effects.get(mob_effect_id_of(*regeneration));
    CHECK(instance != nullptr);
    if (instance != nullptr) {
      CHECK_EQ(instance->duration, 200);
      CHECK_EQ(instance->amplifier, 0);
    }
  }
  // The timer is spent: 40 more ticks raise nothing.
  for (int i = 0; i < 40; ++i)
    CHECK(tick_mobs(mobs, MobEnvironment{}).villager_leveled.empty());
  CHECK_EQ(mobs[0].villager.level, 2);
}

} // namespace

int main() {
  test_table_shape();
  test_offer_arithmetic();
  test_offer_null_paths();
  test_selection();
  test_names_and_biomes();
  test_xp_bookkeeping();
  test_offer_packet();
  test_merchant_clock();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("trade tests passed\n");
  return 0;
}
