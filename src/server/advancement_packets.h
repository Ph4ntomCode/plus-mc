// SPDX-License-Identifier: GPL-3.0-or-later
//
// The advancement packets (Phase 4 milestone N), kept in a header so a unit
// test can pin every byte against the codecs without a live client — the same
// shape scoreboard_packets.h uses.
//
// 26.3 moved the tree coordinates out of DisplayInfo and into a
// `PositionedAdvancement` wrapper: each added entry is now
// `[utf id][Advancement][f32 x][f32 y]`, while 26.1/26.2 wrote the two floats
// at the end of DisplayInfo. The builder takes the layout; the canonical 26.3
// session uses `Positioned`, a translated 26.1/26.2 session `Legacy`.
//
// The layouts:
//
//   update_advancements (ClientboundUpdateAdvancementsPacket):
//     [bool reset][varint added count]{[utf id][Advancement]}
//     [varint removed count]{[utf id]}
//     [varint progress count]{[utf id][AdvancementProgress]}
//     [bool showAdvancements]
//     (26.3 appends `[f32 x][f32 y]` after each added entry's Advancement.)
//     An `AdvancementProgress` is `writeMap(criteria, writeUtf,
//     CriterionProgress::serializeToNetwork)` — a map of criterion name to a
//     nullable Instant, so `[varint count]{[utf name][bool obtained][long ms]}`
//     with the long present only when obtained.
//
//   Advancement (`Advancement.write`):
//     [bool + utf parent][bool + DisplayInfo]
//     [AdvancementRequirements: varint sets][per set: varint count]{[utf name]}
//     [bool sendsTelemetryEvent]
//     — the rewards and criteria are *not* on the wire; the client only needs
//     the tree shape and the display.
//
//   DisplayInfo (`DisplayInfo.serializeToNetwork`):
//     [Component title][Component description]
//     [ItemStackTemplate icon: varint item id][varint count]
//       [DataComponentPatch: varint added][varint removed]]
//     [varint AdvancementType: TASK 0, CHALLENGE 1, GOAL 2]
//     [u32 flags: bit 0 background, bit 1 show toast, bit 2 hidden]
//     [utf background when flag 0]
//     The title and description are translation keys (the client's own
//     language file renders them). The coordinates are the generated table's —
//     `TreeNodePosition` already laid the tree out.
//
//   select_advancements_tab (ClientboundSelectAdvancementsTabPacket):
//     [bool + utf tab] — a null tab means "nothing selected".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "protocol/ids.h"
#include "world/advancement.h"
#include "world/item.h"

namespace mc::server {

// Where the tree coordinates live: after the AdvancementHolder in 26.3, or at
// the end of DisplayInfo in 26.1/26.2.
enum class AdvancementLayout { Positioned, Legacy };

// `AdvancementType` ordinals (the enum's declaration order).
inline constexpr uint8_t kAdvancementFrameTask = 0;
inline constexpr uint8_t kAdvancementFrameChallenge = 1;
inline constexpr uint8_t kAdvancementFrameGoal = 2;

inline void write_advancement_display(mc::net::Writer& w,
                                      const mc::world::GeneratedAdvancementDisplay& display,
                                      AdvancementLayout layout) {
  // `ComponentSerialization.TRUSTED_STREAM_CODEC`: network NBT, here a bare
  // translatable component ({translate: key}), the shape Writer::component_translate
  // emits.
  w.component_translate(display.title, {});
  w.component_translate(display.description, {});
  // `ItemStackTemplate.STREAM_CODEC`: item id, then count, then the
  // DataComponentPatch (empty: both counts zero). The order is *not* the menu
  // ItemStack's (which writes count first). Every icon in the pack resolves;
  // stone stands in for a name a future pack drops.
  const std::optional<int32_t> icon = mc::world::ItemRegistry::instance().id_of(display.icon);
  w.varint(static_cast<uint32_t>(icon.value_or(1)));
  w.varint(display.icon_count);
  w.varint(0); // added components
  w.varint(0); // removed components
  w.varint(display.frame);
  uint32_t flags = 0;
  if (display.background != nullptr)
    flags |= 1;
  if (display.show_toast)
    flags |= 2;
  if (display.hidden)
    flags |= 4;
  w.u32(flags);
  if (display.background != nullptr)
    w.string(display.background);
  if (layout == AdvancementLayout::Legacy) {
    w.f32(display.x);
    w.f32(display.y);
  }
}

inline void write_advancement(mc::net::Writer& w,
                              const mc::world::GeneratedAdvancement& advancement,
                              AdvancementLayout layout) {
  w.u8(advancement.parent != nullptr ? 1 : 0);
  if (advancement.parent != nullptr)
    w.string(advancement.parent);
  w.u8(advancement.display != nullptr ? 1 : 0);
  if (advancement.display != nullptr)
    write_advancement_display(w, *advancement.display, layout);
  // `AdvancementRequirements.write`: the sets, each its own count.
  w.varint(advancement.requirement_set_count);
  uint16_t cursor = advancement.requirement_criteria_first;
  for (int set = 0; set < advancement.requirement_set_count; ++set) {
    const uint8_t size =
        mc::world::kAdvancementRequirementSizes[advancement.requirement_sets_first + set];
    w.varint(size);
    for (uint8_t i = 0; i < size; ++i) {
      const uint8_t local = mc::world::kAdvancementRequirementCriteria[cursor++];
      w.string(mc::world::kAdvancementCriteria[advancement.criteria_first + local].name);
    }
  }
  w.u8(advancement.sends_telemetry_event ? 1 : 0);
}

inline void write_advancement_progress(mc::net::Writer& w,
                                       const mc::world::AdvancementProgress& progress) {
  const std::map<std::string, mc::world::CriterionState>& criteria = progress.criteria();
  w.varint(static_cast<uint32_t>(criteria.size()));
  for (const auto& [name, state] : criteria) {
    w.string(name);
    w.u8(state.obtained ? 1 : 0);
    if (state.obtained)
      w.i64(state.obtained_ms); // FriendlyByteBuf.writeInstant: epoch millis
  }
}

inline std::vector<uint8_t> make_update_advancements(
    bool reset, const std::vector<const mc::world::GeneratedAdvancement*>& added,
    const std::vector<std::string>& removed,
    const std::vector<std::pair<const mc::world::GeneratedAdvancement*,
                                const mc::world::AdvancementProgress*>>& progress,
    bool show_advancements, AdvancementLayout layout = AdvancementLayout::Positioned) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayUpdateAdvancementsId));
  w.u8(reset ? 1 : 0);
  w.varint(static_cast<uint32_t>(added.size()));
  for (const mc::world::GeneratedAdvancement* advancement : added) {
    w.string(advancement->id);
    write_advancement(w, *advancement, layout);
    if (layout == AdvancementLayout::Positioned) {
      w.f32(advancement->display != nullptr ? advancement->display->x : 0.0f);
      w.f32(advancement->display != nullptr ? advancement->display->y : 0.0f);
    }
  }
  w.varint(static_cast<uint32_t>(removed.size()));
  for (const std::string& id : removed)
    w.string(id);
  w.varint(static_cast<uint32_t>(progress.size()));
  for (const auto& [advancement, entry] : progress) {
    w.string(advancement->id);
    write_advancement_progress(w, *entry);
  }
  w.u8(show_advancements ? 1 : 0);
  return std::move(w).take();
}

inline std::vector<uint8_t> make_select_advancements_tab(const std::string* tab) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySelectAdvancementsTabId));
  w.u8(tab != nullptr ? 1 : 0);
  if (tab != nullptr)
    w.string(*tab);
  return std::move(w).take();
}

// `AdvancementType`'s own lang key for an announcement (`createAnnouncement`):
// "chat.type.advancement.<task|challenge|goal>".
inline const char* advancement_announcement_key(uint8_t frame) {
  switch (frame) {
  case kAdvancementFrameChallenge:
    return "chat.type.advancement.challenge";
  case kAdvancementFrameGoal:
    return "chat.type.advancement.goal";
  default:
    return "chat.type.advancement.task";
  }
}

// `AdvancementType.getChatColor`: TASK and GOAL are green, CHALLENGE dark purple.
inline const char* advancement_announcement_color(uint8_t frame) {
  return frame == kAdvancementFrameChallenge ? "dark_purple" : "green";
}

// The chat line a completed advancement is announced with: `system_chat` (0x79)
// carrying `AdvancementType.createAnnouncement` —
//
//   Component.translatable("chat.type.advancement." + type,
//                          player.getDisplayName(),
//                          Advancement.name(holder))
//
// — where the name is the display title wrapped in square brackets and coloured
// by the frame (`ComponentUtils.wrapInSquareBrackets(title).withStyle(color)`).
// The title travels as its own translatable component, because the server ships
// no language file: "advancements.<id>.title" only means something on the
// client, which resolves it — and the frame wording — from its own lang table.
// A display-less advancement has nothing to announce, which is why this takes
// the display and not the advancement: the caller only reaches it when there is
// one. Vanilla also carries the title's hover tooltip; this does not (the chat
// text is what the announcement is for).
inline std::vector<uint8_t>
make_advancement_announcement(const mc::world::GeneratedAdvancementDisplay& display,
                              const std::string& player) {
  const char* color = advancement_announcement_color(display.frame);
  mc::net::ComponentNode title;
  title.translate = display.title;
  mc::net::ComponentNode closing;
  closing.text = "]";
  mc::net::ComponentNode bracketed;
  bracketed.text = "[";
  bracketed.color = color;
  bracketed.extra = {title, closing};
  mc::net::ComponentNode who;
  who.text = player;
  mc::net::ComponentNode message;
  message.translate = advancement_announcement_key(display.frame);
  message.with = {who, bracketed};

  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySystemChatId));
  w.component(message);
  w.u8(0); // chat, not actionbar
  return std::move(w).take();
}

} // namespace mc::server
