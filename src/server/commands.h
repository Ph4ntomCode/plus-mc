// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc::server::cmd {

// Tokenization + tiny parser utilities shared between the in-server command
// dispatcher and the vanilla-jar report generators. No brigadier dependency;
// `/gamemode`, `/tp`, `/give`, `/time` cover the Milestone D surface.

// Splits `s` on whitespace (no quoting) and filters empties.
std::vector<std::string> split_ws(const std::string& s);

// Case-folded gamemode parse. Accepts "0"-"3", "s/c/a/sp", "survival", etc.
// Returns -1 when `s` is not a gamemode token.
int parse_gamemode_token(const std::string& s);

// Coordinate parse helpers for /tp: absolute or relative ("~" / "~N").
// Returns nullopt on bad syntax.
std::optional<double> parse_coord(const std::string& tok, double origin);
std::optional<float> parse_angle(const std::string& tok, float origin);

// Namespaced item/block id normalization: "stone" -> "minecraft:stone".
std::string normalize_id(const std::string& raw);

// /time argument parsing: "day"/"night"/"noon"/"midnight" or an integer
// (supports comma/_ separators like "1_000").
std::optional<int64_t> parse_time_value(const std::string& s);
int64_t time_marker_ticks(const std::string& marker);

// Validates an item count in [1, 64] (max stack size). Returns nullopt when invalid.
std::optional<int> parse_count(const std::string& s);

// Case-folded player-name comparison, which is how every command matches its
// target ("Notch" and "notch" are the same player to a command).
std::string lower_ascii(const std::string& s);
bool name_equals_ci(const std::string& a, const std::string& b);

// Whether a command's target names the executing player: `@s`, `@p`, or the
// player's own name. The selector forms are accepted (not resolved) because the
// command surface is self-targeted until permissions and the registry can
// address another session's state.
bool target_is_self(const std::string& target, const std::string& self_name);

} // namespace mc::server::cmd
