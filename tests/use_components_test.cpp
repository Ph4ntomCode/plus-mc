// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the two components that run after a finished use
// (`ItemStack.applyAfterUseComponentSideEffects`):
//
//   * `minecraft:use_remainder`: the generated columns name the right items — a
//     stew leaves a bowl, a milk bucket a bucket, a honey bottle a glass bottle —
//     and `UseRemainder.convertIntoRemainder` takes each of its four branches:
//     the emptied stack hands the remainder to the *hand*, a surviving stack
//     (there were more of them) hands it to the inventory instead, creative mode
//     produces neither, and a use that did not shrink the stack is treated the
//     same way creative is;
//   * `minecraft:use_cooldown`: the three items that carry it and their
//     durations, the group falling back to the item's own registry key
//     (`ItemCooldowns.getCooldownGroup`), and the map's own behaviour —
//     `isOnCooldown`/`getCooldownPercent` across the range's exactly `ticks`
//     ticks, a re-add replacing a running range, `tick()` reporting the groups it
//     dropped so the server can send each a zero duration, and `clear` emptying
//     the map without resetting the player's clock;
//   * the `cooldown` packet (0x16) as bytes: `[varint id][string group][varint
//     duration]`, which is `Identifier.STREAM_CODEC` then a varint.
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "protocol/ids.h"
#include "server/entity_packets.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/use_components.h"

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

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

const GeneratedItem* item(const char* name) {
  const std::optional<int32_t> id = ItemRegistry::instance().id_of(name);
  return id ? ItemRegistry::instance().by_id(*id) : nullptr;
}

int32_t item_id(const char* name) {
  return ItemRegistry::instance().id_of(name).value_or(-1);
}

// The four items whose `use_remainder` is a bowl, and the three that hand back a
// container. Every other item in the table has no remainder at all.
void test_table() {
  struct Row {
    const char* item;
    const char* remainder;
  };
  const Row bowls[] = {{"minecraft:mushroom_stew", "minecraft:bowl"},
                       {"minecraft:rabbit_stew", "minecraft:bowl"},
                       {"minecraft:beetroot_soup", "minecraft:bowl"},
                       {"minecraft:suspicious_stew", "minecraft:bowl"}};
  for (const Row& row : bowls) {
    const GeneratedItem* info = item(row.item);
    CHECK(info != nullptr);
    if (!info)
      continue;
    CHECK_EQ(info->remainder_item, item_id(row.remainder));
    CHECK_EQ(info->remainder_count, 1);
  }
  const Row bottles[] = {{"minecraft:milk_bucket", "minecraft:bucket"},
                         {"minecraft:honey_bottle", "minecraft:glass_bottle"},
                         {"minecraft:potion", "minecraft:glass_bottle"}};
  for (const Row& row : bottles) {
    const GeneratedItem* info = item(row.item);
    CHECK(info != nullptr);
    if (!info)
      continue;
    CHECK_EQ(info->remainder_item, item_id(row.remainder));
    CHECK_EQ(info->remainder_count, 1);
  }
  // The remainder ids really are those items, which is the property a wrong
  // column would break silently (the id would still be a valid item).
  CHECK(std::string(ItemRegistry::instance().name_of(item_id("minecraft:bowl"))) ==
        std::string("minecraft:bowl"));
  CHECK(std::string(ItemRegistry::instance().name_of(item_id("minecraft:bucket"))) ==
        std::string("minecraft:bucket"));

  // A stew is food with a remainder; a bowl is neither.
  CHECK((item("minecraft:mushroom_stew")->flags & kItemIsFood) != 0);
  CHECK_EQ(item("minecraft:bowl")->remainder_item, 0);

  // Exactly seven items have a remainder (the four stews/one soup family count
  // above is five rows, plus the three containers), so a regenerated column that
  // started inventing remainders would show up here.
  int with_remainder = 0;
  for (int i = 0; i < kItemCount; ++i)
    if (kItems[i].remainder_item != 0)
      ++with_remainder;
  CHECK_EQ(with_remainder, 7);
}

// The three items carrying `minecraft:use_cooldown`, with the durations
// `UseCooldown.ticks()` computes from their `seconds`.
void test_cooldown_table() {
  struct Row {
    const char* item;
    int ticks;
  };
  const Row rows[] = {
      {"minecraft:ender_pearl", 20}, {"minecraft:wind_charge", 10}, {"minecraft:chorus_fruit", 20}};
  for (const Row& row : rows) {
    const GeneratedItem* info = item(row.item);
    CHECK(info != nullptr);
    if (!info)
      continue;
    CHECK_EQ(info->cooldown_ticks, row.ticks);
    // No 26.2 item names a cooldown group, so every one of them falls back to
    // the item's own registry key — which is also the string the packet carries.
    CHECK(info->cooldown_group == nullptr);
    CHECK(cooldown_group_of(*info) == std::string(row.item));
  }
  int with_cooldown = 0;
  for (int i = 0; i < kItemCount; ++i)
    if (kItems[i].cooldown_ticks != 0)
      ++with_cooldown;
  CHECK_EQ(with_cooldown, 3);

  // The fallback is a fallback: an explicit group is used as it stands.
  GeneratedItem grouped = *item("minecraft:chorus_fruit");
  grouped.cooldown_group = "minecraft:test_group";
  CHECK(cooldown_group_of(grouped) == std::string("minecraft:test_group"));
}

// `UseRemainder.convertIntoRemainder`'s branches, with a stew's bowl as the
// template.
void test_remainder_conversion() {
  const int32_t stew = item_id("minecraft:mushroom_stew");
  const int32_t bowl = item_id("minecraft:bowl");

  // 1. The last stew of a stack: the bowl takes the hand slot.
  {
    ItemStack used = ItemStack::of_id(stew, 0); // after ItemStack.consume(1, ...)
    const RemainderResult r = convert_into_remainder(used, 1, false, bowl, 1);
    CHECK_EQ(r.hand.item, bowl);
    CHECK_EQ(r.hand.count, 1);
    CHECK(r.extra.empty());
  }
  // 2. More than one stew: the stack survives, so the bowl cannot take the hand
  //    slot and goes to the inventory (handleExtraItemsCreatedOnUse) instead.
  {
    const ItemStack used = ItemStack::of_id(stew, 2);
    const RemainderResult r = convert_into_remainder(used, 3, false, bowl, 1);
    CHECK_EQ(r.hand.item, stew);
    CHECK_EQ(r.hand.count, 2);
    CHECK_EQ(r.extra.item, bowl);
    CHECK_EQ(r.extra.count, 1);
  }
  // 3. Creative (hasInfiniteMaterials): nothing was consumed, nothing comes
  //    back — the *first* branch, ahead of the count comparison.
  {
    const ItemStack used = ItemStack::of_id(stew, 1);
    const RemainderResult r = convert_into_remainder(used, 1, true, bowl, 1);
    CHECK_EQ(r.hand.item, stew);
    CHECK_EQ(r.hand.count, 1);
    CHECK(r.extra.empty());
  }
  // 4. A use that left the stack as it was is treated exactly like creative:
  //    vanilla compares the result's count against the pre-use count.
  {
    const ItemStack used = ItemStack::of_id(stew, 4);
    const RemainderResult r = convert_into_remainder(used, 4, false, bowl, 1);
    CHECK_EQ(r.hand.item, stew);
    CHECK_EQ(r.hand.count, 4);
    CHECK(r.extra.empty());
  }
  // The template's own count is carried (a stack of two bowls would arrive as
  // count 2), and a bogus zero count still yields one item rather than an empty
  // stack, because the table's column is an unsigned count.
  {
    const ItemStack used = ItemStack::of_id(stew, 0);
    const RemainderResult r = convert_into_remainder(used, 1, false, bowl, 2);
    CHECK_EQ(r.hand.count, 2);
    const RemainderResult one = convert_into_remainder(used, 1, false, bowl, 0);
    CHECK_EQ(one.hand.item, bowl);
    CHECK_EQ(one.hand.count, 1);
  }
}

// `ItemCooldowns`: the range is exactly `ticks` ticks long, the percent is what
// the client draws with, and the tick reports what it dropped.
void test_cooldowns() {
  const std::string group = cooldown_group_of(*item("minecraft:chorus_fruit"));
  CHECK(group == std::string("minecraft:chorus_fruit"));
  ItemCooldowns cooldowns;
  CHECK(cooldowns.empty());
  CHECK(!cooldowns.is_on_cooldown(group));
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 0.0f, 1e-6);

  cooldowns.add(group, 20);
  CHECK(!cooldowns.empty());
  CHECK(cooldowns.is_on_cooldown(group));
  // A fresh range is at 100%, and the counter has not moved yet.
  CHECK_EQ(cooldowns.tick_count(), 0);
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 1.0f, 1e-6);

  // Ten ticks in, half of it is left — and the partial-tick argument is the
  // client's interpolation, so half a tick later is half a tick less.
  for (int i = 0; i < 10; ++i)
    CHECK(cooldowns.tick().empty());
  CHECK_EQ(cooldowns.tick_count(), 10);
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 0.5f, 1e-5);
  CHECK_NEAR(cooldowns.percent(group, 0.5f), 0.475f, 1e-5);
  CHECK(cooldowns.is_on_cooldown(group));

  // Nine more ticks still leave it running (the range is [0, 20)), and the
  // twentieth reports it ended and leaves nothing behind.
  for (int i = 0; i < 9; ++i)
    CHECK(cooldowns.tick().empty());
  CHECK(cooldowns.is_on_cooldown(group));
  const std::vector<std::string> ended = cooldowns.tick();
  CHECK_EQ(ended.size(), 1);
  if (!ended.empty())
    CHECK(ended[0] == group);
  CHECK(!cooldowns.is_on_cooldown(group));
  CHECK(cooldowns.empty());
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 0.0f, 1e-6);
  // ...and a tick with nothing left to drop reports nothing.
  CHECK(cooldowns.tick().empty());

  // addCooldown replaces a range that is already running rather than extending
  // it: re-adding mid-range puts the percent back to 100%.
  cooldowns.add(group, 20);
  for (int i = 0; i < 10; ++i)
    cooldowns.tick();
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 0.5f, 1e-5);
  cooldowns.add(group, 20);
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 1.0f, 1e-6);

  // Two groups are independent, and only the one that ran out is reported.
  const std::string short_group = cooldown_group_of(*item("minecraft:wind_charge"));
  cooldowns.remove(group);
  CHECK(!cooldowns.is_on_cooldown(group));
  cooldowns.add(short_group, 10);
  int ticks = 0;
  std::vector<std::string> reported;
  while (reported.empty() && ticks < 40) {
    reported = cooldowns.tick();
    ++ticks;
  }
  CHECK_EQ(reported.size(), 1);
  if (!reported.empty())
    CHECK(reported[0] == short_group);
  CHECK_EQ(ticks, 10);

  // clear empties the map without touching the player's clock: the next range
  // still starts where the clock is.
  cooldowns.add(group, 20);
  cooldowns.clear();
  CHECK(cooldowns.empty());
  CHECK(!cooldowns.is_on_cooldown(group));
  const int clock = cooldowns.tick_count();
  cooldowns.add(group, 5);
  CHECK_NEAR(cooldowns.percent(group, 0.0f), 1.0f, 1e-6);
  CHECK_EQ(cooldowns.tick_count(), clock);
}

// The packet as bytes, and the id it is registered under.
void test_packet() {
  CHECK_EQ(mc::protocol::kPlayCooldownId, 0x16);
  const std::string group = "minecraft:chorus_fruit";
  const std::vector<uint8_t> bytes = mc::server::make_cooldown(group, 20);
  std::vector<uint8_t> expected = {0x16, static_cast<uint8_t>(group.size())};
  expected.insert(expected.end(), group.begin(), group.end());
  expected.push_back(0x14); // duration 20
  CHECK(bytes == expected);
  if (bytes != expected) {
    std::fprintf(stderr, "  got ");
    for (uint8_t b : bytes)
      std::fprintf(stderr, "%02x ", b);
    std::fprintf(stderr, "\n");
  }
  // The end-of-cooldown form: the same packet with a duration of 0. The client
  // reads it as "forget this group".
  const std::vector<uint8_t> zero = mc::server::make_cooldown(group, 0);
  CHECK_EQ(zero.size(), bytes.size());
  CHECK_EQ(zero.back(), 0);
}

} // namespace

int main() {
  test_table();
  test_cooldown_table();
  test_remainder_conversion();
  test_cooldowns();
  test_packet();
  if (failures == 0)
    std::printf("use_components: all checks passed\n");
  else
    std::printf("use_components: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
