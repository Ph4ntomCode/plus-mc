// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone F: the drop path's entity packets, pinned byte-for-byte against the
// vanilla 26.3 codecs (ClientboundAddEntityPacket / ClientboundSetEntityDataPacket
// / ItemStack.OPTIONAL_STREAM_CODEC). Pure logic, no sockets.
//
// The regression this guards: Set Entity Data's ItemStack is
//   [varint count][varint item id][patch added=0][patch removed=0]
// with NO "present" flag byte between count and the item id. An extra byte
// there makes the client read the item id as air and then misparse the rest of
// the synched-data list. The last check walks the whole packet with a decoder
// and asserts it lands exactly on the 0xFF terminator.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "server/entity_packets.h"

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

// Reads the first byte as a VarInt; returns the value (packet ids/entity ids
// here are all one byte, so this is enough for the assertions).
bool read_varint(const std::vector<uint8_t>& b, size_t* pos, uint32_t* out) {
  uint32_t v = 0;
  int shift = 0;
  while (*pos < b.size() && shift < 35) {
    uint8_t c = b[(*pos)++];
    v |= static_cast<uint32_t>(c & 0x7F) << shift;
    if (!(c & 0x80)) {
      *out = v;
      return true;
    }
    shift += 7;
  }
  return false;
}

// Walks a set_entity_data body the way the client does and asserts it consumes
// exactly to the 0xFF terminator with no trailing bytes.
void expect_set_entity_data_consumes_all(int32_t eid, const std::vector<uint8_t>& pkt) {
  size_t pos = 0;
  uint32_t pid = 0, id = 0;
  if (!read_varint(pkt, &pos, &pid) || pid != mc::protocol::kPlaySetEntityDataId) {
    ++g_failures;
    std::printf("FAIL: set_entity_data packet id mismatch (%u vs %d)\n", pid,
                mc::protocol::kPlaySetEntityDataId);
    return;
  }
  if (!read_varint(pkt, &pos, &id) || id != static_cast<uint32_t>(eid)) {
    ++g_failures;
    std::printf("FAIL: set_entity_data entity id mismatch\n");
    return;
  }
  while (pos < pkt.size()) {
    uint8_t index = pkt[pos++];
    if (index == 0xFF)
      break;
    uint32_t serializer = 0;
    if (!read_varint(pkt, &pos, &serializer)) {
      ++g_failures;
      std::printf("FAIL: truncated serializer id\n");
      return;
    }
    if (serializer != mc::server::kItemStackSerializerId) {
      ++g_failures;
      std::printf("FAIL: serializer id %u != ITEM_STACK (%u)\n", serializer,
                  mc::server::kItemStackSerializerId);
      return;
    }
    uint32_t count = 0;
    if (!read_varint(pkt, &pos, &count)) {
      ++g_failures;
      std::printf("FAIL: truncated stack count\n");
      return;
    }
    uint32_t item = 0;
    if (!read_varint(pkt, &pos, &item)) {
      ++g_failures;
      std::printf("FAIL: truncated item id (extra byte before the item id?)\n");
      return;
    }
    if (item == 0) {
      ++g_failures;
      std::printf("FAIL: item id decoded as air (extra byte before the item id?)\n");
      return;
    }
    uint32_t added = 0, removed = 0;
    if (!read_varint(pkt, &pos, &added) || !read_varint(pkt, &pos, &removed) || added != 0 ||
        removed != 0) {
      ++g_failures;
      std::printf("FAIL: DataComponentPatch not the empty (0,0) patch\n");
      return;
    }
    (void)count;
  }
  if (pos != pkt.size()) {
    ++g_failures;
    std::printf("FAIL: %zu trailing byte(s) after the Set Entity Data terminator\n",
                pkt.size() - pos);
  }
}

void expect_u64(const char* what, const std::vector<uint8_t>& b, size_t off, uint64_t want) {
  if (off + 8 > b.size()) {
    ++g_failures;
    std::printf("FAIL: %s out of range\n", what);
    return;
  }
  uint64_t got = 0;
  for (int i = 0; i < 8; ++i)
    got = (got << 8) | b[off + static_cast<size_t>(i)];
  if (got != want) {
    ++g_failures;
    std::printf("FAIL: %s got %016llx want %016llx\n", what, static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
  }
}

void expect_u32(const char* what, const std::vector<uint8_t>& b, size_t off, uint32_t want) {
  if (off + 4 > b.size()) {
    ++g_failures;
    std::printf("FAIL: %s out of range\n", what);
    return;
  }
  uint32_t got = 0;
  for (int i = 0; i < 4; ++i)
    got = (got << 8) | b[off + static_cast<size_t>(i)];
  if (got != want) {
    ++g_failures;
    std::printf("FAIL: %s got %08x want %08x\n", what, got, want);
  }
}

} // namespace

int main() {
  // --- Set Entity Data: cobblestone (item id 1), one item -------------------
  const auto data = mc::server::make_item_entity_data(7, 1, 1);
  const std::vector<uint8_t> want_data = {
      0x65,       // set_entity_data
      0x07,       // entity id
      0x08,       // DATA_ITEM index
      0x07,       // ITEM_STACK serializer
      0x01,       // count
      0x01,       // item id (minecraft:stone/cobblestone registry id 1)
      0x00, 0x00, // empty DataComponentPatch (added, removed)
      0xFF,       // end of synched data
  };
  expect_bytes("set_entity_data bytes", data, want_data);
  expect_set_entity_data_consumes_all(7, data);

  // A count of 64 encodes as a multi-byte... no: 64 = 0x40, still one byte.
  // Verify a >1 count and a multi-byte entity id still round-trip.
  const auto data_big = mc::server::make_item_entity_data(300, 961, 5);
  expect_set_entity_data_consumes_all(300, data_big);

  // --- Add Entity: item entity type 72, zero velocity ----------------------
  uint8_t uuid[16] = {0};
  const auto spawn = mc::server::make_item_entity_spawn(7, uuid, 0.5, 4.5, 1.5);
  const std::vector<uint8_t> want_spawn = {
      0x01, // add_entity
      0x07, // entity id
      // uuid (16 zero bytes)
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0x48, // entity type 72 = minecraft:item
      0x3F,
      0xE0,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00, // x = 0.5
      0x40,
      0x12,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00, // y = 4.5
      0x3F,
      0xF8,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00, // z = 1.5
      0x00, // zero LpVec3 velocity
      0x00,
      0x00,
      0x00, // xRot, yRot, yHeadRot
      0x00, // data
  };
  expect_bytes("add_entity bytes", spawn, want_spawn);
  expect_u64("add_entity x", spawn, 19, 0x3FE0000000000000ULL);
  expect_u64("add_entity y", spawn, 27, 0x4012000000000000ULL);
  expect_u64("add_entity z", spawn, 35, 0x3FF8000000000000ULL);

  // --- Experience orbs (milestone E follow-up) ------------------------------
  // Add Entity for an orb is an item's layout with the orb's own entity type
  // (50), and the launch velocity lands in the LpVec3 field — the client starts
  // integrating from there, so a zeroed velocity would leave the orb hanging in
  // the air where it spawned.
  uint8_t orb_uuid[16] = {0};
  const auto orb_at_rest = mc::server::make_experience_orb_spawn(7, orb_uuid, 0.5, 4.5, 1.5);
  const auto orb_launched =
      mc::server::make_experience_orb_spawn(7, orb_uuid, 0.5, 4.5, 1.5, -0.1, 0.2, 0.0);
  if (orb_at_rest[0] != 0x01 || orb_at_rest[1] != 0x07 || orb_at_rest[18] != 0x32) {
    ++g_failures;
    std::printf("FAIL: orb add_entity id/entity/type (type byte %02x, want 32)\n",
                orb_at_rest.size() > 18 ? orb_at_rest[18] : 0);
  }
  // 2 header + 16 uuid + 1 type + 24 position + LpVec3 + 3 rotations + 1 data:
  // a zero velocity is the single 0x00 form, a real one is 6 bytes.
  if (orb_at_rest.size() != 48 || orb_launched.size() != 53) {
    ++g_failures;
    std::printf("FAIL: orb add_entity lengths %zu/%zu (want 48/53)\n", orb_at_rest.size(),
                orb_launched.size());
  }
  expect_u64("orb add_entity x", orb_launched, 19, 0x3FE0000000000000ULL);
  expect_u64("orb add_entity y", orb_launched, 27, 0x4012000000000000ULL);
  expect_u64("orb add_entity z", orb_launched, 35, 0x3FF8000000000000ULL);

  // Set Entity Data for the orb's value: DATA_VALUE is the orb's first accessor
  // (index 8, Entity's eight values), carrying an INT (serializer 1), which is
  // four raw bytes, big-endian — not a varint, and not little-endian.
  const auto orb_data = mc::server::make_experience_orb_data(7, 3);
  const std::vector<uint8_t> want_orb_data = {
      0x65,                   // set_entity_data
      0x07,                   // entity id
      0x08,                   // DATA_VALUE index
      0x01,                   // INT serializer
      0x00, 0x00, 0x00, 0x03, // 3, big-endian i32
      0xFF,                   // end of synched data
  };
  expect_bytes("orb set_entity_data bytes", orb_data, want_orb_data);

  // sync_entity_position (0x23): 26.3's PositionPath form. Entity id, then
  // LINEAR (type 0) and its end-position Vec3, then the two f32 rotations and
  // the on-ground bool. The 26.2 PositionMoveRotation velocity is gone.
  const auto sync = mc::server::make_entity_position_sync(7, 0.5, 4.5, 1.5, -0.1, 0.2, 0.0, true);
  if (sync.size() != 36 || sync[0] != 0x23 || sync[1] != 0x07 || sync[2] != 0x00 ||
      sync[35] != 0x01) {
    ++g_failures;
    std::printf("FAIL: sync_entity_position framing (len %zu)\n", sync.size());
  }
  expect_u64("sync x", sync, 3, 0x3FE0000000000000ULL);
  expect_u64("sync y", sync, 11, 0x4012000000000000ULL);
  expect_u64("sync z", sync, 19, 0x3FF8000000000000ULL);
  expect_u32("sync yaw", sync, 27, 0);
  expect_u32("sync pitch", sync, 31, 0);

  // take_item_entity (0x7c): three varints — the entity taken, the player that
  // took it, and the amount (always 1 for an orb).
  const auto take = mc::server::make_take_item_entity(7, 2, 1);
  const std::vector<uint8_t> want_take = {0x7F, 0x07, 0x02, 0x01};
  expect_bytes("take_item_entity bytes", take, want_take);

  // remove_entities (0x4d): a count then one varint per entity, so a
  // two-byte id round-trips.
  const auto remove = mc::server::make_remove_entity_ids({7, 300});
  const std::vector<uint8_t> want_remove = {0x4E, 0x02, 0x07, 0xAC, 0x02};
  expect_bytes("remove_entities bytes", remove, want_remove);

  if (g_failures == 0) {
    std::printf("PASS: drop-packet\t(add entity + set entity data + orb packets byte layout)\n");
    return 0;
  }
  std::printf("%d drop-packet check(s) failed\n", g_failures);
  return 1;
}
