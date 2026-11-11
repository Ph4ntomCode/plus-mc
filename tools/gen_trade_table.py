#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates `src/world/trade_table.h`: every villager trade in the pack, as data.

Trading in 26.2 is **data-driven**: `data/<ns>/trade_set/<profession>/level_N.json`
names a tag of `villager_trade` entries and how many of them a villager of that
profession and level picks. This tool walks that tree of the extracted version:

  * `trade_set/**/level_*.json` — the amount (a NumberProvider, constant in the
    pack), the trades tag and the random sequence;
  * `tags/villager_trade/**` — resolved recursively (a tag may name another
    tag: every smith level pulls `#minecraft:common_smith/level_N` first);
  * `villager_trade/**/*.json` — one trade: `wants`, optional `additional_wants`,
    `gives`, `max_uses`, `reputation_discount`, `xp`, and the optional dynamic
    pieces (`given_item_modifiers`, `merchant_predicate`,
    `double_trade_price_enchantments`).

What the table carries, and what it does not, is the whole point of the tool:

  * `wants` / `additional_wants` / `gives` item ids and counts, `max_uses`,
    `xp` (default 1, `VillagerTrade.CODEC`'s `lenientOptionalFieldOf`) and
    `reputation_discount` (which is **not** a discount: the codec hands it to
    `MerchantOffer`'s `priceMultiplier` argument) are exact. Every number in the
    pack is a constant provider, so none of them needs a loot engine.
  * `merchant_predicate` is modelled **only** when it is exactly the
    `minecraft:entity_properties` predicate on `minecraft:villager/variant`
    (sixteen cartographer banner trades and five fisherman boat trades are);
    the allowed types become a 7-bit `variant_mask`, so the trade is offered to
    the same villagers vanilla offers it to. Any other predicate is left to the
    runtime's null path, exactly as a predicate that *fails* would be.
  * a trade with `given_item_modifiers` or `double_trade_price_enchantments`
    cannot be reproduced without a loot-function engine (an enchanted-on-trade
    diamond chestplate, an explorer map, a dyed sheep's wool...), so it is
    emitted with `modelled = false`: the runtime treats it as the null offer
    vanilla's own `getOffer` returns when a modifier discards the stack. Four
    level pools are entirely unmodelled (armorer 4 and 5, toolsmith 5,
    weaponsmith 5) and are named in docs/PHASE4.md; every other level keeps its
    modelled subset, and the selection loop's count is whatever that subset can
    fill.

Usage:
    gen_trade_table.py <data dir> <out header>

e.g.
    gen_trade_table.py data/vanilla/26.2/data/minecraft src/world/trade_table.h
"""

import argparse
import json
import pathlib
import sys

# The seven villager types, in `VillagerType.bootstrap`'s registration order —
# the order the wire's registry ids come from. The generator needs it only to
# turn a variant predicate's names into mask bits.
TYPE_IDS = {
    "desert": 0,
    "jungle": 1,
    "plains": 2,
    "savanna": 3,
    "snow": 4,
    "swamp": 5,
    "taiga": 6,
}
VARIANT_ANY = 0x7F


def c_string(value):
    """A C++ string literal for a JSON identifier (only [a-z0-9_/:.] appears)."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def number(value, what, modelled):
    """A constant NumberProvider, as the int/float the pack spells."""
    if isinstance(value, (int, float)):
        return value
    print(f"  ! non-constant {what}: {value!r}", file=sys.stderr)
    modelled.append(f"provider:{what}")
    return None


class Table:
    def __init__(self, data: pathlib.Path):
        self.data = data
        self.trades = []
        self.sets = []
        self.unmodelled = 0
        self.predicates = 0
        self.skipped_levels = []

    def tag_values(self, tag: str):
        """The entries of `#minecraft:<tag>`, in file order, recursively."""
        path = self.data / "tags" / "villager_trade" / (tag + ".json")
        doc = json.loads(path.read_text())
        out = []
        for value in doc["values"]:
            if value.startswith("#"):
                out.extend(self.tag_values(value[len("#minecraft:") :]))
            else:
                out.append(value)
        return out

    def predicate_mask(self, predicate):
        """`entity_properties` on `villager/variant` -> a type mask, else None."""
        try:
            if predicate["condition"] != "minecraft:entity_properties":
                return None
            if predicate["entity"] != "this":
                return None
            sub = predicate["predicate"]["minecraft:predicates"]
            if list(sub.keys()) != ["minecraft:villager/variant"]:
                return None
            variant = sub["minecraft:villager/variant"]
            names = [variant] if isinstance(variant, str) else list(variant)
            mask = 0
            for name in names:
                short = name[len("minecraft:") :]
                if short not in TYPE_IDS:
                    return None
                mask |= 1 << TYPE_IDS[short]
            return mask
        except (KeyError, TypeError):
            return None

    def read_trade(self, trade_id: str):
        """One `villager_trade` entry as a row, or None when unmodelled."""
        path = self.data / "villager_trade" / (trade_id[len("minecraft:") :] + ".json")
        doc = json.loads(path.read_text())
        modelled = []
        mask = VARIANT_ANY
        if "given_item_modifiers" in doc:
            modelled.append("given_item_modifiers")
        if "double_trade_price_enchantments" in doc:
            modelled.append("double_trade_price_enchantments")
        if "merchant_predicate" in doc:
            got = self.predicate_mask(doc["merchant_predicate"])
            if got is None:
                modelled.append("merchant_predicate")
            else:
                mask = got
                self.predicates += 1
        known = {
            "gives",
            "max_uses",
            "reputation_discount",
            "wants",
            "xp",
            "given_item_modifiers",
            "additional_wants",
            "merchant_predicate",
            "double_trade_price_enchantments",
        }
        for key in doc:
            if key not in known:
                modelled.append(f"key:{key}")

        want = doc["wants"]
        want_count = number(want.get("count", 1), "wants.count", modelled)
        add = doc.get("additional_wants")
        add_count = None
        if add is not None:
            add_count = number(add.get("count", 1), "additional_wants.count", modelled)
        give = doc["gives"]
        give_count = number(give.get("count", 1), "gives.count", modelled)
        max_uses = number(doc.get("max_uses", 4), "max_uses", modelled)
        xp = number(doc.get("xp", 1), "xp", modelled)
        rep = number(doc.get("reputation_discount", 0), "reputation_discount", modelled)
        for part in ("wants", "additional_wants", "gives"):
            value = doc.get(part)
            if value is not None and "components" in value:
                modelled.append(f"{part}.components")

        row = {
            "name": trade_id,
            "want": want["id"],
            "want_count": want_count,
            "additional": add["id"] if add else None,
            "additional_count": add_count,
            "give": give["id"],
            "give_count": give_count,
            "max_uses": max_uses,
            "xp": xp,
            "reputation_discount": rep,
            "variant_mask": mask,
        }
        if modelled:
            self.unmodelled += 1
            row["modelled"] = False
        else:
            row["modelled"] = True
        self.trades.append(row)
        return row

    def read_set(self, path: pathlib.Path, profession: str, level: int):
        doc = json.loads(path.read_text())
        tag = doc["trades"]
        assert tag.startswith("#minecraft:"), tag
        entries = self.tag_values(tag[len("#minecraft:") :])
        first = len(self.trades)
        for entry in entries:
            self.read_trade(entry)
        count = len(self.trades) - first
        modelled_rows = [
            row for row in self.trades[first:] if row["modelled"]
        ]
        if not modelled_rows:
            self.skipped_levels.append((profession, level, count))
        amount = number(doc["amount"], "amount", [])
        self.sets.append(
            {
                "profession": profession,
                "level": level,
                "amount": amount,
                "first": first,
                "count": count,
            }
        )

    def run(self):
        for path in sorted((self.data / "trade_set").glob("*/level_*.json")):
            profession = path.parent.name
            level = int(path.stem.split("_")[1])
            self.read_set(path, profession, level)


def emit(table: Table, out: pathlib.Path):
    lines = []
    a = lines.append
    a("// SPDX-License-Identifier: GPL-3.0-or-later")
    a("// clang-format off")
    a("//")
    a(f"// The villager trade table ({len(table.trades)} trades, "
      f"{len(table.sets)} trade sets, generated — do not hand-edit).")
    a("//")
    a("// Every `trade_set/<profession>/level_<n>.json` in the 26.2 data pack,")
    a("// with its tag of `villager_trade` entries resolved in file order — a tag")
    a("// may name another tag, and every smith level names")
    a("// `#minecraft:common_smith/level_<n>` first. A row therefore repeats when")
    a("// two sets share a trade (the Smith entries), and the runtime never sees a")
    a("// tag. Column by column:")
    a("//")
    a("//   * `want`/`additional`/`give` are the item ids `wants`, `additional_wants`")
    a("//     and `gives` name, with their constant counts (`count` defaults to 1);")
    a("//     `additional` is null when the trade wants no second cost.")
    a("//   * `max_uses` and `xp` are `VillagerTrade.getOffer`'s clamped numbers —")
    a("//     `xp` defaults to 1 when the trade leaves it out (266 of 291 do), and")
    a("//     `reputation_discount` is what the codec hands MerchantOffer as its")
    a("//     `priceMultiplier` (it scales demand's price rise and the reputation")
    a("//     discount, it is not a discount by itself).")
    a("//   * `variant_mask` is set for the two `merchant_predicate` shapes the pack")
    a("//     uses — an `entity_properties` predicate on `minecraft:villager/variant`")
    a("//     (cartographer banners, fisherman boats): bit N set means villager type")
    a("//     N may buy it. `0x7F` is \"every type\", i.e. no predicate.")

    a("//   * `modelled` is false when the pack gives the trade a")
    a("//     `given_item_modifiers` or `double_trade_price_enchantments` entry, or")
    a("//     any predicate shape other than the one above: the offer needs a loot-")
    a("//     function engine this milestone does not have, so the runtime takes the")
    a("//     null path (`VillagerTrade.getOffer` returning null) for it. Levels")
    a(f"//     those trades leave empty: {len(table.skipped_levels)}.")
    a("")
    a("#pragma once")
    a("")
    a("#include <cstdint>")
    a("")
    a("namespace mc::world {")
    a("")
    a("// One `villager_trade` entry. `additional` is nullptr when absent.")
    a("struct GeneratedTrade {")
    a("  const char* name;")
    a("  const char* want;")
    a("  int32_t want_count;")
    a("  const char* additional;")
    a("  int32_t additional_count;")
    a("  const char* give;")
    a("  int32_t give_count;")
    a("  int32_t max_uses;")
    a("  int32_t xp;")
    a("  float reputation_discount;")
    a("  uint8_t variant_mask;")
    a("  bool modelled;")
    a("};")
    a("")
    a("// One `trade_set`: `amount` trades picked from rows [first, first + count).")
    a("struct GeneratedTradeSet {")
    a("  const char* profession;")
    a("  int32_t level;")
    a("  int32_t amount;")
    a("  int32_t first;")
    a("  int32_t count;")
    a("};")
    a("")
    a(f"inline constexpr GeneratedTrade kGeneratedTrades[] = {{")
    for row in table.trades:
        additional = c_string(row["additional"]) if row["additional"] else "nullptr"
        additional_count = row["additional_count"] if row["additional_count"] is not None else 0
        a(
            "    {%s, %s, %d, %s, %d, %s, %d, %d, %d, %gf, 0x%02X, %s},"
            % (
                c_string(row["name"]),
                c_string(row["want"]),
                row["want_count"],
                additional,
                additional_count,
                c_string(row["give"]),
                row["give_count"],
                row["max_uses"],
                row["xp"],
                row["reputation_discount"],
                row["variant_mask"],
                "true" if row["modelled"] else "false",
            )
        )
    a("};")
    a(f"inline constexpr int kGeneratedTradeCount = {len(table.trades)};")
    a("")
    a("inline constexpr GeneratedTradeSet kGeneratedTradeSets[] = {")
    for row in table.sets:
        a(
            '    {%s, %d, %d, %d, %d},'
            % (
                c_string(row["profession"]),
                row["level"],
                row["amount"],
                row["first"],
                row["count"],
            )
        )
    a("};")
    a(f"inline constexpr int kGeneratedTradeSetCount = {len(table.sets)};")
    a("")
    a("} // namespace mc::world")
    a("")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("data", type=pathlib.Path, help="the extracted data/minecraft dir")
    parser.add_argument("out", type=pathlib.Path, help="the header to write")
    args = parser.parse_args()

    table = Table(args.data)
    table.run()
    emit(table, args.out)
    modelled = len(table.trades) - table.unmodelled
    print(
        f"{args.out}: {len(table.trades)} trades ({modelled} modelled, "
        f"{table.unmodelled} not), {len(table.sets)} sets, "
        f"{table.predicates} variant predicates",
        file=sys.stderr,
    )
    for profession, level, count in table.skipped_levels:
        print(
            f"  ! {profession} level {level}: all {count} trades unmodelled",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
