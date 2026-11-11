#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Network-NBT text components, shared by the integration tests.

Almost every Component field in the play/configuration states is encoded with
``NbtIo.writeAnyTag``: a tag-id byte followed by the payload, with the root
name omitted. The client decodes it through ``ComponentSerialization.CODEC`` on
``NbtOps`` (``FriendlyByteBuf.writeNbt`` -> ``NbtIo.writeAnyTag``); a bare
``TAG_String`` is the literal component and colouring uses the compound
``{text, color}`` form.

Writing something else — most temptingly a length-prefixed JSON string, which
is what the login-state disconnect actually wants — makes the client read a
*length* as a tag id, fail ``TagTypes.getType``, and drop the player with

    DecoderException: Failed to decode packet
    'clientbound/minecraft:system_chat'

Decoding here rather than eyeballing the bytes is what keeps that from
regressing: every SystemChat in the suite goes through ``chat_text``.
"""

import struct


def rd_nbt_component(body, pos=0):
    """Decodes a network-NBT text component; returns (text, new_pos).

    Only the two shapes this server emits are accepted (a literal string and
    ``{text, color}``), so an unexpected tag is a hard failure rather than a
    silent misparse.
    """
    tag = body[pos]
    pos += 1
    if tag == 0x08:  # TAG_String -> Component.literal(text)
        n = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2
        return body[pos:pos + n].decode(), pos + n
    if tag == 0x0A:  # TAG_Compound {text, color?, ...}
        fields = {}
        while True:
            t = body[pos]
            pos += 1
            if t == 0x00:  # TAG_End
                break
            assert t == 0x08, f"component: nested tag {t} is not a string"
            nl = struct.unpack(">H", body[pos:pos + 2])[0]
            pos += 2
            name = body[pos:pos + nl].decode()
            pos += nl
            vl = struct.unpack(">H", body[pos:pos + 2])[0]
            pos += 2
            fields[name] = body[pos:pos + vl].decode()
            pos += vl
        assert "text" in fields, f"component compound has no text: {fields}"
        return fields["text"], pos
    raise AssertionError(f"text component: tag {tag} is not a component "
                         f"(a length byte? that is the length-prefixed-string bug)")


def rd_nbt_translate(body, pos=0):
    """Decodes a network-NBT `{translate, with?}` component.

    Returns ``(key, [args], new_pos)``. Death messages are
    ``Component.translatable(key, args)``: the server sends the lang key and the
    arguments and the *client* resolves the wording from its own language file,
    so an assertion has to look at the key, not at rendered text.
    """
    assert body[pos] == 0x0A, f"translate component: tag {body[pos]} is not a compound"
    pos += 1
    fields = {}
    while True:
        t = body[pos]
        pos += 1
        if t == 0x00:  # TAG_End
            break
        nl = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2
        name = body[pos:pos + nl].decode()
        pos += nl
        if t == 0x08:  # TAG_String
            vl = struct.unpack(">H", body[pos:pos + 2])[0]
            pos += 2
            fields[name] = body[pos:pos + vl].decode()
            pos += vl
        elif t == 0x09:  # TAG_List of {text} compounds
            assert name == "with", f"component list {name!r} is not `with`"
            elem = body[pos]
            pos += 1
            assert elem == 0x0A, f"`with` element type {elem} is not a compound"
            count = struct.unpack(">i", body[pos:pos + 4])[0]
            pos += 4
            args = []
            for _ in range(count):
                # A list element is the compound's payload: its entries and
                # TAG_End, with no tag id of its own. The element type in the
                # header already declared it (an id here is one byte the client
                # would read as the first entry's type — a corrupt tag tree).
                text = None
                while True:
                    t2 = body[pos]
                    pos += 1
                    if t2 == 0x00:
                        break
                    assert t2 == 0x08, f"`with` element field: tag {t2} is not a string"
                    nl2 = struct.unpack(">H", body[pos:pos + 2])[0]
                    pos += 2
                    name2 = body[pos:pos + nl2].decode()
                    pos += nl2
                    vl2 = struct.unpack(">H", body[pos:pos + 2])[0]
                    pos += 2
                    value = body[pos:pos + vl2].decode()
                    pos += vl2
                    if name2 == "text":
                        text = value
                args.append(text)
            fields[name] = args
        else:
            raise AssertionError(f"translate component: unexpected tag {t}")
    assert "translate" in fields, f"component compound has no translate: {fields}"
    return fields["translate"], fields.get("with", []), pos


def chat_translate(body):
    """SystemChat (0x79) body -> (key, args), with the trailing overlay byte
    checked, so a field-order slip cannot hide."""
    key, args, pos = rd_nbt_translate(body, 0)
    assert pos == len(body) - 1, (
        f"system_chat translate left {len(body) - 1 - pos} byte(s) before the overlay")
    return key, args


def chat_text(body):
    """SystemChat (0x79) body -> text.

    The component must decode as NBT and leave exactly the trailing overlay
    byte, so a field-order or length slip cannot hide.
    """
    text, pos = rd_nbt_component(body, 0)
    assert pos == len(body) - 1, (
        f"system_chat component left {len(body) - 1 - pos} byte(s) before the "
        f"overlay (body={len(body)}, consumed={pos})")
    return text


def rd_nbt_tree(body, pos=0):
    """Decodes a network-NBT text component of *any* shape into a dict.

    Returns ``(node, new_pos)`` where ``node`` holds whichever fields the
    component carries — ``text``/``translate``/``color`` as strings and
    ``with``/``extra`` as lists of child nodes. This is the general form the
    flat ``rd_nbt_component`` and ``rd_nbt_translate`` deliberately are not: an
    advancement announcement nests a translatable title inside a coloured,
    bracketed component, which neither flat decoder can express.

    The component is a *root* tag: its tag id comes first (`writeAnyTag`). The
    children of a list are payloads (`rd_nbt_payload`).
    """
    tag = body[pos]
    pos += 1
    if tag == 0x08:  # TAG_String -> Component.literal(text)
        n = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2
        return {"text": body[pos:pos + n].decode()}, pos + n
    assert tag == 0x0A, (
        f"component: tag {tag} is not a component (a length byte? that is the "
        f"length-prefixed-string bug)")
    return rd_nbt_payload(body, pos)


def rd_nbt_payload(body, pos):
    """Decodes a compound's payload (its entries + TAG_End) into a dict.

    This is what a TAG_List element is: the list header declares the element
    type once, so the elements carry no tag id of their own.
    """
    node = {}
    while True:
        t = body[pos]
        pos += 1
        if t == 0x00:  # TAG_End
            break
        nl = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2
        name = body[pos:pos + nl].decode()
        pos += nl
        if t == 0x08:  # TAG_String
            vl = struct.unpack(">H", body[pos:pos + 2])[0]
            pos += 2
            node[name] = body[pos:pos + vl].decode()
            pos += vl
        elif t == 0x09:  # TAG_List of compounds (`with`, `extra`)
            elem = body[pos]
            pos += 1
            assert elem == 0x0A, f"{name!r} element type {elem} is not a compound"
            count = struct.unpack(">i", body[pos:pos + 4])[0]
            pos += 4
            children = []
            for _ in range(count):
                # A list element carries the compound's payload only; the header
                # already said `compound`. Writing a tag id here would shift
                # every following entry, which is exactly the encoding bug the
                # server's first announcement/death-message components shipped.
                child, pos = rd_nbt_payload(body, pos)
                children.append(child)
            node[name] = children
        else:
            raise AssertionError(f"component field {name!r}: tag {t} unsupported")
    return node, pos


def chat_tree(body):
    """SystemChat (0x79) body -> the whole component tree, overlay checked."""
    node, pos = rd_nbt_tree(body, 0)
    assert pos == len(body) - 1, (
        f"system_chat component left {len(body) - 1 - pos} byte(s) before the "
        f"overlay (body={len(body)}, consumed={pos})")
    return node
