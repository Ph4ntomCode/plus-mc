// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte buffer codec for the Minecraft protocol. All multi-byte integers are
// big-endian. VarInt/VarLong follow the standard 7-bit continuation format.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::net {

// Encodes `value` as a VarInt. Returns the number of bytes written.
int encode_varint(uint32_t value, std::vector<uint8_t>& out);
// Encodes `value` as a VarLong. Returns the number of bytes written.
int encode_varlong(uint64_t value, std::vector<uint8_t>& out);

// Decodes a VarInt from `data` starting at `*pos`. On success advances `*pos`
// and returns true; on truncation or overflow returns false (pos untouched).
bool decode_varint(const std::vector<uint8_t>& data, size_t* pos, uint32_t* out);

// A text component tree, for the shapes the two simple writers below cannot
// express: a component that *nests* — an announcement wraps a translatable
// title inside a coloured bracket component. It mirrors the fields
// `ComponentSerialization`'s NBT codec reads: `text` for a literal, `translate`
// (with `with`) for a translatable, `color` for a style colour, and `extra` for
// the children a component appends. A field left empty is simply not written.
struct ComponentNode {
  std::string text;      // literal text (when `translate` is empty)
  std::string translate; // lang key (when `text` is empty)
  std::string color;     // a style colour name or #rrggbb
  bool has_bold = false;
  bool bold = false;
  bool has_italic = false;
  bool italic = false;
  bool has_underlined = false;
  bool underlined = false;
  bool has_strikethrough = false;
  bool strikethrough = false;
  bool has_obfuscated = false;
  bool obfuscated = false;
  std::string insertion;    // shift-click insertion
  std::string font;         // resource-pack font id
  std::string click_action; // open_url, run_command, ...
  std::string click_value;
  std::shared_ptr<ComponentNode> hover_text;
  std::vector<ComponentNode> with;  // a translatable's arguments
  std::vector<ComponentNode> extra; // appended children
};

// Growable writer. Appends length-prefixed strings (VarInt length + UTF-8).
class Writer {
public:
  void u8(uint8_t v) {
    buf_.push_back(v);
  }
  // The fixed-width writers grow once and store straight into the buffer. The
  // per-byte `push_back` form updated `size()` and re-checked capacity eight
  // times for a long, and a chunk body writes thousands of longs per send; the
  // bytes are identical either way.
  void u16(uint16_t v) {
    const size_t at = buf_.size();
    buf_.resize(at + 2);
    uint8_t* p = buf_.data() + at;
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
  }
  void u32(uint32_t v) {
    const size_t at = buf_.size();
    buf_.resize(at + 4);
    uint8_t* p = buf_.data() + at;
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
  }
  void i64(int64_t v) {
    u64(static_cast<uint64_t>(v));
  }
  void u64(uint64_t v) {
    const size_t at = buf_.size();
    buf_.resize(at + 8);
    uint8_t* p = buf_.data() + at;
    for (int shift = 56, i = 0; shift >= 0; shift -= 8, ++i)
      p[i] = static_cast<uint8_t>(v >> shift);
  }
  // A run of big-endian longs: one grow for the whole run. Chunk heightmaps and
  // a section's packed cells are the arrays this exists for.
  void i64s(const int64_t* values, size_t n) {
    if (n == 0)
      return;
    const size_t at = buf_.size();
    buf_.resize(at + n * 8);
    uint8_t* p = buf_.data() + at;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t v = static_cast<uint64_t>(values[i]);
      for (int shift = 56; shift >= 0; shift -= 8)
        *p++ = static_cast<uint8_t>(v >> shift);
    }
  }
  void f32(float v) {
    uint32_t bits;
    // Bit-copy float into a uint32_t (IEEE 754, big-endian on the wire).
    __builtin_memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }
  void f64(double v) {
    uint64_t bits;
    __builtin_memcpy(&bits, &v, sizeof(bits));
    u64(bits);
  }
  void varint(uint32_t v) {
    encode_varint(v, buf_);
  }
  void varlong(uint64_t v) {
    encode_varlong(v, buf_);
  }
  void string(const std::string& s) {
    varint(static_cast<uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  // --- Text components (network NBT) ---
  //
  // Most Component fields on the wire are NOT JSON strings: they are NBT tags
  // written by NbtIo.writeAnyTag, i.e. `u8 tag id` followed by the payload
  // with the root name omitted (NbtIo.writeAnyTag writes the id, returns for
  // TAG_End, then the tag body). The client reads them with
  // NbtIo.readAnyTag and decodes through ComponentSerialization.CODEC on
  // NbtOps. A bare TAG_String is the literal component
  // (Codec.either(Codec.STRING, ...) -> Component.literal); colouring needs
  // the compound form {text, color}.
  //
  // The login-state disconnect is the exception: ClientboundLoginDisconnect
  // Packet uses ByteBufCodecs.lenientJson, so that one really is a JSON string
  // (see send_disconnect_json in server/session.cpp).
  void component_text(const std::string& text, const std::string& color = {}) {
    const size_t n = text.size() > kMaxComponentString ? kMaxComponentString : text.size();
    if (color.empty()) {
      buf_.push_back(kTagString);
      u16(static_cast<uint16_t>(n));
      buf_.insert(buf_.end(), text.begin(), text.begin() + static_cast<ptrdiff_t>(n));
      return;
    }
    buf_.push_back(kTagCompound);
    named_string("text", text);
    named_string("color", color);
    buf_.push_back(0x00); // TAG_End
  }
  // A translatable component: {translate: <key>, with: [{text: <arg>}...]}.
  // This is Component.translatable(key, args) — the death messages are built
  // from one of these, which is why the wording comes from the client's own
  // language file and why an unknown key renders as the key itself.
  // A whole component *tree* (`ComponentNode`): the general form the two above
  // are special cases of. Recursive, so a child can nest as deeply as the
  // component it mirrors — an announcement's bracketed title is the one shape
  // in the tree today.
  void component(const ComponentNode& node) {
    buf_.push_back(kTagCompound);
    component_payload(node);
  }
  // A translatable component: {translate: <key>, with: [{text: <arg>}...]}.
  // This is Component.translatable(key, args) — the death messages are built
  // from one of these, which is why the wording comes from the client's own
  // language file and why an unknown key renders as the key itself.
  void component_translate(const std::string& key, const std::vector<std::string>& args) {
    buf_.push_back(kTagCompound);
    named_string("translate", key);
    if (!args.empty()) {
      buf_.push_back(kTagList);
      named("with");
      buf_.push_back(kTagCompound); // the list's element type
      u32(static_cast<uint32_t>(args.size()));
      // A TAG_List element is the payload alone: the element type is declared
      // once in the list header, and each element carries no tag id of its
      // own. Writing one here is not "a compound entry", it is a byte the
      // client reads as the first entry's type — which desynchronises the
      // whole component (the death screen's message and a chat announcement
      // both go out through this path).
      for (const std::string& arg : args) {
        named_string("text", arg);
        buf_.push_back(0x00); // TAG_End of this element
      }
    }
    buf_.push_back(0x00); // TAG_End of the root compound
  }
  void bytes(const uint8_t* p, size_t n) {
    buf_.insert(buf_.end(), p, p + n);
  }
  void bytes(const std::vector<uint8_t>& v) {
    bytes(v.data(), v.size());
  }
  void reserve(size_t n) {
    buf_.reserve(n);
  }
  // Empties the writer but keeps its capacity, so a buffer reused across a loop
  // (one chunk section after another) stops paying a fresh allocation a turn.
  void clear() {
    buf_.clear();
  }
  // Appends what has been written so far to `out` and leaves this writer empty
  // with its capacity intact. The staging pattern `encode_chunk_body` uses: one
  // reusable writer per section, appended into the section-array buffer.
  void append_to(std::vector<uint8_t>* out) const {
    out->insert(out->end(), buf_.begin(), buf_.end());
  }
  std::vector<uint8_t> take() && {
    return std::move(buf_);
  }

private:
  // NBT tag ids (net.minecraft.nbt.TagTypes).
  static constexpr uint8_t kTagString = 0x08;
  static constexpr uint8_t kTagList = 0x09;
  static constexpr uint8_t kTagCompound = 0x0A;
  // A TAG_String payload length is a u16.
  static constexpr size_t kMaxComponentString = 65535;

  // One NBT list of components under `name` — a translatable's `with`, or a
  // component's `extra`: `u8 tag id` + `u16 name len` + name + `u8 elem type`
  // + `i32 count` + each element (a compound).
  //
  // The element type is declared once, in the list header; each element is the
  // compound's payload only (`component_payload`). An element that wrote its
  // own tag id — which this did until the death-screen message was read against
  // vanilla's own NBT layout — is one byte too long and shifts every following
  // entry's type/name fields.
  void named_components(const char* name, const std::vector<ComponentNode>& nodes) {
    const std::string_view view(name);
    buf_.push_back(kTagList);
    u16(static_cast<uint16_t>(view.size()));
    buf_.insert(buf_.end(), view.begin(), view.end());
    buf_.push_back(kTagCompound); // the list's element type
    u32(static_cast<uint32_t>(nodes.size()));
    for (const ComponentNode& node : nodes)
      component_payload(node);
  }

  // A component compound's body: the same fields `component` writes, without
  // the leading tag id. `component` adds that id because a component sent as a
  // packet field is a *root* tag (`NbtIo.writeAnyTag` writes the id and omits
  // the name); a component inside a list is not a root, and writing the id
  // there corrupts the list.
  void component_payload(const ComponentNode& node) {
    if (!node.translate.empty())
      named_string("translate", node.translate);
    else
      named_string("text", node.text);
    if (!node.color.empty())
      named_string("color", node.color);
    if (node.has_bold)
      named_bool("bold", node.bold);
    if (node.has_italic)
      named_bool("italic", node.italic);
    if (node.has_underlined)
      named_bool("underlined", node.underlined);
    if (node.has_strikethrough)
      named_bool("strikethrough", node.strikethrough);
    if (node.has_obfuscated)
      named_bool("obfuscated", node.obfuscated);
    if (!node.insertion.empty())
      named_string("insertion", node.insertion);
    if (!node.font.empty())
      named_string("font", node.font);
    if (!node.click_action.empty()) {
      buf_.push_back(kTagCompound);
      named("clickEvent");
      named_string("action", node.click_action);
      named_string("value", node.click_value);
      buf_.push_back(0x00); // TAG_End
    }
    if (node.hover_text) {
      buf_.push_back(kTagCompound);
      named("hoverEvent");
      named_string("action", "show_text");
      buf_.push_back(kTagCompound);
      named("value");
      component_payload(*node.hover_text);
      buf_.push_back(0x00); // TAG_End of value
      buf_.push_back(0x00); // TAG_End of hoverEvent
    }
    if (!node.with.empty())
      named_components("with", node.with);
    if (!node.extra.empty())
      named_components("extra", node.extra);
    buf_.push_back(0x00); // TAG_End
  }

  // A bare (nameless) entry name: `u16 length` + bytes, used inside TAG_List.
  void named(std::string_view name) {
    u16(static_cast<uint16_t>(name.size()));
    buf_.insert(buf_.end(), name.begin(), name.end());
  }

  // A TAG_Byte boolean style field.
  void named_bool(std::string_view name, bool value) {
    buf_.push_back(0x01);
    u16(static_cast<uint16_t>(name.size()));
    buf_.insert(buf_.end(), name.begin(), name.end());
    buf_.push_back(value ? 1 : 0);
  }

  // One `u8 tag id` + `u16 name len` + name + payload entry of a compound.
  void named_string(std::string_view name, const std::string& value) {
    buf_.push_back(kTagString);
    u16(static_cast<uint16_t>(name.size()));
    buf_.insert(buf_.end(), name.begin(), name.end());
    const size_t n = value.size() > kMaxComponentString ? kMaxComponentString : value.size();
    u16(static_cast<uint16_t>(n));
    buf_.insert(buf_.end(), value.begin(), value.begin() + static_cast<ptrdiff_t>(n));
  }

  std::vector<uint8_t> buf_;
};

// Non-owning reader over a byte vector. Methods return false on truncation
// instead of throwing, so protocol code can close the connection cleanly.
class Reader {
public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

  bool remaining(size_t n) const {
    return pos_ + n <= data_.size();
  }
  size_t position() const {
    return pos_;
  }

  bool u8(uint8_t* out) {
    if (!remaining(1))
      return false;
    *out = data_[pos_++];
    return true;
  }
  bool u16(uint16_t* out) {
    if (!remaining(2))
      return false;
    *out = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
    pos_ += 2;
    return true;
  }
  bool u32(uint32_t* out) {
    if (!remaining(4))
      return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v = (v << 8) | data_[pos_ + static_cast<size_t>(i)];
    *out = v;
    pos_ += 4;
    return true;
  }
  bool i64(int64_t* out) {
    uint64_t v = 0;
    if (!u64(&v))
      return false;
    *out = static_cast<int64_t>(v);
    return true;
  }
  bool f32(float* out) {
    uint32_t bits = 0;
    if (!u32(&bits))
      return false;
    __builtin_memcpy(out, &bits, sizeof(bits));
    return true;
  }
  bool f64(double* out) {
    uint64_t bits = 0;
    if (!u64(&bits))
      return false;
    __builtin_memcpy(out, &bits, sizeof(bits));
    return true;
  }
  // Reads the next `n` bytes into `out`. Returns false on truncation.
  bool bytes(uint32_t n, std::vector<uint8_t>* out) {
    if (!remaining(n))
      return false;
    out->assign(data_.begin() + static_cast<ptrdiff_t>(pos_),
                data_.begin() + static_cast<ptrdiff_t>(pos_ + n));
    pos_ += n;
    return true;
  }
  // Appends the remaining unread bytes to `out`. Always succeeds.
  void rest(std::vector<uint8_t>* out) const {
    out->insert(out->end(), data_.begin() + static_cast<ptrdiff_t>(pos_), data_.end());
  }
  bool u64(uint64_t* out) {
    if (!remaining(8))
      return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v = (v << 8) | data_[pos_ + static_cast<size_t>(i)];
    *out = v;
    pos_ += 8;
    return true;
  }
  bool varint(uint32_t* out) {
    return decode_varint(data_, &pos_, out);
  }
  bool string(std::string* out) {
    uint32_t len = 0;
    if (!varint(&len))
      return false;
    if (!remaining(len))
      return false;
    out->assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    return true;
  }
  bool at_end() const {
    return pos_ == data_.size();
  }

private:
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
};

} // namespace mc::net
