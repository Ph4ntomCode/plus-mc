// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the advancement model (Phase 4 milestone N):
//   * the generated registry: 1,688 advancements and 3,689 criteria from the
//     26.2 pack, 126 displays, and the table's own invariants (every
//     advancement's criteria run and requirement indices stay inside the
//     arrays — a regeneration that shifts a column fails here, not on a
//     client);
//   * one advancement read field by field: `story/root`'s parent, display,
//     criterion, item predicate and telemetry bit;
//   * `AdvancementProgress`: the requirements test (an OR set completes on any
//     member, an AND of sets needs every one), grant/revoke idempotence and
//     the obtained timestamp;
//   * the trigger events: an inventory change completes `story/mine_stone`
//     (its `#minecraft:stone_tool_materials` predicate was expanded at
//     generation), a tick completes the crafting table's recipe advancement,
//     a consume records one criterion of `husbandry/balanced_diet` without
//     completing it, and a kill does complete `adventure/kill_a_mob`;
//   * the three trigger families this milestone wired late — recipe_unlocked,
//     recipe_crafted and bred_animals — including each call's *count*: the
//     handlers promise "how many criteria it granted", so a multi-criterion
//     event (a trim's recipe names one in each trim advancement; a cow's birth
//     also matches the condition-less `bred`) must report two, not one;
//   * the `location` trigger: of the pack's 66 location criteria, 60 are a bare
//     `location.biomes` list and are resolved by the generator while the other
//     six (structures, or an equipment/stepping_on sibling) stay flagged; one
//     biome completes that biome's criterion of `adventuring_time` without
//     completing the AND-of-55 advancement, and walking every biome the
//     generated lists name does complete it;
//   * the `enter_block` trigger: the pack's eleven criteria all name one block
//     (ten `in_water` on the boat recipe advancements, `entered_end_gateway`),
//     entering one grants exactly the criteria that name it, and a block no
//     criterion names — or no block at all — grants none;
//   * the `effects_changed` trigger: `nether/all_potions`' 17 effects and
//     `nether/all_effects`' 34 are an "every one of them" test, so a set one
//     effect short of the list grants nothing and the set with all of them
//     completes the advancement; the trigger's third criterion,
//     `kill_axolotl_target`, names only a `source` entity and never matches;
//   * the visibility walk and the flush: earning `story/mine_stone` reveals
//     its branch (the root, itself and the two nodes within the two-generation
//     window) with `reset` set, and the next flush has nothing to say.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "world/advancement.h"
#include "world/inventory.h"
#include "world/item.h"

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

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    const std::string va = (a), vb = (b);                                                          \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a,      \
                   va.c_str(), vb.c_str());                                                        \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

int main() {
  // ---- the registry --------------------------------------------------------
  CHECK_EQ(advancement_count(), kAdvancementCount);
  CHECK_EQ(kAdvancementCount, 1688);
  CHECK_EQ(kAdvancementCriteriaCount, 3689);
  CHECK_EQ(kAdvancementDisplayCount, 126);
  CHECK(advancement_at(0) == &kAdvancements[0]);
  CHECK(advancement_at(kAdvancementCount) == nullptr);
  CHECK(advancement_at(-1) == nullptr);
  CHECK_EQ(advancement_index(&kAdvancements[7]), 7);
  CHECK_EQ(advancement_index(nullptr), -1);

  // Every row's pointers stay inside their arrays, and every requirement names
  // a criterion the advancement actually has.
  int displayed = 0;
  for (int i = 0; i < kAdvancementCount; ++i) {
    const GeneratedAdvancement& row = kAdvancements[i];
    CHECK(advancement_at(i) == &row);
    CHECK(row.criteria_first + row.criteria_count <= kAdvancementCriteriaCount);
    CHECK(row.requirement_sets_first + row.requirement_set_count <=
          kAdvancementRequirementSetCount);
    CHECK(row.requirement_criteria_first + row.criteria_count <=
          kAdvancementRequirementCriteriaCount);
    if (row.display != nullptr)
      ++displayed;
    for (int s = 0; s < row.requirement_set_count; ++s) {
      const uint8_t size = kAdvancementRequirementSizes[row.requirement_sets_first + s];
      for (uint8_t c = 0; c < size; ++c) {
        const uint8_t local = kAdvancementRequirementCriteria[row.requirement_criteria_first + c];
        CHECK(local < row.criteria_count);
      }
    }
  }
  CHECK_EQ(displayed, 126);

  // `story/root`, field by field.
  const GeneratedAdvancement* root = advancement_by_id("minecraft:story/root");
  CHECK(root != nullptr);
  if (root != nullptr) {
    CHECK(root->parent == nullptr);
    CHECK(root->display != nullptr);
    CHECK_STR(root->display->title, "advancements.story.root.title");
    CHECK_STR(root->display->description, "advancements.story.root.description");
    CHECK_STR(root->display->icon, "minecraft:grass_block");
    CHECK_EQ(root->display->frame, 0); // AdvancementType.TASK
    CHECK_STR(root->display->background, "minecraft:gui/advancements/backgrounds/stone");
    CHECK(!root->display->show_toast);
    CHECK(!root->display->announce_chat);
    CHECK(!root->display->hidden);
    CHECK_EQ(root->criteria_count, 1);
    CHECK_EQ(root->requirement_set_count, 1);
    const GeneratedCriterion& criterion = kAdvancementCriteria[root->criteria_first];
    CHECK_STR(criterion.name, "crafting_table");
    CHECK_EQ(criterion.trigger, kAdvTriggerInventoryChanged);
    CHECK_EQ(criterion.item_count, 1);
    CHECK_STR(criterion.items[0], "minecraft:crafting_table");
    CHECK_EQ(root->reward_recipe_count, 0);
    CHECK_EQ(root->reward_experience, 0);
    CHECK(root->sends_telemetry_event);
    CHECK(advancement_root(*root) == root);
  }
  // Lookup is namespace- and case-insensitive, and a miss is a miss.
  CHECK(advancement_by_id("story/root") == root);
  CHECK(advancement_by_id("STORY/ROOT") == root);
  CHECK(advancement_by_id("minecraft:not/real") == nullptr);
  CHECK(advancement_by_id("") == nullptr);

  const GeneratedAdvancement* mine_stone = advancement_by_id("minecraft:story/mine_stone");
  CHECK(mine_stone != nullptr);
  CHECK(root != nullptr && mine_stone != nullptr);
  if (root != nullptr && mine_stone != nullptr) {
    // The pack's story root has exactly one child, and the whole story tree
    // hangs off it.
    CHECK_EQ(advancement_children(*root).size(), 1u);
    CHECK(advancement_children(*root)[0] == mine_stone);
    CHECK(advancement_root(*mine_stone) == root);
    // `#minecraft:stone_tool_materials` expanded to its three items.
    const GeneratedCriterion& stone = kAdvancementCriteria[mine_stone->criteria_first];
    CHECK_STR(stone.name, "get_stone");
    CHECK_EQ(stone.trigger, kAdvTriggerInventoryChanged);
    CHECK_EQ(stone.item_count, 3);
    CHECK_STR(stone.items[0], "minecraft:blackstone");
    CHECK_STR(stone.items[1], "minecraft:cobbled_deepslate");
    CHECK_STR(stone.items[2], "minecraft:cobblestone");
  }

  // An unmodelled criterion is marked and never matches: 137 of the 3,689 —
  // the unmodelled trigger families plus the condition shapes the generator
  // dropped. The `location` family used to be the big one (66 criteria); its 60
  // biome ones are evaluated now (the location block below), the eleven
  // `enter_block` ones and `nether`'s two `effects_changed` ones are too, and
  // the rest (five location criteria naming `structures`, the
  // `equipment`/`stepping_on` sibling, `kill_axolotl_target`'s `source`, the
  // unmodelled trigger families) stay flagged.
  int unsupported = 0;
  for (int i = 0; i < kAdvancementCriteriaCount; ++i)
    if ((kAdvancementCriteria[i].flags & kAdvConditionUnsupported) != 0)
      ++unsupported;
  CHECK_EQ(unsupported, 137);

  // ---- progress and requirements ------------------------------------------
  {
    AdvancementProgress progress;
    progress.update(*mine_stone);
    CHECK(!progress.is_done());
    CHECK(!progress.has_progress());
    CHECK_EQ(progress.requirement_count(), 1);
    CHECK_EQ(progress.completed_requirements(), 0);
    CHECK(progress.grant("get_stone", 4242));
    CHECK(progress.is_done());
    CHECK(progress.has_progress());
    CHECK_EQ(progress.completed_requirements(), 1);
    CHECK(progress.criterion_done("get_stone"));
    CHECK_EQ(progress.criteria().at("get_stone").obtained_ms, 4242);
    CHECK(!progress.grant("get_stone", 1)); // already done: no second grant
    CHECK(!progress.grant("not_a_criterion", 1));
    CHECK(progress.revoke("get_stone"));
    CHECK(!progress.is_done());
    CHECK(!progress.revoke("get_stone"));
  }
  {
    // A recipe advancement's requirement set is OR over its two criteria, so
    // either one completes it.
    const GeneratedAdvancement* andesite =
        advancement_by_id("minecraft:recipes/building_blocks/andesite");
    CHECK(andesite != nullptr);
    if (andesite != nullptr) {
      CHECK_EQ(andesite->criteria_count, 2);
      CHECK_EQ(andesite->requirement_set_count, 1);
      CHECK_EQ(kAdvancementRequirementSizes[andesite->requirement_sets_first], 2);
      AdvancementProgress progress;
      progress.update(*andesite);
      CHECK(progress.grant("has_the_recipe", 7));
      CHECK(progress.is_done());
      // The reward recipe lives on the row, not the progress.
      CHECK_EQ(andesite->reward_recipe_count, 1);
      CHECK_STR(andesite->reward_recipes[0], "minecraft:andesite");
    }
  }

  // ---- the trigger events --------------------------------------------------
  Inventory inventory(kPlayerInventorySize);
  {
    PlayerAdvancements advancements;
    const std::optional<int32_t> cobblestone =
        ItemRegistry::instance().id_of("minecraft:cobblestone");
    CHECK(cobblestone.has_value());
    if (cobblestone.has_value()) {
      CHECK(advancements.handle_inventory_changed(inventory, *cobblestone) >= 1u);
      // The pickup also satisfies the `has_*` criteria of every recipe
      // advancement whose ingredients include cobblestone (the slab, the wall,
      // the stairs...), so the story advancement is one completion among many
      // display-less ones.
      bool completed_stone = false;
      for (const GeneratedAdvancement* advancement : advancements.completed())
        if (std::string(advancement->id) == "minecraft:story/mine_stone")
          completed_stone = true;
      CHECK(completed_stone);
      CHECK(advancements.is_done(*mine_stone));
      // Until the flush, nothing is visible.
      CHECK(!advancements.is_visible(*mine_stone));
    }
    // The flush: reset on the first packet that says anything, the branch the
    // completion revealed, and the progress of every visible node.
    PlayerAdvancements::Update update = advancements.flush(true);
    CHECK(update.reset);
    CHECK(update.show);
    CHECK(update.removed.empty());
    // The root and the two nodes within two generations of the done node are
    // visible; so are the done display-less recipe advancements (vanilla sends
    // them and the client keeps them hidden).
    const auto added = [&](const char* id) {
      for (const GeneratedAdvancement* advancement : update.added)
        if (std::string(advancement->id) == id)
          return true;
      return false;
    };
    CHECK(added("minecraft:story/mine_stone"));
    CHECK(added("minecraft:story/root"));
    CHECK(added("minecraft:story/smelt_iron"));
    CHECK(added("minecraft:story/upgrade_tools"));
    bool progress_stone = false;
    for (const auto& entry : update.progress) {
      CHECK(advancements.is_visible(*entry.first));
      if (std::string(entry.first->id) == "minecraft:story/mine_stone")
        progress_stone = true;
    }
    CHECK(progress_stone);
    // The second flush has nothing to say and no reset bit.
    PlayerAdvancements::Update again = advancements.flush(true);
    CHECK(again.empty());
    CHECK(!again.reset);
    // The visibility is real: the nodes are visible now, and an unrelated
    // branch (the nether root) is not.
    CHECK(advancements.is_visible(*mine_stone));
    CHECK(advancements.is_visible(*root));
    const GeneratedAdvancement* nether_root = advancement_by_id("minecraft:nether/root");
    CHECK(nether_root != nullptr);
    if (nether_root != nullptr)
      CHECK(!advancements.is_visible(*nether_root));
  }
  {
    // The tick trigger: the crafting table's recipe advancement unlocks on the
    // player's own timer (`unlock_right_away`), whose requirement set is OR
    // with the recipe's `has_the_recipe`.
    PlayerAdvancements advancements;
    const GeneratedAdvancement* table =
        advancement_by_id("minecraft:recipes/decorations/crafting_table");
    CHECK(table != nullptr);
    CHECK_EQ(advancements.handle_tick(), 1u);
    CHECK_EQ(advancements.completed().size(), 1u);
    if (table != nullptr) {
      CHECK_STR(advancements.completed()[0]->id, "minecraft:recipes/decorations/crafting_table");
      CHECK(advancements.is_done(*table));
      CHECK_EQ(table->reward_recipe_count, 1);
      CHECK_STR(table->reward_recipes[0], "minecraft:crafting_table");
    }
  }
  {
    // A consume drives two criteria: `husbandry/root`'s condition-less
    // `consumed_item` (which completes the root) and `balanced_diet`'s `apple`
    // (one of its 40 AND requirement sets, so the item alone does not finish
    // it).
    PlayerAdvancements advancements;
    const GeneratedAdvancement* diet = advancement_by_id("minecraft:husbandry/balanced_diet");
    const GeneratedAdvancement* husbandry_root = advancement_by_id("minecraft:husbandry/root");
    const std::optional<int32_t> apple = ItemRegistry::instance().id_of("minecraft:apple");
    CHECK(diet != nullptr && husbandry_root != nullptr && apple.has_value());
    if (diet != nullptr && husbandry_root != nullptr && apple.has_value()) {
      CHECK(advancements.handle_consume_item(*apple) >= 1u);
      bool completed_root = false;
      bool completed_diet = false;
      for (const GeneratedAdvancement* advancement : advancements.completed()) {
        if (std::string(advancement->id) == "minecraft:husbandry/root")
          completed_root = true;
        if (std::string(advancement->id) == "minecraft:husbandry/balanced_diet")
          completed_diet = true;
      }
      CHECK(completed_root);
      CHECK(!completed_diet);
      CHECK(advancements.is_done(*husbandry_root));
      const AdvancementProgress* progress = advancements.progress_of(*diet);
      CHECK(progress != nullptr);
      if (progress != nullptr) {
        CHECK(progress->criterion_done("apple"));
        CHECK(!progress->is_done());
        CHECK_EQ(progress->requirement_count(), 40);
        CHECK_EQ(progress->completed_requirements(), 1);
      }
    }
  }
  {
    // A kill: `kill_a_mob`'s criteria are the victim types (one OR set) and
    // `adventure/root`'s `killed_something` carries no predicate at all, so one
    // zombie kill completes both.
    PlayerAdvancements advancements;
    const GeneratedAdvancement* kill_a_mob = advancement_by_id("minecraft:adventure/kill_a_mob");
    const GeneratedAdvancement* adventure_root = advancement_by_id("minecraft:adventure/root");
    CHECK(kill_a_mob != nullptr && adventure_root != nullptr);
    if (kill_a_mob != nullptr && adventure_root != nullptr) {
      CHECK(advancements.handle_player_killed_entity("minecraft:zombie") >= 1u);
      bool completed_mob = false;
      for (const GeneratedAdvancement* advancement : advancements.completed())
        if (std::string(advancement->id) == "minecraft:adventure/kill_a_mob")
          completed_mob = true;
      CHECK(completed_mob);
      CHECK(advancements.is_done(*kill_a_mob));
      CHECK(advancements.is_done(*adventure_root));
      CHECK(advancements.completed().size() >= 2u);
    }
    // A victim type no criterion names still satisfies the root's
    // condition-less criterion, but not `kill_a_mob`.
    PlayerAdvancements other;
    CHECK(other.handle_player_killed_entity("minecraft:armor_stand") >= 1u);
    if (kill_a_mob != nullptr && adventure_root != nullptr) {
      CHECK(!other.is_done(*kill_a_mob));
      CHECK(other.is_done(*adventure_root));
    }
  }

  {
    // `minecraft:recipe_unlocked`: naming a recipe completes that recipe's own
    // `has_the_recipe` criterion — the recipe advancement tree's OR partner of
    // `unlock_right_away`. A recipe the pack has no advancement for still
    // matches nothing but the condition-less criteria, which there are none of.
    PlayerAdvancements advancements;
    // The count is criteria, and exactly one recipe_unlocked criterion names
    // `crafting_table` (generated table: line 9366 -- the tree's own `has_the_recipe`).
    CHECK_EQ(advancements.handle_recipe_unlocked("minecraft:crafting_table"), 1u);
    const GeneratedAdvancement* table =
        advancement_by_id("minecraft:recipes/decorations/crafting_table");
    CHECK(table != nullptr);
    if (table != nullptr)
      CHECK(advancements.is_done(*table));
    CHECK_EQ(advancements.handle_recipe_unlocked("minecraft:not_a_recipe"), 0u);
  }
  {
    // `minecraft:recipe_crafted`: a smithing trim's craft names that trim's own
    // criteria — one in each of the two trim advancements (`armor_trimmed_*`).
    // The exclusive-pattern one is an AND of eight, so the single craft leaves it
    // open; the any-pattern one is a single OR set of eighteen, so the craft
    // completes it. (`kAdvConditionUnsupported` is not set on either: a trim
    // names a concrete recipe key, which the generated table carries.)
    PlayerAdvancements advancements;
    // Two criteria name this trim recipe -- one in each trim advancement -- and
    // the event must report both: a count that stopped at 1 hid a `bool` where
    // the contract says "how many criteria it granted".
    CHECK_EQ(advancements.handle_recipe_crafted(
                 "minecraft:rib_armor_trim_smithing_template_smithing_trim"),
             2u);
    const GeneratedAdvancement* any_trim =
        advancement_by_id("minecraft:adventure/trim_with_any_armor_pattern");
    const GeneratedAdvancement* all_trims =
        advancement_by_id("minecraft:adventure/trim_with_all_exclusive_armor_patterns");
    CHECK(any_trim != nullptr && all_trims != nullptr);
    if (any_trim != nullptr && all_trims != nullptr) {
      CHECK(advancements.is_done(*any_trim));
      CHECK(!advancements.is_done(*all_trims));
      // Both advancements' own `..._rib_...` criteria moved, whatever the event
      // reports as its count.
      const AdvancementProgress* exclusive = advancements.progress_of(*all_trims);
      CHECK(exclusive != nullptr &&
            exclusive->criterion_done(
                "armor_trimmed_minecraft:rib_armor_trim_smithing_template_smithing_trim"));
    }
    // A recipe no trim criterion names matches nothing.
    PlayerAdvancements other;
    CHECK_EQ(other.handle_recipe_crafted("minecraft:oak_planks"), 0u);
  }
  {
    // `minecraft:bred_animals`: the child's type completes `breed_an_animal`
    // (its one condition-less `bred` criterion) and names one of
    // `bred_all_animals`' twenty-six per-animal criteria, which is an AND and so
    // stays open. The pair's *one* type fills both parent slots — `Animal.canMate`
    // compares classes, so the two parents of a birth share an entity type.
    PlayerAdvancements advancements;
    // Two criteria again: `bred_all_animals`' `minecraft:cow` and
    // `breed_an_animal`'s condition-less `bred`.
    CHECK_EQ(advancements.handle_bred_animals("minecraft:cow", "minecraft:cow", "minecraft:cow"),
             2u);
    const GeneratedAdvancement* breed = advancement_by_id("minecraft:husbandry/breed_an_animal");
    const GeneratedAdvancement* all_animals =
        advancement_by_id("minecraft:husbandry/bred_all_animals");
    CHECK(breed != nullptr && all_animals != nullptr);
    if (breed != nullptr && all_animals != nullptr) {
      CHECK(advancements.is_done(*breed));
      CHECK(!advancements.is_done(*all_animals));
      // The per-animal criterion the child matched did move, though.
      const AdvancementProgress* per_animal = advancements.progress_of(*all_animals);
      CHECK(per_animal != nullptr && per_animal->criterion_done("minecraft:cow"));
    }
    // A kind no criterion names still satisfies the condition-less `bred`.
    PlayerAdvancements other;
    // Only the condition-less `bred` names no kind, so a kind none of the 26
    // criteria names grants one criterion, not two.
    CHECK_EQ(other.handle_bred_animals("minecraft:armor_stand", "minecraft:armor_stand",
                                       "minecraft:armor_stand"),
             1u);
    if (breed != nullptr)
      CHECK(other.is_done(*breed));
    if (all_animals != nullptr)
      CHECK(!other.is_done(*all_animals));
  }

  // ---- the `location` trigger ----------------------------------------------
  {
    // The pack's 66 `location` criteria are all one shape: a `player`
    // `entity_properties` predicate whose own `minecraft:location` field carries
    // a `biomes` holder set. Sixty of them are exactly that (55
    // `adventuring_time` biomes plus the five nether ones) and the generator
    // resolves each to a concrete biome key; the other six name `structures`, or
    // a sibling `equipment`/`stepping_on` predicate this engine does not
    // evaluate, and stay `kAdvConditionUnsupported` — a criterion never matches
    // on a part of its predicate.
    int location_criteria = 0, location_supported = 0, location_flagged = 0;
    for (int i = 0; i < kAdvancementCriteriaCount; ++i) {
      const GeneratedCriterion& criterion = kAdvancementCriteria[i];
      if (criterion.biome_count != 0)
        CHECK(criterion.biomes != nullptr);
      if (criterion.trigger != kAdvTriggerLocation)
        continue;
      ++location_criteria;
      if ((criterion.flags & kAdvConditionUnsupported) != 0) {
        ++location_flagged;
        continue;
      }
      ++location_supported;
      CHECK(criterion.biomes != nullptr);
      CHECK(criterion.biome_count > 0);
    }
    CHECK_EQ(location_criteria, 66);
    CHECK_EQ(location_supported, 60);
    CHECK_EQ(location_flagged, 6);

    // One biome completes that biome's *own* criterion and not the advancement:
    // `adventuring_time`'s 55 requirement sets are an AND of one biome each, so
    // its progress counts one completed requirement and it stays open.
    const GeneratedAdvancement* adventuring =
        advancement_by_id("minecraft:adventure/adventuring_time");
    CHECK(adventuring != nullptr);
    PlayerAdvancements advancements;
    CHECK_EQ(advancements.handle_location("minecraft:badlands"), 1u);
    if (adventuring != nullptr) {
      CHECK(!advancements.is_done(*adventuring));
      const AdvancementProgress* progress = advancements.progress_of(*adventuring);
      CHECK(progress != nullptr && progress->criterion_done("minecraft:badlands"));
      CHECK_EQ(progress->completed_requirements(), 1);
    }
    // The same biome again is idempotent, and a biome no criterion names grants
    // nothing — including the empty name a session without a world hands it.
    CHECK_EQ(advancements.handle_location("minecraft:badlands"), 0u);
    CHECK_EQ(advancements.handle_location("minecraft:the_void"), 0u);
    CHECK_EQ(advancements.handle_location(""), 0u);

    // Walking every biome the criteria name completes the advancement: the
    // generated lists are the only input, so a biome the generator had dropped
    // would leave it open.
    if (adventuring != nullptr) {
      const GeneratedAdvancement& adv = *adventuring;
      PlayerAdvancements walker;
      std::size_t granted = 0;
      for (int c = 0; c < adv.criteria_count; ++c) {
        const GeneratedCriterion& criterion = kAdvancementCriteria[adv.criteria_first + c];
        CHECK_EQ(criterion.biome_count, 1); // one biome per criterion in the pack
        if (criterion.biome_count != 1)
          continue;
        granted += walker.handle_location(criterion.biomes[0]);
      }
      CHECK_EQ(granted, 55);
      CHECK(walker.is_done(adv));
    }

    // The nether five are the same shape with an AND of five — and the walk's
    // last one completes the advancement, which is what the session's pending
    // queue announces.
    const GeneratedAdvancement* explore_nether =
        advancement_by_id("minecraft:nether/explore_nether");
    CHECK(explore_nether != nullptr);
    PlayerAdvancements nether;
    CHECK_EQ(nether.handle_location("minecraft:nether_wastes"), 1u);
    CHECK_EQ(nether.handle_location("minecraft:soul_sand_valley"), 1u);
    CHECK_EQ(nether.handle_location("minecraft:crimson_forest"), 1u);
    CHECK_EQ(nether.handle_location("minecraft:warped_forest"), 1u);
    // Four of the five leaves the AND open, so nothing has completed yet.
    CHECK(nether.completed().empty());
    CHECK_EQ(nether.handle_location("minecraft:basalt_deltas"), 1u);
    if (explore_nether != nullptr) {
      CHECK(nether.is_done(*explore_nether));
      CHECK_EQ(nether.completed().size(), 1u);
      if (!nether.completed().empty())
        CHECK(nether.completed()[0] == explore_nether);
    }
  }

  // ---- the `enter_block` trigger -------------------------------------------
  {
    // The pack's eleven `enter_block` criteria all name one block and nothing
    // else: ten `in_water` criteria on the boat recipe advancements (each boat's
    // advancement has that one criterion) and `end/enter_end_gateway`'s
    // `entered_end_gateway`. The generator models no other shape, so every one
    // of them is earnable and none is flagged — before this slice the ten boat
    // advancements could not be completed at all.
    int enter_block_criteria = 0, enter_block_water = 0, enter_block_flagged = 0;
    for (int i = 0; i < kAdvancementCriteriaCount; ++i) {
      const GeneratedCriterion& criterion = kAdvancementCriteria[i];
      if (criterion.block_count != 0)
        CHECK(criterion.blocks != nullptr);
      if (criterion.trigger != kAdvTriggerEnterBlock)
        continue;
      ++enter_block_criteria;
      if ((criterion.flags & kAdvConditionUnsupported) != 0) {
        ++enter_block_flagged;
        continue;
      }
      CHECK_EQ(criterion.block_count, 1);
      if (criterion.block_count == 1 && std::string(criterion.blocks[0]) == "minecraft:water")
        ++enter_block_water;
    }
    CHECK_EQ(enter_block_criteria, 11);
    CHECK_EQ(enter_block_flagged, 0);
    CHECK_EQ(enter_block_water, 10);

    // Entering water grants exactly the criteria that name water — the ten boat
    // recipe advancements (each is one OR set of that one criterion) complete,
    // and the end gateway's does not.
    PlayerAdvancements advancements;
    const std::string_view water[] = {"minecraft:water"};
    CHECK_EQ(advancements.handle_enter_block(water), 10u);
    const GeneratedAdvancement* oak_boat =
        advancement_by_id("minecraft:recipes/transportation/oak_boat");
    const GeneratedAdvancement* gateway = advancement_by_id("minecraft:end/enter_end_gateway");
    CHECK(oak_boat != nullptr && gateway != nullptr);
    if (oak_boat != nullptr)
      CHECK(advancements.is_done(*oak_boat));
    if (gateway != nullptr)
      CHECK(!advancements.is_done(*gateway));
    // The same block again is idempotent, a block no criterion names grants
    // nothing, and neither does the empty set — the tick a box covers no
    // non-air block, which vanilla's own `state.isAir()` skip turns into no
    // `onInsideBlock` call at all.
    CHECK_EQ(advancements.handle_enter_block(water), 0u);
    const std::string_view stone[] = {"minecraft:stone", "minecraft:lava"};
    CHECK_EQ(advancements.handle_enter_block(stone), 0u);
    CHECK_EQ(advancements.handle_enter_block({}), 0u);
    // A tick's set can hold both: entering water and an end gateway at once
    // grants every criterion either block names.
    const std::string_view both[] = {"minecraft:water", "minecraft:end_gateway"};
    PlayerAdvancements pair;
    CHECK_EQ(pair.handle_enter_block(both), 11u);
    if (gateway != nullptr)
      CHECK(pair.is_done(*gateway));
  }

  // ---- the `effects_changed` trigger ---------------------------------------
  {
    int effects_criteria = 0, effects_flagged = 0;
    for (int i = 0; i < kAdvancementCriteriaCount; ++i) {
      const GeneratedCriterion& criterion = kAdvancementCriteria[i];
      if (criterion.effect_count != 0)
        CHECK(criterion.effects != nullptr);
      if (criterion.trigger != kAdvTriggerEffectsChanged)
        continue;
      ++effects_criteria;
      if ((criterion.flags & kAdvConditionUnsupported) != 0)
        ++effects_flagged;
    }
    CHECK_EQ(effects_criteria, 3);
    CHECK_EQ(effects_flagged, 1); // kill_axolotl_target, whose only field is `source`

    const GeneratedAdvancement* all_potions = advancement_by_id("minecraft:nether/all_potions");
    const GeneratedAdvancement* all_effects = advancement_by_id("minecraft:nether/all_effects");
    CHECK(all_potions != nullptr && all_effects != nullptr);
    if (all_potions != nullptr && all_effects != nullptr) {
      const GeneratedCriterion& potions = kAdvancementCriteria[all_potions->criteria_first];
      CHECK_EQ(potions.trigger, kAdvTriggerEffectsChanged);
      CHECK_EQ(potions.effect_count, 17);
      // `MobEffectsPredicate.matches` is an "every effect named" test, so a set
      // one short of the list matches nothing at all.
      std::vector<std::string_view> active;
      for (uint16_t i = 0; i + 1 < potions.effect_count; ++i)
        active.push_back(potions.effects[i]);
      PlayerAdvancements advancements;
      CHECK_EQ(advancements.handle_effects_changed(active), 0u);
      CHECK(!advancements.is_done(*all_potions));
      active.push_back(potions.effects[potions.effect_count - 1]);
      CHECK_EQ(advancements.handle_effects_changed(active), 1u);
      CHECK(advancements.is_done(*all_potions));
      CHECK_EQ(advancements.completed().size(), 1u);
      if (!advancements.completed().empty())
        CHECK(advancements.completed()[0] == all_potions);
      // The same set again grants nothing (the criterion is already obtained),
      // an empty set matches nothing, and these seventeen are not the
      // thirty-four `nether/all_effects` wants.
      CHECK_EQ(advancements.handle_effects_changed(active), 0u);
      CHECK_EQ(advancements.handle_effects_changed({}), 0u);
      CHECK(!advancements.is_done(*all_effects));

      // The whole thirty-four completes `nether/all_effects` — and, in the same
      // event, `nether/all_potions` too, because the seventeen that advancement
      // names are a subset of the thirty-four (vanilla has the same two
      // criteria on one event) — and still leaves `kill_axolotl_target` open:
      // its only condition is the `source` entity no sink in this engine
      // carries, so it is flagged and never matches.
      const GeneratedCriterion& everything = kAdvancementCriteria[all_effects->criteria_first];
      CHECK_EQ(everything.effect_count, 34);
      const std::vector<std::string_view> full(everything.effects,
                                               everything.effects + everything.effect_count);
      PlayerAdvancements walker;
      CHECK_EQ(walker.handle_effects_changed(full), 2u);
      CHECK(walker.is_done(*all_effects));
      CHECK(walker.is_done(*all_potions));
      const GeneratedAdvancement* axolotl =
          advancement_by_id("minecraft:husbandry/kill_axolotl_target");
      CHECK(axolotl != nullptr);
      if (axolotl != nullptr) {
        CHECK(!walker.is_done(*axolotl));
        CHECK((kAdvancementCriteria[axolotl->criteria_first].flags & kAdvConditionUnsupported) !=
              0);
      }
    }
  }

  // ---- the selected tab ----------------------------------------------------
  {
    PlayerAdvancements advancements;
    std::string tab = "minecraft:story/root";
    CHECK(advancements.set_selected_tab(&tab));
    CHECK(advancements.selected_tab().has_value());
    CHECK_STR(*advancements.selected_tab(), "minecraft:story/root");
    CHECK(!advancements.set_selected_tab(&tab)); // unchanged
    // A node that is not a displayed root selects nothing.
    std::string inner = "minecraft:story/mine_stone";
    CHECK(advancements.set_selected_tab(&inner));
    CHECK(!advancements.selected_tab().has_value());
    CHECK(!advancements.set_selected_tab(nullptr)); // already closed
    CHECK(advancements.set_selected_tab(&tab));
    CHECK(advancements.set_selected_tab(nullptr));
    CHECK(!advancements.selected_tab().has_value());
  }

  if (failures == 0)
    std::printf("advancement: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
