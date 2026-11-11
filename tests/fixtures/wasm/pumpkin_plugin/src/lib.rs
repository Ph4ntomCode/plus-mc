#![allow(warnings)]

mod bindings;

use bindings::pumpkin::plugin::attributes::{Attribute, AttributeModifier, ModifierOperation};
use bindings::pumpkin::plugin::common::{
    Locale, NamedColor, NbtEntry, NbtTag, NbtTree, RgbColor,
};
use bindings::pumpkin::plugin::data_components::DataComponent;
use bindings::pumpkin::plugin::enchantments::{AttributeModifierSlot, Enchantment};
use bindings::pumpkin::plugin::command::{ArgumentType, CommandNode, StringType};
use bindings::pumpkin::plugin::event::{Event, EventPriority, EventType};
use bindings::pumpkin::plugin::i18n;
use bindings::pumpkin::plugin::item_stack;
use bindings::pumpkin::plugin::logging::{self, Level};
use bindings::pumpkin::plugin::permission::{Permission, PermissionChild, PermissionDefault};
use bindings::pumpkin::plugin::player::Player;
use bindings::pumpkin::plugin::scheduler;
use bindings::pumpkin::plugin::text::TextComponent;
use bindings::pumpkin::plugin::uuid;
use bindings::pumpkin::plugin::world::{Block, BlockFlags, BlockPos};
use bindings::Guest;

struct Component;

fn report(ctx: bindings::Context, phase: &str) {
    let server = ctx.get_server();
    let players = server.get_player_count();
    logging::log(Level::Info, phase);
    logging::log(Level::Info, &format!("players={players}"));
    let all_players = server.get_all_players();
    let all_names = all_players
        .iter()
        .map(|player| player.get_name())
        .collect::<Vec<_>>()
        .join(",");
    logging::log(
        Level::Info,
        &format!("all-players={} names={all_names}", all_players.len()),
    );
    let max_players = server.get_max_players();
    let view_distance = server.get_view_distance();
    let simulation_distance = server.get_simulation_distance();
    let default_gamemode = server.get_default_gamemode() as u8;
    let difficulty = server.get_difficulty() as u8;
    let online_mode = server.is_online_mode();
    let whitelist = server.has_whitelist();
    logging::log(
        Level::Info,
        &format!(
            "server max={max_players} view={view_distance} sim={simulation_distance} gamemode={default_gamemode} difficulty={difficulty} online={online_mode} whitelist={whitelist}"
        ),
    );
    let motd = server.get_motd();
    logging::log(Level::Info, &format!("motd={motd}"));
    if let Some(world) = server.get_world_by_name("minecraft:overworld") {
        let time = world.get_time_of_day();
        let raining = world.is_raining();
        let thundering = world.is_thundering();
        let state = world.get_block_state_id(BlockPos { x: 1, y: 64, z: -2 });
        let block_id = world.get_block_id(BlockPos { x: 1, y: 64, z: -2 });
        let has_world = server.has_world("minecraft:overworld");
        let in_world = server.get_player_count_in_world(world);
        logging::log(
            Level::Info,
            &format!(
                "world time={time} rain={raining} thunder={thundering} state={state} block-id={block_id} has-world={has_world} in-world={in_world}"
            ),
        );
    } else {
        logging::log(Level::Warn, "no world");
    }
    if let Some(world) = server.get_world_by_name("minecraft:overworld") {
        let world_players = server.get_players_in_world(world);
        let world_names = world_players
            .iter()
            .map(|player| player.get_name())
            .collect::<Vec<_>>()
            .join(",");
        logging::log(
            Level::Info,
            &format!(
                "world-players={} names={world_names}",
                world_players.len()
            ),
        );
    }
    bindings::pumpkin::plugin::server::Server::broadcast(&server, "compiled broadcast");
    if let Some(player) = server.get_player_by_name("Buffy") {
        let player_id = uuid::to_string(player.get_id());
        logging::log(Level::Info, &format!("player id={player_id}"));
        let player_name = player.get_name();
        logging::log(Level::Info, &format!("player name={player_name}"));
        let (x, y, z) = player.get_position();
        let yaw = player.get_yaw();
        let pitch = player.get_pitch();
        logging::log(
            Level::Info,
            &format!("player geometry={x},{y},{z},{yaw},{pitch}"),
        );
        let gamemode = player.get_gamemode() as u8;
        logging::log(Level::Info, &format!("player gamemode={gamemode}"));
        let health = player.get_health();
        logging::log(Level::Info, &format!("player health={health}"));
        let max_health = player.get_max_health();
        let food_level = player.get_food_level();
        let saturation = player.get_saturation();
        let exhaustion = player.get_exhaustion();
        logging::log(
            Level::Info,
            &format!(
                "player vitals max={max_health} food={food_level} saturation={saturation} exhaustion={exhaustion}"
            ),
        );
        let absorption = player.get_absorption();
        logging::log(
            Level::Info,
            &format!("player absorption={absorption}"),
        );
        let experience_level = player.get_experience_level();
        let experience_progress = player.get_experience_progress();
        let experience_points = player.get_experience_points();
        logging::log(
            Level::Info,
            &format!(
                "player experience level={experience_level} progress={experience_progress} points={experience_points}"
            ),
        );
        let camera_entity_id = player.get_camera_entity_id();
        logging::log(
            Level::Info,
            &format!("player camera-entity={camera_entity_id}"),
        );
        let message = TextComponent::text("compiled system message");
        message.add_text(" for Buffy");
        message.color_named(NamedColor::Gold);
        message.bold(true);
        message.italic(true);
        message.underlined(true);
        message.strikethrough(false);
        message.obfuscated(false);
        message.insertion("Buffy");
        message.font("minecraft:default");
        message.click_run_command("/say Buffy");
        message.click_change_page(2);
        let hover = TextComponent::text("hover for Buffy");
        hover.color_rgb(RgbColor { r: 255, g: 0, b: 0 });
        message.hover_show_text(hover);
        player.send_system_message(message, true);
    }
}

fn exercise_async_members(ctx: &bindings::Context) {
    let server = ctx.get_server();
    if let Some(world) = server.get_world_by_name("minecraft:overworld") {
        world.set_time_of_day(6000);
        let block = Block {
            id: 1,
            name: "minecraft:stone".to_string(),
            hardness: 1.5,
            blast_resistance: 6.0,
            map_color: 0,
            slipperiness: 0.6,
            velocity_multiplier: 1.0,
            jump_velocity_multiplier: 1.0,
            item_id: 1,
            default_state_id: 7,
            state_ids: vec![7],
            is_solid: true,
            is_air: false,
            is_flammable: false,
            flammable: None,
        };
        bindings::pumpkin::plugin::world::World::set_block(
            &world,
            BlockPos { x: 2, y: 64, z: -3 },
            &block,
            BlockFlags::empty(),
        );
        logging::log(Level::Info, "compiled set-block");
    }

    // Chat only when the host actually has this player. The handle comes from
    // `server.get-player-by-name`, so the compiled `player.chat` call uses the
    // host's registry handle rather than a guest-fabricated index.
    if let Some(player) = server.get_player_by_name("Buffy") {
        Player::chat(&player, "compiled player chat");
    }
}

impl Guest for Component {
    fn init_plugin() {}

    fn on_load(ctx: bindings::Context) {
        ctx.register_event(
            7,
            EventType::ServerTickStartEvent,
            EventPriority::Normal,
            true,
        );
        let delayed = scheduler::schedule_delayed_task(11, 2);
        let repeating = scheduler::schedule_repeating_task(12, 1, 100_000);
        let cancelled = scheduler::schedule_repeating_task(13, 1, 2);
        scheduler::cancel_task(cancelled);
        logging::log(
            Level::Info,
            &format!("scheduled delayed={delayed} repeating={repeating} cancelled={cancelled}"),
        );
        let command = bindings::pumpkin::plugin::command::Command::new(
            &["compiled".to_string(), "compiledalias".to_string()],
            "Compiled fixture command",
        );
        let child = CommandNode::literal("sub");
        child.execute_with_handler_id(21);
        child.suggest_with_handler_id(22);
        child.require_with_handler_id(23);
        let leaf = CommandNode::literal("leaf");
        leaf.execute_with_handler_id(24);
        child.then(leaf);
        let count = CommandNode::argument(
            "count",
            &ArgumentType::Integer((Some(1), Some(10))),
        );
        count.execute_with_handler_id(25);
        child.then(count);
        let label = CommandNode::argument(
            "label",
            &ArgumentType::String(StringType::Greedy),
        );
        label.execute_with_handler_id(26);
        child.then(label);
        command.then(child);
        command.execute_with_handler_id(7);
        ctx.register_command(command, "compiled:command");
        logging::log(
            Level::Info,
            "command tree nodes=4 execute=21 suggest=22 require=23 leaf=24 count=25 label=26",
        );
        let permission = Permission {
            node: "compiled:use".to_string(),
            description: "Allows the compiled fixture command".to_string(),
            default: PermissionDefault::Allow,
            children: vec![PermissionChild {
                node: "compiled:child".to_string(),
                value: true,
            }],
        };
        let registered = ctx.register_permission(&permission);
        let invalid = Permission {
            node: "compiled".to_string(),
            description: "Not a namespaced node".to_string(),
            default: PermissionDefault::Deny,
            children: vec![],
        };
        let invalid_registered = ctx.register_permission(&invalid);
        let marketplace = ctx.get_marketplace_metadata();
        let data_folder = ctx.get_data_folder();
        logging::log(
            Level::Info,
            &format!(
                "context registered={registered:?} invalid={invalid_registered:?} marketplace={marketplace:?} folder={data_folder}"
            ),
        );
        let generated = uuid::generate();
        let generated_text = uuid::to_string(generated);
        let roundtrip = uuid::parse(&generated_text)
            .map(|parsed| parsed.high == generated.high && parsed.low == generated.low)
            .unwrap_or(false);
        let invalid = uuid::parse("not-a-uuid").is_none();
        let fixed = uuid::parse("123e4567-e89b-12d3-a456-426614174000").unwrap();
        let fixed_text = uuid::to_string(fixed);
        logging::log(
            Level::Info,
            &format!("uuid roundtrip={roundtrip} invalid={invalid} fixed={fixed_text}"),
        );
        i18n::load_translations(
            "compiled",
            "{\"greeting\":\"hello from i18n\",\"farewell\":\"goodbye\"}",
            Locale::EnUs,
        );
        let translated = i18n::translate("compiled:greeting", Locale::EnUs);
        let missing = i18n::translate("compiled:missing", Locale::EnUs);
        logging::log(
            Level::Info,
            &format!("i18n translated={translated} missing={missing}"),
        );
        let stack = item_stack::ItemStack::new("minecraft:diamond", 3);
        let registry_key = item_stack::ItemStack::get_registry_key(&stack);
        let count = item_stack::ItemStack::get_count(&stack);
        let max_count = item_stack::ItemStack::get_max_count(&stack);
        item_stack::ItemStack::set_count(&stack, 5);
        let cloned = item_stack::ItemStack::clone(&stack);
        let cloned_count = item_stack::ItemStack::get_count(&cloned);
        let with_count = item_stack::ItemStack::with_count(&stack, 7);
        let with_count_count = item_stack::ItemStack::get_count(&with_count);
        let similar = item_stack::ItemStack::is_similar(&stack, cloned);
        logging::log(
            Level::Info,
            &format!(
                "item-stack key={registry_key} count={count} max={max_count} set={} clone={cloned_count} with-count={with_count_count} similar={similar}",
                item_stack::ItemStack::get_count(&stack)
            ),
        );
        let sword = item_stack::ItemStack::new("minecraft:diamond_sword", 1);
        let damage = item_stack::ItemStack::get_damage(&sword);
        let max_damage = item_stack::ItemStack::get_max_damage(&sword);
        let damageable = item_stack::ItemStack::is_damageable(&sword);
        item_stack::ItemStack::set_damage(&sword, 50);
        let set_damage = item_stack::ItemStack::get_damage(&sword);
        let unbreakable_before = item_stack::ItemStack::is_unbreakable(&sword);
        item_stack::ItemStack::set_unbreakable(&sword, true);
        let unbreakable_after = item_stack::ItemStack::is_unbreakable(&sword);
        let repair_before = item_stack::ItemStack::get_repair_cost(&sword);
        item_stack::ItemStack::set_repair_cost(&sword, 4);
        let repair_after = item_stack::ItemStack::get_repair_cost(&sword);
        item_stack::ItemStack::add_enchantment(&sword, Enchantment::Sharpness, 3);
        item_stack::ItemStack::add_enchantment(&sword, Enchantment::Unbreaking, 2);
        let enchantments = item_stack::ItemStack::get_enchantments(&sword)
            .iter()
            .map(|value| format!("{}:{}", value.enchantment as u8, value.level))
            .collect::<Vec<_>>()
            .join(",");
        item_stack::ItemStack::remove_enchantment(&sword, Enchantment::Unbreaking);
        let enchantment_count = item_stack::ItemStack::get_enchantments(&sword).len();
        item_stack::ItemStack::add_custom_enchantment(&sword, "custom:echo", 4);
        let custom_present =
            item_stack::ItemStack::has_custom_enchantment(&sword, "custom:echo");
        let custom_level =
            item_stack::ItemStack::get_custom_enchantment_level(&sword, "custom:echo")
                .unwrap_or(0);
        let custom = item_stack::ItemStack::get_custom_enchantments(&sword)
            .iter()
            .map(|value| format!("{}:{}", value.enchantment_id, value.level))
            .collect::<Vec<_>>()
            .join(",");
        item_stack::ItemStack::remove_custom_enchantment(&sword, "custom:echo");
        let custom_removed =
            item_stack::ItemStack::has_custom_enchantment(&sword, "custom:echo");
        let first_lore = TextComponent::text("compiled lore one");
        let second_lore = TextComponent::text("compiled lore two");
        item_stack::ItemStack::set_lore(&sword, vec![first_lore]);
        item_stack::ItemStack::add_lore(&sword, second_lore);
        let lore_count = item_stack::ItemStack::get_lore(&sword).len();
        let custom_name = TextComponent::text("Compiled Sword");
        item_stack::ItemStack::set_custom_name(&sword, Some(custom_name));
        let name_set = item_stack::ItemStack::get_custom_name(&sword).is_some();
        item_stack::ItemStack::set_custom_name(&sword, None);
        let name_cleared = item_stack::ItemStack::get_custom_name(&sword).is_none();
        let name_after_clear = !name_cleared;
        logging::log(
            Level::Info,
            &format!(
                "item-stack durability damage={damage} max={max_damage} damageable={damageable} set-damage={set_damage} unbreakable={unbreakable_before}->{unbreakable_after} repair={repair_before}->{repair_after} enchantments={enchantments} count={enchantment_count} custom={custom} level={custom_level} present={custom_present} removed={custom_removed} lore={lore_count} custom-name={name_set}->{name_after_clear}"
            ),
        );

        let item_model_before = item_stack::ItemStack::get_item_model(&sword).is_none();
        item_stack::ItemStack::set_item_model(&sword, Some("minecraft:netherite_sword"));
        let item_model_after = item_stack::ItemStack::get_item_model(&sword)
            .map(|model| model == "minecraft:netherite_sword")
            .unwrap_or(false);
        item_stack::ItemStack::set_item_model(&sword, None);
        let item_model_cleared = item_stack::ItemStack::get_item_model(&sword).is_none();

        let glint_before =
            item_stack::ItemStack::get_enchantment_glint_override(&sword).unwrap_or(false);
        item_stack::ItemStack::set_enchantment_glint_override(&sword, Some(true));
        let glint_after = item_stack::ItemStack::get_enchantment_glint_override(&sword)
            == Some(true);
        item_stack::ItemStack::set_enchantment_glint_override(&sword, None);
        let glint_cleared =
            item_stack::ItemStack::get_enchantment_glint_override(&sword).is_none();

        let modifier = item_stack::ItemAttributeModifier {
            attribute: Attribute::AttackDamage,
            modifier: AttributeModifier {
                id: "compiled:bonus".to_string(),
                amount: 2.5,
                operation: ModifierOperation::Add,
            },
            slot: AttributeModifierSlot::MainHand,
        };
        item_stack::ItemStack::add_attribute_modifier(&sword, &modifier);
        let modifiers = item_stack::ItemStack::get_attribute_modifiers(&sword);
        let modifier_count = modifiers.len();
        let modifier_roundtrip = modifiers.iter().any(|value| {
            value.attribute == Attribute::AttackDamage
                && value.modifier.id == "compiled:bonus"
                && value.modifier.amount == 2.5
                && value.modifier.operation == ModifierOperation::Add
                && value.slot == AttributeModifierSlot::MainHand
        });
        item_stack::ItemStack::remove_attribute_modifiers(&sword, Attribute::AttackDamage);
        let modifiers_removed = item_stack::ItemStack::get_attribute_modifiers(&sword).is_empty();
        item_stack::ItemStack::add_attribute_modifier(&sword, &modifier);
        item_stack::ItemStack::clear_attribute_modifiers(&sword);
        let modifiers_cleared = item_stack::ItemStack::get_attribute_modifiers(&sword).is_empty();

        let tree = NbtTree {
            root: 0,
            tags: vec![
                NbtTag::Compound(vec![
                    NbtEntry {
                        key: "level".to_string(),
                        value: 1,
                    },
                    NbtEntry {
                        key: "name".to_string(),
                        value: 2,
                    },
                    NbtEntry {
                        key: "bytes".to_string(),
                        value: 3,
                    },
                    NbtEntry {
                        key: "items".to_string(),
                        value: 4,
                    },
                ]),
                NbtTag::Int(7),
                NbtTag::StringTag("compiled".to_string()),
                NbtTag::ByteArray(vec![1, -2]),
                NbtTag::ListTag(vec![5, 6]),
                NbtTag::Int(11),
                NbtTag::Int(12),
            ],
        };
        item_stack::ItemStack::set_custom_data(&sword, "compiled", "meta", &tree);
        let data_present = item_stack::ItemStack::has_custom_data(&sword, "compiled", "meta");
        let data_roundtrip = match item_stack::ItemStack::get_custom_data(&sword, "compiled", "meta") {
            Some(value) => match &value.tags[value.root as usize] {
                NbtTag::Compound(fields) => {
                    fields.len() == 4
                        && fields[0].key == "level"
                        && matches!(value.tags[fields[0].value as usize], NbtTag::Int(7))
                        && fields[1].key == "name"
                        && matches!(&value.tags[fields[1].value as usize], NbtTag::StringTag(text) if text == "compiled")
                        && fields[2].key == "bytes"
                        && matches!(&value.tags[fields[2].value as usize], NbtTag::ByteArray(bytes) if bytes == &vec![1, -2])
                        && fields[3].key == "items"
                        && matches!(&value.tags[fields[3].value as usize], NbtTag::ListTag(items) if items.len() == 2)
                }
                _ => false,
            },
            None => false,
        };
        item_stack::ItemStack::remove_custom_data(&sword, "compiled", "meta");
        let data_removed = !item_stack::ItemStack::has_custom_data(&sword, "compiled", "meta")
            && item_stack::ItemStack::get_custom_data(&sword, "compiled", "meta").is_none();

        item_stack::ItemStack::set_component(
            &sword,
            DataComponent::CustomModelData,
            &[1, 2, 3, 4],
        );
        let components = item_stack::ItemStack::get_components(&sword);
        let component_roundtrip = components.iter().any(|value| {
            value.component == DataComponent::CustomModelData && value.value == vec![1, 2, 3, 4]
        });
        item_stack::ItemStack::remove_component(&sword, DataComponent::CustomModelData);
        let component_removed = item_stack::ItemStack::get_components(&sword)
            .iter()
            .all(|value| value.component != DataComponent::CustomModelData);
        logging::log(
            Level::Info,
            &format!(
                "item-stack extended model={item_model_before}->{item_model_after}->{item_model_cleared} glint={glint_before}->{glint_after}->{glint_cleared} modifiers={modifier_count}/{modifier_roundtrip}->{modifiers_removed}->{modifiers_cleared} nbt={data_present}/{data_roundtrip}->{data_removed} component={component_roundtrip}->{component_removed}"
            ),
        );
        exercise_async_members(&ctx);
        report(ctx, "compiled on-load");
    }

    fn on_unload(ctx: bindings::Context) {
        report(ctx, "compiled on-unload");
    }

    fn handle_event(
        _event_id: u32,
        _server: bindings::Server,
        event: Event,
    ) -> Event {
        if let Event::ServerTickStartEvent(data) = &event {
            if data.tick % 20 == 0 {
                logging::log(Level::Info, &format!("tick {}", data.tick));
            }
        }
        event
    }

    fn handle_task(handler_id: u32, _server: bindings::Server) {
        logging::log(Level::Info, &format!("task {handler_id}"));
    }
}

bindings::export!(Component with_types_in bindings);
