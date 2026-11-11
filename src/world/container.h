// SPDX-License-Identifier: GPL-3.0-or-later
//
// Block-backed containers (Phase 3, milestone C): the container a block hosts,
// its size, and the menu the client must be told to draw for it.
//
// The menu id is the wire id of the `minecraft:menu` registry entry. Unlike the
// item/block/entity-type registries that registry is **not** synchronised with
// the client (it is absent from the configuration registry payload we send), so
// its ids are the static registration order in `MenuType` — the same rule that
// reproduces the two entity-type ids already verified byte-for-byte on the wire
// (`minecraft:player` = 156, `minecraft:item` = 71).
//
// Persistence rides on the chunk's verbatim `block_entities` list (see
// world/chunk.h). The only fields the server reads or writes are the item list
// ("Items"), which vanilla stores as `ContainerHelper.saveAllItems` ->
// `ItemStackWithSlot.CODEC`: `[{Slot: byte, id: string, count: int,
// components: {...}}]`. Every component an ItemStack models is carried in
// vanilla's own NBT shape: `minecraft:damage` (int), `minecraft:enchantments`
// (the map of enchantment id -> level), `minecraft:potion_contents` (with its
// custom effects and their hidden-effect chains) and `minecraft:trim`
// ({material, pattern}). Each is written by resource location where vanilla
// names a registry entry, so a record written by vanilla reads back onto the
// same registry ids (and vice versa). A component the engine does not model is
// still dropped. Everything else in the compound is passed through untouched.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/inventory.h"
#include "world/nbt.h"

namespace mc::world {

// How a container block presents itself to a viewer.
struct ContainerSpec {
  int slots = 0;          // container slots of *one* block (not the player inventory)
  int menu_id = 0;        // wire id of the MENU registry entry for open_screen
  int double_menu_id = 0; // menu id when two of these join (0 = never joins)
  const char* title;      // default window title
  // The furnace family: the three blocks whose slots are input/fuel/output and
  // whose block entity *smelts*. `cooking.h` owns the tick; this flag is what
  // tells a persisted record to carry the timers and which menus have data
  // slots (milestone E).
  bool cooking = false;
  // The brewing stand: five slots (three bottles, an ingredient, a fuel) whose
  // block entity brews on a 400-tick timer. `brewing.h` owns the tick; like
  // `cooking` this is what makes a persisted record carry its timers and the
  // menu carry its data slots.
  bool brewing = false;
};

// A cooking container's live state: exactly the fields
// AbstractFurnaceBlockEntity keeps beyond its three slots, under vanilla's own
// NBT names. Only `cooking` containers use it.
struct FurnaceState {
  // `lit_time_remaining` / `lit_total_time`: fuel ticks left, and the total the
  // current fuel unit started with (the client's flame-sprite denominator).
  int lit_time_remaining = 0;
  int lit_total_time = 0;
  // `cooking_time_spent` / `cooking_total_time`: the progress bar.
  int cooking_timer = 0;
  int cooking_total_time = 0;
  // `RecipesUsed`: recipe file name -> crafts, cleared when the output is taken
  // (awardUsedRecipesAndPopExperience). Kept per recipe so a chest of output
  // still pays each recipe's own experience.
  std::map<std::string, int> recipes_used;
  // Not persisted by vanilla. It is how the tick notices that the input became
  // a *different item*, which is the write that re-derives the total cook time
  // (`setItem` slot 0). Vanilla does it on the write; a session writes slots
  // directly, so the tick does it before cooking. Same trigger, same result.
  int32_t last_input_item = kAirItemId;
};

// A brewing stand's live state: exactly the fields BrewingStandBlockEntity
// keeps beyond its five slots, under vanilla's own NBT names (`BrewTime`, a
// Short, and `Fuel`, a Byte). Only `brewing` containers use it.
struct BrewingState {
  // `BrewTime`: ticks left in the current brew, 0 when idle. Counts down from
  // kBrewingTimeTicks (400) and is the menu's first data slot.
  int brew_time = 0;
  // `Fuel`: brew *uses* left, not ticks. One unit of the brewing fuel tag is
  // worth kBrewingFuelUses (20) and one use is spent to start each brew. The
  // menu's second data slot.
  int fuel = 0;
  // The item the running brew started from (`BrewingStandBlockEntity.ingredient`).
  // Vanilla stores it in a field rather than in NBT: it re-derives it from the
  // ingredient slot on load, and only ever compares it to notice that the
  // ingredient was swapped mid-brew (which cancels the brew).
  int32_t ingredient_item = kAirItemId;
  // The `has_bottle_0..2` block properties the block entity last wrote, so the
  // tick only rewrites the block state when a bottle slot gained or lost a
  // stack. Vanilla starts this at null and therefore writes on the first tick;
  // `known` is that null.
  bool bottles_known = false;
  bool bottles[3] = {false, false, false};
};

// The spec for a block id, or nullopt when the block does not host a
// container. Covers the containers whose whole behaviour is "N slots in a
// grid" today; the furnace family is listed so it opens rather than places,
// even though its smelting tick arrives with milestone E.
std::optional<ContainerSpec> container_spec_for_block(std::string_view block);

// A block position in absolute world coordinates, ordered for std::map.
struct BlockPosKey {
  int x = 0;
  int y = 0;
  int z = 0;

  bool operator<(const BlockPosKey& o) const {
    if (x != o.x)
      return x < o.x;
    if (y != o.y)
      return y < o.y;
    return z < o.z;
  }
  bool operator==(const BlockPosKey&) const = default;
};

// True for the blocks that join into a double block (vanilla only has the
// chest family today): same name, same facing, both single -> one 9x6 menu.
bool is_double_block(std::string_view block);

// The position of the other half of a chest whose *state properties* say
// `type=left|right`, or nullopt for `type=single`/an unknown property set.
// Vanilla's ChestBlock.getConnectedDirection: LEFT -> facing clockwise,
// RIGHT -> facing counter-clockwise.
std::optional<BlockPosKey> chest_partner_pos(const BlockPosKey& pos, std::string_view props);

// True when `props` report the given chest type ("single", "left", "right").
bool chest_type_is(std::string_view props, std::string_view type);

// The `facing=` value of a chest state's properties ("" when absent).
std::string chest_facing(std::string_view props);

// The `waterlogged=` value of a state's properties (false when absent).
bool chest_waterlogged(std::string_view props);

// The state properties of a chest with the given canonical facing and type,
// e.g. chest_props("north", "left", false) -> "facing=north,type=left,waterlogged=false".
std::string chest_props(std::string_view facing, std::string_view type, bool waterlogged);

// The value of `key` in a canonical "a=1,b=2" properties string, or an empty
// string when the property is absent. The reader `with_property`'s comment
// describes below, for the callers that only want to *ask* (is this campfire
// lit, is this block waterlogged).
std::string block_property(std::string_view props, std::string_view key);

// `props` with `key` set to `value`, added when absent, kept in the canonical
// key-sorted form the block-state table stores (so the result looks up). This
// is how the world tick flips a furnace's `lit` property while leaving its
// facing alone.
std::string with_property(std::string_view props, std::string_view key, std::string_view value);

// --- placement geometry (vanilla's Direction helpers) -----------------------

// The horizontal direction a yaw points at (Direction.fromYRot): "south" at 0,
// "west" at 90, "north" at 180, "east" at 270.
std::string horizontal_direction(float yaw);

// The facing a horizontal block gets when placed by a player looking along
// `yaw` (BlockPlaceContext.getHorizontalDirection().getOpposite(): a block
// faces the player who placed it).
std::string placement_facing(float yaw);

// Direction.getClockWise / getCounterClockWise for the four horizontals
// (NORTH -> EAST -> SOUTH -> WEST).
std::string clockwise_direction(std::string_view facing);
std::string counter_clockwise_direction(std::string_view facing);

// The (dx, dz) step of a horizontal direction name (0, 0 for anything else).
void horizontal_step(std::string_view facing, int* dx, int* dz);

// The live contents of a block-backed container. Shared by every viewer of the
// position, which is what makes two players watching the same chest see the
// same screen.
//
// A double chest is *one* container spanning two blocks (`parts`), exactly like
// vanilla's CompoundContainer over the two block entities: slots
// [0, spec.slots) live in parts[0] and the next spec.slots in parts[1]. Both
// positions share this object, so either half opens the same screen, and
// breaking one half keeps the other half's items.
struct BlockContainer {
  std::string block; // "minecraft:chest"
  ContainerSpec spec;
  Inventory inventory;
  std::vector<BlockPosKey> parts; // 1, or 2 for a double chest (menu order)
  // The furnace family's counters. Unused (and never persisted) unless
  // spec.cooking; see cooking.h for the tick that advances them.
  FurnaceState furnace;
  // The brewing stand's counters, likewise unused unless spec.brewing; see
  // brewing.h for the tick that advances them.
  BrewingState brewing;

  BlockContainer(std::string block_id, ContainerSpec s, std::vector<BlockPosKey> p)
      : block(std::move(block_id)), spec(s), inventory(s.slots * static_cast<int>(p.size())),
        parts(std::move(p)) {}

  // True for the three blocks whose block entity smelts (a furnace, blast
  // furnace or smoker).
  bool is_cooking() const {
    return spec.cooking;
  }
  // True for the block whose block entity brews (a brewing stand).
  bool is_brewing() const {
    return spec.brewing;
  }
  // True for a container whose block entity has a tick of its own and counters
  // that persist with it — the furnace family and the brewing stand, which is
  // what the world tick walks the live containers for.
  bool is_timed() const {
    return spec.cooking || spec.brewing;
  }

  int total_slots() const {
    return spec.slots * static_cast<int>(parts.size());
  }
  int menu_id() const {
    return parts.size() > 1 ? spec.double_menu_id : spec.menu_id;
  }
  bool is_double() const {
    return parts.size() > 1;
  }
  // Index of `pos` in parts, or -1 when this position is not part of it.
  int part_index(const BlockPosKey& pos) const {
    for (size_t i = 0; i < parts.size(); ++i)
      if (parts[i] == pos)
        return static_cast<int>(i);
    return -1;
  }
  // The stacks of one part, in its own slot numbering (0..spec.slots-1).
  std::vector<ItemStack> part_items(int index) const;
};

// --- block-entity NBT (ContainerHelper / ItemStackWithSlot, 26.2) -----------

// True when the compound's x/y/z fields (all optional, defaulting to 0, like
// vanilla's `getIntOr`) name this position.
bool block_entity_at_pos(const nbt::Value& be, int x, int y, int z);

// True when the compound names this *absolute* world position. Vanilla stores
// absolute world coordinates in a chunk's block_entity records, and so do we;
// the chunk-local x/z form this server wrote before the coordinate fix is also
// accepted, so a world already saved by an older build keeps its container
// contents. `y` is absolute in both forms.
bool block_entity_matches(const nbt::Value& be, int x, int y, int z);

// Reads the "Items" list of a block-entity compound into a `size`-slot vector.
// Entries that are malformed, out of range, or name an item the registry does
// not know are skipped (the stack stays empty) rather than failing the load.
std::vector<ItemStack> read_block_entity_items(const nbt::Value& be, int size);

// Replaces (or adds) the "Items" list of `be`, writing one entry per non-empty
// slot in the layout above.
void write_block_entity_items(nbt::Value* be, const std::vector<ItemStack>& items);

// The compound vanilla writes for a container block entity: id, x, y, z and
// the Items list.
nbt::Value make_container_block_entity(std::string_view id, const BlockPosKey& pos,
                                       const std::vector<ItemStack>& items);

// --- cooking block-entity NBT (AbstractFurnaceBlockEntity, 26.2) -------------
//
// The four timers are `cooking_time_spent`, `cooking_total_time`,
// `lit_time_remaining` and `lit_total_time`, all Shorts (fields the item list
// does not carry), plus `RecipesUsed`, a compound of recipe resource path ->
// Int crafts. A record with no timers reads as a cold furnace, which is what a
// hand-written or vanilla furnace without them means too.
void read_block_entity_furnace(const nbt::Value& be, FurnaceState* state);

// Replaces (or adds) those fields on a furnace record, leaving everything else
// (a custom name, a lock, ...) untouched.
void write_block_entity_furnace(nbt::Value* be, const FurnaceState& state);

// --- brewing block-entity NBT (BrewingStandBlockEntity, 26.2) ----------------
//
// `BrewTime` (Short) and `Fuel` (Byte). A record without them reads as an idle,
// unlit stand, which is what a hand-written record means too. The ingredient
// the brew started from is not a persisted field in vanilla either: it is
// re-derived from slot 3 when `BrewTime > 0`, and a stand with no ingredient
// left under a running brew simply cancels it on the next tick.
void read_block_entity_brewing(const nbt::Value& be, BrewingState* state);

// Replaces (or adds) those two fields on a brewing-stand record.
void write_block_entity_brewing(nbt::Value* be, const BrewingState& state);

} // namespace mc::world
