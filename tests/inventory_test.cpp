// SPDX-License-Identifier: GPL-3.0-or-later
//
// ItemStack / Inventory / slot-mapping unit tests.
//
// The wire part is the important half: an ItemStack's encoding
// (ItemStack.OPTIONAL_STREAM_CODEC) is
//
//     [varint count]  -> 0 means empty, and nothing follows
//     [varint item id]
//     [DataComponentPatch] = [varint added][varint removed][entries]
//
// Client-sent packets (set_creative_mode_slot, container_click) use the
// *delimited* patch (DELIMITED_STREAM_CODEC), where every component value is
// byte-length prefixed so unknown components can be skipped without desyncing
// the rest of the packet. A server that assumes the trusted (non-delimited)
// shape desyncs on the first stack that carries any component at all.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "world/inventory.h"
#include "world/item.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (ok)
    return;
  ++g_failures;
  std::printf("FAIL: %s\n", what.c_str());
}

std::string hex(const std::vector<uint8_t>& b) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) {
    s.push_back(k[c >> 4]);
    s.push_back(k[c & 0x0F]);
  }
  return s;
}

void expect_bytes(const char* what, const std::vector<uint8_t>& got,
                  const std::vector<uint8_t>& want) {
  if (got == want)
    return;
  ++g_failures;
  std::printf("FAIL: %s\n  got  %s\n  want %s\n", what, hex(got).c_str(), hex(want).c_str());
}

// Builds a *client-sent* (delimited) item stack by hand.
void put_varint(std::vector<uint8_t>* out, uint32_t v) {
  mc::net::encode_varint(v, *out);
}

} // namespace

int main() {
  using mc::world::Inventory;
  using mc::world::ItemRegistry;
  using mc::world::ItemStack;

  const ItemRegistry& items = ItemRegistry::instance();

  // --- Registry lookups -----------------------------------------------------
  check(items.id_of("stone").value_or(-1) == 1, "stone should be item id 1");
  check(items.id_of("minecraft:stone").value_or(-1) == 1, "minecraft:stone should resolve");
  check(items.id_of("MINECRAFT:Stone").value_or(-1) == 1, "names should normalize (case)");
  check(items.id_of(" minecraft:stone ").value_or(-1) == 1, "names should normalize (spaces)");
  check(!items.id_of("minecraft:not_a_real_item"), "unknown items must not resolve");
  check(!items.id_of("minecraft:stone;drop table"), "malformed identifiers must be rejected");
  check(items.max_stack_size(1) == 64, "stone stacks to 64");
  check(items.max_stack_size(1265) == 16, "ender_pearl stacks to 16");
  check(items.max_stack_size(1052) == 1, "diamond_pickaxe stacks to 1");
  check(items.is_damageable(1052), "diamond_pickaxe is damageable");
  check(!items.is_damageable(1), "stone is not damageable");
  check(items.is_block_item(1) && !items.is_block_item(974), "block flag: stone yes, stick no");

  // --- ItemStack arithmetic -------------------------------------------------
  ItemStack stone = ItemStack::of("minecraft:stone", 64);
  check(!stone.empty() && stone.count == 64, "stone x64");
  check(stone.grow(10) == 0, "a full stack accepts nothing");
  stone.shrink(4);
  check(stone.count == 60, "shrink(4) leaves 60");
  check(stone.grow(10) == 4, "grow(10) into 60/64 takes 4");
  check(stone.count == 64, "the stack refilled to 64/64");
  check(stone.shrink(64) == 64 && stone.empty(), "shrinking to zero empties the stack");
  check(stone.shrink(5) == 0, "shrinking an empty stack removes nothing");
  check(ItemStack{}.empty(), "a default stack is empty");

  ItemStack pearl = ItemStack::of("minecraft:ender_pearl", 16);
  check(!pearl.can_merge_with(pearl), "a full 16-stack cannot merge with another");
  pearl.shrink(1);
  check(pearl.can_merge_with(ItemStack::of("minecraft:ender_pearl", 1)), "partial stack merges");
  check(!pearl.can_merge_with(stone), "different items never merge");

  // Durability: the damage component caps at max_damage.
  ItemStack pick = ItemStack::of("minecraft:diamond_pickaxe", 1);
  check(pick.remaining_durability() == 1561, "a fresh pickaxe has full durability");
  check(pick.has_tool(), "a pickaxe carries a tool component");
  check(!ItemStack::of("minecraft:stone", 1).has_tool(), "stone is not a tool");
  pick.damage = 1561;
  check(pick.max_damaged() && pick.remaining_durability() == 0, "a fully damaged tool is spent");
  check(stone.remaining_durability() == -1, "non-damageable items have no durability");
  // damage_by() wears the item and destroys it at max damage (vanilla never
  // leaves a 0-durability stack behind).
  ItemStack wear = ItemStack::of("minecraft:diamond_shovel", 1);
  check(!wear.damage_by(1) && wear.damage == 1, "one use adds one damage");
  check(wear.damage_by(1560) && wear.empty(), "reaching max damage destroys the tool");
  ItemStack no_wear = ItemStack::of("minecraft:stone", 64);
  check(!no_wear.damage_by(10) && no_wear.count == 64, "non-damageable items do not wear");

  // --- Inventory ------------------------------------------------------------
  Inventory inv(mc::world::kPlayerInventorySize);
  check(inv.size() == 41, "the player inventory has 41 slots");
  // Vanilla's add() prefers the selected hotbar slot, then any stack with room,
  // then the first empty slot scanning from 0.
  check(inv.add(ItemStack::of("minecraft:stone", 10)) == 0, "10 stone fit");
  check(inv.get(0).count == 10, "the selected hotbar slot is filled first");
  check(inv.get(1).empty(), "nothing spills into the next slot when it all fits");
  // A second add merges into the same (partial) stack rather than taking a slot.
  check(inv.add(ItemStack::of("minecraft:stone", 5)) == 0, "5 more stone fit");
  check(inv.get(0).count == 15, "the second add merged into the selected slot");
  // A 200-item add must spread across slots and report the overflow.
  // 15 + 200 = 215 = 3x64 + 23 -> one more slot needed, and it fits.
  check(inv.add(ItemStack::of("minecraft:stone", 200)) == 0, "200 stone fit across slots");
  check(inv.get(0).count == 64, "the selected slot topped out at 64");
  check(inv.get(1).count == 64 && inv.get(2).count == 64 && inv.get(3).count == 23,
        "the overflow spread over the following slots");
  // Armour slots are reachable but only last: a 41-slot inventory must report
  // the true remainder once every slot is occupied.
  Inventory tiny(2);
  check(tiny.add(ItemStack::of("minecraft:stone", 200)) == 72, "two slots hold 128 of 200");
  Inventory small(1);
  check(small.add(ItemStack::of("minecraft:stone", 100)) == 36, "a 1-slot container holds 64");
  check(small.get(0).count == 64, "and it holds exactly one stack");

  // --- Slot numbering -------------------------------------------------------
  // Armour is head-first in the menu and feet-first in the inventory.
  check(mc::world::menu_slot_to_inventory_slot(5) == mc::world::kHeadSlot, "menu 5 -> head");
  check(mc::world::menu_slot_to_inventory_slot(8) == mc::world::kFeetSlot, "menu 8 -> feet");
  check(mc::world::inventory_slot_to_menu_slot(mc::world::kHeadSlot) == 5, "head -> menu 5");
  check(mc::world::inventory_slot_to_menu_slot(mc::world::kFeetSlot) == 8, "feet -> menu 8");
  // Main and hotbar map with a constant offset.
  check(mc::world::menu_slot_to_inventory_slot(9) == 9, "menu 9 -> inv 9");
  check(mc::world::menu_slot_to_inventory_slot(36) == 0, "menu 36 -> hotbar 0");
  check(mc::world::menu_slot_to_inventory_slot(44) == 8, "menu 44 -> hotbar 8");
  check(mc::world::menu_slot_to_inventory_slot(45) == mc::world::kOffhandSlot,
        "menu 45 -> offhand");
  // The crafting result and grid have no inventory slot behind them.
  check(mc::world::menu_slot_to_inventory_slot(0) == -1, "menu 0 (result) is not an inv slot");
  check(mc::world::menu_slot_to_inventory_slot(4) == -1, "menu 4 (craft grid) is not an inv slot");
  // Every inv<->menu pair the mapping claims must round-trip both ways, and
  // every menu slot outside the craft area must map to exactly one inv slot
  // (the vanilla InventoryMenu constants: ARMOR 5..8, INV 9..35, hotbar 36..44,
  // offhand 45).
  int mapped = 0;
  for (int menu = 1; menu <= 45; ++menu) {
    const int inv = mc::world::menu_slot_to_inventory_slot(menu);
    if (inv < 0)
      continue;
    ++mapped;
    check(mc::world::inventory_slot_to_menu_slot(inv) == menu,
          "menu slot " + std::to_string(menu) + " must round-trip through inv slot " +
              std::to_string(inv));
  }
  check(mapped == 41, "41 of the 45 menu slots have a backing inventory slot");
  check(mc::world::menu_slot_to_inventory_slot(mc::world::kMenuOffhandSlot) ==
            mc::world::kOffhandSlot,
        "menu 45 is the offhand slot");

  // --- Wire encoding --------------------------------------------------------
  {
    mc::net::Writer w;
    mc::world::write_item_stack(w, ItemStack::of("minecraft:stone", 3));
    // count 3, item 1, empty patch (0 added, 0 removed).
    expect_bytes("stone x3", std::move(w).take(), {0x03, 0x01, 0x00, 0x00});
  }
  {
    mc::net::Writer w;
    mc::world::write_item_stack(w, ItemStack{}); // empty: count 0 and nothing else
    expect_bytes("empty stack", std::move(w).take(), {0x00});
  }
  {
    ItemStack used = ItemStack::of("minecraft:diamond_pickaxe", 1);
    used.damage = 5;
    mc::net::Writer w;
    mc::world::write_item_stack(w, used);
    // count 1, item 1052 (varint 9c 08), patch added=1 removed=0,
    // component 3 (damage) = 5.
    expect_bytes("damaged pickaxe", std::move(w).take(),
                 {0x01, 0x9C, 0x08, 0x01, 0x00, 0x03, 0x05});
  }

  // Reading the *delimited* client form: keep an unknown component opaque,
  // keep the damage one, and land exactly on the end of the stack.
  {
    std::vector<uint8_t> body;
    put_varint(&body, 1);    // count
    put_varint(&body, 1052); // diamond_pickaxe
    put_varint(&body, 2);    // added
    put_varint(&body, 0);    // removed
    // Unknown component (type 99) with a 4-byte payload -> kept verbatim.
    put_varint(&body, 99);
    put_varint(&body, 4);
    body.insert(body.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    // damage (type 3), length-prefixed value.
    put_varint(&body, 3);
    put_varint(&body, 1);
    put_varint(&body, 7);
    // A trailing marker stands in for the rest of the packet.
    const uint8_t kMarker = 0xAA;
    body.push_back(kMarker);

    mc::net::Reader r(body);
    ItemStack stack;
    check(mc::world::read_item_stack(r, &stack), "delimited stack should parse");
    check(stack.item == 1052 && stack.count == 1, "delimited stack keeps its item and count");
    check(stack.damage == 7, "the damage component should be read");
    check(stack.raw_components.size() == 1, "the unknown component should be kept opaque");
    if (stack.raw_components.size() == 1) {
      check(stack.raw_components[0].type == 99, "the opaque component keeps its registry id");
      check(stack.raw_components[0].value == std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}),
            "the opaque component keeps its exact value bytes");
    }
    uint8_t marker = 0;
    check(r.u8(&marker) && marker == kMarker,
          "the reader must land exactly after the stack (opaque component preserved)");

    // The server echo is the clientbound (undelimited) form: the same opaque
    // value goes back out after the modelled damage component, and the leading
    // added count includes it.
    mc::net::Writer echo;
    mc::world::write_item_stack(echo, stack);
    expect_bytes("opaque component echoed", std::move(echo).take(),
                 {0x01, 0x9C, 0x08, 0x02, 0x00, 0x03, 0x07, 0x63, 0xDE, 0xAD, 0xBE, 0xEF});
  }

  // A component that lies about its length (a damage value claiming zero
  // bytes) must not desync the reader: the value is read as an empty slice, so
  // whatever follows the stack is still read from the right place.
  {
    std::vector<uint8_t> body;
    put_varint(&body, 1);    // count
    put_varint(&body, 1052); // diamond_pickaxe
    put_varint(&body, 1);    // added
    put_varint(&body, 0);    // removed
    put_varint(&body, 3);    // damage ...
    put_varint(&body, 0);    // ... with an empty (lying) value
    const uint8_t kMarker = 0xBB;
    body.push_back(kMarker);

    mc::net::Reader r(body);
    ItemStack stack;
    check(mc::world::read_item_stack(r, &stack), "a zero-length component should still parse");
    check(stack.item == 1052 && stack.count == 1, "the stack keeps its item and count");
    check(stack.damage == 0, "an empty damage value leaves damage at 0");
    uint8_t marker = 0;
    check(r.u8(&marker) && marker == kMarker,
          "a lying component length must not desync the reader");
  }

  // A removed-component entry is id-only, with no value.
  {
    std::vector<uint8_t> body;
    put_varint(&body, 4);
    put_varint(&body, 1); // stone
    put_varint(&body, 0); // added
    put_varint(&body, 1); // removed
    put_varint(&body, 3); // damage removed
    mc::net::Reader r(body);
    ItemStack stack;
    check(mc::world::read_item_stack(r, &stack), "removed component should parse");
    check(stack.item == 1 && stack.count == 4, "removed component keeps item/count");
    check(r.at_end(), "a removed entry consumes exactly one varint");
  }

  // Truncated input must fail rather than guess.
  {
    std::vector<uint8_t> body = {0x04, 0x01, 0x01}; // promises an added entry, none follows
    mc::net::Reader r(body);
    ItemStack stack;
    check(!mc::world::read_item_stack(r, &stack), "truncated component must fail to parse");
  }

  if (g_failures == 0) {
    std::printf("PASS: inventory\t(ItemStack, containers, slot numbering, wire codec)\n");
    return 0;
  }
  std::printf("%d inventory check(s) failed\n", g_failures);
  return 1;
}
