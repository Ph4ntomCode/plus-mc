// SPDX-License-Identifier: GPL-3.0-or-later
//
// Text components on the wire are NOT JSON strings. Almost every Component
// field is encoded with NbtIo.writeAnyTag, i.e.
//
//     u8 tag id, then the payload with the root name omitted
//
// and decoded by the client through ComponentSerialization.CODEC on NbtOps
// (net.minecraft.network.FriendlyByteBuf.writeNbt -> NbtIo.writeAnyTag).
// A bare TAG_String is the literal component; colouring needs the compound
// {text, color} form.
//
// The regression this guards: writing `varint(len) + utf8` (a length-prefixed
// JSON string, the pre-fix encoding) makes the client read the length as an
// NBT tag id, fail TagTypes.getType, and drop the player with
//   DecoderException: Failed to decode packet
//   'clientbound/minecraft:system_chat'
// which is exactly what happened the first time a real client chatted.
//
// The login-state disconnect is deliberately different (lenientJson), so it is
// asserted here too: that one MUST stay a VarInt-prefixed JSON string.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "net/byte_buffer.h"

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

// Byte helpers that spell out the expected wire layout below.
std::vector<uint8_t> utf16(const std::string& s) {
  return {static_cast<uint8_t>(s.size() >> 8), static_cast<uint8_t>(s.size() & 0xFF)};
}
std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::string& s) {
  a.insert(a.end(), s.begin(), s.end());
  return a;
}
// One compound entry: tag id + name + payload.
std::vector<uint8_t> named(const std::string& name, const std::string& value) {
  return cat(cat(cat(cat({0x08}, utf16(name)), name), utf16(value)), value);
}

} // namespace

int main() {
  // --- Literal component: TAG_String, no root name --------------------------
  {
    mc::net::Writer w;
    w.component_text("hi");
    const std::vector<uint8_t> want = {
        0x08,       // TAG_String
        0x00, 0x02, // u16 length
        'h',  'i',
    };
    expect_bytes("literal component", std::move(w).take(), want);
  }

  // --- Coloured component: TAG_Compound {text, color} + TAG_End -------------
  {
    mc::net::Writer w;
    w.component_text("hi", "gray");
    std::vector<uint8_t> expect = {0x0A};
    expect = cat(std::move(expect), named("text", "hi"));
    expect = cat(std::move(expect), named("color", "gray"));
    expect.push_back(0x00); // TAG_End
    expect_bytes("coloured component", std::move(w).take(), expect);
  }

  // --- The payload starts with a tag id, not a length ----------------------
  // The pre-fix encoding emitted 0x02 ('hi' length) first, which the client
  // reads as NBT tag id 2 = TAG_Short -> DecoderException. Assert the first
  // byte is a tag id the client accepts for a component.
  {
    mc::net::Writer w;
    w.component_text("hello world");
    const auto bytes = std::move(w).take();
    if (bytes.empty() || (bytes[0] != 0x08 && bytes[0] != 0x0A)) {
      ++g_failures;
      std::printf("FAIL: component does not start with an NBT tag id (got 0x%02x — a length?\n",
                  bytes.empty() ? 0 : bytes[0]);
      std::printf("      this is the length-prefixed-string encoding that kicks clients)\n");
    }
  }

  // --- Empty text still encodes a valid literal ----------------------------
  {
    mc::net::Writer w;
    w.component_text("");
    expect_bytes("empty literal", std::move(w).take(), {0x08, 0x00, 0x00});
  }

  // --- A translatable with arguments: the list holds payloads, not tags ----
  // `Component.translatable(key, args)` is the shape a death message and a
  // chat announcement use, and its `with` is a TAG_List of compounds. An NBT
  // list declares its element type once, in the header; each element is the
  // compound's own entries and TAG_End, with **no** leading tag id. The
  // pre-fix encoder wrote 0x0A in front of every element, which the client
  // reads as the first entry's type and name length — a corrupt tag tree on
  // the packet that puts a player on the death screen.
  {
    mc::net::Writer w;
    w.component_translate("death.attack.player", {"Victim", "Killer"});
    std::vector<uint8_t> expect = {0x0A};
    expect = cat(std::move(expect), named("translate", "death.attack.player"));
    expect.push_back(0x09); // TAG_List
    expect = cat(expect, utf16("with"));
    expect = cat(expect, std::string("with"));
    expect.push_back(0x0A); // the list's element type
    expect = cat(expect, std::vector<uint8_t>{0x00, 0x00, 0x00, 0x02});
    for (const char* arg : {"Victim", "Killer"}) {
      expect = cat(std::move(expect), named("text", arg));
      expect.push_back(0x00); // TAG_End of this element
    }
    expect.push_back(0x00); // TAG_End of the root compound
    expect_bytes("translatable with args", std::move(w).take(), expect);
  }

  // --- A component tree: `extra` nests payloads the same way ---------------
  {
    mc::net::ComponentNode child;
    child.text = "!";
    mc::net::ComponentNode root;
    root.text = "[";
    root.color = "green";
    root.extra = {child};
    mc::net::Writer w;
    w.component(root);
    std::vector<uint8_t> expect = {0x0A};
    expect = cat(std::move(expect), named("text", "["));
    expect = cat(std::move(expect), named("color", "green"));
    expect.push_back(0x09);
    expect = cat(expect, utf16("extra"));
    expect = cat(expect, std::string("extra"));
    expect.push_back(0x0A);
    expect = cat(expect, std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01});
    expect = cat(std::move(expect), named("text", "!"));
    expect.push_back(0x00);
    expect.push_back(0x00);
    expect_bytes("component tree", std::move(w).take(), expect);
  }

  // --- Login-state reason stays a VarInt-prefixed JSON string --------------
  // ClientboundLoginDisconnectPacket uses ByteBufCodecs.lenientJson, so the
  // fix must NOT have touched this path.
  {
    mc::net::Writer w;
    w.string("{\"text\":\"nope\"}");
    const std::vector<uint8_t> want = {
        0x0F, // varint length 15
        '{',  '"', 't', 'e', 'x', 't', '"', ':', '"', 'n', 'o', 'p', 'e', '"', '}',
    };
    expect_bytes("login disconnect reason (JSON)", std::move(w).take(), want);
  }

  if (g_failures == 0) {
    std::printf("PASS: component\t(network-NBT text components)\n");
    return 0;
  }
  std::printf("%d component check(s) failed\n", g_failures);
  return 1;
}
