// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for thrown potions and their lingering clouds (Phase 3 milestone G
// follow-up):
//   * `throwable_potion_kind`: only the two `ThrowablePotionItem` classes throw;
//   * `throw_potion` / `Projectile.shoot`: the spawn point (the thrower's eyes
//     minus 0.1), the -20 degree pitch offset that makes a throw arc, the 0.5
//     power, the uncertainty triangle's spread, and the thrower's own momentum
//     (with its vertical part gated on being airborne);
//   * `tick_potions` / `ThrowableProjectile.tick`: gravity 0.05 and air drag
//     0.99 applied to the *velocity* before the step, the block clip that
//     truncates the segment, `Projectile.canHitEntity`'s refusal to hit the
//     thrower until the potion has left their box (checked after the move, as
//     `checkLeftOwner` is), and the three impact branches — a water bottle, a
//     splash potion's 4 x 2 x 4 box with `1 - sqrt(distance) / 4` falloff, and
//     a lingering potion that leaves a cloud instead;
//   * `potion_color` / `potion_has_instant_effects` / `potion_contents_is_water`;
//   * the cloud: `radius 3 -> 0.5` over its 600 ticks, the 10-tick wait, an
//     application every 5 ticks but only once per victim per 20, the horizontal
//     radius gate, the 0.5 instantaneous scale, a quarter-length instance
//     (minecraft:potion_duration_scale), the `radius_on_use` shrink that can
//     discard a cloud, and the age-out.
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "world/effect.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/potion.h"
#include "world/potion_projectile.h"

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

int32_t effect_id(const char* name) {
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  return def ? mob_effect_id_of(*def) : -1;
}

StackPotionContents contents_of(const char* name) {
  StackPotionContents contents;
  const GeneratedPotion* p = potion_by_name(name);
  contents.potion = p ? potion_id_of(*p) : -1;
  return contents;
}

MobEffectInstance custom(const char* effect_name, int duration, int amplifier) {
  MobEffectInstance instance;
  instance.effect = effect_id(effect_name);
  instance.duration = duration;
  instance.amplifier = amplifier;
  instance.ambient = false;
  instance.visible = true;
  instance.show_icon = true;
  return instance;
}

// A target standing at (x, y, z), with y the bottom of its box.
PotionTarget target(int32_t id, double x, double y, double z, double half_width = 0.3,
                    double height = 1.8) {
  PotionTarget t;
  t.entity_id = id;
  t.is_player = false;
  t.x = x;
  t.y = y;
  t.z = z;
  t.half_width = half_width;
  t.height = height;
  t.alive = true;
  return t;
}

PotionProjectile launched(PotionProjectileKind kind, const StackPotionContents& contents,
                          float duration_scale) {
  PotionProjectile projectile;
  const double draws[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  throw_potion(projectile, kind, contents, duration_scale, /*owner_entity_id=*/7,
               /*owner_x=*/10.0, /*owner_y=*/64.62, /*owner_z=*/-3.0, /*owner_yaw=*/0.0f,
               /*owner_pitch=*/0.0f, /*owner_vx=*/0.0, /*owner_vy=*/0.0, /*owner_vz=*/0.0,
               /*owner_on_ground=*/true, draws);
  return projectile;
}

// A single potion placed by hand, so the impact geometry is the test's to
// choose rather than the launcher's.
PotionProjectile at(double x, double y, double z, double vx, double vy, double vz) {
  PotionProjectile projectile;
  projectile.entity_id = 42;
  projectile.kind = PotionProjectileKind::kSplash;
  projectile.contents = contents_of("minecraft:harming");
  projectile.owner_entity_id = -1;
  projectile.left_owner = true;
  projectile.x = x;
  projectile.y = y;
  projectile.z = z;
  projectile.vx = vx;
  projectile.vy = vy;
  projectile.vz = vz;
  return projectile;
}

// A wall filling cell (1, 0, 0) only: everything else is air, so the potion
// breaks on the block it enters and on nothing before it.
bool wall_at_1_0_0(int x, int y, int z) {
  return x == 1 && y == 0 && z == 0;
}

// Ticks a cloud population `n` times, advancing each cloud's age first, which is
// `Entity.tick`'s `++tickCount` before `serverTick`.
CloudTickReport tick_cloud_n(std::vector<AreaEffectCloud>& clouds, const PotionEnvironment& env,
                             int n) {
  CloudTickReport last;
  for (int i = 0; i < n; ++i) {
    for (AreaEffectCloud& cloud : clouds)
      ++cloud.age;
    last = tick_clouds(clouds, env);
  }
  return last;
}

} // namespace

int main() {
  // ---- which items throw -----------------------------------------------------
  {
    const std::optional<PotionProjectileKind> splash =
        throwable_potion_kind("minecraft:splash_potion");
    const std::optional<PotionProjectileKind> lingering =
        throwable_potion_kind("minecraft:lingering_potion");
    CHECK(splash.has_value() && *splash == PotionProjectileKind::kSplash);
    CHECK(lingering.has_value() && *lingering == PotionProjectileKind::kLingering);
    // A plain potion is drunk, and the projectiles whose item class this server
    // does not model do not throw either.
    CHECK(!throwable_potion_kind("minecraft:potion").has_value());
    CHECK(!throwable_potion_kind("minecraft:snowball").has_value());
    CHECK(!throwable_potion_kind("").has_value());
    // Both thrown items are really in the registry under those keys.
    CHECK_EQ(ItemRegistry::instance().id_of("minecraft:splash_potion").value_or(-1), 1442);
    CHECK_EQ(ItemRegistry::instance().id_of("minecraft:lingering_potion").value_or(-1), 1445);
  }

  // ---- the launch ------------------------------------------------------------
  {
    const StackPotionContents contents = contents_of("minecraft:harming");
    PotionProjectile projectile = launched(PotionProjectileKind::kSplash, contents, 1.0f);
    CHECK(projectile.kind == PotionProjectileKind::kSplash);
    CHECK_EQ(projectile.owner_entity_id, 7);
    CHECK_EQ(projectile.age, 0);
    CHECK(!projectile.left_owner);
    CHECK_EQ(projectile.contents.potion, contents.potion);
    // `owner.getEyeY() - 0.1f`, with the position the box's *bottom*.
    CHECK_NEAR(projectile.x, 10.0, 1e-12);
    CHECK_NEAR(projectile.y, 64.52, 1e-12);
    CHECK_NEAR(projectile.z, -3.0, 1e-12);
    // A level throw (yaw 0, pitch 0) goes along +z at the *power* 0.5, and the
    // -20 degree offset tilts it up by sin(20) against the unchanged horizontal
    // (the aim's horizontal component is `cos(pitch)`, not `cos(pitch + 20)`).
    const double radians_20 = 20.0 * 3.14159265358979323846 / 180.0;
    CHECK_NEAR(projectile.vx, 0.0, 1e-12);
    CHECK(projectile.vy > 0.0 && projectile.vz > 0.0);
    CHECK_NEAR(projectile.vy / projectile.vz, std::sin(radians_20), 1e-12);
    CHECK_NEAR(std::sqrt(projectile.vx * projectile.vx + projectile.vy * projectile.vy +
                         projectile.vz * projectile.vz),
               0.5, 1e-12);

    // yaw 90 faces -x (`atan2(-sin(yaw), cos(yaw))`), and pitch 90 is straight
    // down: no horizontal component at all.
    {
      PotionProjectile straight_down = launched(PotionProjectileKind::kSplash, contents, 1.0f);
      const double draws[6] = {0, 0, 0, 0, 0, 0};
      throw_potion(straight_down, PotionProjectileKind::kSplash, contents, 1.0f, 7, 0.0, 0.0, 0.0,
                   90.0f, 90.0f, 0.0, 0.0, 0.0, true, draws);
      CHECK_NEAR(straight_down.vx, 0.0, 1e-12);
      CHECK_NEAR(straight_down.vz, 0.0, 1e-12);
      CHECK(straight_down.vy < 0.0);

      PotionProjectile west = launched(PotionProjectileKind::kSplash, contents, 1.0f);
      throw_potion(west, PotionProjectileKind::kSplash, contents, 1.0f, 7, 0.0, 0.0, 0.0, 90.0f,
                   0.0f, 0.0, 0.0, 0.0, true, draws);
      CHECK(west.vx < 0.0);
      CHECK_NEAR(west.vz, 0.0, 1e-12);
    }

    // The uncertainty triangle: `random.triangle(0, 0.0172275)` is
    // `mean + spread * (a - b)`, so draws {1, 0} push each axis by the full
    // spread — and the whole vector is scaled by the power afterwards, so the
    // jitter is scaled too.
    {
      PotionProjectile jittered = launched(PotionProjectileKind::kSplash, contents, 1.0f);
      const double draws[6] = {1.0, 0.0, 1.0, 0.0, 1.0, 0.0};
      throw_potion(jittered, PotionProjectileKind::kSplash, contents, 1.0f, 7, 10.0, 64.62, -3.0,
                   0.0f, 0.0f, 0.0, 0.0, 0.0, true, draws);
      CHECK_NEAR(jittered.vx - projectile.vx, 0.0172275 * 0.5, 1e-12);
      CHECK_NEAR(jittered.vy - projectile.vy, 0.0172275 * 0.5, 1e-12);
      CHECK_NEAR(jittered.vz - projectile.vz, 0.0172275 * 0.5, 1e-12);
    }

    // `delta.add(sourceMovement)`: a running throw carries the thrower's own
    // momentum, and only a thrower who is *airborne* carries their vertical
    // speed.
    {
      const double draws[6] = {0, 0, 0, 0, 0, 0};
      PotionProjectile grounded = launched(PotionProjectileKind::kSplash, contents, 1.0f);
      throw_potion(grounded, PotionProjectileKind::kSplash, contents, 1.0f, 7, 0.0, 0.0, 0.0, 0.0f,
                   0.0f, 0.1, 0.4, 0.2, true, draws);
      PotionProjectile falling = launched(PotionProjectileKind::kSplash, contents, 1.0f);
      throw_potion(falling, PotionProjectileKind::kSplash, contents, 1.0f, 7, 0.0, 0.0, 0.0, 0.0f,
                   0.0f, 0.1, 0.4, 0.2, false, draws);
      CHECK_NEAR(grounded.vx - projectile.vx, 0.1, 1e-12);
      CHECK_NEAR(grounded.vz - projectile.vz, 0.2, 1e-12);
      CHECK_NEAR(grounded.vy, projectile.vy, 1e-12);
      CHECK_NEAR(falling.vy - projectile.vy, 0.4, 1e-12);
    }
  }

  // ---- the spawn rotation ----------------------------------------------------
  {
    PotionProjectile projectile = at(0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    float yaw = 0.0f, pitch = 0.0f;
    potion_spawn_rotation(projectile, &yaw, &pitch);
    CHECK_NEAR(yaw, 45.0, 1e-4);
    CHECK_NEAR(pitch, 0.0, 1e-6);
    projectile.vx = 0.0;
    projectile.vz = 0.0;
    projectile.vy = 1.0;
    potion_spawn_rotation(projectile, &yaw, &pitch);
    CHECK_NEAR(yaw, 0.0, 1e-6);
    CHECK_NEAR(pitch, 90.0, 1e-4);
  }

  // ---- colour and the two data predicates ------------------------------------
  {
    CHECK(potion_has_instant_effects(contents_of("minecraft:harming")));
    CHECK(potion_has_instant_effects(contents_of("minecraft:healing")));
    CHECK(!potion_has_instant_effects(contents_of("minecraft:swiftness")));
    CHECK(!potion_has_instant_effects(contents_of("minecraft:water")));
    // `Potion.hasInstantEffects` reads the *named* potion only: a custom instant
    // effect on a water bottle does not make the bottle an instant one.
    StackPotionContents custom_water = contents_of("minecraft:water");
    custom_water.custom_effects.push_back(custom("minecraft:instant_damage", 1, 0));
    CHECK(!potion_has_instant_effects(custom_water));

    // Water is the named potion *and* an empty custom list.
    CHECK(potion_contents_is_water(contents_of("minecraft:water")));
    CHECK(!potion_contents_is_water(custom_water));
    CHECK(!potion_contents_is_water(contents_of("minecraft:harming")));

    // A potion with no visible effect of its own is drawn with BASE_POTION_COLOR.
    CHECK_EQ(potion_color(contents_of("minecraft:water")), kBasePotionColor);
    CHECK((potion_color(contents_of("minecraft:harming")) & 0xFF000000u) == 0xFF000000u);
    // An explicit custom_color wins over the effects' average.
    StackPotionContents tinted = contents_of("minecraft:healing");
    tinted.custom_color = 0x123456;
    CHECK_EQ(potion_color(tinted), 0x123456);
    // The colour a cloud's particle carries is `ARGB.opaque(getColor())`.
    CHECK_EQ(cloud_particle_color(contents_of("minecraft:water")),
             static_cast<uint32_t>(0xFF000000) | static_cast<uint32_t>(kBasePotionColor));
  }

  // ---- geometry --------------------------------------------------------------
  {
    const PotionBox box = potion_box(1.0, 2.0, 3.0);
    CHECK_NEAR(box.min_x, 0.875, 1e-12);
    CHECK_NEAR(box.max_x, 1.125, 1e-12);
    CHECK_NEAR(box.min_y, 2.0, 1e-12);
    CHECK_NEAR(box.max_y, 2.25, 1e-12);
    CHECK_NEAR(box.min_z, 2.875, 1e-12);
    CHECK(box_contains(box, 1.0, 2.1, 3.0));
    CHECK(!box_contains(box, 1.0, 2.3, 3.0));

    // Two boxes one block apart on x, overlapping on y and z.
    const PotionBox other = potion_target_box(2.625, 2.0, 3.0, 0.5, 1.0);
    CHECK_NEAR(box_distance_to_sqr(box, other), 1.0, 1e-12);
    CHECK(!box_intersects(box, other));
    CHECK_NEAR(box_distance_to_sqr(box, box), 0.0, 1e-12);
    CHECK(box_intersects(box, box));
    // `AABB.inflate`, which the splash and the margin both use.
    const PotionBox inflated = box_inflate(box, 1.0, 2.0, 3.0);
    CHECK_NEAR(box_distance_to_sqr(inflated, other), 0.0, 1e-12);

    // ProjectileUtil.computeMargin: `max(0, min(0.3, (tickCount - 2) / 20))`.
    CHECK_NEAR(potion_entity_margin(0), 0.0, 1e-12);
    CHECK_NEAR(potion_entity_margin(2), 0.0, 1e-12);
    CHECK_NEAR(potion_entity_margin(6), 0.2, 1e-6);
    CHECK_NEAR(potion_entity_margin(22), 0.3, 1e-6);
    CHECK_NEAR(potion_entity_margin(1000), 0.3, 1e-6);
  }

  // ---- gravity, drag and the flight ------------------------------------------
  {
    std::vector<PotionProjectile> projectiles;
    projectiles.push_back(at(0.0, 0.0, 0.0, 0.5, 0.0, 0.0));
    PotionEnvironment env; // nothing to hit, no world
    const PotionTickReport first = tick_potions(projectiles, env);
    CHECK_EQ(first.moved.size(), 1);
    CHECK(first.removed.empty());
    CHECK(first.impacts.empty());
    CHECK_NEAR(projectiles[0].vx, 0.5 * 0.99, 1e-12);
    CHECK_NEAR(projectiles[0].vy, -0.05 * 0.99, 1e-12);
    CHECK_NEAR(projectiles[0].x, 0.5 * 0.99, 1e-12);
    CHECK_NEAR(projectiles[0].y, -0.05 * 0.99, 1e-12);
    CHECK_EQ(projectiles[0].age, 1);
    // The order is applyGravity *then* applyInertia, so the second tick's
    // gravity starts from the already-dragged velocity.
    tick_potions(projectiles, env);
    CHECK_NEAR(projectiles[0].vx, 0.5 * 0.99 * 0.99, 1e-12);
    CHECK_NEAR(projectiles[0].vy, (-0.05 * 0.99 - 0.05) * 0.99, 1e-12);
  }

  // ---- breaking on a block ---------------------------------------------------
  {
    std::vector<PotionProjectile> projectiles;
    projectiles.push_back(at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0));
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK(projectiles.empty());
    CHECK_EQ(report.removed.size(), 1);
    CHECK_EQ(report.removed[0], 42);
    CHECK_EQ(report.impacts.size(), 1);
    const PotionImpact& impact = report.impacts[0];
    CHECK_EQ(impact.projectile_entity_id, 42);
    CHECK(impact.kind == PotionProjectileKind::kSplash);
    CHECK(!impact.spawns_cloud);
    CHECK(!impact.as_water);
    // A harming potion has an instantaneous effect, so the event is 2007 rather
    // than 2002 (`Potion.hasInstantEffects`).
    CHECK_EQ(impact.level_event, 2007);

    // A potion with no effects breaks and does nothing at all: no applications,
    // no cloud, and the 2002 sound.
    std::vector<PotionProjectile> harmless;
    PotionProjectile awkward = at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0);
    awkward.contents = contents_of("minecraft:awkward");
    harmless.push_back(awkward);
    const PotionTickReport silent = tick_potions(harmless, env);
    CHECK_EQ(silent.impacts.size(), 1);
    CHECK_EQ(silent.impacts[0].level_event, 2002);
    CHECK(silent.impacts[0].applications.empty());
    CHECK(!silent.impacts[0].spawns_cloud);
  }

  // ---- the potion cannot hit its own thrower until it has left them ----------
  {
    // (a) The gate itself: `Projectile.canHitEntity` refuses an entity that is
    // the owner while `!leftOwner`, and the owner here is squarely inside the
    // first tick's segment (its box starts at x = 0.2 of a 0.495-block step), so
    // without the gate this would be a direct hit on tick one.
    {
      std::vector<PotionProjectile> projectiles;
      projectiles.push_back(at(0.0, 0.5, 0.0, 0.5, 0.0, 0.0));
      projectiles[0].owner_entity_id = 9;
      projectiles[0].left_owner = false;
      PotionEnvironment env;
      env.targets.push_back(target(9, 0.5, 0.0, 0.0));
      tick_potions(projectiles, env);
      CHECK(!projectiles[0].left_owner);
      CHECK_EQ(projectiles.size(), 1u); // still flying: the owner was skipped
      tick_potions(projectiles, env);
      CHECK(!projectiles[0].left_owner);
      CHECK_EQ(projectiles.size(), 1u);
    }
    // (b) The other half: the same shot with the flag already set hits the owner
    // outright — the only thing that was stopping it was the gate.
    {
      std::vector<PotionProjectile> projectiles;
      projectiles.push_back(at(0.0, 0.5, 0.0, 0.5, 0.0, 0.0));
      projectiles[0].owner_entity_id = 9;
      projectiles[0].left_owner = true;
      PotionEnvironment env;
      PotionTarget owner = target(9, 0.5, 0.0, 0.0);
      owner.is_player = true;
      env.targets.push_back(owner);
      const PotionTickReport report = tick_potions(projectiles, env);
      CHECK(projectiles.empty());
      CHECK_EQ(report.impacts.size(), 1);
      CHECK_EQ(report.impacts[0].applications.size(), 1u);
      CHECK(report.impacts[0].applications[0].is_player);
      CHECK_EQ(report.impacts[0].applications[0].entity_id, 9);
    }
    // (c) `checkLeftOwner` runs on the *moved* box, expanded towards the same
    // delta and inflated by one block, so a potion thrown from x = 0 at vx = 0.5
    // reports "outside" for the first time on the third tick.
    {
      std::vector<PotionProjectile> projectiles;
      projectiles.push_back(at(0.0, 0.5, 0.0, 0.5, 0.0, 0.0));
      projectiles[0].owner_entity_id = 9;
      projectiles[0].left_owner = false;
      PotionEnvironment env;
      env.targets.push_back(target(9, 0.0, 0.0, 0.0));
      tick_potions(projectiles, env);
      CHECK(!projectiles[0].left_owner);
      tick_potions(projectiles, env);
      CHECK(!projectiles[0].left_owner);
      tick_potions(projectiles, env);
      CHECK(projectiles[0].left_owner);
    }
  }

  // ---- a splash's application, at full strength ------------------------------
  {
    // The target's box *contains* the impact point (a wall at cell (1,0,0) stops
    // the potion at x = 1.0), so the distance is zero and the falloff is 1.0: a
    // harming potion does its full `6 << 0` to a target that is not inverted.
    std::vector<PotionProjectile> projectiles;
    projectiles.push_back(at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0));
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    env.targets.push_back(target(1, 1.0, 0.0, 0.5, /*half_width=*/0.5));
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK_EQ(report.impacts.size(), 1);
    const PotionImpact& impact = report.impacts[0];
    CHECK_EQ(impact.applications.size(), 1);
    CHECK(impact.applications[0].instantaneous);
    CHECK(impact.applications[0].harm);
    CHECK_EQ(impact.applications[0].amount, 6.0f);
    CHECK(!impact.applications[0].inverted_heal_and_harm);
    CHECK(std::string(impact.applications[0].damage_type) == "indirect_magic");
    CHECK_EQ(impact.applications[0].effect, effect_id("minecraft:instant_damage"));
  }

  // ---- the instantaneous amounts and the rounding ----------------------------
  {
    // `(int)(scale * ((harm ? 6 : 4) << amplifier) + 0.5)`.
    CHECK_EQ(instant_effect_amount(true, 0, 1.0), 6.0f);
    CHECK_EQ(instant_effect_amount(false, 0, 1.0), 4.0f);
    CHECK_EQ(instant_effect_amount(true, 1, 1.0), 12.0f);
    CHECK_EQ(instant_effect_amount(false, 1, 1.0), 8.0f);
    // The splash falloff and the cloud's fixed 0.5.
    CHECK_EQ(instant_effect_amount(true, 0, 0.5), 3.0f);
    CHECK_EQ(instant_effect_amount(true, 0, 0.85625), 5.0f);
    // A scale small enough rounds to nothing (which is why the float is kept
    // rather than a rounded int).
    CHECK_EQ(instant_effect_amount(true, 0, 0.05), 0.0f);
  }

  // ---- the splash's box, falloff and 20-tick skip ----------------------------
  {
    // A target inside the potion's own box: full strength.
    // A target 2 blocks away: `1 - 2/4` = half, so 3.
    // A corner target that the 4 x 2 x 4 box reaches but the squared-distance
    // gate does not: excluded.
    // A target 4 blocks away: outside the box entirely.
    std::vector<PotionProjectile> projectiles;
    projectiles.push_back(at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0));
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    // The potion ends at x = 1.0 with its box [0.875, 1.125] x [0.481, 0.731] x [0.375, 0.625].
    PotionTarget near = target(1, 1.0, 0.0, 0.5, /*half_width=*/0.5);
    PotionTarget two_away = target(2, 1.125 + 2.0 + 0.3, 0.0, 0.5);
    PotionTarget corner = target(3, 1.125 + 3.9 + 0.3, 0.74 + 1.9, 0.5);
    PotionTarget far = target(4, 1.125 + 4.6 + 0.3, 0.0, 0.5);
    env.targets = {near, two_away, corner, far};
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK_EQ(report.impacts.size(), 1);
    const std::vector<PotionApplication>& applications = report.impacts[0].applications;
    std::vector<int32_t> hit;
    std::vector<float> amounts;
    for (const PotionApplication& application : applications) {
      hit.push_back(application.entity_id);
      amounts.push_back(application.amount);
    }
    CHECK_EQ(hit.size(), 2u);
    CHECK_EQ(hit[0], 1);
    CHECK_EQ(hit[1], 2);
    CHECK_EQ(amounts[0], 6.0f);
    // `1 - sqrt(2^2) / 4` = 0.5, so `0.5 * 6 + 0.5` = 3.5 -> 3.
    CHECK_EQ(amounts[1], 3.0f);
  }

  // ---- the 20-tick skip and the splash's duration scaling --------------------
  {
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    // A custom effect with 20 ticks or fewer left after the scaling is dropped
    // (`newEffect.endsWithin(20)`); 100 ticks survives.
    std::vector<PotionProjectile> projectiles;
    PotionProjectile projectile = at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0);
    projectile.contents = contents_of("minecraft:water");
    projectile.contents.custom_effects.push_back(custom("minecraft:speed", 20, 0));
    projectile.contents.custom_effects.push_back(custom("minecraft:strength", 100, 1));
    projectiles.push_back(projectile);
    env.targets = {target(1, 1.0, 0.0, 0.5, /*half_width=*/0.5)};
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK_EQ(report.impacts.size(), 1);
    CHECK(!report.impacts[0].as_water); // custom effects make it not a water bottle
    CHECK_EQ(report.impacts[0].applications.size(), 1u);
    CHECK(!report.impacts[0].applications[0].instantaneous);
    CHECK_EQ(report.impacts[0].applications[0].instance.effect, effect_id("minecraft:strength"));
    CHECK_EQ(report.impacts[0].applications[0].instance.amplifier, 1);
    // The distance is zero, so the factor is 1.0 and the duration is unmoved.
    CHECK_EQ(report.impacts[0].applications[0].instance.duration, 100);
  }

  // ---- the lingering potion leaves a cloud instead ---------------------------
  {
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    std::vector<PotionProjectile> projectiles;
    PotionProjectile projectile = at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0);
    projectile.kind = PotionProjectileKind::kLingering;
    projectile.contents = contents_of("minecraft:swiftness");
    projectile.duration_scale = 0.25f;
    projectile.owner_entity_id = 7;
    projectiles.push_back(projectile);
    env.targets = {target(1, 1.0, 0.0, 0.5, /*half_width=*/0.5)};
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK_EQ(report.impacts.size(), 1);
    CHECK(report.impacts[0].spawns_cloud);
    CHECK(report.impacts[0].applications.empty()); // the cloud does the applying
    CHECK_EQ(report.impacts[0].duration_scale, 0.25f);
    CHECK_EQ(report.impacts[0].owner_entity_id, 7);
  }

  // ---- the water bottle ------------------------------------------------------
  {
    PotionEnvironment env;
    env.is_solid = wall_at_1_0_0;
    std::vector<PotionProjectile> projectiles;
    PotionProjectile projectile = at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0);
    projectile.contents = contents_of("minecraft:water");
    projectiles.push_back(projectile);
    // Nothing in this server is water-sensitive or on fire, so the water branch
    // reports the empty list vanilla would — and the sound is still 2002.
    env.targets = {target(1, 1.0, 0.0, 0.5, /*half_width=*/0.5)};
    const PotionTickReport report = tick_potions(projectiles, env);
    CHECK_EQ(report.impacts.size(), 1);
    CHECK(report.impacts[0].as_water);
    CHECK(report.impacts[0].applications.empty());
    CHECK(!report.impacts[0].spawns_cloud);
    CHECK_EQ(report.impacts[0].level_event, 2002);

    // A water-sensitive target takes 1 point of indirect magic, which is the one
    // way that branch can be observed at all.
    PotionTarget wet = target(5, 1.0, 0.0, 0.5, 0.5);
    wet.sensitive_to_water = true;
    env.targets = {wet};
    std::vector<PotionProjectile> second;
    second.push_back(at(0.85, 0.5, 0.5, 0.4, 0.0, 0.0));
    second[0].contents = contents_of("minecraft:water");
    const PotionTickReport splash = tick_potions(second, env);
    CHECK_EQ(splash.impacts[0].applications.size(), 1u);
    CHECK_EQ(splash.impacts[0].applications[0].amount, 1.0f);
    CHECK(splash.impacts[0].applications[0].harm);
  }

  // ---- the lingering cloud ---------------------------------------------------
  {
    const StackPotionContents swiftness = contents_of("minecraft:swiftness");
    AreaEffectCloud cloud = make_lingering_cloud(100, 0.0, 64.0, 0.0, swiftness, 0.25f, 7);
    CHECK_EQ(cloud.entity_id, 100);
    CHECK_EQ(cloud.owner_entity_id, 7);
    CHECK_EQ(cloud.radius, 3.0f);
    CHECK_EQ(cloud.duration, 600);
    CHECK_EQ(cloud.wait_time, 10);
    CHECK_EQ(cloud.reapplication_delay, 20);
    CHECK_EQ(cloud.radius_on_use, -0.5f);
    CHECK_NEAR(cloud.radius_per_tick, -3.0f / 600.0f, 1e-12);
    CHECK(!cloud.waiting);
    // `EntityDimensions.scalable(radius * 2, 0.5)`.
    const PotionBox box = cloud_box(cloud);
    CHECK_NEAR(box.min_x, -3.0, 1e-6);
    CHECK_NEAR(box.max_x, 3.0, 1e-6);
    CHECK_NEAR(box.min_y, 64.0, 1e-6);
    CHECK_NEAR(box.max_y, 64.5, 1e-6);
  }

  // ---- the cloud's wait, cadence and per-victim delay ------------------------
  {
    PotionEnvironment env;
    env.targets = {target(1, 0.5, 64.0, 0.0)};
    std::vector<AreaEffectCloud> clouds;
    clouds.push_back(
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:strength"), 1.0f, -1));
    // Nine ticks of waiting: nothing lands, and the flag is set.
    tick_cloud_n(clouds, env, 9);
    CHECK_EQ(clouds[0].age, 9);
    // The first application tick is the tenth, because `age < waitTime` is the
    // wait and the caller has already incremented the age.
    const CloudTickReport first = tick_cloud_n(clouds, env, 1);
    CHECK_EQ(first.applications.size(), 1u);
    CHECK_EQ(first.applications[0].entity_id, 1);
    CHECK(!first.applications[0].instantaneous);
    // The radius is not the box: the candidate set is everything the box
    // reaches, and the real gate is the *horizontal* distance against the
    // radius. At age 10 the radius is 2.95, so a target centred at 3.2 is inside
    // the box (whose edge is at 2.95) and outside the radius — skipped, and not
    // even recorded as a victim.
    {
      PotionEnvironment edge;
      edge.targets = {target(2, 3.2, 64.0, 0.0)};
      std::vector<AreaEffectCloud> one;
      one.push_back(
          make_lingering_cloud(101, 0.0, 64.0, 0.0, contents_of("minecraft:strength"), 1.0f, -1));
      const CloudTickReport wide = tick_cloud_n(one, edge, 10);
      CHECK_EQ(one[0].age, 10);
      CHECK(wide.applications.empty());
      CHECK(one[0].victims.empty());

      PotionEnvironment inside;
      inside.targets = {target(3, 2.5, 64.0, 0.0)};
      std::vector<AreaEffectCloud> other;
      other.push_back(
          make_lingering_cloud(102, 0.0, 64.0, 0.0, contents_of("minecraft:strength"), 1.0f, -1));
      const CloudTickReport narrow = tick_cloud_n(other, inside, 10);
      CHECK_EQ(narrow.applications.size(), 1u);
      CHECK_EQ(narrow.applications[0].entity_id, 3);
      CHECK_EQ(other[0].victims.size(), 1u);
    }
    // Nothing again until the victim's 20-tick delay has run out: the
    // application ticks are 10 and 30.
    const CloudTickReport between = tick_cloud_n(clouds, env, 19);
    CHECK_EQ(between.applications.size(), 0u);
    const CloudTickReport again = tick_cloud_n(clouds, env, 1);
    CHECK_EQ(again.applications.size(), 1u);
    CHECK_EQ(clouds[0].age, 30);
    CHECK_EQ(clouds[0].victims.size(), 1u);
    CHECK_EQ(clouds[0].victims[0].next_tick, 50);
  }

  // ---- the cloud's instantaneous scale is a fixed 0.5 ------------------------
  {
    PotionEnvironment env;
    env.targets = {target(1, 0.5, 64.0, 0.0)};
    std::vector<AreaEffectCloud> clouds;
    clouds.push_back(
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:harming"), 1.0f, -1));
    const CloudTickReport report = tick_cloud_n(clouds, env, 10);
    CHECK_EQ(report.applications.size(), 1u);
    CHECK(report.applications[0].instantaneous);
    CHECK(report.applications[0].harm);
    // `instant_effect_amount(true, 0, 0.5)` = `(int)(3 + 0.5)`.
    CHECK_EQ(report.applications[0].amount, 3.0f);

    // An inverted target takes the *heal* branch of a harming potion, at the
    // other amount: `isHarm == isInvertedHealAndHarm` heals.
    PotionEnvironment inverted;
    PotionTarget zombie = target(1, 0.5, 64.0, 0.0);
    zombie.inverted_heal_and_harm = true;
    inverted.targets = {zombie};
    std::vector<AreaEffectCloud> second;
    second.push_back(
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:harming"), 1.0f, -1));
    const CloudTickReport healed = tick_cloud_n(second, inverted, 10);
    CHECK_EQ(healed.applications.size(), 1u);
    CHECK(!healed.applications[0].harm);
    // `instant_effect_amount(false, 0, 0.5)` = `(int)(2 + 0.5)`.
    CHECK_EQ(healed.applications[0].amount, 2.0f);
  }

  // ---- the duration scale a lingering potion carries -------------------------
  {
    PotionEnvironment env;
    env.targets = {target(1, 0.5, 64.0, 0.0)};
    std::vector<AreaEffectCloud> clouds;
    // minecraft:potion_duration_scale 0.25 is what the lingering potion's own
    // item prototype carries, so a cloud grants a quarter of what it declares —
    // `withScaledDuration` floors, and never reaches zero.
    clouds.push_back(
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:swiftness"), 0.25f, -1));
    const CloudTickReport report = tick_cloud_n(clouds, env, 10);
    CHECK_EQ(report.applications.size(), 1u);
    const GeneratedPotion* swiftness = potion_by_name("minecraft:swiftness");
    CHECK(swiftness != nullptr && swiftness->count > 0);
    const int declared = kPotionEffects[swiftness->first].duration;
    CHECK_EQ(report.applications[0].instance.duration, declared / 4);
    CHECK(report.applications[0].instance.duration > 0);
  }

  // ---- the radius shrink, the radius_on_use discard and the age-out ----------
  {
    PotionEnvironment env;
    std::vector<AreaEffectCloud> clouds;
    AreaEffectCloud cloud =
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:swiftness"), 1.0f, -1);
    // 100 ticks of shrinking: `radius += radiusPerTick` on every *non-waiting*
    // tick, which is ages 10..100 — 91 of them, each -0.005. The radius is a
    // float and the step is accumulated in place, so the tolerance is loose
    // enough for the rounding but far tighter than one step.
    clouds.push_back(cloud);
    tick_cloud_n(clouds, env, 100);
    CHECK_EQ(clouds.size(), 1u);
    CHECK_NEAR(clouds[0].radius, 3.0f - 91.0f * 0.005f, 1e-4);
    CHECK(!clouds[0].waiting);
    // The age-out: `duration != -1 && age - waitTime >= duration`.
    clouds[0].age = clouds[0].wait_time + clouds[0].duration;
    const CloudTickReport expired = tick_cloud_n(clouds, env, 1);
    CHECK(clouds.empty());
    CHECK_EQ(expired.removed.size(), 1u);
    CHECK_EQ(expired.removed[0], 100);
  }

  {
    // A victim shrinks the cloud by 0.5 per application, and one that drops it
    // under MINIMAL_RADIUS discards it outright.
    PotionEnvironment env;
    env.targets = {target(1, 0.5, 64.0, 0.0)};
    std::vector<AreaEffectCloud> clouds;
    clouds.push_back(
        make_lingering_cloud(100, 0.0, 64.0, 0.0, contents_of("minecraft:swiftness"), 1.0f, -1));
    clouds[0].radius = 0.6f;
    const CloudTickReport report = tick_cloud_n(clouds, env, 10);
    CHECK(clouds.empty());
    CHECK_EQ(report.removed.size(), 1u);
    CHECK_EQ(report.applications.size(), 1u); // the effects still landed
    // The radius-per-tick path discards the same way, without a victim.
    PotionEnvironment empty;
    std::vector<AreaEffectCloud> fast;
    fast.push_back(
        make_lingering_cloud(101, 0.0, 64.0, 0.0, contents_of("minecraft:swiftness"), 1.0f, -1));
    fast[0].wait_time = 0;
    fast[0].radius = 0.6f;
    fast[0].radius_per_tick = -0.3f;
    const CloudTickReport shrunk = tick_cloud_n(fast, empty, 1);
    CHECK(fast.empty());
    CHECK_EQ(shrunk.removed.size(), 1u);
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("potion_projectile: all checks passed\n");
  return 0;
}
