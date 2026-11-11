#!/usr/bin/env python3
"""Phase 8 slice 3: link and execute a component's canon-lifted lifecycle.

Every fixture is a spec-shaped component assembled in Python: component types,
instance imports, member aliases, `canon lower`, a nested core module, a core
instantiation, core-export aliases and `canon lift`. The linker must bind the one hundred and thirty tracked Pumpkin v0.2 members the host can actually carry
(`server.get-player-count`, `server.get-player-by-name`, `server.broadcast`,
`world.get-time-of-day`, `world.is-raining`, `world.is-thundering`,
`logging.log`, `logging.log-tracing`, `world.get-block-state-id`,
`world.get-block`, `world.set-block`, `player.get-id`, `player.get-name`, `player.get-position`,
`player.get-yaw`, `player.get-pitch`, `player.get-gamemode`, `player.get-health`,
`player.get-max-health`, `player.get-food-level`, `player.get-saturation`,
`player.get-exhaustion`, `player.get-absorption`, `player.get-experience-level`,
`player.get-experience-progress`,
`player.get-experience-points`, `player.get-camera-entity-id`, `player.chat`, `player.send-system-message`,
and the `text-component` builders through hover text), run the module against those
bindings,
and refuse the wider shapes by name. Check 17 pins a direct `plusmc:host` import
whose UTF-8 string parameter is read from the module's memory and reaches
`plusmc:host/log`; check 18 does the same through Pumpkin's enum-plus-string
`logging.log`; check 19 reads a lifted `string` result from its return area and
runs the lift's post-return function; checks 20-22 drive the real v0.2 async
lifecycle (`init-plugin`, async `on-load`/`on-unload`, `task.return`, and
`resource.drop`) through success, an error result, and a missing
`task.return`; checks 23-25 pin a non-function function import, a duplicate
instance member, and a string `task.return` without module memory. Check 26 drives
a lowered string result, where the host allocates the return area and the UTF-8 bytes
through the module's `cabi_realloc`; check 28 does the same for a `list<string>`
result, allocating the canonical pair array and each key, and check 30 does it for a
two-string record. Check 27 runs a checked-in compiler-produced component with the full 55-import v0.2
world through the explicit compiled-core path; check 29 runs a component produced from Rust
by `wasm-tools component embed` and `component new`, including the spaced lift indices
that `wit-component` emits. Check 34 runs a checked-in `cargo component` artifact built
from the real v0.2 package/interface/member names with a synchronous lifecycle: its
`context` -> `server` -> `world` resource chain, six reads, `logging.log`, the compiled
async `player.chat` and `world.set-block` imports, and both callbacks must reach the
host. Check 35 drives the async `player.chat` member through a synchronous lower, and
check 36 drives the async `world.set-block` member with the full block record. Check 37
replays the checked-in artifact with `--host-player` to exercise `server.broadcast`,
the player lookup, the live pose/yaw/pitch reads, the text resource, and
`player.send-system-message`. Check 38
lowers async `player.chat` and `world.set-block` through the task ABI, joins both
subtasks, and waits for both returned events. Check 40 strictly lowers the item-stack
constructor plus option, attribute-modifier, NBT and data-component shapes in a
component small enough that it must use the strict path rather than the compiler-artifact
compiled-core path. Checks 41 and 42 strictly lower i18n.translate and uuid.to-string
and lift their returned strings back into the report; check 43 calls player.get-name
through an async context -> server -> host-issued player handle chain, so the name is
never read from a fabricated handle.

usage: wasm_component_run_test.py <plusmc-wasm>
"""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 60

MAGIC = b"\x00asm"
CORE_VERSION = b"\x01\x00\x00\x00"
COMPONENT_VERSION = b"\x0d\x00\x01\x00"

I32 = 0x7F
I64 = 0x7E

SERVER = "pumpkin:plugin/server@0.2.0"
CONTEXT = "pumpkin:plugin/context@0.2.0"
I18N = "pumpkin:plugin/i18n@0.2.0"
UUID = "pumpkin:plugin/uuid@0.2.0"
WORLD = "pumpkin:plugin/world@0.2.0"
PLAYER = "pumpkin:plugin/player@0.2.0"
LOGGING = "pumpkin:plugin/logging@0.2.0"
LOG = "log"

PLAYER_COUNT = "[method]server.get-player-count"
TIME_OF_DAY = "[method]world.get-time-of-day"
IS_RAINING = "[method]world.is-raining"
IS_THUNDERING = "[method]world.is-thundering"
BLOCK_STATE_ID = "[method]world.get-block-state-id"
SET_BLOCK = "[method]world.set-block"
PLAYER_CHAT = "[method]player.chat"


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"{Path(cli).name} exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    return proc


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if (value == 0 and not byte & 0x40) or (value == -1 and byte & 0x40):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def name(text):
    encoded = text.encode("utf-8") if isinstance(text, str) else text
    return uleb(len(encoded)) + encoded


def vec(items):
    return uleb(len(items)) + b"".join(items)


def section(section_id, payload):
    return bytes([section_id]) + uleb(len(payload)) + payload


def module(sections):
    return MAGIC + CORE_VERSION + b"".join(sections)


def component(*sections):
    return MAGIC + COMPONENT_VERSION + b"".join(sections)


def core_functype(params=(), results=()):
    return b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])


def body(code):
    payload = b"\x00" + code  # no locals
    return uleb(len(payload)) + payload


def core_module(imports=(), on_load_params=(), on_load_code=None, memory=False, extra_funcs=False):
    """A nested core module.

    `imports` is [(module, field, results)]. The on-load body calls each import
    in order and drops its result, which is enough to prove every lower reached
    the host. `extra_funcs` appends a realloc and a post-return function so the
    canonical options can name real indices.
    """
    types = []
    import_entries = []
    for module_name, field, results in imports:
        types.append(core_functype((), results))
        import_entries.append(name(module_name) + name(field) + b"\x00" + uleb(len(types) - 1))
    life_type = len(types)
    types.append(core_functype(on_load_params, ()))
    imported = len(imports)
    funcs = [life_type, life_type, life_type]
    if on_load_code is None:
        on_load_code = b""
        for index, (_, _, results) in enumerate(imports):
            on_load_code += b"\x10" + uleb(index)  # call
            if results:
                on_load_code += b"\x1a"  # drop
    on_load_code += b"\x0b"
    code = [body(on_load_code), body(b"\x0b"), body(b"\x0b")]
    if extra_funcs:
        realloc_type = len(types)
        types.append(core_functype((I32, I32, I32, I32), (I32,)))
        post_type = len(types)
        types.append(core_functype((), ()))
        funcs += [realloc_type, post_type]
        code += [body(b"\x41" + sleb(0) + b"\x0b"), body(b"\x0b")]
    sections = [section(1, vec(types))]
    if import_entries:
        sections.append(section(2, vec(import_entries)))
    sections.append(section(3, vec([uleb(t) for t in funcs])))
    if memory:
        sections.append(section(5, vec([b"\x00\x01"])))  # one memory, min 1 page
    exports = [
        name("on-load") + b"\x00" + uleb(imported),
        name("on-enable") + b"\x00" + uleb(imported + 1),
        name("on-disable") + b"\x00" + uleb(imported + 2),
    ]
    sections.append(section(7, vec(exports)))
    sections.append(section(10, vec(code)))
    return module(sections)


# --- component layer -------------------------------------------------------


def comp_func(results=()):
    if not results:
        return b"\x40\x00\x01\x00"  # no params, no results
    return b"\x40\x00\x00" + bytes([results[0]])  # no params, one unnamed result


def instance_type(members):
    """members: [(export name, component function type index)]."""
    declarations = []
    for member, type_index in members:
        declarations.append(b"\x04\x00" + name(member) + b"\x01" + uleb(type_index))
    return b"\x42" + vec(declarations)


def type_section(entries):
    return section(7, vec(entries))


def instance_import(text, type_index):
    return b"\x00" + name(text) + b"\x05" + uleb(type_index)


def func_alias(instance, member):
    return b"\x01\x00" + uleb(instance) + name(member)


def canon_options(items=()):
    return vec(list(items))


def canon_lift(core_function, type_index=0, options=()):
    return b"\x00\x00" + uleb(core_function) + canon_options(options) + uleb(type_index)


def canon_lower(function, options=()):
    return b"\x01\x00" + uleb(function) + canon_options(options)


def core_inline(exports):
    return b"\x01" + vec([name(text) + b"\x00" + uleb(index) for text, index in exports])


def core_instantiate(module_index, args):
    return b"\x00" + uleb(module_index) + vec(
        [name(text) + b"\x12" + uleb(instance) for text, instance in args]
    )


def core_instance_section(defs):
    return section(2, vec(defs))


def core_alias(instance, export_name):
    return b"\x00\x00\x01" + uleb(instance) + name(export_name)


def component_export(text, sort=0x01, index=0):
    return b"\x00" + name(text) + bytes([sort]) + uleb(index) + b"\x00"


def expect_error(proc, *fragments):
    for fragment in fragments:
        if fragment not in proc.stderr:
            raise AssertionError(
                f"stderr does not contain {fragment!r}:\n{proc.stderr}\nstdout:\n{proc.stdout}"
            )


def hosted_component(core=None, lift_options=(), export_names=("on-load", "on-enable", "on-disable")):
    """The linked shape with no component imports: instantiate, alias, lift."""
    core = core if core is not None else core_module()
    parts = [
        type_section([comp_func()]),
        section(1, core),
        core_instance_section([core_instantiate(0, [])]),
        section(
            6,
            vec(
                [
                    core_alias(0, "on-load"),
                    core_alias(0, "on-enable"),
                    core_alias(0, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(0, 0, lift_options),
                    canon_lift(1),
                    canon_lift(2),
                ]
            ),
        ),
    ]
    exports = [component_export(text, index=index) for index, text in enumerate(export_names)]
    parts.append(section(11, vec(exports)))
    return component(*parts)


def member_component(interface, member, results=(I32,)):
    """One imported member, lowered and called by on-load."""
    core = core_module(imports=[(interface, member, results)])
    component_result = (0x79,) if results else ()
    return component(
        type_section([comp_func(component_result), instance_type([(member, 0)])]),
        section(10, vec([instance_import(interface, 1)])),
        section(6, vec([func_alias(0, member)])),
        section(8, vec([canon_lower(0)])),
        section(1, core),
        core_instance_section(
            [core_inline([(member, 0)]), core_instantiate(0, [(interface, 0)])]
        ),
    )


def four_member_component(player_result=I32, lift_options=(), memory=False):
    """The four tracked members the host can back, each called by on-load."""
    types = [
        comp_func(),
        comp_func((0x79,)),  # u32
        comp_func((0x77,)),  # u64 (component primitive encoding)
        comp_func((0x7F,)),  # bool
        instance_type([(PLAYER_COUNT, 1)]),
        instance_type([(TIME_OF_DAY, 2), (IS_RAINING, 3), (IS_THUNDERING, 3)]),
    ]
    core = core_module(
        imports=[
            (SERVER, PLAYER_COUNT, (player_result,)),
            (WORLD, TIME_OF_DAY, (I64,)),
            (WORLD, IS_RAINING, (I32,)),
            (WORLD, IS_THUNDERING, (I32,)),
        ],
        memory=memory,
    )
    return component(
        type_section(types),
        section(10, vec([instance_import(SERVER, 4), instance_import(WORLD, 5)])),
        section(
            6,
            vec(
                [
                    func_alias(0, PLAYER_COUNT),
                    func_alias(1, TIME_OF_DAY),
                    func_alias(1, IS_RAINING),
                    func_alias(1, IS_THUNDERING),
                ]
            ),
        ),
        section(8, vec([canon_lower(0), canon_lower(1), canon_lower(2), canon_lower(3)])),
        section(1, core),
        core_instance_section(
            [
                core_inline([(PLAYER_COUNT, 0)]),
                core_inline([(TIME_OF_DAY, 1), (IS_RAINING, 2), (IS_THUNDERING, 3)]),
                core_instantiate(0, [(SERVER, 0), (WORLD, 1)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(2, "on-load"),
                    core_alias(2, "on-enable"),
                    core_alias(2, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(4, 0, lift_options),
                    canon_lift(5),
                    canon_lift(6),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=4),
                    component_export("on-enable", index=5),
                    component_export("on-disable", index=6),
                ]
            ),
        ),
    )


def option_player_link_component():
    """Strictly lower server.get-player-by-name -> option<player>.

    The core import takes the explicit 8-byte option return area as its fourth
    parameter, so the linker must understand option<handle> rather than
    refusing the component.
    """
    server_resource = b"\x3f\x7f\x00"
    player_resource = b"\x3f\x7f\x00"
    get_player = (
        b"\x40\x02"
        + name("self")
        + b"\x69\x00"
        + name("name")
        + b"\x73"
        + b"\x00"
        + b"\x6b\x69\x01"
    )
    core = module(
        [
            section(
                1,
                vec([
                    core_functype((I32, I32, I32, I32), ()),
                    core_functype((), ()),
                ]),
            ),
            section(
                2,
                vec([name(SERVER) + name("[method]server.get-player-by-name") + b"\x00\x00"]),
            ),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec([
                    name("on-load") + b"\x00" + uleb(1),
                    name("on-enable") + b"\x00" + uleb(2),
                    name("on-disable") + b"\x00" + uleb(3),
                ]),
            ),
            section(10, vec([body(b"\x0b"), body(b"\x0b"), body(b"\x0b")])),
        ]
    )
    return component(
        type_section(
            [
                server_resource,
                player_resource,
                get_player,
                instance_type([("[method]server.get-player-by-name", 2)]),
                comp_func(),
            ]
        ),
        section(10, vec([instance_import(SERVER, 3)])),
        section(6, vec([func_alias(0, "[method]server.get-player-by-name")])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [
                core_inline([("[method]server.get-player-by-name", 0)]),
                core_instantiate(0, [(SERVER, 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 4), canon_lift(2, 4), canon_lift(3, 4)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def strict_i18n_result_component():
    """Strictly lower i18n.load-translations and i18n.translate -> string.

    The lifecycle returns the translated text through the guest's return area,
    so the test observes the host's string result rather than only a host call.
    """
    namespace = b"strict"
    json_text = b'{"greeting":"hello from strict i18n"}'
    key = b"strict:greeting"
    locale = b"\x6d" + vec([name("en-us")])
    load_func = (
        b"\x40\x03"
        + name("namespace")
        + b"\x73"
        + name("json")
        + b"\x73"
        + name("locale")
        + b"\x00"
        + b"\x01\x00"
    )
    translate_func = (
        b"\x40\x02"
        + name("key")
        + b"\x73"
        + name("locale")
        + b"\x00"
        + b"\x00\x73"
    )
    i18n_instance = instance_type([("load-translations", 1), ("translate", 2)])
    on_load_type = b"\x40\x00\x00\x73"
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32, I32, I32), ()),  # load
                        core_functype((I32, I32, I32, I32), ()),  # translate
                        core_functype((), (I32,)),  # on-load
                        core_functype((I32, I32, I32, I32), (I32,)),  # realloc
                        core_functype((), ()),  # on-enable / on-disable
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name(I18N) + name("load-translations") + b"\x00" + uleb(0),
                        name(I18N) + name("translate") + b"\x00" + uleb(1),
                    ]
                ),
            ),
            section(3, vec([uleb(2), uleb(3), uleb(4), uleb(4)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(2),
                        name("on-enable") + b"\x00" + uleb(4),
                        name("on-disable") + b"\x00" + uleb(5),
                        name("cabi_realloc") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # load-translations(namespace, json, en-us)
                        body(
                            b"\x41"
                            + sleb(0)
                            + b"\x41"
                            + sleb(len(namespace))
                            + b"\x41"
                            + sleb(16)
                            + b"\x41"
                            + sleb(len(json_text))
                            + b"\x41\x00\x10\x00"
                            # translate(key, en-us) into the pair at 64
                            b"\x41"
                            + sleb(80)
                            + b"\x41"
                            + sleb(len(key))
                            + b"\x41\x00\x41"
                            + sleb(64)
                            + b"\x10\x01\x41"
                            + sleb(64)
                            + b"\x0b"
                        ),
                        body(b"\x41" + sleb(128) + b"\x0b"),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
            section(
                11,
                vec(
                    [
                        active_data(0, namespace),
                        active_data(16, json_text),
                        active_data(80, key),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section([locale, load_func, translate_func, i18n_instance, on_load_type, comp_func()]),
        section(10, vec([instance_import(I18N, 3)])),
        section(
            6,
            vec([func_alias(0, "load-translations"), func_alias(0, "translate")]),
        ),
        section(
            8,
            vec([
                canon_lower(0, [b"\x03" + uleb(0)]),
                canon_lower(1, [b"\x03" + uleb(0), b"\x04" + uleb(3)]),
            ]),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline([("load-translations", 0), ("translate", 1)]),
                core_instantiate(0, [(I18N, 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(2, 4, [b"\x03" + uleb(0)]),
                    canon_lift(3, 5),
                    canon_lift(4, 5),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=2),
                    component_export("on-enable", index=3),
                    component_export("on-disable", index=4),
                ]
            ),
        ),
    )


def strict_uuid_result_component():
    """Strictly lower uuid.to-string through its two-u64 record parameter."""
    uuid_type = b"\x72" + vec([name("high") + b"\x77", name("low") + b"\x77"])
    to_string_type = (
        b"\x40\x01" + name("id") + uleb(0) + b"\x00\x73"
    )
    uuid_instance = instance_type([("to-string", 1)])
    on_load_type = b"\x40\x00\x00\x73"
    high = int("123e4567e89b12d3", 16)
    low = int("a456426614174000", 16)
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I64, I64, I32), ()),
                        core_functype((), (I32,)),
                        core_functype((I32, I32, I32, I32), (I32,)),
                        core_functype((), ()),
                    ]
                ),
            ),
            section(2, vec([name(UUID) + name("to-string") + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(2), uleb(3), uleb(3)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(3),
                        name("on-disable") + b"\x00" + uleb(4),
                        name("cabi_realloc") + b"\x00" + uleb(2),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        body(
                            b"\x42"
                            + sleb(high)
                            + b"\x42"
                            + sleb(low if low < 2**63 else low - 2**64)
                            + b"\x41"
                            + sleb(64)
                            + b"\x10\x00\x41"
                            + sleb(64)
                            + b"\x0b"
                        ),
                        body(b"\x41" + sleb(128) + b"\x0b"),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section([uuid_type, to_string_type, uuid_instance, on_load_type, comp_func()]),
        section(10, vec([instance_import(UUID, 2)])),
        section(6, vec([func_alias(0, "to-string")])),
        section(
            8,
            vec([canon_lower(0, [b"\x03" + uleb(0), b"\x04" + uleb(2)])]),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline([("to-string", 0)]),
                core_instantiate(0, [(UUID, 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(1, 3, [b"\x03" + uleb(0)]),
                    canon_lift(2, 4),
                    canon_lift(3, 4),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def strict_player_name_component():
    """Strictly call player.get-name on a host-issued async handle chain.

    The component's async on-load receives a real v0.2 context, asks it for the
    server, looks up Buffy, and calls get-name through the returned typed player
    handle. No guest-side handle is fabricated, and the host's string result is
    logged so the test sees the exact canonical-ABI bytes.
    """
    def body_with_local(code):
        payload = b"\x01\x01\x7f" + code
        return uleb(len(payload)) + payload

    context_resource = b"\x3f\x7f\x00"
    server_resource = b"\x3f\x7f\x00"
    player_resource = b"\x3f\x7f\x00"
    get_server = (
        b"\x40\x01" + name("self") + b"\x68\x00" + b"\x00" + uleb(1)
    )
    get_player = (
        b"\x40\x02"
        + name("self")
        + b"\x68\x01"
        + name("name")
        + b"\x73"
        + b"\x00"
        + b"\x6b\x69\x02"
    )
    get_name = (
        b"\x40\x01" + name("self") + b"\x68\x02" + b"\x00\x73"
    )
    async_load = (
        b"\x43"
        + vec([name("context") + b"\x69\x00"])
        + b"\x00"
        + b"\x6a\x00\x01\x73"
    )
    init_func = b"\x40\x00\x01\x00"
    level = b"\x6d" + vec(
        [name("trace"), name("debug"), name("info"), name("warn"), name("error")]
    )
    log_func = (
        b"\x40\x02"
        + name("level")
        + uleb(11)
        + name("message")
        + b"\x73"
        + b"\x01\x00"
    )
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32,), (I32,)),  # get-server
                        core_functype((I32, I32, I32, I32), ()),  # get-player
                        core_functype((I32, I32), ()),  # get-name
                        core_functype((I32, I32, I32), ()),  # log
                        core_functype((I32, I32, I32), ()),  # task.return
                        core_functype((I32,), ()),  # resource.drop
                        core_functype((), ()),  # init
                        core_functype((I32,), ()),  # load / unload
                        core_functype((I32, I32, I32, I32), (I32,)),  # realloc
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name(CONTEXT) + name("[method]context.get-server") + b"\x00" + uleb(0),
                        name(SERVER) + name("[method]server.get-player-by-name") + b"\x00" + uleb(1),
                        name(PLAYER) + name("[method]player.get-name") + b"\x00" + uleb(2),
                        name(LOGGING) + name(LOG) + b"\x00" + uleb(3),
                        name("$root") + name("[task-return]on-load") + b"\x00" + uleb(4),
                        name("$root") + name("[resource-drop]context") + b"\x00" + uleb(5),
                    ]
                ),
            ),
            section(3, vec([uleb(6), uleb(7), uleb(7), uleb(8)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("init-plugin") + b"\x00" + uleb(6),
                        name("on-load") + b"\x00" + uleb(7),
                        name("on-unload") + b"\x00" + uleb(8),
                        name("cabi_realloc") + b"\x00" + uleb(9),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        body(b"\x0b"),
                        body_with_local(
                            # server = ctx.get-server()
                            b"\x20\x00\x10\x00\x21\x01"
                            # server.get-player-by-name(server, "Buffy", option area 64)
                            b"\x20\x01\x41"
                            + sleb(32)
                            + b"\x41"
                            + sleb(5)
                            + b"\x41"
                            + sleb(64)
                            + b"\x10\x01"
                            # player.get-name(handle at 68, string area 72)
                            + b"\x41"
                            + sleb(68)
                            + b"\x28\x02\x00\x41"
                            + sleb(72)
                            + b"\x10\x02"
                            # log(level=info, ptr, len) from the returned pair
                            + b"\x41\x02\x41"
                            + sleb(72)
                            + b"\x28\x02\x00\x41"
                            + sleb(76)
                            + b"\x28\x02\x00\x10\x03"
                            # task.return(ok) and drop the context
                            b"\x41\x00\x41\x00\x41\x00\x10\x04"
                            b"\x20\x00\x10\x05\x0b"
                        ),
                        body(
                            b"\x41\x00\x41\x00\x41\x00\x10\x04"
                            b"\x20\x00\x10\x05\x0b"
                        ),
                        body(b"\x41" + sleb(128) + b"\x0b"),
                    ]
                ),
            ),
            section(11, vec([active_data(32, b"Buffy")])),
        ]
    )
    task_return = b"\x09" + b"\x00" + b"\x6a\x00\x01\x73" + vec([b"\x03\x00"])
    resource_drop = b"\x03" + uleb(0)
    root_inline = core_inline(
        [("[task-return]on-load", 0), ("[resource-drop]context", 1)]
    )
    context_inline = core_inline([("[method]context.get-server", 2)])
    server_inline = core_inline([("[method]server.get-player-by-name", 3)])
    player_inline = core_inline([("[method]player.get-name", 4)])
    logging_inline = core_inline([(LOG, 5)])
    instantiate = core_instantiate(
        0,
        [
            ("$root", 0),
            (CONTEXT, 1),
            (SERVER, 2),
            (PLAYER, 3),
            (LOGGING, 4),
        ],
    )
    return component(
        type_section(
            [
                context_resource,
                server_resource,
                player_resource,
                get_server,
                get_player,
                get_name,
                async_load,
                init_func,
                instance_type([("[method]context.get-server", 3)]),
                instance_type([("[method]server.get-player-by-name", 4)]),
                instance_type([("[method]player.get-name", 5)]),
                level,
                log_func,
                instance_type([(LOG, 12)]),
            ]
        ),
        section(
            10,
            vec(
                [
                    instance_import(CONTEXT, 8),
                    instance_import(SERVER, 9),
                    instance_import(PLAYER, 10),
                    instance_import(LOGGING, 13),
                ]
            ),
        ),
        section(
            6,
            vec(
                [
                    func_alias(0, "[method]context.get-server"),
                    func_alias(1, "[method]server.get-player-by-name"),
                    func_alias(2, "[method]player.get-name"),
                    func_alias(3, LOG),
                ]
            ),
        ),
        section(8, vec([task_return, resource_drop])),
        section(
            8,
            vec(
                [
                    canon_lower(0),
                    canon_lower(1, [b"\x03" + uleb(0)]),
                    canon_lower(2, [b"\x03" + uleb(0), b"\x04" + uleb(9)]),
                    canon_lower(3, [b"\x03" + uleb(0)]),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [root_inline, context_inline, server_inline, player_inline, logging_inline, instantiate]
        ),
        section(
            6,
            vec(
                [
                    core_alias(5, "on-load"),
                    core_alias(5, "on-unload"),
                    core_alias(5, "init-plugin"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(6, 6, [b"\x06", b"\x03" + uleb(0)]),
                    canon_lift(7, 6, [b"\x06", b"\x03" + uleb(0)]),
                    canon_lift(8, 7),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=4),
                    component_export("on-unload", index=5),
                    component_export("init-plugin", index=6),
                ]
            ),
        ),
    )


def block_state_component():
    """Pumpkin's get-block-state-id(block-pos) -> u16, with a named record type."""
    block_pos = b"\x72" + vec(
        [name("x") + b"\x7a", name("y") + b"\x7a", name("z") + b"\x7a"]
    )
    get_block_state_id = (
        b"\x40\x01"
        + name("pos")
        + b"\x00"  # block-pos is type index 0
        + b"\x00\x7b"  # one u16 result
    )
    core = module(
        [
            section(
                1,
                vec([core_functype((I32, I32, I32), (I32,)), core_functype((), ())]),
            ),
            section(2, vec([name(WORLD) + name(BLOCK_STATE_ID) + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            # Call get-block-state-id(1, 64, -3). The core signature needs all
            # three coordinates, so a wrong flattening cannot complete.
            section(
                10,
                vec(
                    [
                        body(
                            b"\x41"
                            + sleb(1)
                            + b"\x41"
                            + sleb(64)
                            + b"\x41"
                            + sleb(-3)
                            + b"\x10\x00\x1a\x0b"
                        ),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                block_pos,
                get_block_state_id,
                comp_func(),
                instance_type([(BLOCK_STATE_ID, 1)]),
            ]
        ),
        section(10, vec([instance_import(WORLD, 3)])),
        section(6, vec([func_alias(0, BLOCK_STATE_ID)])),
        section(8, vec([canon_lower(0)])),
        section(1, core),
        core_instance_section(
            [
                core_inline([(BLOCK_STATE_ID, 0)]),
                core_instantiate(0, [(WORLD, 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 2), canon_lift(2, 2), canon_lift(3, 2)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def logging_component():
    """Pumpkin's logging.log(level: level, message: string), lowered and called."""
    level_enum = b"\x6d" + vec([name("trace"), name("debug"), name("info"), name("warn"), name("error")])
    log_func = (
        b"\x40\x02"
        + name("level")
        + b"\x00"  # enum type index 0
        + name("message")
        + b"\x73"  # string
        + b"\x01\x00"  # no results
    )
    core = module(
        [
            section(1, vec([core_functype((I32, I32, I32), ()), core_functype((), ())])),
            section(2, vec([name(LOGGING) + name(LOG) + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            # level = warn (3), message = (ptr 0, len 2)
            section(10, vec([body(b"\x41\x03\x41\x00\x41\x02\x10\x00\x0b"), body(b"\x0b"), body(b"\x0b")])),
            section(11, vec([uleb(0) + b"\x41\x00\x0b" + uleb(2) + b"hi"])),
        ]
    )
    return component(
        type_section([level_enum, log_func, instance_type([(LOG, 1)])]),
        section(10, vec([instance_import(LOGGING, 2)])),
        section(6, vec([func_alias(0, LOG)])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [core_inline([(LOG, 0)]), core_instantiate(0, [(LOGGING, 0)])]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 0), canon_lift(2), canon_lift(3)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def log_tracing_component():
    """Pumpkin's logging.log-tracing(event: list<u8>), lowered and called."""
    trace_func = b"\x40\x01" + name("event") + b"\x70\x7d" + b"\x01\x00"
    core = module(
        [
            section(1, vec([core_functype((I32, I32), ()), core_functype((), ())])),
            section(2, vec([name(LOGGING) + name("log-tracing") + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            # event = (ptr 0, len 3), bytes 01 02 ff.
            section(10, vec([body(b"\x41\x00\x41\x03\x10\x00\x0b"), body(b"\x0b"), body(b"\x0b")])),
            section(11, vec([uleb(0) + b"\x41\x00\x0b" + uleb(3) + b"\x01\x02\xff"])),
        ]
    )
    return component(
        type_section(
            [comp_func(), trace_func, instance_type([("log-tracing", 1)])]
        ),
        section(10, vec([instance_import(LOGGING, 2)])),
        section(6, vec([func_alias(0, "log-tracing")])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [core_inline([("log-tracing", 0)]), core_instantiate(0, [(LOGGING, 0)])]
        ),
        section(6, vec([core_alias(1, "on-load")])),
        section(8, vec([canon_lift(1)])),
        section(11, vec([component_export("on-load", index=1)])),
    )


def player_chat_component():
    """Pumpkin's async player.chat(message), lowered synchronously to the host."""
    chat_func = b"\x43\x01" + name("message") + b"\x73" + b"\x01\x00"
    core = module(
        [
            section(1, vec([core_functype((I32, I32), ()), core_functype((), ())])),
            section(2, vec([name(PLAYER) + name(PLAYER_CHAT) + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        body(b"\x41\x00\x41\x05\x10\x00\x0b"),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
            section(11, vec([uleb(0) + b"\x41\x00\x0b" + uleb(5) + b"hello"])),
        ]
    )
    return component(
        type_section([comp_func(), chat_func, instance_type([(PLAYER_CHAT, 1)])]),
        section(10, vec([instance_import(PLAYER, 2)])),
        section(6, vec([func_alias(0, PLAYER_CHAT)])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [core_inline([(PLAYER_CHAT, 0)]), core_instantiate(0, [(PLAYER, 0)])]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 0), canon_lift(2), canon_lift(3)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def set_block_component():
    """Pumpkin's async world.set-block with a block-pos and block record."""
    block_pos = b"\x72" + vec(
        [name("x") + b"\x7a", name("y") + b"\x7a", name("z") + b"\x7a"]
    )
    flammable = b"\x72" + vec(
        [name("spread-chance") + b"\x7d", name("burn-chance") + b"\x7d"]
    )
    block = b"\x72" + vec(
        [
            name("id") + b"\x7b",
            name("name") + b"\x73",
            name("hardness") + b"\x76",
            name("blast-resistance") + b"\x76",
            name("map-color") + b"\x7d",
            name("slipperiness") + b"\x76",
            name("velocity-multiplier") + b"\x76",
            name("jump-velocity-multiplier") + b"\x76",
            name("item-id") + b"\x7b",
            name("default-state-id") + b"\x7b",
            name("state-ids") + b"\x70\x7b",
            name("is-solid") + b"\x7f",
            name("is-air") + b"\x7f",
            name("is-flammable") + b"\x7f",
            name("flammable") + b"\x6b" + uleb(1),
        ]
    )
    set_block = (
        b"\x43\x03"
        + name("pos")
        + b"\x00"
        + name("block")
        + b"\x02"
        + name("update-flags")
        + b"\x7d"
        + b"\x01\x00"
    )
    payload = bytearray(56)
    payload[0] = 1
    payload[38] = 7  # default-state-id, little-endian u16
    core = module(
        [
            section(
                1,
                vec([core_functype((I32, I32, I32, I32, I32), ()), core_functype((), ())]),
            ),
            section(2, vec([name(WORLD) + name(SET_BLOCK) + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        body(
                            b"\x41\x01\x41\x40\x41\x00\x41\x00\x41\x00\x10\x00\x0b"
                        ),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
            section(
                11,
                vec([uleb(0) + b"\x41\x00\x0b" + uleb(len(payload)) + bytes(payload)]),
            ),
        ]
    )
    return component(
        type_section(
            [
                block_pos,
                flammable,
                block,
                set_block,
                comp_func(),
                instance_type([(SET_BLOCK, 3)]),
            ]
        ),
        section(10, vec([instance_import(WORLD, 5)])),
        section(6, vec([func_alias(0, SET_BLOCK)])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [core_inline([(SET_BLOCK, 0)]), core_instantiate(0, [(WORLD, 0)])]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 4), canon_lift(2, 4), canon_lift(3, 4)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def async_task_component():
    """Call both async lowers through the async ABI and a waitable set.

    The core module gets a status from each lower, joins both subtasks, and
    waits for each returned event before the lifecycle ends. This is the task
    path; the host has already completed the one-shot work behind each subtask.
    """
    block_pos = b"\x72" + vec(
        [name("x") + b"\x7a", name("y") + b"\x7a", name("z") + b"\x7a"]
    )
    flammable = b"\x72" + vec(
        [name("spread-chance") + b"\x7d", name("burn-chance") + b"\x7d"]
    )
    block = b"\x72" + vec(
        [
            name("id") + b"\x7b",
            name("name") + b"\x73",
            name("hardness") + b"\x76",
            name("blast-resistance") + b"\x76",
            name("map-color") + b"\x7d",
            name("slipperiness") + b"\x76",
            name("velocity-multiplier") + b"\x76",
            name("jump-velocity-multiplier") + b"\x76",
            name("item-id") + b"\x7b",
            name("default-state-id") + b"\x7b",
            name("state-ids") + b"\x70\x7b",
            name("is-solid") + b"\x7f",
            name("is-air") + b"\x7f",
            name("is-flammable") + b"\x7f",
            name("flammable") + b"\x6b" + uleb(1),
        ]
    )
    set_block = (
        b"\x43\x03"
        + name("pos")
        + b"\x00"
        + name("block")
        + b"\x02"
        + name("update-flags")
        + b"\x7d"
        + b"\x01\x00"
    )
    chat_func = b"\x43\x01" + name("message") + b"\x73" + b"\x01\x00"
    payload = bytearray(56)
    payload[0] = 1
    payload[38] = 7

    def local_body(code):
        body = b"\x03\x01\x7f\x01\x7f\x01\x7f" + code
        return uleb(len(body)) + body

    # Local 0/1: the two async statuses; local 2: the waitable set. The two
    # wait calls remove the two returned events from the set.
    code = (
        b"\x41\x00\x41\x05\x10\x00\x21\x00"
        + b"\x41\x20\x10\x01\x21\x01"
        + b"\x10\x02\x21\x02"
        + b"\x20\x00\x41\x04\x76\x20\x02\x10\x03"
        + b"\x20\x01\x41\x04\x76\x20\x02\x10\x03"
        + b"\x20\x02\x41\x08\x10\x04\x1a"
        + b"\x20\x02\x41\x08\x10\x04\x1a\x0b"
    )
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32), (I32,)),  # chat status
                        core_functype((I32,), (I32,)),       # set-block in-ptr
                        core_functype((), (I32,)),           # waitable-set.new
                        core_functype((I32, I32), ()),       # waitable-set.join
                        core_functype((I32, I32), (I32,)),    # waitable-set.wait
                        core_functype((), ()),               # on-load
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name(PLAYER) + name(PLAYER_CHAT) + b"\x00" + uleb(0),
                        name(WORLD) + name(SET_BLOCK) + b"\x00" + uleb(1),
                        name("waitable") + name("new") + b"\x00" + uleb(2),
                        name("waitable") + name("join") + b"\x00" + uleb(3),
                        name("waitable") + name("wait") + b"\x00" + uleb(4),
                    ]
                ),
            ),
            section(3, vec([uleb(5), uleb(5), uleb(5)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(5),
                        name("on-enable") + b"\x00" + uleb(6),
                        name("on-disable") + b"\x00" + uleb(7),
                    ]
                ),
            ),
            section(10, vec([local_body(code), body(b"\x0b"), body(b"\x0b")])),
            section(
                11,
                vec(
                    [
                        active_data(0, b"hello"),
                        active_data(32, struct.pack("<iiiii", 2, 64, -3, 64, 0)),
                        active_data(64, bytes(payload)),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                block_pos,
                flammable,
                block,
                set_block,
                chat_func,
                comp_func(),
                instance_type([(PLAYER_CHAT, 4)]),
                instance_type([(SET_BLOCK, 3)]),
            ]
        ),
        section(10, vec([instance_import(PLAYER, 6), instance_import(WORLD, 7)])),
        section(6, vec([func_alias(0, PLAYER_CHAT), func_alias(1, SET_BLOCK)])),
        section(
            8,
            vec(
                [
                    canon_lower(0, [b"\x03" + uleb(0), b"\x06"]),
                    canon_lower(1, [b"\x03" + uleb(0), b"\x06"]),
                    b"\x1f",
                    b"\x23",
                    b"\x20\x00" + uleb(0),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline([(PLAYER_CHAT, 0), (SET_BLOCK, 1)]),
                core_inline([("new", 2), ("join", 3), ("wait", 4)]),
                core_instantiate(0, [(PLAYER, 0), (WORLD, 0), ("waitable", 1)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(2, "on-load"),
                    core_alias(2, "on-enable"),
                    core_alias(2, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(5, 5), canon_lift(6, 5), canon_lift(7, 5)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=2),
                    component_export("on-enable", index=3),
                    component_export("on-disable", index=4),
                ]
            ),
        ),
    )


def string_log_component():
    """A direct `plusmc:host/log` import whose UTF-8 string argument is logged."""
    string_func = b"\x40\x01\x00\x73\x01\x00"  # (string) -> ()
    core = module(
        [
            section(1, vec([core_functype((I32, I32), ()), core_functype((), ())])),
            section(2, vec([name("plusmc:host") + name("log") + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(1), uleb(1)])),
            section(5, vec([b"\x00\x01"])),  # one memory, one page
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(1),
                        name("on-enable") + b"\x00" + uleb(2),
                        name("on-disable") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # (ptr, len) = (0, 5); call the imported log.
                        body(b"\x41\x00\x41\x05\x10\x00\x0b"),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
            # Active data at offset 0: "hello".
            section(11, vec([uleb(0) + b"\x41\x00\x0b" + uleb(5) + b"hello"])),
        ]
    )
    return component(
        type_section([comp_func(), string_func, instance_type([("log", 1)])]),
        section(10, vec([instance_import("plusmc:host", 2)])),
        section(6, vec([func_alias(0, "log")])),
        # The lower names the module's memory 0; utf8 is the default encoding.
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section(
            [
                core_inline([("log", 0)]),
                core_instantiate(0, [("plusmc:host", 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 0), canon_lift(2), canon_lift(3)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def string_result_component():
    """A lifted `on-load` that returns a string, allocated by cabi_realloc."""

    def body_with_local(code):
        payload = b"\x01\x01\x7f" + code  # one local declaration: one i32
        return uleb(len(payload)) + payload

    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32), ()),  # log
                        core_functype((I32, I32, I32, I32), (I32,)),  # cabi_realloc
                        core_functype((I32,), ()),  # post-return
                        core_functype((), (I32,)),  # on-load
                        core_functype((), ()),  # on-enable / on-disable
                    ]
                ),
            ),
            section(2, vec([name("plusmc:host") + name("log") + b"\x00" + uleb(0)])),
            section(3, vec([uleb(1), uleb(3), uleb(2), uleb(4), uleb(4)])),
            section(5, vec([b"\x00\x01"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(2),
                        name("on-enable") + b"\x00" + uleb(4),
                        name("on-disable") + b"\x00" + uleb(5),
                        name("cabi_realloc") + b"\x00" + uleb(1),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # cabi_realloc: one fixed 64-byte page.
                        body(b"\x41" + sleb(64) + b"\x0b"),
                        # on-load: realloc(0, 0, 4, 5); store (64, 5) at 80;
                        # return 80, the return-area pointer.
                        body_with_local(
                            b"\x41"
                            + sleb(0)
                            + b"\x41"
                            + sleb(0)
                            + b"\x41"
                            + sleb(4)
                            + b"\x41"
                            + sleb(5)
                            + b"\x10"
                            + uleb(1)
                            + b"\x21"
                            + uleb(0)
                            + b"\x41"
                            + sleb(80)
                            + b"\x20"
                            + uleb(0)
                            + b"\x36"
                            + uleb(2)
                            + uleb(0)
                            + b"\x41"
                            + sleb(84)
                            + b"\x41"
                            + sleb(5)
                            + b"\x36"
                            + uleb(2)
                            + uleb(0)
                            + b"\x41"
                            + sleb(80)
                            + b"\x0b"
                        ),
                        # post-return: log "freed" so the test sees it run.
                        body(
                            b"\x41"
                            + sleb(96)
                            + b"\x41"
                            + sleb(5)
                            + b"\x10"
                            + uleb(0)
                            + b"\x0b"
                        ),
                        body(b"\x0b"),
                        body(b"\x0b"),
                    ]
                ),
            ),
            section(
                11,
                vec(
                    [
                        uleb(0) + b"\x41" + sleb(64) + b"\x0b" + uleb(5) + b"hello",
                        uleb(0) + b"\x41" + sleb(96) + b"\x0b" + uleb(5) + b"freed",
                    ]
                ),
            ),
        ]
    )
    string_func = b"\x40\x00\x00\x73"  # () -> string
    return component(
        type_section([string_func, comp_func()]),
        section(1, core),
        core_instance_section([core_instantiate(0, [])]),
        section(
            6,
            vec(
                [
                    core_alias(0, "on-load"),
                    core_alias(0, "on-enable"),
                    core_alias(0, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(
                        0,
                        0,
                        [b"\x03" + uleb(0), b"\x04" + uleb(1), b"\x05" + uleb(3)],
                    ),
                    canon_lift(1, 1),
                    canon_lift(2, 1),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=0),
                    component_export("on-enable", index=1),
                    component_export("on-disable", index=2),
                ]
            ),
        ),
    )


def active_data(offset, payload):
    return uleb(0) + b"\x41" + sleb(offset) + b"\x0b" + uleb(len(payload)) + payload


def lower_string_result_component():
    """A lowered `config_get_string`; the host returns text through cabi_realloc."""

    def body_with_local(code):
        payload = b"\x01\x01\x7f" + code  # one local declaration: one i32
        return uleb(len(payload)) + payload

    # (key: string, value: string) -> u32
    config_set_type = (
        b"\x40\x02" + name("key") + b"\x73" + name("value") + b"\x73" + b"\x00\x79"
    )
    # (key: string) -> string
    config_get_type = b"\x40\x01" + name("key") + b"\x73" + b"\x00\x73"
    # (text: string) -> ()
    log_type = b"\x40\x01" + name("text") + b"\x73" + b"\x01\x00"
    on_load_type = comp_func()

    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32, I32), (I32,)),  # config_set
                        core_functype((I32, I32), (I32,)),  # config_get_string
                        core_functype((I32, I32), ()),  # log
                        core_functype((), ()),  # on-load
                        core_functype((), ()),  # on-enable / on-disable
                        core_functype((I32, I32, I32, I32), (I32,)),  # cabi_realloc
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name("plusmc:host") + name("config_set") + b"\x00" + uleb(0),
                        name("plusmc:host") + name("config_get_string") + b"\x00" + uleb(1),
                        name("plusmc:host") + name("log") + b"\x00" + uleb(2),
                    ]
                ),
            ),
            section(3, vec([uleb(3), uleb(4), uleb(4), uleb(5)])),
            section(5, vec([b"\x00\x01"])),  # one memory, one page
            # Global 0 is the bump pointer, after the static data.
            section(6, vec([b"\x7f\x01" + b"\x41" + sleb(128) + b"\x0b"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(3),
                        name("on-enable") + b"\x00" + uleb(4),
                        name("on-disable") + b"\x00" + uleb(5),
                        name("cabi_realloc") + b"\x00" + uleb(6),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # config_set("greeting", "hi from host");
                        # config_get_string("greeting"); log the returned pair.
                        body_with_local(
                            b"\x41\x00\x41\x08\x41\x10\x41\x0c\x10\x00\x1a"
                            b"\x41\x00\x41\x08\x10\x01\x21\x00"
                            b"\x20\x00\x28\x02\x00"
                            b"\x20\x00\x28\x02\x04"
                            b"\x10\x02\x0b"
                        ),
                        body(b"\x0b"),  # on-enable
                        body(b"\x0b"),  # on-disable
                        # cabi_realloc: a bump allocator over one page.
                        body_with_local(
                            b"\x20\x03\x45\x04\x40\x41\x00\x0f\x0b"
                            b"\x23\x00\x20\x02\x41\x01\x6b\x6a"
                            b"\x41\x00\x20\x02\x6b\x71"
                            b"\x22\x04\x20\x03\x6a\x24\x00"
                            b"\x20\x04\x0b"
                        ),
                    ]
                ),
            ),
            section(
                11,
                vec(
                    [
                        active_data(0, b"greeting"),
                        active_data(16, b"hi from host"),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                config_set_type,
                config_get_type,
                log_type,
                on_load_type,
                instance_type(
                    [
                        ("config_set", 0),
                        ("config_get_string", 1),
                        ("log", 2),
                    ]
                ),
            ]
        ),
        section(10, vec([instance_import("plusmc:host", 4)])),
        section(
            6,
            vec(
                [
                    func_alias(0, "config_set"),
                    func_alias(0, "config_get_string"),
                    func_alias(0, "log"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lower(0, [b"\x03" + uleb(0)]),
                    # The result string needs memory 0 and the module's realloc.
                    canon_lower(1, [b"\x03" + uleb(0), b"\x04" + uleb(6)]),
                    canon_lower(2, [b"\x03" + uleb(0)]),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline(
                    [
                        ("config_set", 0),
                        ("config_get_string", 1),
                        ("log", 2),
                    ]
                ),
                core_instantiate(0, [("plusmc:host", 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(3, 3),
                    canon_lift(4, 3),
                    canon_lift(5, 3),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=3),
                    component_export("on-enable", index=4),
                    component_export("on-disable", index=5),
                ]
            ),
        ),
    )


def lower_list_result_component():
    """A lowered `config_keys`; the host returns list<string> via cabi_realloc."""

    def body_with_locals(code):
        payload = b"\x01\x02\x7f" + code  # one local group: two i32s
        return uleb(len(payload)) + payload

    # (key: string, value: string) -> u32
    config_set_type = (
        b"\x40\x02" + name("key") + b"\x73" + name("value") + b"\x73" + b"\x00\x79"
    )
    # () -> list<string>
    config_keys_type = b"\x40\x00\x00\x70\x73"
    # (text: string) -> ()
    log_type = b"\x40\x01" + name("text") + b"\x73" + b"\x01\x00"
    on_load_type = comp_func()

    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32, I32), (I32,)),  # config_set
                        core_functype((), (I32,)),  # config_keys
                        core_functype((I32, I32), ()),  # log
                        core_functype((), ()),  # on-load
                        core_functype((), ()),  # on-enable / on-disable
                        core_functype((I32, I32, I32, I32), (I32,)),  # cabi_realloc
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name("plusmc:host") + name("config_set") + b"\x00" + uleb(0),
                        name("plusmc:host") + name("config_keys") + b"\x00" + uleb(1),
                        name("plusmc:host") + name("log") + b"\x00" + uleb(2),
                    ]
                ),
            ),
            section(3, vec([uleb(3), uleb(4), uleb(4), uleb(5)])),
            section(5, vec([b"\x00\x01"])),  # one memory, one page
            # Global 0 is the bump pointer, after the static data.
            section(6, vec([b"\x7f\x01" + b"\x41" + sleb(128) + b"\x0b"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(3),
                        name("on-enable") + b"\x00" + uleb(4),
                        name("on-disable") + b"\x00" + uleb(5),
                        name("cabi_realloc") + b"\x00" + uleb(6),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # config_set("beta", ""), config_set("alpha", "").
                        body_with_locals(
                            b"\x41\x00\x41\x04\x41\x10\x41\x00\x10\x00\x1a"
                            b"\x41\x08\x41\x05\x41\x10\x41\x00\x10\x00\x1a"
                            # area = config_keys(); array = load(area)
                            b"\x10\x01\x21\x00"
                            b"\x20\x00\x28\x02\x00\x21\x01"
                            # log the two keys in the host's map order.
                            b"\x20\x01\x28\x02\x00"
                            b"\x20\x01\x28\x02\x04"
                            b"\x10\x02"
                            b"\x20\x01\x28\x02\x08"
                            b"\x20\x01\x28\x02\x0c"
                            b"\x10\x02\x0b"
                        ),
                        body(b"\x0b"),  # on-enable
                        body(b"\x0b"),  # on-disable
                        # cabi_realloc: a bump allocator over one page.
                        body_with_locals(
                            b"\x20\x03\x45\x04\x40\x41\x00\x0f\x0b"
                            b"\x23\x00\x20\x02\x41\x01\x6b\x6a"
                            b"\x41\x00\x20\x02\x6b\x71"
                            b"\x22\x04\x20\x03\x6a\x24\x00"
                            b"\x20\x04\x0b"
                        ),
                    ]
                ),
            ),
            section(
                11,
                vec(
                    [
                        active_data(0, b"beta"),
                        active_data(8, b"alpha"),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                config_set_type,
                config_keys_type,
                log_type,
                on_load_type,
                instance_type(
                    [
                        ("config_set", 0),
                        ("config_keys", 1),
                        ("log", 2),
                    ]
                ),
            ]
        ),
        section(10, vec([instance_import("plusmc:host", 4)])),
        section(
            6,
            vec(
                [
                    func_alias(0, "config_set"),
                    func_alias(0, "config_keys"),
                    func_alias(0, "log"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lower(0, [b"\x03" + uleb(0)]),
                    # The list result needs memory 0 and the module's realloc.
                    canon_lower(1, [b"\x03" + uleb(0), b"\x04" + uleb(6)]),
                    canon_lower(2, [b"\x03" + uleb(0)]),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline(
                    [
                        ("config_set", 0),
                        ("config_keys", 1),
                        ("log", 2),
                    ]
                ),
                core_instantiate(0, [("plusmc:host", 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(3, 3),
                    canon_lift(4, 3),
                    canon_lift(5, 3),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=3),
                    component_export("on-enable", index=4),
                    component_export("on-disable", index=5),
                ]
            ),
        ),
    )


def lower_record_result_component():
    """A lowered `config_pair`; the host returns a two-string record via cabi_realloc."""

    def body_with_local(code):
        payload = b"\x01\x01\x7f" + code  # one local declaration: one i32
        return uleb(len(payload)) + payload

    # (key: string, value: string) -> u32
    config_set_type = (
        b"\x40\x02" + name("key") + b"\x73" + name("value") + b"\x73" + b"\x00\x79"
    )
    # (key: string) -> record { key: string, value: string }
    config_pair_type = (
        b"\x40\x01"
        + name("key")
        + b"\x73"
        + b"\x00"
        + b"\x72"
        + vec([name("key") + b"\x73", name("value") + b"\x73"])
    )
    # (text: string) -> ()
    log_type = b"\x40\x01" + name("text") + b"\x73" + b"\x01\x00"
    on_load_type = comp_func()

    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32, I32), (I32,)),  # config_set
                        core_functype((I32, I32), (I32,)),  # config_pair
                        core_functype((I32, I32), ()),  # log
                        core_functype((), ()),  # on-load
                        core_functype((), ()),  # on-enable / on-disable
                        core_functype((I32, I32, I32, I32), (I32,)),  # cabi_realloc
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name("plusmc:host") + name("config_set") + b"\x00" + uleb(0),
                        name("plusmc:host") + name("config_pair") + b"\x00" + uleb(1),
                        name("plusmc:host") + name("log") + b"\x00" + uleb(2),
                    ]
                ),
            ),
            section(3, vec([uleb(3), uleb(4), uleb(4), uleb(5)])),
            section(5, vec([b"\x00\x01"])),  # one memory, one page
            # Global 0 is the bump pointer, after the static data.
            section(6, vec([b"\x7f\x01" + b"\x41" + sleb(128) + b"\x0b"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(3),
                        name("on-enable") + b"\x00" + uleb(4),
                        name("on-disable") + b"\x00" + uleb(5),
                        name("cabi_realloc") + b"\x00" + uleb(6),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # config_set("greeting", "hi from host");
                        # config_pair("greeting"); log the returned key and value.
                        body_with_local(
                            b"\x41\x00\x41\x08\x41\x10\x41\x0c\x10\x00\x1a"
                            b"\x41\x00\x41\x08\x10\x01\x21\x00"
                            b"\x20\x00\x28\x02\x00"
                            b"\x20\x00\x28\x02\x04"
                            b"\x10\x02"
                            b"\x20\x00\x28\x02\x08"
                            b"\x20\x00\x28\x02\x0c"
                            b"\x10\x02\x0b"
                        ),
                        body(b"\x0b"),  # on-enable
                        body(b"\x0b"),  # on-disable
                        # cabi_realloc: a bump allocator over one page.
                        body_with_local(
                            b"\x20\x03\x45\x04\x40\x41\x00\x0f\x0b"
                            b"\x23\x00\x20\x02\x41\x01\x6b\x6a"
                            b"\x41\x00\x20\x02\x6b\x71"
                            b"\x22\x04\x20\x03\x6a\x24\x00"
                            b"\x20\x04\x0b"
                        ),
                    ]
                ),
            ),
            section(
                11,
                vec(
                    [
                        active_data(0, b"greeting"),
                        active_data(16, b"hi from host"),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                config_set_type,
                config_pair_type,
                log_type,
                on_load_type,
                instance_type(
                    [
                        ("config_set", 0),
                        ("config_pair", 1),
                        ("log", 2),
                    ]
                ),
            ]
        ),
        section(10, vec([instance_import("plusmc:host", 4)])),
        section(
            6,
            vec(
                [
                    func_alias(0, "config_set"),
                    func_alias(0, "config_pair"),
                    func_alias(0, "log"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lower(0, [b"\x03" + uleb(0)]),
                    # The record result needs memory 0 and the module's realloc.
                    canon_lower(1, [b"\x03" + uleb(0), b"\x04" + uleb(6)]),
                    canon_lower(2, [b"\x03" + uleb(0)]),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline(
                    [
                        ("config_set", 0),
                        ("config_pair", 1),
                        ("log", 2),
                    ]
                ),
                core_instantiate(0, [("plusmc:host", 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(3, 3),
                    canon_lift(4, 3),
                    canon_lift(5, 3),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=3),
                    component_export("on-enable", index=4),
                    component_export("on-disable", index=5),
                ]
            ),
        ),
    )


def block_info_component():
    """Pumpkin's get-block(block-pos) -> block, with the full 15-field record."""

    def body_with_local(code):
        payload = b"\x01\x01\x7f" + code  # one local declaration: one i32
        return uleb(len(payload)) + payload

    block_pos = b"\x72" + vec(
        [name("x") + b"\x7a", name("y") + b"\x7a", name("z") + b"\x7a"]
    )
    flammable = b"\x72" + vec(
        [name("spread-chance") + b"\x7d", name("burn-chance") + b"\x7d"]
    )
    block = b"\x72" + vec(
        [
            name("id") + b"\x7b",
            name("name") + b"\x73",
            name("hardness") + b"\x76",
            name("blast-resistance") + b"\x76",
            name("map-color") + b"\x7d",
            name("slipperiness") + b"\x76",
            name("velocity-multiplier") + b"\x76",
            name("jump-velocity-multiplier") + b"\x76",
            name("item-id") + b"\x7b",
            name("default-state-id") + b"\x7b",
            name("state-ids") + b"\x70\x7b",
            name("is-solid") + b"\x7f",
            name("is-air") + b"\x7f",
            name("is-flammable") + b"\x7f",
            name("flammable") + b"\x6b" + uleb(1),  # option<flammable>
        ]
    )
    get_block = (
        b"\x40\x01"
        + name("pos")
        + b"\x00"  # block-pos is type index 0
        + b"\x00"
        + b"\x02"  # one block result, type index 2
    )
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32), (I32,)),  # get-block
                        core_functype((I32, I32), ()),  # log
                        core_functype((), ()),  # lifecycle
                        core_functype((I32, I32, I32, I32), (I32,)),  # cabi_realloc
                    ]
                ),
            ),
            section(
                2,
                vec(
                    [
                        name(WORLD) + name("[method]world.get-block") + b"\x00" + uleb(0),
                        name("plusmc:host") + name("log") + b"\x00" + uleb(1),
                    ]
                ),
            ),
            section(3, vec([uleb(2), uleb(2), uleb(2), uleb(3)])),
            section(5, vec([b"\x00\x01"])),  # one page
            section(6, vec([b"\x7f\x01" + b"\x41" + sleb(128) + b"\x0b"])),
            section(
                7,
                vec(
                    [
                        name("on-load") + b"\x00" + uleb(2),
                        name("on-enable") + b"\x00" + uleb(3),
                        name("on-disable") + b"\x00" + uleb(4),
                        name("cabi_realloc") + b"\x00" + uleb(5),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        # get-block(1, 64, -3); log(name.pointer, name.length).
                        body_with_local(
                            b"\x41"
                            + sleb(1)
                            + b"\x41"
                            + sleb(64)
                            + b"\x41"
                            + sleb(-3)
                            + b"\x10\x00\x21\x00"
                            b"\x20\x00\x28\x02\x04"
                            b"\x20\x00\x28\x02\x08"
                            b"\x10\x01\x0b"
                        ),
                        body(b"\x0b"),
                        body(b"\x0b"),
                        # cabi_realloc: a bump allocator over one page.
                        body_with_local(
                            b"\x20\x03\x45\x04\x40\x41\x00\x0f\x0b"
                            b"\x23\x00\x20\x02\x41\x01\x6b\x6a"
                            b"\x41\x00\x20\x02\x6b\x71"
                            b"\x22\x04\x20\x03\x6a\x24\x00"
                            b"\x20\x04\x0b"
                        ),
                    ]
                ),
            ),
        ]
    )
    return component(
        type_section(
            [
                block_pos,
                flammable,
                block,
                get_block,
                comp_func(),
                instance_type([("[method]world.get-block", 3)]),
            ]
        ),
        section(10, vec([instance_import(WORLD, 5)])),
        section(6, vec([func_alias(0, "[method]world.get-block")])),
        section(
            8,
            vec(
                [
                    # The block result needs memory 0 and the module's realloc
                    # (core function 5: two imports + four definitions).
                    canon_lower(0, [b"\x03" + uleb(0), b"\x04" + uleb(5)]),
                ]
            ),
        ),
        section(1, core),
        core_instance_section(
            [
                core_inline([("[method]world.get-block", 0)]),
                core_instantiate(0, [(WORLD, 0)]),
            ]
        ),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-enable"),
                    core_alias(1, "on-disable"),
                ]
            ),
        ),
        section(8, vec([canon_lift(1, 4), canon_lift(2, 4), canon_lift(3, 4)])),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-enable", index=2),
                    component_export("on-disable", index=3),
                ]
            ),
        ),
    )


def pumpkin_v02_core(error_text=None, call_task_return=True):
    """A core module with Pumpkin v0.2's callback shapes.

    It imports the two canonical built-ins (`task.return`, `resource.drop`) and
    `pumpkin:plugin/logging.log(level, message)`, then exports `init-plugin`,
    async `on-load` and async `on-unload`. The lifecycle functions take the
    owned context handle and return through `task.return`.
    """
    imports = [
        ("$root", "[task-return]on-load", (I32, I32, I32), ()),
        ("$root", "[resource-drop]context", (I32,), ()),
        ("pumpkin:plugin/logging", "log", (I32, I32, I32), ()),
    ]
    types = [core_functype(params, results) for _, _, params, results in imports]
    import_entries = [
        name(module_name) + name(field) + b"\x00" + uleb(index)
        for index, (module_name, field, _, _) in enumerate(imports)
    ]
    types.append(core_functype((), ()))  # 3: init-plugin
    types.append(core_functype((I32,), ()))  # 4: async lifecycle entry
    funcs = [uleb(3), uleb(4), uleb(4)]

    # The operand order is the canonical flattened order: level, ptr, len.
    init_code = b"\x41\x02\x41\x00\x41\x04\x10\x02\x0b"  # info, "init"
    load_code = b"\x41\x02\x41\x10\x41\x06\x10\x02"  # info, "loaded"
    unload_code = b"\x41\x02\x41\x20\x41\x06\x10\x02"  # info, "unload"
    if call_task_return:
        if error_text is None:
            load_code += b"\x41\x00\x41\x00\x41\x00\x10\x00"  # ok()
        else:
            encoded = error_text.encode("utf-8")
            load_code += (
                b"\x41\x01\x41" + sleb(48) + b"\x41" + sleb(len(encoded)) + b"\x10\x00"
            )
    load_code += b"\x20\x00\x10\x01\x0b"  # resource.drop(context)
    unload_code += (
        b"\x41\x00\x41\x00\x41\x00\x10\x00"  # task.return(ok())
        b"\x20\x00\x10\x01\x0b"  # resource.drop(context)
    )

    data = [
        active_data(0, b"init"),
        active_data(16, b"loaded"),
        active_data(32, b"unload"),
    ]
    if error_text is not None:
        data.append(active_data(48, error_text.encode("utf-8")))

    exports = [
        name("init-plugin") + b"\x00" + uleb(3),
        name("on-load") + b"\x00" + uleb(4),
        name("on-unload") + b"\x00" + uleb(5),
    ]
    return module(
        [
            section(1, vec(types)),
            section(2, vec(import_entries)),
            section(3, vec(funcs)),
            section(5, vec([b"\x00\x01"])),
            section(7, vec(exports)),
            section(10, vec([body(init_code), body(load_code), body(unload_code)])),
            section(11, vec(data)),
        ]
    )


def pumpkin_v02_component(error_text=None, call_task_return=True):
    """The real v0.2 ABI shape: resource context + async result<_, string>."""
    resource_type = b"\x3f\x7f\x00"
    async_func = (
        b"\x43"
        + vec([name("context") + b"\x69" + uleb(0)])
        + b"\x00"
        + b"\x6a\x00\x01\x73"
    )
    init_func = b"\x40\x00\x01\x00"
    level_enum = b"\x6d" + vec(
        [name("trace"), name("debug"), name("info"), name("warn"), name("error")]
    )
    # `logging.log` is type 4; its `level` parameter names enum type 3.
    log_func = (
        b"\x40\x02"
        + name("level")
        + uleb(3)
        + name("message")
        + b"\x73"
        + b"\x01\x00"
    )
    logging_instance = instance_type([(LOG, 4)])
    core = pumpkin_v02_core(error_text, call_task_return)
    task_return = b"\x09" + b"\x00" + b"\x6a\x00\x01\x73" + vec([b"\x03\x00"])
    resource_drop = b"\x03" + uleb(0)
    root_inline = core_inline(
        [("[task-return]on-load", 0), ("[resource-drop]context", 1)]
    )
    logging_inline = core_inline([(LOG, 2)])
    instantiate = core_instantiate(0, [("$root", 0), (LOGGING, 1)])
    return component(
        type_section(
            [resource_type, async_func, init_func, level_enum, log_func, logging_instance]
        ),
        # Core functions 0/1 are the canonical built-ins; the lower is 2.
        section(8, vec([task_return, resource_drop])),
        section(10, vec([instance_import(LOGGING, 5)])),
        section(6, vec([func_alias(0, LOG)])),
        section(8, vec([canon_lower(0, [b"\x03" + uleb(0)])])),
        section(1, core),
        core_instance_section([root_inline, logging_inline, instantiate]),
        # Core functions 3/4/5 are the module's three lifecycle exports.
        section(
            6,
            vec(
                [
                    core_alias(2, "on-load"),
                    core_alias(2, "on-unload"),
                    core_alias(2, "init-plugin"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(3, 1, [b"\x06", b"\x03\x00"]),
                    canon_lift(4, 1, [b"\x06", b"\x03\x00"]),
                    canon_lift(5, 2),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=1),
                    component_export("on-unload", index=2),
                    component_export("init-plugin", index=3),
                ]
            ),
        ),
    )


def pumpkin_v02_no_memory_component():
    """A v0.2 lifecycle whose task.return carries a string without memory."""
    resource_type = b"\x3f\x7f\x00"
    async_func = (
        b"\x43"
        + vec([name("context") + b"\x69" + uleb(0)])
        + b"\x00"
        + b"\x6a\x00\x01\x73"
    )
    init_func = b"\x40\x00\x01\x00"
    core = module(
        [
            section(
                1,
                vec(
                    [
                        core_functype((I32, I32, I32), ()),
                        core_functype((I32,), ()),
                        core_functype((), ()),
                    ]
                ),
            ),
            section(
                2,
                vec([name("$root") + name("[task-return]on-load") + b"\x00" + uleb(0)]),
            ),
            section(3, vec([uleb(2), uleb(1), uleb(1)])),
            section(
                7,
                vec(
                    [
                        name("init-plugin") + b"\x00" + uleb(1),
                        name("on-load") + b"\x00" + uleb(2),
                        name("on-unload") + b"\x00" + uleb(3),
                    ]
                ),
            ),
            section(
                10,
                vec(
                    [
                        body(b"\x0b"),
                        body(b"\x41\x00\x41\x00\x41\x00\x10\x00\x0b"),
                        body(b"\x41\x00\x41\x00\x41\x00\x10\x00\x0b"),
                    ]
                ),
            ),
        ]
    )
    task_return = b"\x09" + b"\x00" + b"\x6a\x00\x01\x73" + vec([])
    root_inline = core_inline([("[task-return]on-load", 0)])
    instantiate = core_instantiate(0, [("$root", 0)])
    return component(
        type_section([resource_type, async_func, init_func]),
        section(1, core),
        section(8, vec([task_return])),
        core_instance_section([root_inline, instantiate]),
        section(
            6,
            vec(
                [
                    core_alias(1, "on-load"),
                    core_alias(1, "on-unload"),
                    core_alias(1, "init-plugin"),
                ]
            ),
        ),
        section(
            8,
            vec(
                [
                    canon_lift(1, 1, [b"\x06"]),
                    canon_lift(2, 1, [b"\x06"]),
                    canon_lift(3, 2),
                ]
            ),
        ),
        section(
            11,
            vec(
                [
                    component_export("on-load", index=0),
                    component_export("on-unload", index=1),
                    component_export("init-plugin", index=2),
                ]
            ),
        ),
    )


def strict_item_stack_component():
    """A small item-stack component that exercises the strict type reader.

    It imports the constructor plus option<string>, option<bool>, attribute
    modifier, NBT tree and data-component-value signatures. Every call is made
    from on-load; result methods read into explicit guest return areas. Because
    it has no component-instance definitions, a strict linker failure cannot be
    hidden by the compiler-artifact compiled-core path.
    """
    interface = "pumpkin:plugin/item-stack@0.2.0"

    def i32(value):
        return b"\x41" + sleb(value)

    def f64(value):
        return b"\x44" + struct.pack("<d", value)

    def call(index):
        return b"\x10" + uleb(index)

    def rec(fields):
        return b"\x72" + vec([name(field) + value_type for field, value_type in fields])

    def func(params, result=None):
        encoded = b"\x40" + vec([name(param) + value_type for param, value_type in params])
        return encoded + (b"\x01\x00" if result is None else b"\x00" + result)

    resource = b"\x3f\x7f\x00"
    borrow_item = b"\x68\x00"
    attribute = b"\x6d" + vec([
        name("armor"), name("attack-damage"), name("attack-speed")
    ])
    operation = b"\x6d" + vec([
        name("add-value"), name("add-multiplied-base"), name("add-multiplied-total")
    ])
    modifier = rec([
        ("id", b"\x73"), ("amount", b"\x75"), ("operation", b"\x03")
    ])
    slot = b"\x6d" + vec([name("any"), name("main-hand"), name("off-hand")])
    item_modifier = rec([
        ("attribute", b"\x02"), ("modifier", b"\x04"), ("slot", b"\x05")
    ])
    nbt_tag = b"\x71" + vec([name("int") + b"\x01\x7a"]) + b"\x00"
    nbt_list = b"\x70\x07"
    nbt_tree = rec([("root", b"\x79"), ("tags", b"\x08")])
    data_component = b"\x6d" + vec([name("item-model"), name("custom-name")])
    list_u8 = b"\x70\x7d"
    component_value = rec([("component", b"\x0a"), ("value", b"\x0b")])
    component_list = b"\x70\x0c"
    modifier_list = b"\x70\x06"
    option_nbt = b"\x6b\x09"
    option_string = b"\x6b\x73"
    option_bool = b"\x6b\x7f"

    ctor = func([("registry-key", b"\x73"), ("count", b"\x7d")], b"\x00")
    set_model = func([("self", b"\x01"), ("model", b"\x10")])
    get_model = func([("self", b"\x01")], b"\x10")
    get_modifiers = func([("self", b"\x01")], b"\x0e")
    add_modifier = func([("self", b"\x01"), ("modifier", b"\x06")])
    set_custom = func([
        ("self", b"\x01"), ("namespace", b"\x73"), ("key", b"\x73"),
        ("value", b"\x09")
    ])
    get_custom = func([
        ("self", b"\x01"), ("namespace", b"\x73"), ("key", b"\x73")
    ], b"\x0f")
    set_component = func([
        ("self", b"\x01"), ("component", b"\x0a"), ("value", b"\x0b")
    ])
    get_components = func([("self", b"\x01")], b"\x0d")
    set_glint = func([("self", b"\x01"), ("glint", b"\x11")])
    get_glint = func([("self", b"\x01")], b"\x11")
    enchantment = b"\x6d" + vec([name("sharpness"), name("unbreaking"), name("mending")])
    enchantment_value = rec([("enchantment", b"\x1d"), ("level", b"\x79")])
    enchantment_list = b"\x70\x1e"
    custom_enchantment = rec([("enchantment-id", b"\x73"), ("level", b"\x79")])
    custom_list = b"\x70\x20"
    option_handle = b"\x6b\x00"
    lore_list = b"\x70\x00"
    get_enchantments = func([("self", b"\x01")], b"\x1f")
    add_enchantment = func([("self", b"\x01"), ("enchantment", b"\x1d"),
                            ("level", b"\x79")])
    get_custom_enchantments = func([("self", b"\x01")], b"\x21")
    add_custom_enchantment = func([("self", b"\x01"), ("enchantment-id", b"\x73"),
                                   ("level", b"\x79")])
    get_lore = func([("self", b"\x01")], b"\x23")
    set_lore = func([("self", b"\x01"), ("lore", b"\x23")])
    get_custom_name = func([("self", b"\x01")], b"\x22")
    set_custom_name = func([("self", b"\x01"), ("name", b"\x22")])
    get_registry_key = func([("self", b"\x01")], b"\x73")
    text_resource = b"\x3f\x7f\x00"
    text_text = func([("plain", b"\x73")], b"\x2c")
    text_instance = instance_type([("[static]text-component.text", 45)])
    text_interface = "pumpkin:plugin/text@0.2.0"
    text_member = "[static]text-component.text"

    types = [
        resource, borrow_item, attribute, operation, modifier, slot, item_modifier,
        nbt_tag, nbt_list, nbt_tree, data_component, list_u8, component_value,
        component_list, modifier_list, option_nbt, option_string, option_bool,
        ctor, set_model, get_model, get_modifiers, add_modifier, set_custom,
        get_custom, set_component, get_components, set_glint, get_glint,
        enchantment, enchantment_value, enchantment_list, custom_enchantment,
        custom_list, option_handle, lore_list, get_enchantments, add_enchantment,
        get_custom_enchantments, add_custom_enchantment, get_lore, set_lore,
        get_custom_name, set_custom_name,        text_resource, text_text, text_instance, get_registry_key,
        instance_type([
            ("[constructor]item-stack", 18),
            ("[async method]item-stack.set-item-model", 19),
            ("[async method]item-stack.get-item-model", 20),
            ("[async method]item-stack.get-attribute-modifiers", 21),
            ("[async method]item-stack.add-attribute-modifier", 22),
            ("[async method]item-stack.set-custom-data", 23),
            ("[async method]item-stack.get-custom-data", 24),
            ("[async method]item-stack.set-component", 25),
            ("[async method]item-stack.get-components", 26),
            ("[async method]item-stack.set-enchantment-glint-override", 27),
            ("[async method]item-stack.get-enchantment-glint-override", 28),
            ("[async method]item-stack.get-enchantments", 36),
            ("[async method]item-stack.add-enchantment", 37),
            ("[async method]item-stack.get-custom-enchantments", 38),
            ("[async method]item-stack.add-custom-enchantment", 39),
            ("[async method]item-stack.get-lore", 40),
            ("[async method]item-stack.set-lore", 41),
            ("[async method]item-stack.get-custom-name", 42),
            ("[async method]item-stack.set-custom-name", 43),
            ("[async method]item-stack.get-registry-key", 47),
        ]),
        comp_func(),
    ]
    lift_type = len(types) - 1

    members = [
        "[constructor]item-stack",
        "[async method]item-stack.set-item-model",
        "[async method]item-stack.get-item-model",
        "[async method]item-stack.get-attribute-modifiers",
        "[async method]item-stack.add-attribute-modifier",
        "[async method]item-stack.get-custom-data",
        "[async method]item-stack.set-custom-data",
        "[async method]item-stack.set-component",
        "[async method]item-stack.get-components",
        "[async method]item-stack.set-enchantment-glint-override",
        "[async method]item-stack.get-enchantment-glint-override",
        "[async method]item-stack.get-enchantments",
        "[async method]item-stack.add-enchantment",
        "[async method]item-stack.get-custom-enchantments",
        "[async method]item-stack.add-custom-enchantment",
        "[async method]item-stack.get-lore",
        "[async method]item-stack.set-lore",
        "[async method]item-stack.get-custom-name",
        "[async method]item-stack.set-custom-name",
    ]
    core_types = [
        core_functype((I32, I32, I32), (I32,)),
        core_functype((I32, I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32, I32, 0x7C, I32, I32), ()),
        core_functype((I32, I32, I32, I32, I32, I32), ()),
        core_functype((I32, I32, I32, I32, I32, I32, I32, I32), ()),
        core_functype((I32, I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32), ()),
        core_functype((I32, I32), ()),
        core_functype((I32, I32, I32), ()),
        core_functype((I32, I32), (I32,)),
    ]
    life_type = len(core_types)
    core_types.append(core_functype((), ()))
    realloc_type = len(core_types)
    core_types.append(core_functype((I32, I32, I32, I32), (I32,)))
    imports = [
        name(interface) + name(member) + b"\x00" + uleb(index)
        for index, member in enumerate(members)
    ] + [
        name(interface) + name("[async method]item-stack.get-registry-key") + b"\x00" + uleb(2),
        name(text_interface) + name(text_member) + b"\x00" + uleb(19),
    ]

    load = b""
    load += i32(0) + i32(17) + i32(1) + call(0) + b"\x21\x00"  # handle = new(...)
    load += b"\x20\x00" + i32(20) + call(19)                   # get-registry-key
    load += b"\x20\x00" + i32(96) + call(2)                    # get-item-model
    load += b"\x20\x00" + i32(0) + i32(0) + i32(0) + call(1)    # set-item-model(None)
    load += b"\x20\x00" + i32(104) + call(10)                   # get-glint
    load += b"\x20\x00" + i32(0) + i32(0) + call(9)             # set-glint(None)
    load += b"\x20\x00" + i32(112) + call(3)                    # get-modifiers
    load += (b"\x20\x00" + i32(0) + i32(32) + i32(3) + f64(2.5) +
             i32(0) + i32(0) + call(4))                          # add-modifier
    load += (b"\x20\x00" + i32(40) + i32(2) + i32(48) + i32(3) +
             i32(128) + call(5))                                 # get-custom-data
    load += (b"\x20\x00" + i32(40) + i32(2) + i32(48) + i32(3) +
             i32(0) + i32(64) + i32(1) + call(6))                # set-custom-data
    load += b"\x20\x00" + i32(144) + call(8)                    # get-components
    load += b"\x20\x00" + i32(0) + i32(80) + i32(3) + call(7)    # set-component
    load += b"\x20\x00" + i32(1) + i32(3) + call(12)             # add-enchantment
    load += b"\x20\x00" + i32(192) + call(11)                    # get-enchantments
    load += (b"\x20\x00" + i32(84) + i32(2) + i32(4) + call(14)) # add-custom
    load += b"\x20\x00" + i32(200) + call(13)                    # get-custom
    load += i32(96) + i32(4) + call(20) + b"\x21\x01"           # text line
    load += i32(240) + b"\x20\x01" + b"\x36\x02\x00"             # line handle array
    load += b"\x20\x00" + i32(240) + i32(1) + call(16)           # set-lore(line)
    load += b"\x20\x00" + i32(208) + call(15)                    # get-lore
    load += i32(104) + i32(4) + call(20) + b"\x21\x02"          # text name
    load += b"\x20\x00" + i32(1) + b"\x20\x02" + call(18)        # set name
    load += b"\x20\x00" + i32(216) + call(17)                    # get name
    load += b"\x20\x00" + i32(0) + i32(0) + call(18)             # clear name
    load += b"\x20\x00" + i32(224) + call(17)                    # get name
    load += b"\x0b"

    data = bytearray(256)
    data[0:17] = b"minecraft:diamond"
    data[32:35] = b"abc"
    data[40:42] = b"ns"
    data[48:51] = b"key"
    data[64] = 2  # nbt int tag
    struct.pack_into("<i", data, 72, 7)
    data[80:83] = b"xyz"
    data[84:86] = b"ce"
    data[96:100] = b"line"
    data[104:108] = b"name"

    core = module([
        section(1, vec(core_types)),
        section(2, vec(imports)),
        section(3, vec([uleb(life_type), uleb(life_type), uleb(life_type),
                        uleb(realloc_type)])),
        section(5, vec([b"\x00\x01"])),
        section(7, vec([
            name("on-load") + b"\x00" + uleb(len(imports)),
            name("on-enable") + b"\x00" + uleb(len(imports) + 1),
            name("on-disable") + b"\x00" + uleb(len(imports) + 2),
            name("cabi_realloc") + b"\x00" + uleb(len(imports) + 3),
        ])),
        section(10, vec([
            uleb(len(b"\x01\x03\x7f" + load)) + b"\x01\x03\x7f" + load,
            body(b"\x0b"), body(b"\x0b"), body(i32(1024) + b"\x0b"),
        ])),
        section(11, vec([active_data(0, bytes(data))])),
    ])

    memory = [b"\x03\x00"]
    # Function indices start after every import; cabi_realloc is the fourth
    # defined function (on-load, on-enable, on-disable, cabi_realloc).
    allocating = [b"\x03\x00", b"\x04" + uleb(len(imports) + 3)]
    return component(
        type_section(types),
        section(10, vec([
            instance_import(interface, 48), instance_import(text_interface, 46),
        ])),
        section(6, vec(
            [func_alias(0, member) for member in members]
            + [func_alias(0, "[async method]item-stack.get-registry-key"),
               func_alias(1, text_member)]
        )),
        section(8, vec([
            canon_lower(0, memory), canon_lower(1, memory), canon_lower(2, allocating),
            canon_lower(3, allocating), canon_lower(4, memory), canon_lower(5, allocating),
            canon_lower(6, memory), canon_lower(7, memory), canon_lower(8, allocating),
            canon_lower(9, memory), canon_lower(10, memory), canon_lower(11, allocating),
            canon_lower(12, ()), canon_lower(13, allocating), canon_lower(14, memory),
            canon_lower(15, allocating), canon_lower(16, memory), canon_lower(17, memory),
            canon_lower(18, ()), canon_lower(19, allocating), canon_lower(20, memory),
        ])),
        section(1, core),
        core_instance_section([
            core_inline(
                [(member, index) for index, member in enumerate(members)]
                + [("[async method]item-stack.get-registry-key", 19)]
            ),
            core_inline([(text_member, 20)]),
            core_instantiate(0, [(interface, 0), (text_interface, 1)]),
        ]),
        section(6, vec([
            core_alias(2, "on-load"), core_alias(2, "on-enable"),
            core_alias(2, "on-disable"),
        ])),
        section(8, vec([
            canon_lift(21, lift_type), canon_lift(22, lift_type),
            canon_lift(23, lift_type),
        ])),
        section(11, vec([
            component_export("on-load", index=21),
            component_export("on-enable", index=22),
            component_export("on-disable", index=23),
        ])),
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: wasm_component_run_test.py <plusmc-wasm>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-component-run-") as temporary:
        root = Path(temporary)

        def fixture(label, data):
            path = root / f"{label}.wasm"
            path.write_bytes(data)
            return path

        hosted = fixture("hosted", hosted_component())

        # 1. The linked no-import shape runs: instantiate, core alias, lift.
        proc = run(cli, ["run", hosted])
        for fragment in (
            "component: ",
            "status: completed",
            "lifecycle: load=0 enable=0 disable=0",
        ):
            if fragment not in proc.stdout:
                raise AssertionError(f"run output misses {fragment!r}:\n{proc.stdout}")
        print("ok 1 linked lifecycle runs")

        # 2. JSON is deterministic, names its kind, and has no host calls.
        first = run(cli, ["run", hosted, "--json"])
        second = run(cli, ["run", hosted, "--json"])
        if first.stdout != second.stdout:
            raise AssertionError("two identical JSON runs differ")
        document = json.loads(first.stdout)
        if document["kind"] != "component" or document["status"] != "completed":
            raise AssertionError(f"unexpected JSON verdict: {document}")
        if document["host_calls"] != []:
            raise AssertionError(f"unexpected host calls: {document['host_calls']}")
        if document["lifecycle"] != {
            "load": {"ran": True, "result": 0},
            "enable": {"ran": True, "result": 0},
            "disable": {"ran": True, "result": 0},
        }:
            raise AssertionError(f"unexpected lifecycle: {document['lifecycle']}")
        print("ok 2 JSON report is kind=component and deterministic")

        # 3. All four zero-argument backed Pumpkin members are lowered, imported
        # and called.
        four = fixture("four-members", four_member_component())
        proc = run(cli, ["run", four, "--json"])
        report = json.loads(proc.stdout)
        expected_calls = [
            "player_count=0",
            "world_time=0",
            "world_is_raining=0",
            "world_is_thundering=0",
        ]
        if report["host_calls"] != expected_calls:
            raise AssertionError(f"unexpected host calls: {report['host_calls']}")
        if report["status"] != "completed":
            raise AssertionError(f"four-member run did not complete: {report}")
        text = run(cli, ["run", four]).stdout
        for call in expected_calls:
            if call not in text:
                raise AssertionError(f"text report misses {call!r}:\n{text}")
        print("ok 3 the four zero-argument backed members reach plusmc:host")

        # 4. A component without on-load is refused.
        missing = fixture(
            "missing-load",
            hosted_component(export_names=("on-enable", "on-disable")),
        )
        expect_error(run(cli, ["run", missing], expect=1), "does not export on-load")
        print("ok 4 a component without on-load is refused")

        # 5. An untracked interface has no host mapping.
        gui = fixture("untracked", member_component("pumpkin:plugin/gui@0.2.0", "show"))
        expect_error(run(cli, ["run", gui], expect=1), "tracked pumpkin:plugin@0.2.0")
        print("ok 5 an untracked interface is refused")

        # 6. An async member with the flat shape the linker expects is backed;
        # a helper component that omits the implicit server handle is refused
        # for its shape rather than mistaken for a valid call.
        broadcast = fixture(
            "broadcast",
            member_component(SERVER, "[method]server.broadcast", results=()),
        )
        expect_error(
            run(cli, ["run", broadcast], expect=1),
            "[method]server.broadcast flattens to ()->()",
            "tracked signature is (i32,i32,i32)->()",
        )
        print("ok 6 a wrongly shaped async member is refused")

        # 7. A block member with the wrong component signature is refused for
        # its shape, not for the ABI. world.get-block itself is backed below.
        block = fixture("get-block", member_component(WORLD, "[method]world.get-block"))
        expect_error(
            run(cli, ["run", block], expect=1),
            "[method]world.get-block flattens to ()->(i32)",
        )
        print("ok 7 a wrongly shaped block member is refused")

        # 8. A core import whose flattened signature disagrees is refused.
        mismatch = fixture("signature-mismatch", four_member_component(player_result=I64))
        expect_error(run(cli, ["run", mismatch], expect=1), "flattens to", "imported as")
        print("ok 8 a signature mismatch is refused")

        # 9. Memory, realloc and post-return options are parsed and validated.
        optioned = fixture(
            "options",
            hosted_component(
                core=core_module(memory=True, extra_funcs=True),
                lift_options=[b"\x03" + uleb(0), b"\x04" + uleb(3), b"\x05" + uleb(4)],
            ),
        )
        proc = run(cli, ["run", optioned])
        if "status: completed" not in proc.stdout:
            raise AssertionError(f"canonical options were not accepted:\n{proc.stdout}")
        print("ok 9 memory/realloc/post-return options are accepted")

        # 10. A memory option that names another memory is refused.
        bad_memory = fixture(
            "bad-memory",
            hosted_component(
                core=core_module(memory=True, extra_funcs=True),
                lift_options=[b"\x03" + uleb(3)],
            ),
        )
        expect_error(run(cli, ["run", bad_memory], expect=1), "names memory 3")
        print("ok 10 a bad memory option is refused")

        # 11. The async canonical option on a synchronous lifecycle lift is
        # refused by name; async lowers are exercised separately by check 38.
        async_option = fixture(
            "async-option",
            hosted_component(core=core_module(), lift_options=[b"\x06"]),
        )
        expect_error(run(cli, ["run", async_option], expect=1), "async")
        print("ok 11 an async option on a synchronous lift is refused")

        # 12. Two core modules are more than the linker hosts.
        two = fixture(
            "two-modules",
            component(
                type_section([comp_func()]),
                section(1, core_module()),
                section(1, core_module()),
            ),
        )
        expect_error(run(cli, ["run", two], expect=1), "holds 2 core module(s)")
        print("ok 12 two core modules are refused")

        # 13. A lifecycle entry with parameters is not the hosted ABI.
        parameterized = fixture(
            "parameterized",
            hosted_component(core=core_module(on_load_params=(I32,))),
        )
        expect_error(run(cli, ["run", parameterized], expect=1), "takes parameters")
        print("ok 13 a parameterised entry point is refused")

        # 14. A lift that names no core alias is caught before execution.
        bad_lift = fixture("bad-lift", hosted_component())
        bad_lift.write_bytes(
            hosted_component().replace(
                canon_lift(0, 0, ()), canon_lift(99, 0, ()), 1
            )
        )
        expect_error(run(cli, ["run", bad_lift], expect=1), "core function 99")
        print("ok 14 an out-of-range lift is refused")

        # 15. A budget stop in the linked module is reported as a budget.
        looping = fixture(
            "looping",
            hosted_component(core=core_module(on_load_code=b"\x03\x40\x0c\x00\x0b")),
        )
        proc = run(cli, ["run", looping, "--max-steps", "200"], expect=1)
        if "status: budget" not in proc.stdout:
            raise AssertionError(f"expected a budget status:\n{proc.stdout}")
        print("ok 15 a budget stop is reported as a budget")

        # 16. inspect still classifies the same file as a component.
        proc = run(cli, ["inspect", hosted, "--json"])
        if json.loads(proc.stdout)["status"] != "component":
            raise AssertionError("inspect no longer reports the component status")
        print("ok 16 inspect still reports component")

        # 17. A UTF-8 string argument crosses canon lower to plusmc:host/log.
        string_log = fixture("string-log", string_log_component())
        report = json.loads(run(cli, ["run", string_log, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"string log run did not complete: {report}")
        if report["log"] != ["log: hello"]:
            raise AssertionError(f"the string did not reach the host: {report['log']}")
        print("ok 17 a UTF-8 string reaches plusmc:host/log")

        # 18. Pumpkin's enum + string logging.log reaches the severitied host call.
        logging = fixture("logging", logging_component())
        report = json.loads(run(cli, ["run", logging, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"logging.log run did not complete: {report}")
        if report["log"] != ["log: hi"]:
            raise AssertionError(f"the message did not reach the host: {report['log']}")
        if report["host_calls"] != ["log_level=3"]:
            raise AssertionError(f"the severity was lost: {report['host_calls']}")
        print("ok 18 logging.log carries its level and string")

        # 19. A lifted string result is read from its return area, then post-return runs.
        string_result = fixture("string-result", string_result_component())
        report = json.loads(run(cli, ["run", string_result, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"string-result run did not complete: {report}")
        if report["returns"] != ["hello"]:
            raise AssertionError(f"the lifted string was not read: {report.get('returns')}")
        if report["log"] != ["log: freed"]:
            raise AssertionError(f"post-return did not run: {report.get('log')}")
        print("ok 19 a lifted string result is read and post-return runs")

        # 20. The real v0.2 lifecycle: init-plugin, async on-load, async
        # on-unload, task.return and resource.drop around a logging lower.
        v02 = fixture("pumpkin-v02", pumpkin_v02_component())
        report = json.loads(run(cli, ["run", v02, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"v0.2 lifecycle did not complete: {report}")
        if report["log"] != ["log: init", "log: loaded", "log: unload"]:
            raise AssertionError(f"v0.2 lifecycle calls were not in order: {report['log']}")
        if report["host_calls"] != ["log_level=2"] * 3:
            raise AssertionError(f"v0.2 logging levels were lost: {report['host_calls']}")
        lifecycle = report["lifecycle"]
        if not lifecycle["load"]["ran"] or not lifecycle["disable"]["ran"]:
            raise AssertionError(f"v0.2 lifecycle flags were not reported: {lifecycle}")
        print("ok 20 v0.2 async lifecycle runs init, load and unload")

        # 21. An async export that returns result::err(string) refuses the run.
        v02_error = fixture("pumpkin-v02-error", pumpkin_v02_component(error_text="boom"))
        expect_error(
            run(cli, ["run", v02_error], expect=1),
            "on-load returned error: boom",
        )
        print("ok 21 a v0.2 error result is reported")

        # 22. An async export that never calls task.return is refused.
        v02_missing = fixture(
            "pumpkin-v02-missing-return",
            pumpkin_v02_component(call_task_return=False),
        )
        expect_error(
            run(cli, ["run", v02_missing], expect=1),
            "without calling task.return",
        )
        print("ok 22 a missing task.return is refused")

        # 23. A function import must name a function type, not an instance.
        bad_import = fixture(
            "function-import-type",
            component(
                type_section([instance_type([])]),
                section(10, vec([b"\x00" + name("bad") + b"\x01" + uleb(0)])),
            ),
        )
        expect_error(run(cli, ["run", bad_import], expect=1), "not a function type")
        print("ok 23 a function import naming a non-function type is refused")

        # 24. An instance type may not export the same member twice.
        duplicate_member = fixture(
            "duplicate-instance-member",
            component(
                type_section([comp_func(), instance_type([("dup", 0), ("dup", 0)])]),
            ),
        )
        expect_error(run(cli, ["run", duplicate_member], expect=1), "more than once")
        print("ok 24 a duplicate instance member is refused")

        # 25. A string task.return needs module memory to read the result area.
        no_memory = fixture("v02-no-memory", pumpkin_v02_no_memory_component())
        expect_error(
            run(cli, ["run", no_memory], expect=1),
            "task.return carries a string result but the module has no memory",
        )
        print("ok 25 a string task.return without memory is refused")

        # 26. A lowered string result: the host allocates the return area and
        # the bytes through the module's cabi_realloc, and the guest reads it.
        lowered_string = fixture(
            "lower-string-result", lower_string_result_component()
        )
        report = json.loads(run(cli, ["run", lowered_string, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"lowered string result did not complete: {report}")
        if report["log"] != ["log: hi from host"]:
            raise AssertionError(f"the host-returned string was not read: {report['log']}")
        if "config_get_string=12" not in report["host_calls"]:
            raise AssertionError(
                f"the host string call was not recorded: {report['host_calls']}"
            )
        print("ok 26 a lowered string result is allocated through cabi_realloc")

        # 27. A compiler-produced component with the full v0.2 world: 55 core
        # imports, standard32 names, and a callback-style lifecycle
        # (`on-load: (i32)->i32`). The compiled-core path must load it, bind the
        # real logging import, and trap only if an unbacked member is called.
        compiled = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "wasm"
            / "v02_compiled_plugin.wasm"
        )
        report = json.loads(run(cli, ["run", compiled, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"compiled full-surface run did not complete: {report}")
        if report["log"] != ["log: init", "log: loaded", "log: unload"]:
            raise AssertionError(f"compiled lifecycle calls were wrong: {report['log']}")
        if report["host_calls"] != ["log_level=2"] * 3:
            raise AssertionError(
                f"compiled logging calls were lost: {report['host_calls']}"
            )
        lifecycle = report["lifecycle"]
        if not lifecycle["load"]["ran"] or not lifecycle["disable"]["ran"]:
            raise AssertionError(f"compiled lifecycle flags were not reported: {lifecycle}")
        print("ok 27 a compiled full-surface component loads and runs its lifecycle")

        # 28. A lowered list<string> result: the host allocates the canonical
        # pair array and every key string through the module's cabi_realloc.
        lowered_list = fixture("lower-list-result", lower_list_result_component())
        report = json.loads(run(cli, ["run", lowered_list, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"lowered list result did not complete: {report}")
        if report["log"] != ["log: alpha", "log: beta"]:
            raise AssertionError(f"the host-returned keys were not read: {report['log']}")
        if "config_keys=2" not in report["host_calls"]:
            raise AssertionError(
                f"the host list call was not recorded: {report['host_calls']}"
            )
        print("ok 28 a lowered list result is allocated through cabi_realloc")

        # 29. A component produced from Rust by wasm-tools component new. Its
        # lifecycle lifts use wit-component's spaced component-function indices
        # (0, 2, 4), which the linker folds back onto the compact lift list.
        rust_component = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "wasm"
            / "rust_component.wasm"
        )
        report = json.loads(run(cli, ["run", rust_component, "--json"]).stdout)
        if report["status"] != "completed" or report["kind"] != "component":
            raise AssertionError(f"the Rust component did not complete: {report}")
        if not report["lifecycle"]["load"]["ran"]:
            raise AssertionError(f"the Rust component did not run on-load: {report}")
        if report["steps"] < 1000:
            raise AssertionError(
                f"the Rust component ran the wrong on-load lift: {report['steps']} steps"
            )
        print("ok 29 a Rust-compiled component runs its lifted lifecycle")

        # 30. A lowered record result: the host allocates a 16-byte record area
        # and one string pair per field through cabi_realloc.
        lowered_record = fixture("lower-record-result", lower_record_result_component())
        report = json.loads(run(cli, ["run", lowered_record, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"lowered record result did not complete: {report}")
        if report["log"] != ["log: greeting", "log: hi from host"]:
            raise AssertionError(f"the host-returned record was not read: {report['log']}")
        if "config_pair=12" not in report["host_calls"]:
            raise AssertionError(
                f"the host record call was not recorded: {report['host_calls']}"
            )
        print("ok 30 a lowered record result is allocated through cabi_realloc")

        # 31. world.get-block-state-id carries a named three-s32 block-pos
        # record through canon lower to plusmc:host/world_get_block.
        block_state = fixture("block-state-id", block_state_component())
        report = json.loads(run(cli, ["run", block_state, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"block-state-id run did not complete: {report}")
        if report["host_calls"] != [
            "world_get_block=0",
            "world_get_block_at=1,64,-3",
        ]:
            raise AssertionError(
                f"the block-pos call did not reach the host: {report['host_calls']}"
            )
        if not report["lifecycle"]["load"]["ran"]:
            raise AssertionError(f"the block-pos lifecycle did not run: {report}")
        print("ok 31 a named block-pos record reaches plusmc:host/world_get_block")

        # 32. world.get-block returns the full 56-byte `block` record through a
        # canon lower with memory and cabi_realloc. The core reads the name out
        # of the returned record and logs it, which proves the host allocated
        # the string pair and payload at the canonical offsets.
        block_info = fixture("block-info", block_info_component())
        proc = run(cli, ["run", block_info, "--json"])
        report = json.loads(proc.stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the block-result lifecycle did not run: {report}")
        if report["host_calls"] != ["world_get_block_info=0"]:
            raise AssertionError(f"the block call did not reach the host: {report['host_calls']}")
        if "log: minecraft:air" not in report["log"]:
            raise AssertionError(f"the block record name was not readable: {report['log']}")
        print("ok 32 world.get-block returns the full block record")

        # 33. logging.log-tracing carries an opaque list<u8> through the canonical
        # (pointer, length) pair; the report renders the bytes so the exact
        # payload is observable.
        tracing = fixture("log-tracing", log_tracing_component())
        report = json.loads(run(cli, ["run", tracing, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the log-tracing lifecycle did not run: {report}")
        if report["log"] != ["trace: 01 02 ff"]:
            raise AssertionError(f"the trace bytes were not preserved: {report['log']}")
        if report["host_calls"] != ["log_tracing=3"]:
            raise AssertionError(f"the trace did not reach the host: {report['host_calls']}")
        print("ok 33 logging.log-tracing carries a byte list")

        # 35. player.chat is async in WIT but the host completes it immediately.
        # Its message string reaches plusmc:host/player_chat, and the component
        # function type keeps the async marker while the lower stays sync.
        chat = fixture("player-chat", player_chat_component())
        report = json.loads(run(cli, ["run", chat, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the player.chat lifecycle did not run: {report}")
        if report["messages"] != ["hello"]:
            raise AssertionError(f"player.chat did not reach the host: {report}")
        if report["host_calls"] != ["player_chat=5"]:
            raise AssertionError(f"player.chat was not recorded: {report['host_calls']}")
        print("ok 35 async player.chat reaches plusmc:host/player_chat")

        # 36. world.set-block's block record is passed by pointer. The host
        # reads its default state id and performs the write through the world
        # capability, completing the async member synchronously.
        set_block = fixture("set-block", set_block_component())
        report = json.loads(run(cli, ["run", set_block, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the set-block lifecycle did not run: {report}")
        if report["host_calls"] != ["world_set_block=1"]:
            raise AssertionError(
                f"set-block did not reach the host: {report['host_calls']}"
            )
        print("ok 36 async world.set-block writes through the host")

        # 34. The checked-in cargo-component artifact: real Pumpkin v0.2 package,
        # interface and member names compiled from Rust with a synchronous
        # lifecycle. It imports WASI and real resource methods, so the strict
        # linker does not attempt it; the compiled-core path must host it. The
        # context -> server -> world chain uses typed handles, the compiled async
        # player.chat and world.set-block imports reach the host, the scheduler
        # imports are called during on-load, and the plugin registers a
        # server-tick-start handler plus a handle-task callback. This CLI run has
        # no ticks; the live-server test drives both callbacks.
        pumpkin_plugin = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "wasm"
            / "pumpkin_plugin.wasm"
        )
        fixture_sha = hashlib.sha256(pumpkin_plugin.read_bytes()).hexdigest()
        if fixture_sha != "312c9656007e9580d7dad323bd0489e2028a8b6fe4bb16fa9ef83224bd0027be":
            raise AssertionError(f"compiled Pumpkin fixture changed: {fixture_sha}")
        report = json.loads(run(cli, ["run", pumpkin_plugin, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"compiled Pumpkin plugin did not complete: {report}")
        expected_log = [
            "log: scheduled delayed=0 repeating=0 cancelled=0",
            "log: command tree nodes=4 execute=21 suggest=22 require=23 leaf=24 count=25 label=26",
            'log: context registered=Ok(()) invalid=Err("permission node must be namespaced (namespace:node)") marketplace=Some(MarketplaceMetadata { marketplace-url: "https://marketplace.pumpkinmc.org", plugin-id: 4242, plugin-name: "compiled-plugin", version: "0.2.0", dev-id: 7, dev-name: "Buffy", is-paid: false, user-id: 0, license-key: None, issued-at: "2026-11-08T00:00:00Z" }) folder=plusmc-wasm-data',
            "log: uuid roundtrip=true invalid=true fixed=123e4567-e89b-12d3-a456-426614174000",
            "log: i18n translated=hello from i18n missing=compiled:missing",
            "log: item-stack key=minecraft:diamond count=3 max=64 set=5 clone=5 with-count=7 similar=true",
            "log: item-stack durability damage=0 max=1561 damageable=true set-damage=50 unbreakable=false->true repair=0->4 enchantments=33:3,40:2 count=1 custom=custom:echo:4 level=4 present=true removed=false lore=2 custom-name=true->false",
            "log: item-stack extended model=true->true->true glint=false->true->true modifiers=1/true->true->true nbt=true/true->true component=true->true",
            "log: compiled set-block",
            "log: compiled on-load",
            "log: players=0",
            "log: all-players=0 names=",
            "log: server max=20 view=8 sim=8 gamemode=0 difficulty=2 online=true whitelist=false",
            "log: motd=A +MC server",
            "log: world time=6000 rain=false thunder=false state=0 block-id=0 has-world=true in-world=0",
            "log: world-players=0 names=",
            "log: compiled on-unload",
            "log: players=0",
            "log: all-players=0 names=",
            "log: server max=20 view=8 sim=8 gamemode=0 difficulty=2 online=true whitelist=false",
            "log: motd=A +MC server",
            "log: world time=6000 rain=false thunder=false state=0 block-id=0 has-world=true in-world=0",
            "log: world-players=0 names=",
        ]
        if report["log"] != expected_log:
            raise AssertionError(f"compiled Pumpkin log was wrong: {report['log']}")
        if report["messages"] != ["compiled broadcast", "compiled broadcast"]:
            raise AssertionError(f"compiled messages were wrong: {report['messages']}")
        expected_calls = [
            "register_event=7",
            "scheduler_schedule_delayed=0",
            "scheduler_schedule_repeating=0",
            "scheduler_schedule_repeating=0",
            "scheduler_cancel=0",
            "log_level=2",
            "command_new=2",
            "command_node_literal=3",
            "command_node_execute=21",
            "command_node_suggest=22",
            "command_node_require=23",
            "command_node_literal=4",
            "command_node_execute=24",
            "command_node_then=1",
            "command_node_argument=3",
            "command_node_argument_min=1",
            "command_node_argument_max=10",
            "command_node_execute=25",
            "command_node_then=1",
            "command_node_argument=5",
            "command_node_argument_string=2",
            "command_node_execute=26",
            "command_node_then=1",
            "command_then=1",
            "command_execute=7",
            "register_command=8",
            "log_level=2",
            "register_permission=1",
            "register_permission=0",
            "marketplace_metadata=1",
            "get_data_folder=16",
            "log_level=2",
            "uuid_generate=1",
            "uuid_to_string=36",
            "uuid_parse=1",
            "uuid_parse=0",
            "uuid_parse=1",
            "uuid_to_string=36",
            "log_level=2",
            "i18n_load_translations=2",
            "i18n_translate=15",
            "i18n_translate=16",
            "log_level=2",
            "item_stack_new=3",
            "item_stack_get_registry_key=17",
            "item_stack_get_count=3",
            "item_stack_get_max_count=64",
            "item_stack_set_count=5",
            "item_stack_clone=5",
            "item_stack_get_count=5",
            "item_stack_with_count=7",
            "item_stack_get_count=7",
            "item_stack_is_similar=1",
            "item_stack_get_count=5",
            "log_level=2",
            "item_stack_new=1",
            "item_stack_get_damage=0",
            "item_stack_get_max_damage=1561",
            "item_stack_is_damageable=1",
            "item_stack_set_damage=50",
            "item_stack_get_damage=50",
            "item_stack_is_unbreakable=0",
            "item_stack_set_unbreakable=1",
            "item_stack_is_unbreakable=1",
            "item_stack_get_repair_cost=0",
            "item_stack_set_repair_cost=4",
            "item_stack_get_repair_cost=4",
            "item_stack_add_enchantment=3",
            "item_stack_add_enchantment=2",
            "item_stack_get_enchantments=2",
            "item_stack_remove_enchantment=40",
            "item_stack_get_enchantments=1",
            "item_stack_add_custom_enchantment=4",
            "item_stack_has_custom_enchantment=1",
            "item_stack_get_custom_enchantment_level=4",
            "item_stack_get_custom_enchantments=1",
            "item_stack_remove_custom_enchantment=11",
            "item_stack_has_custom_enchantment=0",
            "text_text=17",
            "text_text=17",
            "item_stack_set_lore=1",
            "item_stack_add_lore=2",
            "item_stack_get_lore=2",
            "text_text=14",
            "item_stack_set_custom_name=1",
            "item_stack_get_custom_name=1",
            "item_stack_set_custom_name=0",
            "item_stack_get_custom_name=0",
            "log_level=2",
            "item_stack_get_item_model=0",
            "item_stack_set_item_model=1",
            "item_stack_get_item_model=1",
            "item_stack_set_item_model=0",
            "item_stack_get_item_model=0",
            "item_stack_get_enchantment_glint_override=-1",
            "item_stack_set_enchantment_glint_override=1",
            "item_stack_get_enchantment_glint_override=1",
            "item_stack_set_enchantment_glint_override=-1",
            "item_stack_get_enchantment_glint_override=-1",
            "item_stack_add_attribute_modifier=1",
            "item_stack_get_attribute_modifiers=1",
            "item_stack_remove_attribute_modifiers=0",
            "item_stack_get_attribute_modifiers=0",
            "item_stack_add_attribute_modifier=1",
            "item_stack_clear_attribute_modifiers=0",
            "item_stack_get_attribute_modifiers=0",
            "item_stack_set_custom_data=1",
            "item_stack_has_custom_data=1",
            "item_stack_get_custom_data=1",
            "item_stack_remove_custom_data=1",
            "item_stack_has_custom_data=0",
            "item_stack_get_custom_data=0",
            "item_stack_set_component=4",
            "item_stack_get_components=1",
            "item_stack_remove_component=17",
            "item_stack_get_components=0",
            "log_level=2",
            "server_world_by_name=19",
            "world_set_time=1",
            "world_set_block=1",
            "log_level=2",
            "server_player_by_name=5",
            "player_count=0",
            "log_level=2",
            "log_level=2",
            "server_all_players=0",
            "log_level=2",
            "server_max_players=20",
            "server_view_distance=8",
            "server_simulation_distance=8",
            "server_default_gamemode=0",
            "server_difficulty=2",
            "server_online_mode=1",
            "server_whitelist_enabled=0",
            "log_level=2",
            "server_motd=12",
            "log_level=2",
            "server_world_by_name=19",
            "world_time=6000",
            "world_is_raining=0",
            "world_is_thundering=0",
            "world_get_block=0",
            "world_get_block_at=1,64,-2",
            "world_get_block_id=0",
            "world_get_block_id_at=1,64,-2",
            "server_has_world=1",
            "server_player_count_in_world=0",
            "log_level=2",
            "server_world_by_name=19",
            "server_players_in_world=0",
            "log_level=2",
            "broadcast=0",
            "server_player_by_name=5",
            "player_count=0",
            "log_level=2",
            "log_level=2",
            "server_all_players=0",
            "log_level=2",
            "server_max_players=20",
            "server_view_distance=8",
            "server_simulation_distance=8",
            "server_default_gamemode=0",
            "server_difficulty=2",
            "server_online_mode=1",
            "server_whitelist_enabled=0",
            "log_level=2",
            "server_motd=12",
            "log_level=2",
            "server_world_by_name=19",
            "world_time=6000",
            "world_is_raining=0",
            "world_is_thundering=0",
            "world_get_block=0",
            "world_get_block_at=1,64,-2",
            "world_get_block_id=0",
            "world_get_block_id_at=1,64,-2",
            "server_has_world=1",
            "server_player_count_in_world=0",
            "log_level=2",
            "server_world_by_name=19",
            "server_players_in_world=0",
            "log_level=2",
            "broadcast=0",
            "server_player_by_name=5",
        ]
        if report["host_calls"] != expected_calls:
            raise AssertionError(
                f"compiled Pumpkin host calls were wrong: {report['host_calls']}"
            )
        lifecycle = report["lifecycle"]
        if not lifecycle["load"]["ran"] or not lifecycle["disable"]["ran"]:
            raise AssertionError(
                f"compiled Pumpkin lifecycle flags were missing: {lifecycle}"
            )
        print("ok 34 a cargo-component-built Pumpkin plugin runs end to end")

        # 37. The same artifact with a host player proves the messaging path
        # end to end: server.broadcast reaches the host, the player lookup
        # returns a typed host-issued handle, player.chat uses that same handle,
        # the plain text resource is built and mutated, and
        # player.send-system-message receives both handles and the rendered text.
        report = json.loads(
            run(cli, ["run", pumpkin_plugin, "--json", "--host-player", "Buffy"]).stdout
        )
        if report["status"] != "completed":
            raise AssertionError(f"compiled Pumpkin messaging run did not complete: {report}")
        expected_messages = [
            "compiled player chat",
            "compiled broadcast",
            "compiled system message for Buffy",
            "compiled broadcast",
            "compiled system message for Buffy",
        ]
        if report["messages"] != expected_messages:
            raise AssertionError(f"compiled messaging was wrong: {report['messages']}")
        if "log: all-players=1 names=Buffy" not in report["log"]:
            raise AssertionError(f"server.get-all-players was not logged: {report['log']}")
        if "log: world-players=1 names=Buffy" not in report["log"]:
            raise AssertionError(
                f"server.get-players-in-world was not logged: {report['log']}"
            )
        if "log: player id=00112233-4455-6677-8899-aabbccddeeff" not in report["log"]:
            raise AssertionError(f"player.get-id was not logged: {report['log']}")
        if "log: player name=Buffy" not in report["log"]:
            raise AssertionError(f"player.get-name was not logged: {report['log']}")
        if "log: player geometry=12,64,-3,90,-15" not in report["log"]:
            raise AssertionError(f"player geometry was not logged: {report['log']}")
        if "log: player gamemode=0" not in report["log"]:
            raise AssertionError(f"player gamemode was not logged: {report['log']}")
        if "log: player health=20" not in report["log"]:
            raise AssertionError(f"player health was not logged: {report['log']}")
        if "log: player vitals max=20 food=18 saturation=4 exhaustion=1" not in report["log"]:
            raise AssertionError(f"player vitals were not logged: {report['log']}")
        if "log: player absorption=2" not in report["log"]:
            raise AssertionError(f"player absorption was not logged: {report['log']}")
        if "log: player experience level=30 progress=1 points=1234" not in report["log"]:
            raise AssertionError(f"player experience was not logged: {report['log']}")
        if "log: player camera-entity=42" not in report["log"]:
            raise AssertionError(f"player camera entity was not logged: {report['log']}")
        if "player_get_name=5" not in report["host_calls"]:
            raise AssertionError(
                f"player.get-name did not reach the host: {report['host_calls']}"
            )
        for call in ("broadcast=1", "server_player_by_name=5", "player_chat=20",
                     "player_get_id=1", "player_get_position_x=12",
                     "player_get_position_y=64",
                     "player_get_position_z=-3", "player_get_yaw=90",
                     "player_get_pitch=-15", "player_get_gamemode=0",
                     "player_get_health=20", "player_get_max_health=20",
                     "player_get_food_level=18", "player_get_saturation=4",
                     "player_get_exhaustion=1", "player_get_absorption=2",
                     "player_get_experience_level=30",
                     "player_get_experience_progress=1",
                     "player_get_experience_points=1234",
                     "player_get_camera_entity_id=42",
                     "server_player_count_in_world=1",
                     "server_all_players=1", "server_players_in_world=1",
                     "text_text=23", "text_add_text=10", "text_color_named=6",
                     "text_bold=1", "text_italic=1", "text_underlined=1",
                     "text_strikethrough=0", "text_obfuscated=0",
                     "text_insertion=5", "text_font=17",
                     "text_click_run_command=10", "text_click_change_page=2",
                     "text_text=15", "text_color_rgb=255", "text_hover_show_text=15",
                     "player_send_system_message=33",
                     "player_send_system_message_overlay=1"):
            if call not in report["host_calls"]:
                raise AssertionError(f"compiled messaging missed {call!r}: {report['host_calls']}")
        print("ok 37 server broadcast and player system messages reach the host")

        # 38. Both async members use the async lower ABI: each returns a subtask
        # status, the guest joins both subtasks to one waitable set, and calls
        # waitable-set.wait for each returned event. The host operations are
        # one-shot, but delivery goes through the task runtime.
        async_component = fixture("async-task", async_task_component())
        report = json.loads(run(cli, ["run", async_component, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the async task component did not complete: {report}")
        if report["messages"] != ["hello"]:
            raise AssertionError(f"the async chat did not reach the host: {report['messages']}")
        expected_async_calls = [
            "player_chat=5",
            "world_set_block=1",
            "waitable_set_wait=1",
            "waitable_set_wait=1",
        ]
        if report["host_calls"] != expected_async_calls:
            raise AssertionError(f"the async task calls were wrong: {report['host_calls']}")
        print("ok 38 async lowers use subtasks and waitable-set delivery")

        # 39. The strict path can now parse and lower an option of a resource.
        # server.get-player-by-name's core import carries the 8-byte return-area
        # pointer as its fourth parameter; before option<handle> support this
        # component was refused at load time.
        option_link = fixture("option-player-link", option_player_link_component())
        report = json.loads(run(cli, ["run", option_link, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the option<player> link did not run: {report}")
        print("ok 39 option<player> links on the strict path")

        # 40. The strict linker reads the item-stack resource, its option and
        # record shapes, the NBT variant/tree, and data-component-value; all
        # calls go through canon-lowered imports rather than trapping stubs.
        item_stack = fixture("strict-item-stack", strict_item_stack_component())
        report = json.loads(run(cli, ["run", item_stack, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the strict item-stack component did not run: {report}")
        expected_calls = [
            "item_stack_new=1",
            "item_stack_get_registry_key=17",
            "item_stack_get_item_model=0",
            "item_stack_set_item_model=0",
            "item_stack_get_enchantment_glint_override=-1",
            "item_stack_set_enchantment_glint_override=-1",
            "item_stack_get_attribute_modifiers=0",
            "item_stack_add_attribute_modifier=1",
            "item_stack_get_custom_data=0",
            "item_stack_set_custom_data=1",
            "item_stack_get_components=0",
            "item_stack_set_component=3",
            "item_stack_add_enchantment=3",
            "item_stack_get_enchantments=1",
            "item_stack_add_custom_enchantment=4",
            "item_stack_get_custom_enchantments=1",
            "text_text=4",
            "item_stack_set_lore=1",
            "item_stack_get_lore=1",
            "text_text=4",
            "item_stack_set_custom_name=1",
            "item_stack_get_custom_name=1",
            "item_stack_set_custom_name=0",
            "item_stack_get_custom_name=0",
        ]
        if report["host_calls"] != expected_calls:
            raise AssertionError(f"strict item-stack calls were wrong: {report['host_calls']}")
        print("ok 40 item-stack records, NBT and components link strictly")

        # 41. i18n's translated string is a strict lower result: the host writes
        # the pair through the guest's return area and the lifecycle concrete
        # string is lifted back into the report.
        strict_i18n = fixture("strict-i18n", strict_i18n_result_component())
        report = json.loads(run(cli, ["run", strict_i18n, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the strict i18n component did not run: {report}")
        if report["returns"] != ["hello from strict i18n"]:
            raise AssertionError(f"strict i18n did not return its translation: {report}")
        if report["host_calls"] != [
            "i18n_load_translations=1",
            "i18n_translate=22",
        ]:
            raise AssertionError(f"strict i18n calls were wrong: {report['host_calls']}")
        print("ok 41 strict i18n.translate returns a string result")

        # 42. uuid.to-string proves the two-u64 record parameter and the string
        # result together; the value is the fixed RFC 4122 UUID.
        strict_uuid = fixture("strict-uuid", strict_uuid_result_component())
        report = json.loads(run(cli, ["run", strict_uuid, "--json"]).stdout)
        if report["status"] != "completed":
            raise AssertionError(f"the strict uuid component did not run: {report}")
        if report["returns"] != ["123e4567-e89b-12d3-a456-426614174000"]:
            raise AssertionError(f"strict uuid did not return its string: {report}")
        if report["host_calls"] != ["uuid_to_string=36"]:
            raise AssertionError(f"strict uuid calls were wrong: {report['host_calls']}")
        print("ok 42 strict uuid.to-string accepts a uuid and returns a string")

        # 43. The async v0.2 context is the only way to obtain a server, and the
        # server lookup is the only way to obtain a player handle. Calling
        # get-name through that chain proves strict player names are host-issued,
        # not a fabricated handle or a link-only test.
        strict_player = fixture("strict-player-name", strict_player_name_component())
        report = json.loads(
            run(cli, ["run", strict_player, "--json", "--host-player", "Buffy"]).stdout
        )
        if report["status"] != "completed":
            raise AssertionError(f"the strict player-name component did not run: {report}")
        if "log: Buffy" not in report["log"]:
            raise AssertionError(f"strict player.get-name did not log the name: {report['log']}")
        for call in ("server_player_by_name=5", "player_get_name=5"):
            if call not in report["host_calls"]:
                raise AssertionError(
                    f"strict player.get-name missed {call!r}: {report['host_calls']}"
                )
        print("ok 43 strict player.get-name uses a host-issued player handle")
        print("all 43 component-run check groups passed")


if __name__ == "__main__":
    main()
