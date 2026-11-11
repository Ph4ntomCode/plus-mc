// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the Phase 4 milestone H projectiles (world/projectile.{h,cpp}):
//   * `bow_power_for_time` / `bow_shot`: the quadratic, the 0.1 refusal the
//     release takes, and the exact `pow == 1.0f` that makes a full draw critical;
//   * `shoot_arrow` / `ThrowableProjectile`'s launch: the spawn point (the
//     firer's eyes minus 0.1), the power (the draw times BowItem's 3.0, and the
//     throwables' fixed 1.5), the pitch offset each carries (none), the
//     uncertainty triangle, and the firer's own movement;
//   * `tick_projectiles` for the arrow: the movement happening *before* the drag
//     and the gravity (the opposite order from a throwable), the bury that
//     zeroes the velocity and steps back 0.05, the 1200-tick despawn, and
//     `shouldFall`'s un-stick when the block under it goes away;
//   * `AbstractArrow.onHitEntity`: `ceil(|v| * baseDamage)`, Power's add through
//     the `direct_attacker` requirement, the critical's `nextInt(damage / 2 + 2)`
//     roll, the pierce budget (`pierce + 1` victims and the next one discarded
//     undamaged), Punch's flat `push(x, 0.1, z)`, and the deflect that scales the
//     velocity by 0.2 instead of landing;
//   * the three throwables: a snowball's 3 against a blaze and nothing else, an
//     egg's two chicken rolls, and an ender pearl's teleport to its own
//     `oldPosition()`.
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "world/enchantment.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/projectile.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
  do {                                                                                             \
    const double va = static_cast<double>(a), vb = static_cast<double>(b);                         \
    if (!(std::fabs(va - vb) <= (tol))) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s == %.9f, want %.9f (%.1e)\n", __FILE__, __LINE__, #a,   \
                   va, vb, static_cast<double>(tol));                                              \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t item_id(const char* name) {
  // The table's ids are registry indices, so a bow is just its id.
  const std::optional<int32_t> id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

ItemStack weapon_with(const char* enchantment, int level) {
  ItemStack stack;
  stack.item = item_id("minecraft:bow");
  stack.count = 1;
  const GeneratedEnchantment* def = enchantment_by_name(enchantment);
  if (def != nullptr)
    stack.set_enchantment(enchantment_id_of(*def), level);
  return stack;
}

ProjectileTarget target(int32_t id, double x, double y, double z,
                        std::string_view entity_type = "minecraft:zombie") {
  ProjectileTarget t;
  t.entity_id = id;
  t.entity_type = entity_type;
  t.x = x;
  t.y = y;
  t.z = z;
  t.half_width = 0.3;
  t.height = 1.8;
  t.alive = true;
  return t;
}

const double kNoDraws[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

// One arrow already in flight, as `shoot_arrow` would leave it.
Projectile flying_arrow(double x, double y, double z, double vx, double vy, double vz) {
  Projectile p;
  p.entity_id = 1;
  p.kind = ProjectileKind::kArrow;
  p.owner_entity_id = 7;
  p.x = x;
  p.y = y;
  p.z = z;
  p.vx = vx;
  p.vy = vy;
  p.vz = vz;
  p.base_damage = kArrowBaseDamage;
  p.age = 5; // past the margin's ramp, so the margin is 0.15
  return p;
}

} // namespace

int main() {
  // ---- the bow's charge curve ------------------------------------------------
  {
    CHECK_NEAR(bow_power_for_time(0), 0.0, 1e-9);
    // `pow = (u * u + u * 2) / 3` for `u = ticks / 20`.
    CHECK_NEAR(bow_power_for_time(5), (0.0625 + 0.5) / 3.0, 1e-6);
    CHECK_NEAR(bow_power_for_time(10), (0.25 + 1.0) / 3.0, 1e-6);
    CHECK_NEAR(bow_power_for_time(19), 0.9341666666, 1e-6);
    // Twenty ticks is exactly the clamp's edge: the quadratic reaches 1.0 and is
    // *not* clamped, which is what makes `pow == 1.0f` an exact test.
    CHECK_NEAR(bow_power_for_time(20), 1.0, 1e-9);
    CHECK_NEAR(bow_power_for_time(21), 1.0, 1e-9);
    CHECK_NEAR(bow_power_for_time(200), 1.0, 1e-9);

    // `if ((double)pow < 0.1) return false;` — two ticks is 0.07 and refuses,
    // three is 0.1075 and fires.
    CHECK(!bow_shot(0).fires);
    CHECK(!bow_shot(2).fires);
    CHECK(bow_shot(3).fires);
    CHECK_NEAR(bow_shot(3).power, 0.1075, 1e-6);
    CHECK(!bow_shot(3).critical);
    CHECK(!bow_shot(19).critical);
    CHECK(bow_shot(20).fires);
    CHECK(bow_shot(20).critical);
    CHECK(bow_shot(60).critical);
    // A refused release is not a critical either.
    CHECK(!bow_shot(1).critical);
  }

  // ---- the launch ------------------------------------------------------------
  {
    Projectile arrow;
    shoot_arrow(arrow, ProjectileKind::kArrow, weapon_with("minecraft:power", 1), /*owner=*/7, 10.0,
                64.62, -3.0, /*yaw=*/0.0f, /*pitch=*/0.0f, 0.0, 0.0, 0.0,
                /*on_ground=*/true, bow_shot(20), kNoDraws);
    // A straight full draw: the aim is +z and the power is the draw times 3.
    CHECK_NEAR(arrow.x, 10.0, 1e-9);
    CHECK_NEAR(arrow.y, 64.62 - 0.1, 1e-9);
    CHECK_NEAR(arrow.z, -3.0, 1e-9);
    CHECK_NEAR(arrow.vx, 0.0, 1e-9);
    CHECK_NEAR(arrow.vy, 0.0, 1e-9);
    CHECK_NEAR(arrow.vz, 3.0, 1e-9);
    CHECK(arrow.critical);
    CHECK(arrow.owner_entity_id == 7);
    CHECK(!arrow.in_ground);
    CHECK_NEAR(arrow.base_damage, 2.0, 1e-9);

    // A two-tick draw refuses: no velocity at all.
    Projectile weak;
    shoot_arrow(weak, ProjectileKind::kArrow, ItemStack(), 7, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0.0, 0.0,
                0.0, true, bow_shot(2), kNoDraws);
    CHECK_NEAR(weak.vx, 0.0, 1e-12);
    CHECK_NEAR(weak.vz, 0.0, 1e-12);
    CHECK(!weak.critical);

    // A 45 degree shot: the aim is the unit vector, so |v| is exactly the power.
    Projectile angled;
    shoot_arrow(angled, ProjectileKind::kArrow, ItemStack(), 7, 0.0, 0.0, 0.0, 0.0f, 45.0f, 0.0,
                0.0, 0.0, true, bow_shot(20), kNoDraws);
    CHECK_NEAR(angled.vx, 0.0, 1e-6);
    CHECK_NEAR(angled.vy, -3.0 * std::sqrt(0.5), 1e-6);
    CHECK_NEAR(angled.vz, 3.0 * std::sqrt(0.5), 1e-6);

    // The firer's own movement is added, and its vertical part only when they are
    // airborne.
    Projectile running;
    shoot_arrow(running, ProjectileKind::kArrow, ItemStack(), 7, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0.2,
                -0.4, 0.1, /*on_ground=*/true, bow_shot(20), kNoDraws);
    CHECK_NEAR(running.vx, 0.2, 1e-6);
    CHECK_NEAR(running.vy, 0.0, 1e-6);
    CHECK_NEAR(running.vz, 3.1, 1e-6);
    Projectile falling;
    shoot_arrow(falling, ProjectileKind::kArrow, ItemStack(), 7, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0.2,
                -0.4, 0.1, /*on_ground=*/false, bow_shot(20), kNoDraws);
    CHECK_NEAR(falling.vy, -0.4, 1e-6);

    // The uncertainty triangle: with the two draws reversed the jitter flips
    // sign, and it is scaled by the same power the aim is.
    const double draws[6] = {1.0, 0.0, 1.0, 0.0, 1.0, 0.0};
    Projectile jittered;
    shoot_arrow(jittered, ProjectileKind::kArrow, ItemStack(), 7, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0.0,
                0.0, 0.0, true, bow_shot(20), draws);
    CHECK_NEAR(jittered.vx, kProjectileSpread * 3.0, 1e-9);
    CHECK_NEAR(jittered.vy, kProjectileSpread * 3.0, 1e-9);
    CHECK_NEAR(jittered.vz, 3.0 + kProjectileSpread * 3.0, 1e-9);

    // The throwables: power 1.5 and *no* pitch offset (the potions' is -20).
    Projectile snowball;
    throw_item_projectile(snowball, ProjectileKind::kSnowball, 7, 0.0, 64.0, 0.0, 0.0f, 0.0f, 0.0,
                          0.0, 0.0, true, kNoDraws);
    CHECK_NEAR(snowball.y, 63.9, 1e-9);
    CHECK_NEAR(snowball.vz, 1.5, 1e-9);
    CHECK_NEAR(snowball.vy, 0.0, 1e-9);
    // A -20 degree throw: the *horizontal* aim uses the unmodified pitch, so it
    // is the same cos(0) as the straight one and only the vertical leans.
    Projectile lobbed;
    throw_item_projectile(lobbed, ProjectileKind::kSnowball, 7, 0.0, 64.0, 0.0, 0.0f, -20.0f, 0.0,
                          0.0, 0.0, true, kNoDraws);
    CHECK_NEAR(lobbed.vz, 1.5 * std::cos(-20.0 * 3.14159265358979323846 / 180.0), 1e-6);
    CHECK_NEAR(lobbed.vy, -1.5 * std::sin(-20.0 * 3.14159265358979323846 / 180.0), 1e-6);
    CHECK(!lobbed.critical);
  }

  // ---- the box, the margin and the rotation ----------------------------------
  {
    Projectile arrow;
    arrow.x = 1.0;
    arrow.y = 2.0;
    arrow.z = 3.0;
    arrow.kind = ProjectileKind::kArrow;
    const ProjectileBox box = projectile_box(arrow);
    CHECK_NEAR(box.min_x, 1.0 - kArrowWidth * 0.5, 1e-9);
    CHECK_NEAR(box.max_x, 1.0 + kArrowWidth * 0.5, 1e-9);
    // The position's y is the box's bottom.
    CHECK_NEAR(box.min_y, 2.0, 1e-9);
    CHECK_NEAR(box.max_y, 2.0 + kArrowHeight, 1e-9);
    arrow.kind = ProjectileKind::kEgg;
    CHECK_NEAR(projectile_box(arrow).min_x, 1.0 - 0.125, 1e-9);
    CHECK_NEAR(projectile_box(arrow).max_y, 2.0 + 0.25, 1e-9);

    CHECK_NEAR(projectile_entity_margin(0), 0.0, 1e-9);
    CHECK_NEAR(projectile_entity_margin(2), 0.0, 1e-9);
    CHECK_NEAR(projectile_entity_margin(3), 0.05, 1e-6);
    CHECK_NEAR(projectile_entity_margin(8), 0.3, 1e-6);
    CHECK_NEAR(projectile_entity_margin(200), 0.3, 1e-6);

    Projectile spun;
    spun.vx = 0.0;
    spun.vy = 1.0;
    spun.vz = 0.0;
    float yaw = 0.0f, pitch = 0.0f;
    projectile_spawn_rotation(spun, &yaw, &pitch);
    CHECK_NEAR(yaw, 0.0, 1e-4);
    CHECK_NEAR(pitch, 90.0, 1e-4);
    spun.vx = 1.0;
    spun.vz = 1.0;
    spun.vy = 0.0;
    projectile_spawn_rotation(spun, &yaw, &pitch);
    CHECK_NEAR(yaw, 45.0, 1e-4);
    CHECK_NEAR(pitch, 0.0, 1e-4);
  }

  // ---- the arrow's tick order ------------------------------------------------
  {
    // The arrow moves with the *undamped* velocity and only then damps it: the
    // drag and the gravity are `AbstractArrow.tick`'s last two steps.
    std::vector<Projectile> projectiles{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    ProjectileEnvironment env;
    const ProjectileTickReport report = tick_projectiles(projectiles, env);
    CHECK_EQ(projectiles.size(), 1u);
    CHECK_NEAR(projectiles[0].z, 3.0, 1e-9);
    CHECK_NEAR(projectiles[0].vz, 3.0 * kArrowAirDrag, 1e-12);
    CHECK_NEAR(projectiles[0].vx, 0.0, 1e-12);
    CHECK_NEAR(projectiles[0].vy, -kArrowGravity, 1e-12);
    CHECK_EQ(projectiles[0].age, 6);
    CHECK_EQ(report.moved.size(), 1u);
    CHECK_EQ(report.moved[0], 1);

    // The throwable is the other order: gravity, then drag, then move.
    std::vector<Projectile> thrown;
    Projectile snowball;
    snowball.entity_id = 2;
    snowball.kind = ProjectileKind::kSnowball;
    snowball.z = 0.0;
    snowball.vz = 1.5;
    thrown.push_back(snowball);
    tick_projectiles(thrown, env);
    CHECK_NEAR(thrown[0].vy, -kThrowableGravity * kThrowableAirDrag, 1e-12);
    CHECK_NEAR(thrown[0].z, 1.5 * kThrowableAirDrag, 1e-12);
  }

  // ---- burying, the despawn and the fall -------------------------------------
  {
    // A cell in the way: the arrow buries itself on the face it entered (the
    // cell it flies into is the one at x >= 1, entered through the -x face —
    // Direction.WEST), its velocity goes to zero, and it steps back 0.05 along
    // the sign it came in with.
    std::vector<Projectile> projectiles{flying_arrow(0.5, 10.0, 0.0, 3.0, 0.0, 0.0)};
    ProjectileEnvironment env;
    env.is_solid = [](int x, int, int) { return x >= 1; };
    const ProjectileTickReport report = tick_projectiles(projectiles, env);
    CHECK_EQ(projectiles.size(), 1u);
    CHECK(projectiles[0].in_ground);
    CHECK_NEAR(projectiles[0].vx, 0.0, 1e-12);
    CHECK_NEAR(projectiles[0].vy, 0.0, 1e-12);
    CHECK_NEAR(projectiles[0].vz, 0.0, 1e-12);
    CHECK_EQ(report.impacts.size(), 1u);
    CHECK(report.impacts[0].hit_block);
    CHECK(report.impacts[0].stuck);
    CHECK(!report.impacts[0].discarded);
    CHECK_EQ(report.impacts[0].face, 4);
    CHECK_EQ(report.impacts[0].block_x, 1);
    // It stopped at the face it entered (x = 1), then stepped back a twentieth.
    CHECK_NEAR(projectiles[0].x, 1.0 - kArrowGroundBackoff, 1e-9);
    // `onHitBlock`'s `setInGround(true)` is a synched datum, so the bury tick
    // must carry one (this is the flag the caller writes arrow data from).
    CHECK(projectiles[0].in_ground_changed);
    // The bury tick is a position sync; the tick after it is not.
    CHECK_EQ(report.moved.size(), 1u);
    const ProjectileTickReport settled = tick_projectiles(projectiles, env);
    CHECK_EQ(settled.moved.size(), 0u);
    CHECK_EQ(projectiles[0].life, 1);
    CHECK_EQ(projectiles[0].in_ground_time, 1);

    // The 1200-tick despawn: `life` only counts while it is buried.
    projectiles[0].life = kArrowDespawnTicks - 1;
    const ProjectileTickReport gone = tick_projectiles(projectiles, env);
    CHECK_EQ(projectiles.size(), 0u);
    CHECK_EQ(gone.removed.size(), 1u);

    // `shouldFall`: the block under a buried arrow disappears and it starts
    // falling again with a fresh clock.
    std::vector<Projectile> stuck{flying_arrow(0.5, 10.0, 0.0, 3.0, 0.0, 0.0)};
    tick_projectiles(stuck, env);
    CHECK(stuck[0].in_ground);
    stuck[0].life = 40;
    ProjectileEnvironment empty;
    const ProjectileTickReport released = tick_projectiles(stuck, empty);
    CHECK(!stuck[0].in_ground);
    CHECK_EQ(stuck[0].life, 0);
    // `startFalling`'s `setInGround(false)` is a synched datum, so the un-stick
    // tick is in the report even though the arrow does not move: the arrow data
    // the caller writes is driven by exactly this list.
    CHECK(stuck[0].in_ground_changed);
    CHECK_EQ(released.moved.size(), 1u);
    CHECK_EQ(released.moved[0], stuck[0].entity_id);
    // Still no velocity of its own — burying zeroed it — and the tick that
    // un-sticks it does not move it.
    CHECK_NEAR(stuck[0].vx, 0.0, 1e-12);
  }

  // ---- the arrow's damage, Power, the critical and the pierce budget ---------
  {
    // `ceil(|v| * baseDamage)`: a 3.0 arrow is 6 damage.
    std::vector<Projectile> projectiles{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    ProjectileEnvironment env;
    env.targets.push_back(target(11, 0.0, 10.0, 1.0));
    // `hurt` is handed the impact to *apply*; whether it landed is the report's
    // record (`accepted`), because the answer does not exist until `hurt`
    // returns. So the callback counts the attempts and the assertions read
    // `report.impacts`.
    int attempts = 0;
    env.hurt = [&](const ProjectileImpact&) {
      ++attempts;
      return true;
    };
    const ProjectileTickReport report = tick_projectiles(projectiles, env);
    CHECK_EQ(attempts, 1);
    CHECK_EQ(report.impacts.size(), 1u);
    CHECK_EQ(static_cast<int>(report.impacts[0].damage), 6);
    CHECK(report.impacts[0].hit_entity);
    CHECK(report.impacts[0].target_entity_id == 11);
    CHECK(report.impacts[0].accepted);
    CHECK(!report.impacts[0].critical);
    // Nothing is left: a non-piercing arrow that lands is discarded.
    CHECK_EQ(projectiles.size(), 0u);
    CHECK_EQ(report.removed.size(), 1u);

    // ---- the tick an arrow spawns on ----------------------------------------
    // `ProjectileUtil.computeMargin` is `max(0, min(0.3, (tickCount - 2) / 20))`,
    // so a freshly spawned arrow is at `age == 0` and its margin is *zero*: the
    // exact box clip is the entire test, with none of the inflate-and-re-clip
    // help the margin branch gives. A shot at a mob that is a couple of blocks
    // along the aim therefore has to land on this very first tick — and the
    // firer's own box holds the spawn point, so the owner is a candidate the
    // `!leftOwner` clause has to reject before the mob can win.
    std::vector<Projectile> fresh{flying_arrow(-24.0, 81.52, -8.0, 3.0161, -0.0217, 0.019)};
    fresh[0].age = 0;
    fresh[0].owner_entity_id = 7;
    ProjectileEnvironment spawn_env;
    spawn_env.targets.push_back(target(11, -22.0, 81.0, -8.0));
    spawn_env.targets[0].height = 1.9; // a zombie's box
    spawn_env.targets.push_back(target(7, -24.0, 80.0, -8.0));
    spawn_env.targets[1].height = 1.8;
    int spawn_attempts = 0;
    spawn_env.hurt = [&](const ProjectileImpact&) {
      ++spawn_attempts;
      return true;
    };
    const ProjectileTickReport spawn_report = tick_projectiles(fresh, spawn_env);
    CHECK_EQ(spawn_attempts, 1);
    CHECK_EQ(spawn_report.impacts.size(), 1u);
    CHECK(spawn_report.impacts[0].hit_entity);
    CHECK(spawn_report.impacts[0].target_entity_id == 11);
    // A non-piercing arrow that lands is discarded, so `fresh` is empty by now
    // and the clip point is read off the impact, not the erased element (the
    // read ASan's container annotations refuse).
    CHECK_EQ(fresh.size(), 0u);
    CHECK_NEAR(spawn_report.impacts[0].x, -22.3, 1e-9); // the clip point, not the full step

    // Power I is `base 1.0` linear: 3 * (2 + 1) = 9, not 3 * 2 * 1.
    std::vector<Projectile> powered{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    powered[0].weapon = weapon_with("minecraft:power", 1);
    CHECK_EQ(static_cast<int>(tick_projectiles(powered, env).impacts[0].damage), 9);

    // Power III is 1.0 + 0.5 * 2 = 2.0: 3 * (2 + 2) = 12.
    std::vector<Projectile> power_three{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    power_three[0].weapon = weapon_with("minecraft:power", 3);
    CHECK_EQ(static_cast<int>(tick_projectiles(power_three, env).impacts[0].damage), 12);

    // That add only happens because the module names the *attacker* an arrow: the
    // requirement is `direct_attacker` within `#minecraft:arrows`, so a weapon
    // whose attacker is not an arrow gets nothing at all. The same helper call
    // is the witness.
    const MeleeEnchantments as_arrow = item_melee_enchantments(
        weapon_with("minecraft:power", 3), "minecraft:zombie", "minecraft:arrow");
    const MeleeEnchantments as_snowball = item_melee_enchantments(
        weapon_with("minecraft:power", 3), "minecraft:zombie", "minecraft:snowball");
    CHECK_NEAR(as_arrow.damage, 2.0, 1e-6);
    CHECK_NEAR(as_snowball.damage, 0.0, 1e-6);

    // An unenchanted weapon contributes nothing at all.
    const MeleeEnchantments bare =
        item_melee_enchantments(ItemStack{}, "minecraft:zombie", "minecraft:arrow");
    CHECK_NEAR(bare.damage, 0.0, 1e-9);
    CHECK_NEAR(bare.knockback, 0.0, 1e-9);

    // The critical's extra roll is `nextInt(damage / 2 + 2)` added to the damage.
    std::vector<Projectile> critical{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    critical[0].critical = true;
    int asked_bound = -1;
    ProjectileEnvironment crit_env = env;
    crit_env.next_int = [&](int bound) {
      asked_bound = bound;
      return 4;
    };
    const ProjectileTickReport crit_report = tick_projectiles(critical, crit_env);
    CHECK_EQ(asked_bound, 6 / 2 + 2);
    CHECK_EQ(static_cast<int>(crit_report.impacts[0].damage), 10);
    CHECK(crit_report.impacts[0].critical);

    // Punch I is 1.0: the push is the horizontal unit times 1.0 * 0.6, flat.
    std::vector<Projectile> punchy{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    punchy[0].weapon = weapon_with("minecraft:punch", 1);
    const ProjectileTickReport punch_report = tick_projectiles(punchy, env);
    CHECK_NEAR(punch_report.impacts[0].push_x, 0.0, 1e-9);
    CHECK_NEAR(punch_report.impacts[0].push_y, 0.1, 1e-9);
    CHECK_NEAR(punch_report.impacts[0].push_z, 0.6, 1e-9);
    // ... and scaled by the victim's knockback resistance.
    std::vector<Projectile> resisted{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    resisted[0].weapon = weapon_with("minecraft:punch", 2);
    ProjectileEnvironment resistant_env = env;
    resistant_env.targets[0].knockback_resistance = 0.5;
    const ProjectileTickReport resist_report = tick_projectiles(resisted, resistant_env);
    CHECK_NEAR(resist_report.impacts[0].push_z, 2.0 * 0.6 * 0.5, 1e-9);
    CHECK(resist_report.impacts[0].push_z > 0.0);

    // A refused hit (a blocking player) deflects instead: 0.2 of the velocity,
    // and the arrow survives — with the rest of the tick's drag on top of it.
    std::vector<Projectile> deflected{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    ProjectileEnvironment blocking = env;
    blocking.hurt = [](const ProjectileImpact&) { return false; };
    const ProjectileTickReport bent = tick_projectiles(deflected, blocking);
    CHECK_EQ(deflected.size(), 1u);
    CHECK_NEAR(deflected[0].vz, 3.0 * 0.2 * kArrowAirDrag, 1e-9);
    CHECK_EQ(bent.removed.size(), 0u);
    CHECK(!bent.impacts[0].accepted);
    CHECK(!bent.impacts[0].discarded);

    // An arrow that has all but stopped is discarded on the deflection. It has
    // to start inside the victim's box for the hit to resolve at this speed.
    std::vector<Projectile> dead_stop{flying_arrow(0.0, 10.0, 1.0, 0.0, 0.0, 1.0e-4)};
    tick_projectiles(dead_stop, blocking);
    CHECK_EQ(dead_stop.size(), 0u);
  }

  // ---- piercing --------------------------------------------------------------
  {
    // `pierce_level + 1` victims: with 1, two entities are pierced and the third
    // is discarded *before* it is damaged.
    std::vector<Projectile> projectiles{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    projectiles[0].pierce_level = 1;
    ProjectileEnvironment env;
    env.targets.push_back(target(11, 0.0, 10.0, 1.0));
    env.targets.push_back(target(12, 0.0, 10.0, 2.0));
    env.targets.push_back(target(13, 0.0, 10.0, 3.0));
    int attempts = 0;
    env.hurt = [&](const ProjectileImpact&) {
      ++attempts;
      return true;
    };
    const ProjectileTickReport report = tick_projectiles(projectiles, env);
    // Three victims are *recorded*, but only two are ever hurt: the third is
    // discarded before the damage lands.
    CHECK_EQ(report.impacts.size(), 3u);
    CHECK_EQ(attempts, 2);
    CHECK_EQ(report.impacts[0].target_entity_id, 11);
    CHECK_EQ(report.impacts[1].target_entity_id, 12);
    CHECK_EQ(report.impacts[2].target_entity_id, 13);
    CHECK(report.impacts[0].accepted);
    CHECK(report.impacts[1].accepted);
    CHECK_EQ(static_cast<int>(report.impacts[0].damage), 6);
    CHECK_EQ(static_cast<int>(report.impacts[1].damage), 6);
    CHECK(!report.impacts[2].accepted);
    CHECK_EQ(static_cast<int>(report.impacts[2].damage), 0);
    CHECK(report.impacts[2].discarded);
    CHECK_EQ(projectiles.size(), 0u);

    // With 2 the budget is `pierce + 1` = 3, so all three are pierced and the
    // arrow lives on.
    std::vector<Projectile> wider{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
    wider[0].pierce_level = 2;
    attempts = 0;
    const ProjectileTickReport wide_report = tick_projectiles(wider, env);
    CHECK_EQ(wide_report.impacts.size(), 3u);
    CHECK_EQ(attempts, 3);
    CHECK(wide_report.impacts[2].accepted);
    CHECK_EQ(static_cast<int>(wide_report.impacts[2].damage), 6);
    CHECK_EQ(wider.size(), 1u);
    CHECK_EQ(wider[0].pierced_entities.size(), 3u);
    // The arrow it already pierced is invisible to the next tick's search.
    attempts = 0;
    tick_projectiles(wider, env);
    CHECK_EQ(attempts, 0);
  }

  // ---- the throwables --------------------------------------------------------
  {
    // A snowball is 3 against a blaze and nothing against anything else — and it
    // breaks either way.
    const auto throw_at = [](ProjectileKind kind, const ProjectileTarget& victim,
                             ProjectileEnvironment env) {
      std::vector<Projectile> projectiles;
      Projectile p;
      p.entity_id = 3;
      p.kind = kind;
      p.owner_entity_id = 7;
      // Half a block up: the flight's first tick already applies the gravity, so
      // spawned level with the victim's feet the (all downward) segment would
      // never enter their box.
      p.y = victim.y + 0.5;
      p.z = 0.0;
      p.vz = 1.5;
      projectiles.push_back(p);
      env.targets.push_back(victim);
      int attempts = 0;
      env.hurt = [&](const ProjectileImpact&) {
        ++attempts;
        return true;
      };
      const ProjectileTickReport report = tick_projectiles(projectiles, env);
      (void)attempts;
      return std::make_pair(projectiles.size(), report.impacts);
    };
    ProjectileTarget blaze = target(21, 0.0, 0.0, 1.0, "minecraft:blaze");
    blaze.is_blaze = true;
    const auto on_blaze = throw_at(ProjectileKind::kSnowball, blaze, ProjectileEnvironment{});
    CHECK_EQ(on_blaze.first, 0u);
    CHECK_EQ(on_blaze.second.size(), 1u);
    CHECK_NEAR(on_blaze.second[0].damage, 3.0, 1e-9);
    const auto on_zombie =
        throw_at(ProjectileKind::kSnowball, target(22, 0.0, 0.0, 1.0), ProjectileEnvironment{});
    CHECK_EQ(on_zombie.second.size(), 1u);
    CHECK_NEAR(on_zombie.second[0].damage, 0.0, 1e-9);
    CHECK_EQ(on_zombie.first, 0u);

    // An egg rolls `nextInt(8) == 0` for one chicken and `nextInt(32) == 0` for
    // four, in that order.
    ProjectileEnvironment one_roll;
    int calls = 0;
    one_roll.next_int = [&](int bound) {
      ++calls;
      return bound == kEggChickenRoll ? 0 : 5;
    };
    const auto egg = throw_at(ProjectileKind::kEgg, target(23, 0.0, 0.0, 1.0), one_roll);
    CHECK_EQ(egg.second.size(), 1u);
    CHECK_EQ(egg.second[0].chickens, 1);
    CHECK_EQ(calls, 2);

    ProjectileEnvironment quad_roll;
    quad_roll.next_int = [](int) { return 0; };
    const auto quad = throw_at(ProjectileKind::kEgg, target(24, 0.0, 0.0, 1.0), quad_roll);
    CHECK_EQ(quad.second[0].chickens, 4);

    ProjectileEnvironment no_roll;
    no_roll.next_int = [](int) { return 7; };
    const auto none = throw_at(ProjectileKind::kEgg, target(25, 0.0, 0.0, 1.0), no_roll);
    CHECK_EQ(none.second[0].chickens, 0);

    // An ender pearl teleports its thrower to the position the pearl held when
    // the tick started, not to the impact point.
    std::vector<Projectile> pearls;
    Projectile pearl;
    pearl.entity_id = 4;
    pearl.kind = ProjectileKind::kEnderPearl;
    pearl.owner_entity_id = 7;
    pearl.y = 0.5;
    pearl.z = 0.0;
    pearl.vz = 1.5;
    pearls.push_back(pearl);
    ProjectileEnvironment pearl_env;
    pearl_env.targets.push_back(target(26, 0.0, 0.0, 1.0));
    const ProjectileTickReport pearl_report = tick_projectiles(pearls, pearl_env);
    CHECK_EQ(pearls.size(), 0u);
    CHECK_EQ(pearl_report.impacts.size(), 1u);
    CHECK(pearl_report.impacts[0].teleports_owner);
    // The impact is where the segment entered the victim's box (its near face at
    // z = 0.7); the teleport is where the pearl started the tick.
    CHECK_NEAR(pearl_report.impacts[0].z, 0.7, 1e-9);
    CHECK_NEAR(pearl_report.impacts[0].teleport_z, 0.0, 1e-9);
    CHECK_NEAR(pearl_report.impacts[0].teleport_y, 0.5, 1e-9);
    // An ownerless pearl teleports nobody.
    std::vector<Projectile> orphan;
    pearl.owner_entity_id = -1;
    orphan.push_back(pearl);
    const ProjectileTickReport orphan_report = tick_projectiles(orphan, pearl_env);
    CHECK(!orphan_report.impacts[0].teleports_owner);
  }

  // ---- the spectral arrow's Glowing ------------------------------------------
  {
    // `SpectralArrow.doPostHurtEffects` is `mob.addEffect(new MobEffectInstance(
    // MobEffects.GLOWING, this.duration, 0))` with `DEFAULT_DURATION = 200`,
    // called from inside `AbstractArrow.onHitEntity`'s `if (hurt)` branch. The
    // impact states the duration for the sink that owns the mob's effect state
    // (there is no effect packet for a non-player entity) and states it for
    // *every* arrow, 0 for the kinds that carry none.
    const auto hit = [](ProjectileKind kind, bool accept) {
      std::vector<Projectile> projectiles{flying_arrow(0.0, 10.0, 0.0, 0.0, 0.0, 3.0)};
      projectiles[0].kind = kind;
      ProjectileEnvironment env;
      env.targets.push_back(target(11, 0.0, 10.0, 1.0));
      env.hurt = [accept](const ProjectileImpact&) { return accept; };
      return tick_projectiles(projectiles, env);
    };
    CHECK_EQ(kSpectralArrowGlowTicks, 200); // `DEFAULT_DURATION`
    const ProjectileTickReport spectral = hit(ProjectileKind::kSpectralArrow, true);
    CHECK_EQ(spectral.impacts.size(), 1u);
    CHECK(spectral.impacts[0].accepted);
    CHECK_EQ(spectral.impacts[0].glowing_ticks, kSpectralArrowGlowTicks);
    CHECK_EQ(spectral.impacts[0].glowing_ticks, 200);

    // A plain arrow leaves nothing behind on a hit that lands.
    const ProjectileTickReport plain = hit(ProjectileKind::kArrow, true);
    CHECK_EQ(plain.impacts[0].glowing_ticks, 0);

    // The duration is stated even when the hit is refused — the same way the
    // damage is, because which branch the arrow took is the sink's answer, not
    // this module's. What matters is that a refused hit is not an applied one:
    // the sink applies the effect only after its own `hurt` has landed.
    const ProjectileTickReport refused = hit(ProjectileKind::kSpectralArrow, false);
    CHECK(!refused.impacts[0].accepted);
    CHECK_EQ(refused.impacts[0].glowing_ticks, kSpectralArrowGlowTicks);
  }

  // ---- the kind tables -------------------------------------------------------
  {
    CHECK_EQ(projectile_is_arrow(ProjectileKind::kArrow), true);
    CHECK_EQ(projectile_is_arrow(ProjectileKind::kSpectralArrow), true);
    CHECK_EQ(projectile_is_arrow(ProjectileKind::kSnowball), false);
    CHECK_EQ(projectile_is_arrow(ProjectileKind::kEgg), false);
    CHECK_EQ(projectile_is_arrow(ProjectileKind::kEnderPearl), false);
    CHECK(std::string(projectile_entity_type_name(ProjectileKind::kArrow)) == "minecraft:arrow");
    CHECK(std::string(projectile_entity_type_name(ProjectileKind::kSpectralArrow)) ==
          "minecraft:spectral_arrow");
    CHECK(std::string(projectile_entity_type_name(ProjectileKind::kSnowball)) ==
          "minecraft:snowball");
    CHECK(std::string(projectile_entity_type_name(ProjectileKind::kEgg)) == "minecraft:egg");
    CHECK(std::string(projectile_entity_type_name(ProjectileKind::kEnderPearl)) ==
          "minecraft:ender_pearl");
    CHECK_EQ(thrown_projectile_kind("minecraft:snowball").has_value(), true);
    CHECK(thrown_projectile_kind("minecraft:snowball") == ProjectileKind::kSnowball);
    CHECK(thrown_projectile_kind("minecraft:egg") == ProjectileKind::kEgg);
    CHECK(thrown_projectile_kind("minecraft:ender_pearl") == ProjectileKind::kEnderPearl);
    // A bow fires an arrow, it is not *thrown*: this is the caller's line between
    // `use` and `releaseUsing`.
    CHECK(!thrown_projectile_kind("minecraft:arrow").has_value());
    CHECK(!thrown_projectile_kind("minecraft:bow").has_value());
    // `SpectralArrowItem.createArrow` is the one arrow item whose *entity type*
    // differs; `ArrowItem`'s plain arrow covers the tag's other two members (a
    // tipped arrow is an arrow entity carrying potion contents).
    CHECK(arrow_kind_for_item("minecraft:spectral_arrow") == ProjectileKind::kSpectralArrow);
    CHECK(arrow_kind_for_item("minecraft:arrow") == ProjectileKind::kArrow);
    CHECK(arrow_kind_for_item("minecraft:tipped_arrow") == ProjectileKind::kArrow);
    CHECK(std::string(projectile_item_name(ProjectileKind::kEgg)) == "minecraft:egg");
    // Only the spectral arrow carries an effect; the timer is the vanilla 200.
    CHECK_EQ(kSpectralArrowGlowTicks, 200);
  }

  if (failures == 0) {
    std::printf("PASS: projectile unit tests\n");
    return 0;
  }
  std::fprintf(stderr, "%d projectile check(s) failed\n", failures);
  return 1;
}
