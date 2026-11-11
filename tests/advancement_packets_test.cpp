// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4 milestone N: the advancement packets, pinned byte-for-byte against
// the vanilla 26.2 codecs (ClientboundUpdateAdvancementsPacket,
// ClientboundSelectAdvancementsTabPacket). Pure logic, no sockets — the same
// shape tests/scoreboard_packets_test.cpp gives the scoreboard packets.
//
// The layouts this guards, each one field away from a misparse:
//
//   * update_advancements [bool reset][varint added]{[utf id][Advancement]}
//     [varint removed]{[utf id]}
//     [varint progress]{[utf id][varint criteria]{[utf name][bool][long?]}}
//     [bool show]
//   * Advancement        [bool + utf parent][bool + DisplayInfo]
//     [varint sets][per set: varint count, utf names][bool telemetry]
//   * DisplayInfo        [Component title][Component description]
//     [ItemStackTemplate: varint item, varint count, varint 0, varint 0]
//     [varint frame][u32 flags][utf background when flag 0][f32 x][f32 y]
//   * select_advancements_tab [bool + utf tab]
//
// The components are network NBT: `Writer::component_translate` writes
// TAG_Compound { translate: key } — 0x0A, then a TAG_String entry, then
// TAG_End — and NBT string lengths are u16, not varints. The icon is the
// *ItemStackTemplate* order (item before count), which is the opposite of the
// menu ItemStack's.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "server/advancement_packets.h"
#include "world/advancement.h"
#include "world/item_registry.h"

namespace {

int g_failures = 0;

std::string hex(const std::vector<uint8_t>& b) {
  static const char* k = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
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

void push_varint(std::vector<uint8_t>* out, uint32_t v) {
  while (v >= 0x80) {
    out->push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out->push_back(static_cast<uint8_t>(v));
}

void push_u16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void push_u32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v >> 24));
  out->push_back(static_cast<uint8_t>(v >> 16));
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void push_i64(std::vector<uint8_t>* out, int64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out->push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> shift));
}

void push_f32(std::vector<uint8_t>* out, float v) {
  uint32_t bits;
  __builtin_memcpy(&bits, &v, sizeof(bits));
  push_u32(out, bits);
}

// An identifier: varint length + bytes.
void push_identifier(std::vector<uint8_t>* out, const std::string& id) {
  push_varint(out, static_cast<uint32_t>(id.size()));
  out->insert(out->end(), id.begin(), id.end());
}

// The NBT of `Component.translatable(key)` with no arguments.
void push_component_translate(std::vector<uint8_t>* out, const std::string& key) {
  out->push_back(0x0A); // TAG_Compound
  out->push_back(0x08); // TAG_String
  push_u16(out, 9);
  out->insert(out->end(), {'t', 'r', 'a', 'n', 's', 'l', 'a', 't', 'e'});
  push_u16(out, static_cast<uint16_t>(key.size()));
  out->insert(out->end(), key.begin(), key.end());
  out->push_back(0x00); // TAG_End
}

// One `u8 TAG_String` + `u16 name len` + name + `u16 value len` + value entry of
// a compound.
void push_string_field(std::vector<uint8_t>* out, const std::string& name,
                       const std::string& value) {
  out->push_back(0x08);
  push_u16(out, static_cast<uint16_t>(name.size()));
  out->insert(out->end(), name.begin(), name.end());
  push_u16(out, static_cast<uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

// One `u8 TAG_List` + `u16 name len` + name + `u8 element type (compound)` +
// `i32 count` header; the caller appends `count` compound elements. A list
// element is the compound's *payload* — its entries and TAG_End — with no tag
// id in front of it: the element type in the header already said it. (The
// pre-fix encoder wrote one anyway, which is a byte the client reads as the
// first entry's type: every component that carried arguments — a death
// message, a chat announcement — decoded as a corrupt tag tree.)
void push_component_list(std::vector<uint8_t>* out, const std::string& name, uint32_t count) {
  out->push_back(0x09);
  push_u16(out, static_cast<uint16_t>(name.size()));
  out->insert(out->end(), name.begin(), name.end());
  out->push_back(0x0A);
  push_u32(out, count);
}

// `ItemStackTemplate.STREAM_CODEC`: item id, count, then the empty patch.
void push_template_icon(std::vector<uint8_t>* out, const std::string& item, uint32_t count) {
  const std::optional<int32_t> id = mc::world::ItemRegistry::instance().id_of(item);
  push_varint(out, static_cast<uint32_t>(id.value_or(1)));
  push_varint(out, count);
  push_varint(out, 0);
  push_varint(out, 0);
}

} // namespace

int main() {
  using mc::world::AdvancementProgress;
  using mc::world::GeneratedAdvancement;

  // ---- select_advancements_tab (0x57) -------------------------------------
  {
    const std::vector<uint8_t> want = {0x57, 0x00}; // absent tab
    expect_bytes("select_advancements_tab null", mc::server::make_select_advancements_tab(nullptr),
                 want);
  }
  {
    const std::string tab = "minecraft:story/root";
    std::vector<uint8_t> want = {0x57, 0x01};
    push_identifier(&want, tab);
    expect_bytes("select_advancements_tab root", mc::server::make_select_advancements_tab(&tab),
                 want);
  }

  const GeneratedAdvancement* root = mc::world::advancement_by_id("minecraft:story/root");
  const GeneratedAdvancement* mine_stone =
      mc::world::advancement_by_id("minecraft:story/mine_stone");
  if (root == nullptr || mine_stone == nullptr) {
    std::printf("FAIL: the table lost story/root or story/mine_stone\n");
    return 1;
  }

  // ---- update_advancements (0x85), progress only --------------------------
  {
    AdvancementProgress root_progress;
    root_progress.update(*root); // the criterion exists, none obtained
    AdvancementProgress stone_progress;
    stone_progress.update(*mine_stone);
    stone_progress.grant("get_stone", 4242);

    const std::vector<std::pair<const GeneratedAdvancement*, const AdvancementProgress*>> progress =
        {
            {mine_stone, &stone_progress},
            {root, &root_progress},
        };
    // The packet id is a varint: 0x85 is 133, so it takes two bytes.
    std::vector<uint8_t> want = {0x85, 0x01, 0x00, 0x00, 0x00, 0x02};
    push_identifier(&want, "minecraft:story/mine_stone");
    push_varint(&want, 1);
    push_identifier(&want, "get_stone");
    want.push_back(0x01);
    push_i64(&want, 4242); // the Instant is a full long on the wire
    push_identifier(&want, "minecraft:story/root");
    push_varint(&want, 1);
    push_identifier(&want, "crafting_table");
    want.push_back(0x00); // not obtained: no timestamp
    want.push_back(0x01); // show
    expect_bytes("update_advancements progress",
                 mc::server::make_update_advancements(false, {}, {}, progress, true,
                                                      mc::server::AdvancementLayout::Legacy),
                 want);
  }

  // ---- update_advancements (0x85), one added advancement ------------------
  {
    std::vector<uint8_t> want = {0x85, 0x01, 0x01, 0x01};
    push_identifier(&want, "minecraft:story/root");
    want.push_back(0x00); // no parent
    want.push_back(0x01); // a display
    push_component_translate(&want, "advancements.story.root.title");
    push_component_translate(&want, "advancements.story.root.description");
    push_template_icon(&want, "minecraft:grass_block", 1);
    push_varint(&want, 0); // AdvancementType.TASK
    push_u32(&want, 1);    // background present, no toast, not hidden
    push_identifier(&want, "minecraft:gui/advancements/backgrounds/stone");
    push_f32(&want, 0.0f);
    push_f32(&want, 1.75f);
    push_varint(&want, 1); // one requirement set
    push_varint(&want, 1); // of one criterion
    push_identifier(&want, "crafting_table");
    want.push_back(0x01);  // sends_telemetry_event
    push_varint(&want, 0); // no removals
    push_varint(&want, 0); // no progress
    want.push_back(0x00);  // showAdvancements false (the tick's `flushDirty(false)` shape)
    expect_bytes("update_advancements added",
                 mc::server::make_update_advancements(true, {root}, {}, {}, false,
                                                      mc::server::AdvancementLayout::Legacy),
                 want);
  }

  // ---- update_advancements (0x85), 26.3 positioned form -------------------
  {
    std::vector<uint8_t> want = {0x85, 0x01, 0x01, 0x01};
    push_identifier(&want, "minecraft:story/root");
    want.push_back(0x00); // no parent
    want.push_back(0x01); // a display
    push_component_translate(&want, "advancements.story.root.title");
    push_component_translate(&want, "advancements.story.root.description");
    push_template_icon(&want, "minecraft:grass_block", 1);
    push_varint(&want, 0); // AdvancementType.TASK
    push_u32(&want, 1);    // background present, no toast, not hidden
    push_identifier(&want, "minecraft:gui/advancements/backgrounds/stone");
    // 26.3 DisplayInfo stops here: the coordinates moved out of it.
    push_varint(&want, 1); // one requirement set
    push_varint(&want, 1); // of one criterion
    push_identifier(&want, "crafting_table");
    want.push_back(0x01);   // sends_telemetry_event
    push_f32(&want, 0.0f);  // PositionedAdvancement.x
    push_f32(&want, 1.75f); // PositionedAdvancement.y
    push_varint(&want, 0);  // no removals
    push_varint(&want, 0);  // no progress
    want.push_back(0x00);   // showAdvancements false
    expect_bytes("update_advancements positioned",
                 mc::server::make_update_advancements(true, {root}, {}, {}, false,
                                                      mc::server::AdvancementLayout::Positioned),
                 want);
  }

  const GeneratedAdvancement* adventuring =
      mc::world::advancement_by_id("minecraft:adventure/adventuring_time");
  if (adventuring == nullptr || adventuring->display == nullptr ||
      adventuring->display->frame != mc::server::kAdvancementFrameChallenge) {
    std::printf("FAIL: the table lost the adventuring_time challenge\n");
    return 1;
  }

  // ---- the announce_to_chat broadcast (system_chat 0x7c) ------------------
  // `AdvancementType.createAnnouncement` + `Advancement.name`: a translatable
  // whose second argument is the display title in square brackets, coloured by
  // the frame. The title stays a *translatable* child (`{translate: key}`) so
  // the client localizes it — the server ships no language file.
  {
    std::vector<uint8_t> want = {0x7C, 0x0A};
    push_string_field(&want, "translate", "chat.type.advancement.task");
    push_component_list(&want, "with", 2);
    { // the completer's display name
      push_string_field(&want, "text", "AdvBot");
      want.push_back(0x00);
    }
    { // the bracketed, coloured title
      push_string_field(&want, "text", "[");
      push_string_field(&want, "color", "green");
      push_component_list(&want, "extra", 2);
      push_string_field(&want, "translate", "advancements.story.mine_stone.title");
      want.push_back(0x00);
      push_string_field(&want, "text", "]");
      want.push_back(0x00);
      want.push_back(0x00); // TAG_End of the bracket compound
    }
    want.push_back(0x00); // TAG_End of the message compound
    want.push_back(0x00); // chat, not actionbar
    expect_bytes("announcement task",
                 mc::server::make_advancement_announcement(*mine_stone->display, "AdvBot"), want);
  }
  {
    // A CHALLENGE says "challenge" and draws dark purple, the enum's own pair.
    std::vector<uint8_t> want = {0x7C, 0x0A};
    push_string_field(&want, "translate", "chat.type.advancement.challenge");
    push_component_list(&want, "with", 2);
    push_string_field(&want, "text", "AdvBot");
    want.push_back(0x00);
    push_string_field(&want, "text", "[");
    push_string_field(&want, "color", "dark_purple");
    push_component_list(&want, "extra", 2);
    push_string_field(&want, "translate", "advancements.adventure.adventuring_time.title");
    want.push_back(0x00);
    push_string_field(&want, "text", "]");
    want.push_back(0x00);
    want.push_back(0x00);
    want.push_back(0x00);
    want.push_back(0x00);
    expect_bytes("announcement challenge",
                 mc::server::make_advancement_announcement(*adventuring->display, "AdvBot"), want);
  }

  if (g_failures == 0)
    std::printf("advancement packets: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
