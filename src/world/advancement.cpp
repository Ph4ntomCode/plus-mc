// SPDX-License-Identifier: GPL-3.0-or-later
//
// The advancement model (Phase 4 milestone N). See advancement.h for what is
// ported and why; this file is the mechanics: the registry lookups over the
// generated table, the requirements test, the trigger-event matching and the
// visibility walk.
#include "world/advancement.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "world/inventory.h"
#include "world/item.h"

namespace mc::world {
namespace {

// `AdvancementVisibilityEvaluator.VisibilityRule`.
constexpr uint8_t kRuleNoChange = 0;
constexpr uint8_t kRuleShow = 1;
constexpr uint8_t kRuleHide = 2;

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// "Stone" / "minecraft:stone" -> "minecraft:stone"; "" for anything that is not
// a plain identifier. The same normalizer `/give` uses for item names, kept
// local because entity types and recipe keys go through it too.
std::string normalize_id(std::string_view raw) {
  if (raw.empty())
    return {};
  std::string out;
  out.reserve(raw.size() + 10);
  for (char c : raw) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte == ' ')
      return {};
    out.push_back(static_cast<char>(std::tolower(byte)));
  }
  if (out.find(':') == std::string::npos)
    out.insert(0, "minecraft:");
  return out;
}

// The rows are sorted by id, so the lookup is the same one vanilla's registry
// does (a map, here a binary search over the flat table).
bool id_less(const GeneratedAdvancement& row, std::string_view id) {
  return std::string_view(row.id) < id;
}

// The advancement a global criterion index belongs to. The generator emits
// each advancement's criteria as one contiguous run in table order, so the
// owner is a binary search over the run starts.
uint16_t criterion_owner(uint16_t criterion) {
  static const std::vector<uint16_t> owners = [] {
    std::vector<uint16_t> out(kAdvancementCriteriaCount);
    for (int i = 0; i < kAdvancementCount; ++i) {
      const GeneratedAdvancement& row = kAdvancements[i];
      for (int c = 0; c < row.criteria_count; ++c)
        out[row.criteria_first + c] = static_cast<uint16_t>(i);
    }
    return out;
  }();
  return owners[criterion];
}

const std::vector<std::vector<const GeneratedAdvancement*>>& children_table() {
  // `AdvancementTree`: every node keeps its children, display-less ones
  // included — the visibility evaluator walks them and hoists nothing (only
  // TreeNodePosition's *layout* hoists).
  static const std::vector<std::vector<const GeneratedAdvancement*>> table = [] {
    std::vector<std::vector<const GeneratedAdvancement*>> out(kAdvancementCount);
    for (int i = 0; i < kAdvancementCount; ++i) {
      const GeneratedAdvancement& row = kAdvancements[i];
      if (row.parent == nullptr)
        continue;
      const GeneratedAdvancement* parent = advancement_by_id(row.parent);
      if (parent != nullptr)
        out[advancement_index(parent)].push_back(&row);
    }
    return out;
  }();
  return table;
}

} // namespace

// ---- the registry ----------------------------------------------------------

const GeneratedAdvancement* advancement_at(int index) {
  if (index < 0 || index >= kAdvancementCount)
    return nullptr;
  return &kAdvancements[index];
}

int advancement_count() {
  return kAdvancementCount;
}

int advancement_index(const GeneratedAdvancement* advancement) {
  if (advancement == nullptr || advancement < kAdvancements ||
      advancement >= kAdvancements + kAdvancementCount)
    return -1;
  return static_cast<int>(advancement - kAdvancements);
}

const GeneratedAdvancement* advancement_by_id(std::string_view id) {
  const std::string normalized = normalize_id(id);
  if (normalized.empty())
    return nullptr;
  const auto* first = kAdvancements;
  const auto* last = kAdvancements + kAdvancementCount;
  const auto* it = std::lower_bound(
      first, last, normalized,
      [](const GeneratedAdvancement& row, const std::string& key) { return id_less(row, key); });
  if (it == last || normalized != it->id)
    return nullptr;
  return it;
}

const std::vector<const GeneratedAdvancement*>&
advancement_children(const GeneratedAdvancement& parent) {
  static const std::vector<const GeneratedAdvancement*> none;
  const int index = advancement_index(&parent);
  if (index < 0)
    return none;
  return children_table()[index];
}

const GeneratedAdvancement* advancement_root(const GeneratedAdvancement& advancement) {
  const GeneratedAdvancement* node = &advancement;
  while (node->parent != nullptr) {
    const GeneratedAdvancement* parent = advancement_by_id(node->parent);
    if (parent == nullptr)
      break;
    node = parent;
  }
  return node;
}

// ---- one advancement's progress --------------------------------------------

void AdvancementProgress::update(const GeneratedAdvancement& advancement) {
  advancement_ = &advancement;
  requirement_sets_ = advancement.requirement_set_count;
  std::map<std::string, CriterionState> next;
  uint16_t cursor = advancement.requirement_criteria_first;
  for (int set = 0; set < advancement.requirement_set_count; ++set) {
    const uint8_t size = kAdvancementRequirementSizes[advancement.requirement_sets_first + set];
    for (uint8_t i = 0; i < size; ++i) {
      const uint8_t local = kAdvancementRequirementCriteria[cursor++];
      const char* name = kAdvancementCriteria[advancement.criteria_first + local].name;
      const auto found = criteria_.find(name);
      next.emplace(name, found == criteria_.end() ? CriterionState{} : found->second);
    }
  }
  criteria_ = std::move(next);
}

bool AdvancementProgress::is_done() const {
  if (advancement_ == nullptr || requirement_sets_ == 0)
    return false;
  uint16_t cursor = advancement_->requirement_criteria_first;
  for (int set = 0; set < requirement_sets_; ++set) {
    const uint8_t size = kAdvancementRequirementSizes[advancement_->requirement_sets_first + set];
    bool any = false;
    for (uint8_t i = 0; i < size; ++i) {
      const uint8_t local = kAdvancementRequirementCriteria[cursor++];
      const char* name = kAdvancementCriteria[advancement_->criteria_first + local].name;
      const auto found = criteria_.find(name);
      if (found != criteria_.end() && found->second.obtained) {
        any = true;
        break;
      }
    }
    if (!any)
      return false;
  }
  return true;
}

bool AdvancementProgress::has_progress() const {
  for (const auto& [name, state] : criteria_)
    if (state.obtained)
      return true;
  return false;
}

int AdvancementProgress::completed_requirements() const {
  if (advancement_ == nullptr)
    return 0;
  int count = 0;
  uint16_t cursor = advancement_->requirement_criteria_first;
  for (int set = 0; set < requirement_sets_; ++set) {
    const uint8_t size = kAdvancementRequirementSizes[advancement_->requirement_sets_first + set];
    for (uint8_t i = 0; i < size; ++i) {
      const uint8_t local = kAdvancementRequirementCriteria[cursor++];
      const char* name = kAdvancementCriteria[advancement_->criteria_first + local].name;
      const auto found = criteria_.find(name);
      if (found != criteria_.end() && found->second.obtained) {
        ++count;
        break;
      }
    }
  }
  return count;
}

bool AdvancementProgress::grant(std::string_view criterion, int64_t obtained_ms) {
  const auto found = criteria_.find(std::string(criterion));
  if (found == criteria_.end() || found->second.obtained)
    return false;
  found->second.obtained = true;
  found->second.obtained_ms = obtained_ms;
  return true;
}

bool AdvancementProgress::revoke(std::string_view criterion) {
  const auto found = criteria_.find(std::string(criterion));
  if (found == criteria_.end() || !found->second.obtained)
    return false;
  found->second.obtained = false;
  found->second.obtained_ms = 0;
  return true;
}

bool AdvancementProgress::criterion_done(std::string_view criterion) const {
  const auto found = criteria_.find(std::string(criterion));
  return found != criteria_.end() && found->second.obtained;
}

// ---- a player's advancements -----------------------------------------------

AdvancementProgress& PlayerAdvancements::progress_for(uint16_t index) {
  const auto found = progress_.find(index);
  if (found != progress_.end())
    return found->second;
  AdvancementProgress progress;
  progress.update(*advancement_at(index));
  return progress_.emplace(index, std::move(progress)).first->second;
}

void PlayerAdvancements::mark_for_visibility_update(uint16_t index) {
  const GeneratedAdvancement* root = advancement_root(*advancement_at(index));
  const int root_index = advancement_index(root);
  if (root_index >= 0)
    roots_to_update_.insert(static_cast<uint16_t>(root_index));
}

std::size_t
PlayerAdvancements::grant_matching(uint8_t trigger,
                                   const std::function<bool(const GeneratedCriterion&)>& matches) {
  std::size_t granted = 0;
  for (uint16_t c = 0; c < static_cast<uint16_t>(kAdvancementCriteriaCount); ++c) {
    const GeneratedCriterion& criterion = kAdvancementCriteria[c];
    if (criterion.trigger != trigger || (criterion.flags & kAdvConditionUnsupported) != 0)
      continue;
    if (!matches(criterion))
      continue;
    const uint16_t owner = criterion_owner(c);
    AdvancementProgress& progress = progress_for(owner);
    const bool was_done = progress.is_done();
    if (progress.grant(criterion.name, now_ms())) {
      ++granted;
      progress_changed_.insert(owner);
      if (!was_done && progress.is_done()) {
        mark_for_visibility_update(owner);
        completed_.push_back(advancement_at(owner));
      }
    }
  }
  return granted;
}

std::size_t PlayerAdvancements::handle_inventory_changed(const Inventory& inventory,
                                                         int32_t changed_item) {
  // The generated table keeps at most one item predicate per criterion, which
  // is the shape vanilla tests against the *changed* stack -- so the inventory
  // itself is only needed if the changed item is unknown to the registry.
  (void)inventory;
  completed_.clear();
  const std::string changed(ItemRegistry::instance().name_of(changed_item));
  return grant_matching(kAdvTriggerInventoryChanged, [&](const GeneratedCriterion& criterion) {
    if (criterion.item_count == 0)
      return true; // `hasItems()` with no predicate: any change counts
    if (changed.empty())
      return false;
    for (int i = 0; i < criterion.item_count; ++i)
      if (changed == criterion.items[i])
        return true;
    return false;
  });
}

std::size_t PlayerAdvancements::handle_consume_item(int32_t item) {
  completed_.clear();
  const std::string consumed(ItemRegistry::instance().name_of(item));
  return grant_matching(kAdvTriggerConsumeItem, [&](const GeneratedCriterion& criterion) {
    if (criterion.item_count == 0)
      return true;
    for (int i = 0; i < criterion.item_count; ++i)
      if (consumed == criterion.items[i])
        return true;
    return false;
  });
}

std::size_t PlayerAdvancements::handle_recipe_unlocked(std::string_view recipe) {
  completed_.clear();
  const std::string key = normalize_id(recipe);
  return grant_matching(kAdvTriggerRecipeUnlocked, [&](const GeneratedCriterion& criterion) {
    return criterion.recipe != nullptr && key == criterion.recipe;
  });
}

std::size_t PlayerAdvancements::handle_recipe_crafted(std::string_view recipe) {
  completed_.clear();
  const std::string key = normalize_id(recipe);
  return grant_matching(kAdvTriggerRecipeCrafted, [&](const GeneratedCriterion& criterion) {
    return criterion.recipe != nullptr && key == criterion.recipe;
  });
}

std::size_t PlayerAdvancements::handle_player_killed_entity(std::string_view entity_type) {
  completed_.clear();
  const std::string type = normalize_id(entity_type);
  return grant_matching(kAdvTriggerPlayerKilledEntity, [&](const GeneratedCriterion& criterion) {
    return criterion.entity == nullptr || type == criterion.entity;
  });
}

std::size_t PlayerAdvancements::handle_entity_killed_player() {
  completed_.clear();
  return grant_matching(kAdvTriggerEntityKilledPlayer, [](const GeneratedCriterion&) {
    return true; // the one data criterion carries no entity predicate
  });
}

std::size_t PlayerAdvancements::handle_bred_animals(std::string_view child,
                                                    std::string_view parent_a,
                                                    std::string_view parent_b) {
  completed_.clear();
  const std::string born = normalize_id(child);
  const std::string first = normalize_id(parent_a);
  const std::string second = normalize_id(parent_b);
  return grant_matching(kAdvTriggerBredAnimals, [&](const GeneratedCriterion& criterion) {
    if (criterion.child != nullptr && born != criterion.child)
      return false;
    // `BredAnimalsTrigger.matches`: parent/partner may be either parent.
    const auto fits = [](const char* want, const std::string& type) {
      return want == nullptr || type == want;
    };
    return (fits(criterion.parent, first) && fits(criterion.partner, second)) ||
           (fits(criterion.parent, second) && fits(criterion.partner, first));
  });
}

std::size_t PlayerAdvancements::handle_villager_trade() {
  completed_.clear();
  return grant_matching(kAdvTriggerVillagerTrade, [](const GeneratedCriterion&) { return true; });
}

std::size_t PlayerAdvancements::handle_tick() {
  completed_.clear();
  return grant_matching(kAdvTriggerTick, [](const GeneratedCriterion&) { return true; });
}

std::size_t PlayerAdvancements::handle_location(std::string_view biome) {
  completed_.clear();
  const std::string standing_in = normalize_id(biome);
  if (standing_in.empty())
    return 0;
  return grant_matching(kAdvTriggerLocation, [&](const GeneratedCriterion& criterion) {
    // `LocationPredicate.matches`' biome test: `biomes.contains(level.getBiome(pos))`.
    // The generated list is already expanded to concrete biome keys, so this is
    // one name comparison per entry. A criterion of this trigger with no list is
    // a shape the generator never emits (it flags everything but the biome
    // field `unsupported`), so it stays unmatched rather than matching anything.
    for (uint16_t i = 0; i < criterion.biome_count; ++i) {
      if (standing_in == criterion.biomes[i])
        return true;
    }
    return false;
  });
}

std::size_t PlayerAdvancements::handle_enter_block(std::span<const std::string_view> entered) {
  completed_.clear();
  if (entered.empty())
    return 0;
  return grant_matching(kAdvTriggerEnterBlock, [&](const GeneratedCriterion& criterion) {
    // `EnterBlockTrigger.matches`' block test: `!block.isPresent() ||
    // state.is(block)`. A criterion that names no block matches any entry --
    // and every block that reaches here is one `Entity.checkInsideBlocks`
    // walked (air returns before this set is built), which is the loop's own
    // `state.isAir()` skip. The `state` property predicate is a shape the
    // generator flags, so a criterion carrying one never reaches this lambda.
    if (criterion.block_count == 0)
      return true;
    for (std::string_view block : entered)
      for (uint16_t i = 0; i < criterion.block_count; ++i)
        if (block == criterion.blocks[i])
          return true;
    return false;
  });
}

std::size_t PlayerAdvancements::handle_effects_changed(std::span<const std::string_view> active) {
  completed_.clear();
  return grant_matching(kAdvTriggerEffectsChanged, [&](const GeneratedCriterion& criterion) {
    // `MobEffectsPredicate.matches`: *every* effect the map names has to be on
    // the entity (`entity.getEffect(key) != null` and the instance matching the
    // entry's own amount bounds). The bounds are not modelled, which the
    // generator checks: a criterion whose map gives an effect anything but an
    // empty `{}` is flagged and never reaches this lambda. A criterion naming no
    // effect at all is vanilla's absent-`effects` branch, which any change
    // satisfies -- the loop below runs zero times and says true.
    for (uint16_t i = 0; i < criterion.effect_count; ++i) {
      bool present = false;
      for (std::string_view effect : active) {
        if (effect == criterion.effects[i]) {
          present = true;
          break;
        }
      }
      if (!present)
        return false;
    }
    return true;
  });
}

bool PlayerAdvancements::award(const GeneratedAdvancement& advancement, std::string_view criterion,
                               int64_t obtained_ms) {
  completed_.clear();
  const int index = advancement_index(&advancement);
  if (index < 0)
    return false;
  AdvancementProgress& progress = progress_for(static_cast<uint16_t>(index));
  const bool was_done = progress.is_done();
  if (!progress.grant(criterion, obtained_ms != 0 ? obtained_ms : now_ms()))
    return false;
  progress_changed_.insert(static_cast<uint16_t>(index));
  if (!was_done && progress.is_done()) {
    mark_for_visibility_update(static_cast<uint16_t>(index));
    completed_.push_back(&advancement);
  }
  return true;
}

bool PlayerAdvancements::revoke(const GeneratedAdvancement& advancement,
                                std::string_view criterion) {
  completed_.clear();
  const int index = advancement_index(&advancement);
  if (index < 0)
    return false;
  AdvancementProgress& progress = progress_for(static_cast<uint16_t>(index));
  const bool was_done = progress.is_done();
  if (!progress.revoke(criterion))
    return false;
  progress_changed_.insert(static_cast<uint16_t>(index));
  if (was_done && !progress.is_done())
    mark_for_visibility_update(static_cast<uint16_t>(index));
  return true;
}

const AdvancementProgress*
PlayerAdvancements::progress_of(const GeneratedAdvancement& advancement) const {
  const int index = advancement_index(&advancement);
  if (index < 0)
    return nullptr;
  const auto found = progress_.find(static_cast<uint16_t>(index));
  return found == progress_.end() ? nullptr : &found->second;
}

bool PlayerAdvancements::is_done(const GeneratedAdvancement& advancement) const {
  const AdvancementProgress* progress = progress_of(advancement);
  return progress != nullptr && progress->is_done();
}

bool PlayerAdvancements::is_visible(const GeneratedAdvancement& advancement) const {
  const int index = advancement_index(&advancement);
  return index >= 0 && visible_.count(static_cast<uint16_t>(index)) != 0;
}

bool PlayerAdvancements::evaluate_visibility(uint16_t index, std::vector<uint8_t>* rules,
                                             std::set<uint16_t>* added,
                                             std::set<uint16_t>* removed) {
  const GeneratedAdvancement& advancement = *advancement_at(index);
  const bool self_done = progress_for(index).is_done();
  // `evaluateVisibilityRule`: done shows even when hidden; a hidden or
  // display-less node that is not done hides (and stays hidden).
  uint8_t rule = kRuleNoChange;
  if (advancement.display == nullptr)
    rule = kRuleHide;
  else if (self_done)
    rule = kRuleShow;
  else if (advancement.display->hidden)
    rule = kRuleHide;

  bool self_or_descendant_done = self_done;
  rules->push_back(rule);
  for (const GeneratedAdvancement* child : advancement_children(advancement))
    self_or_descendant_done |=
        evaluate_visibility(static_cast<uint16_t>(advancement_index(child)), rules, added, removed);
  // `evaluateVisiblityForUnfinishedNode`: a done descendant shows the node; a
  // SHOW within self/parent/grandparent shows an unfinished one; a HIDE in that
  // window hides it.
  bool visible = self_or_descendant_done;
  if (!visible) {
    for (int i = 0; i <= 2 && static_cast<int>(rules->size()) > i; ++i) {
      const uint8_t ancestor = (*rules)[rules->size() - 1 - i];
      if (ancestor == kRuleShow) {
        visible = true;
        break;
      }
      if (ancestor == kRuleHide)
        break;
    }
  }
  rules->pop_back();

  if (visible) {
    if (visible_.insert(index).second) {
      added->insert(index);
      if (progress_.count(index) != 0)
        progress_changed_.insert(index);
    }
  } else if (visible_.erase(index) != 0) {
    removed->insert(index);
  }
  return self_or_descendant_done;
}

void PlayerAdvancements::update_tree_visibility(uint16_t root, std::set<uint16_t>* added,
                                                std::set<uint16_t>* removed) {
  // The three NO_CHANGE entries are the stack's floor (`evaluateVisibility`
  // seeds them before the root walk).
  std::vector<uint8_t> rules = {kRuleNoChange, kRuleNoChange, kRuleNoChange};
  evaluate_visibility(root, &rules, added, removed);
}

PlayerAdvancements::Update PlayerAdvancements::flush(bool show_advancements) {
  Update update;
  update.show = show_advancements;
  if (first_packet_ || !roots_to_update_.empty() || !progress_changed_.empty()) {
    update.reset = first_packet_;
    std::set<uint16_t> added;
    std::set<uint16_t> removed;
    const std::set<uint16_t> roots = roots_to_update_;
    roots_to_update_.clear();
    for (uint16_t root : roots)
      update_tree_visibility(root, &added, &removed);
    for (uint16_t index : progress_changed_)
      if (visible_.count(index) != 0)
        update.progress.emplace_back(advancement_at(index), &progress_for(index));
    progress_changed_.clear();
    for (uint16_t index : added)
      update.added.push_back(advancement_at(index));
    for (uint16_t index : removed)
      update.removed.push_back(std::string(advancement_at(index)->id));
    // `PlayerAdvancements.flushDirty` clears `firstPacket` only on the path
    // that actually sends (`player.connection.send(...)` and then the flag),
    // and its early return leaves it standing: the reset has to ride the first
    // packet that *says* something, not the first flush that runs. A flush with
    // nothing in it writes no packet at all (`Update::empty`, the caller's
    // rule), so consuming the reset here would drop it — and a rejoining
    // client whose first flush happened to come out empty would then keep the
    // stale tree from its last session for good, because no later packet is a
    // reset either. (The walk above consumed nothing in that case: an empty
    // update means no node entered or left `visible_` and no progress row is
    // owed, so the roots and the changed set were carrying no news.)
    if (!update.empty())
      first_packet_ = false;
  }
  return update;
}

bool PlayerAdvancements::set_selected_tab(const std::string* tab) {
  std::optional<std::string> next;
  if (tab != nullptr) {
    const GeneratedAdvancement* advancement = advancement_by_id(*tab);
    // `setSelectedTab`: only a displayed root is a tab.
    if (advancement != nullptr && advancement->parent == nullptr && advancement->display != nullptr)
      next = advancement->id;
  }
  if (next == selected_tab_)
    return false;
  selected_tab_ = std::move(next);
  return true;
}

// ---- persistence -----------------------------------------------------------

std::vector<SavedAdvancementProgress> PlayerAdvancements::saved_progress() const {
  std::vector<SavedAdvancementProgress> rows;
  for (const auto& [index, progress] : progress_) {
    // `asData` writes the entries `hasProgress` sees. An entry every event so
    // far has left open — the map entry an event created and then matched
    // nothing in — is not a row.
    if (!progress.has_progress())
      continue;
    const GeneratedAdvancement* advancement = advancement_at(index);
    if (advancement == nullptr)
      continue;
    SavedAdvancementProgress row;
    row.advancement = advancement->id;
    // The criteria map serializes only what was obtained (`filter(isDone)`),
    // and it is a std::map, so a row's criteria come out in name order.
    for (const auto& [name, state] : progress.criteria())
      if (state.obtained)
        row.criteria.push_back(SavedCriterion{name, state.obtained_ms});
    rows.push_back(std::move(row));
  }
  // `progress_` is a hash map: sort the rows by id so the file's bytes do not
  // depend on iteration order.
  std::sort(rows.begin(), rows.end(),
            [](const SavedAdvancementProgress& a, const SavedAdvancementProgress& b) {
              return a.advancement < b.advancement;
            });
  return rows;
}

std::size_t
PlayerAdvancements::apply_saved_progress(const std::vector<SavedAdvancementProgress>& saved) {
  std::size_t adopted = 0;
  for (const SavedAdvancementProgress& row : saved) {
    // `applyFrom` resolves the id against the tree: a row for an advancement
    // this build no longer carries is ignored (vanilla warns and moves on).
    const GeneratedAdvancement* advancement = advancement_by_id(row.advancement);
    if (advancement == nullptr)
      continue;
    const uint16_t index = static_cast<uint16_t>(advancement_index(advancement));
    // `startProgress`: `progress_for` runs `AdvancementProgress.update`, so the
    // entry has exactly the criteria this table's requirements name — a saved
    // criterion the requirements dropped fails `grant` and is forgotten.
    AdvancementProgress& progress = progress_for(index);
    for (const SavedCriterion& criterion : row.criteria)
      progress.grant(criterion.criterion, criterion.obtained_ms);
    // ...then `progressChanged.add` and `markForVisibilityUpdate`, in that
    // order. The first `flush` after this walks the roots and answers with
    // `reset` plus every node the restored progress shows.
    progress_changed_.insert(index);
    mark_for_visibility_update(index);
    ++adopted;
  }
  return adopted;
}

} // namespace mc::world
