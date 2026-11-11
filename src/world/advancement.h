// SPDX-License-Identifier: GPL-3.0-or-later
//
// Advancements (Phase 4 milestone N): the registry, the progress model and the
// tree visibility rules.
//
// Vanilla's `ServerAdvancementManager` / `PlayerAdvancements` /
// `AdvancementProgress` / `AdvancementVisibilityEvaluator`, ported as the pure
// model the session drives. The registry itself is *data* — every
// `advancement/**/*.json` in the 26.2 pack, compiled by
// tools/gen_advancement_table.py into `advancement_table.h`, 1,688 rows — so
// nothing here is per-advancement code:
//
//   * `AdvancementProgress` is one advancement's criteria and the requirements
//     test over them. A requirement *set* is OR (any member completes it) and
//     the sets are AND (`AdvancementRequirements.test`), which is why the
//     generated data carries sets, not a flat list.
//   * `PlayerAdvancements` is a player's progress map plus vanilla's three
//     dirty sets and the flushing rule: `flush` returns the `added` /
//     `removed` / `progress` the packet needs, and only the caller knows the
//     socket. `reset` is true exactly on the first packet that has anything to
//     say, which is what makes a rejoining client drop its stale tree.
//   * Visibility is `AdvancementVisibilityEvaluator` verbatim: a node with a
//     display is visible when it is done, when a descendant is done, or when
//     its parent or grandparent is done — and a node whose own display is
//     hidden (or missing) is only ever visible through the done test. That is
//     why a fresh player's screen is empty and why earning one advancement
//     reveals its branch.
//
// The trigger events are the subset of vanilla's ~90 criteria triggers this
// engine can actually see happen. `advancement_table.h` marks the criteria
// whose *conditions* it could not model with `kAdvConditionUnsupported`: they
// stay in the requirements (the client decodes them) but never match, so an
// advancement with one can only be completed through its other criteria.
//
// The model is also the persistence codec's other half: `saved_progress` and
// `apply_saved_progress` are vanilla's `asData` / `applyFrom`, and
// world/advancement_save.h writes the rows to `<world>/advancements.dat`.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/advancement_table.h"

namespace mc::world {

class Inventory;

// ---- the registry ----------------------------------------------------------

// The table row at `index` (0..advancement_count()-1), or nullptr.
const GeneratedAdvancement* advancement_at(int index);
int advancement_count();
// The row's index, or -1 for a pointer that is not from the table.
int advancement_index(const GeneratedAdvancement* advancement);

// `ServerAdvancementManager.get`: an id with or without its namespace and in
// any case resolves; nullptr when the advancement does not exist.
const GeneratedAdvancement* advancement_by_id(std::string_view id);

// The advancement's displayed children (a display-less node is hoisted away,
// exactly as `AdvancementNode`/`TreeNodePosition` skip it), in id order. This
// is what the visibility evaluator walks; the list is built once.
const std::vector<const GeneratedAdvancement*>&
advancement_children(const GeneratedAdvancement& parent);
// The node's tree root (an advancement with no parent), or nullptr for a row
// that is not in the table.
const GeneratedAdvancement* advancement_root(const GeneratedAdvancement& advancement);

// ---- one advancement's progress --------------------------------------------

// `CriterionProgress`: vanilla stores a nullable obtained Instant; the packet
// writes a nullable long. 0 means not obtained.
struct CriterionState {
  bool obtained = false;
  int64_t obtained_ms = 0;
};

// `CriterionProgress` as the file keeps it: only obtained criteria are rows
// (`AdvancementProgress.CODEC`'s criteria map), each with the instant it was
// obtained (the epoch, in milliseconds — this engine's Instant).
struct SavedCriterion {
  std::string criterion;
  int64_t obtained_ms = 0;
};

// One advancement's saved progress: `AdvancementProgress.CODEC`'s view of it,
// which is a row only while `has_progress()`.
struct SavedAdvancementProgress {
  std::string advancement;
  std::vector<SavedCriterion> criteria;
};

class AdvancementProgress {
public:
  // `AdvancementProgress.update`: create the criteria the requirements name
  // and drop any that are no longer referenced.
  void update(const GeneratedAdvancement& advancement);
  // `isDone` / `hasProgress` / the requirement counts.
  bool is_done() const;
  bool has_progress() const;
  int requirement_count() const {
    return requirement_sets_;
  }
  int completed_requirements() const;
  // True when the criterion moved (already-done grants are no-ops).
  bool grant(std::string_view criterion, int64_t obtained_ms);
  bool revoke(std::string_view criterion);
  bool criterion_done(std::string_view criterion) const;
  // Criteria in sorted order (the wire writes them in some order; a sorted map
  // keeps a test's bytes stable).
  const std::map<std::string, CriterionState>& criteria() const {
    return criteria_;
  }

private:
  const GeneratedAdvancement* advancement_ = nullptr;
  int requirement_sets_ = 0;
  std::map<std::string, CriterionState> criteria_;
};

// ---- a player's advancements -----------------------------------------------

class PlayerAdvancements {
public:
  // The trigger events. Each returns how many criteria it granted and leaves
  // the advancements completed by *this* call in `completed()`.
  std::size_t handle_inventory_changed(const Inventory& inventory, int32_t changed_item);
  std::size_t handle_consume_item(int32_t item);
  std::size_t handle_recipe_unlocked(std::string_view recipe);
  std::size_t handle_recipe_crafted(std::string_view recipe);
  std::size_t handle_player_killed_entity(std::string_view entity_type);
  std::size_t handle_entity_killed_player();
  std::size_t handle_bred_animals(std::string_view child, std::string_view parent_a,
                                  std::string_view parent_b);
  std::size_t handle_villager_trade();
  // `CriteriaTriggers.TICK`, fired once per player tick.
  std::size_t handle_tick();
  // `CriteriaTriggers.LOCATION`, fired every 20th player tick (`ServerPlayer.tick`'s
  // `if (this.tickCount % 20 == 0)`). It is a `PlayerTrigger`, so vanilla
  // evaluates its `EntityPredicate` against the player; the one field of that
  // predicate this engine evaluates is `LocationPredicate.biomes`, so the
  // session hands the handler the biome the player stands in and the matching
  // criteria are those whose generated `biomes` list holds it.
  std::size_t handle_location(std::string_view biome);
  // `CriteriaTriggers.ENTER_BLOCK`: `ServerPlayer.onInsideBlock`, which
  // `Entity.checkInsideBlocks` calls once per non-air block the entity's
  // bounding box overlaps (and once per such block position within a tick —
  // `visitedBlocks` dedupes those). `entered` is that set this tick, in the
  // world's own block names; a criterion matches when it names none of them
  // (`EnterBlockTrigger.matches`' absent-block branch) or when one of the blocks
  // it names is in the set.
  std::size_t handle_enter_block(std::span<const std::string_view> entered);
  // `CriteriaTriggers.EFFECTS_CHANGED`: `ServerPlayer.onEffectAdded` /
  // `onEffectUpdated` / `onEffectsRemoved`, each of which fires it with the set
  // as that change left it. `active` is the player's effect set at that point,
  // in the mob_effect registry's own names, and a criterion matches when the
  // `effects` map it names (`MobEffectsPredicate.matches` — *every* effect in
  // it) is entirely present. The trigger's other field, the `source` entity an
  // effect came from, has no counterpart in this engine's effect sinks, so a
  // criterion naming one never matches.
  std::size_t handle_effects_changed(std::span<const std::string_view> active);

  // `PlayerAdvancements.award` / `revoke`, the direct path a `/advancement`
  // command would take. `now_ms` 0 means "this instant".
  bool award(const GeneratedAdvancement& advancement, std::string_view criterion,
             int64_t now_ms = 0);
  bool revoke(const GeneratedAdvancement& advancement, std::string_view criterion);

  // The advancements whose completion crossed on the last event, in award
  // order — the session's announcement and reward list.
  const std::vector<const GeneratedAdvancement*>& completed() const {
    return completed_;
  }

  const AdvancementProgress* progress_of(const GeneratedAdvancement& advancement) const;
  bool is_done(const GeneratedAdvancement& advancement) const;
  bool is_visible(const GeneratedAdvancement& advancement) const;

  // What `flushDirty` decided to send. Empty `added` + `removed` + `progress`
  // means "nothing to say" and the caller sends no packet at all (vanilla's
  // own rule).
  struct Update {
    bool reset = false;
    bool show = true;
    std::vector<const GeneratedAdvancement*> added;
    std::vector<std::string> removed;
    std::vector<std::pair<const GeneratedAdvancement*, const AdvancementProgress*>> progress;
    bool empty() const {
      return added.empty() && removed.empty() && progress.empty();
    }
  };
  Update flush(bool show_advancements);

  // `ServerboundSeenAdvancementsPacket`: null tab = the screen closed. Returns
  // true when the selection changed (vanilla then sends
  // `select_advancements_tab`; the session owns that packet).
  bool set_selected_tab(const std::string* tab);
  // The tab currently selected (absent = the screen is closed), which the
  // session puts in `select_advancements_tab`.
  const std::optional<std::string>& selected_tab() const {
    return selected_tab_;
  }

  // ---- persistence ---------------------------------------------------------
  //
  // `asData`: the progress a save keeps — one row per advancement with any
  // criterion obtained, each holding only the criteria that were, in name
  // order, with rows in id order (a stable file). This is the model's half of
  // the store; world/advancement_save.h is the file's.
  std::vector<SavedAdvancementProgress> saved_progress() const;
  // `applyFrom`: adopt saved rows as if each listed criterion had been granted,
  // *without* announcing or rewarding anything — a reload restores progress, it
  // does not re-pay a completed advancement's rewards. The rows land in the
  // dirty sets and their roots are marked for the visibility walk, so the very
  // next `flush` sends the whole tree back (`reset`) with the progress rows in
  // it. Run on a fresh model, before the first flush; returns the number of
  // rows adopted. A row whose advancement is not in the table, or whose
  // criteria are all gone, is dropped (vanilla logs the first and keeps the
  // second as an empty entry).
  std::size_t apply_saved_progress(const std::vector<SavedAdvancementProgress>& saved);

private:
  // `PlayerAdvancements.getOrStartProgress`: the entry is created on demand.
  AdvancementProgress& progress_for(uint16_t index);
  void mark_for_visibility_update(uint16_t index);
  void update_tree_visibility(uint16_t root, std::set<uint16_t>* added,
                              std::set<uint16_t>* removed);
  // The evaluator. `rules` is vanilla's three-deep ascendant stack (self
  // pushed on entry, popped on exit); returns isSelfOrDescendantDone.
  bool evaluate_visibility(uint16_t index, std::vector<uint8_t>* rules, std::set<uint16_t>* added,
                           std::set<uint16_t>* removed);
  // How many criteria this event newly granted -- the count the public handlers
  // return. Not a bool: one event can name criteria in several advancements (a
  // trim's recipe completes a criterion in each trim advancement, a birth
  // satisfies both `breed_an_animal`'s `bred` and `bred_all_animals`' animal).
  std::size_t grant_matching(uint8_t trigger,
                             const std::function<bool(const GeneratedCriterion&)>& matches);

  std::unordered_map<uint16_t, AdvancementProgress> progress_;
  std::set<uint16_t> visible_;
  std::set<uint16_t> progress_changed_;
  std::set<uint16_t> roots_to_update_;
  std::vector<const GeneratedAdvancement*> completed_;
  bool first_packet_ = true;
  std::optional<std::string> selected_tab_;
};

} // namespace mc::world
