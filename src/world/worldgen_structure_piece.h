// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4's structure application: the piece kinds that are built —
// `ShipwreckStructure` and its single `TemplateStructurePiece`, the hand-coded
// `SwampHutStructure`, the three-template `IglooStructure`, the single buried
// chest `BuriedTreasureStructure`, the desert pyramid and jungle temple
// `SinglePieceStructure`s, the ocean-ruin and ruined-portal template pieces,
// and the jigsaw village. The placement half
// (`worldgen_structure.h`) already answers which chunk is a start and which
// biome it must stand in; this is what turns a start into blocks: the
// template's palettes and rotation pivot, the height adjustments vanilla makes
// at placement time, and the writer that clips a chunk-crossing template into
// one column.
//
// Why shipwrecks first: they are the simplest piece kind in the pack that is a
// real template (no jigsaw assembly, no hand-coded pieces, no terrain
// adaptation), and their templates exercise everything the remaining kinds
// need — multiple material palettes, a rotation pivot, a start whose piece
// crosses a chunk border, and `BlockIgnoreProcessor.STRUCTURE_AND_AIR`.
//
// The second kind is the **swamp hut** (`SwampHutStructure`): the first
// hand-coded piece, with no template at all. It is the smallest structure in
// the pack whose geometry is code rather than NBT, so it pins the other half
// of the writer — a piece's own orientation (which rotates *coordinates*) and
// the `Mirror`/`Rotation` a `ScatteredFeaturePiece` applies to *states* —
// before the template-with-processors and jigsaw types need them. Its 7 x 9
// footprint starts at the source chunk's minimum corner, so unlike a shipwreck
// it never crosses a chunk boundary; the writer is still clipped to the target
// column for the shared entry point.
//
// The third is the **igloo** (`IglooStructure`): a stack of up to twelve
// template pieces (bottom laboratory, ladders, top) whose `postProcess` snaps
// each piece to the surface at its own entrance column. It also pins the
// `BlockIgnoreProcessor.STRUCTURE_BLOCK` (air is *placed*, unlike a shipwreck)
// and `LiquidSettings.IGNORE_WATERLOGGING` variants. The fourth is **buried
// treasure**, the single chest the ocean-floor scan finds under sand.
//
// The boundary is deliberate, and `docs/PHASE4.md` names it:
//
//   * `minecraft:shipwrecks`, `swamp_hut`, `igloo`, `buried_treasure`,
//     `desert_pyramid`, `jungle_pyramid`, the warm/cold `ocean_ruin`s, the
//     seven `ruined_portal*` types, the five villages, mineshafts,
//     strongholds, ocean monuments, woodland mansions, pillager outposts,
//     trail ruins, ancient cities, the nether fortress, the bastion remnant
//     and the end city are built. The last three are Nether/End structures;
//     the dimension bridge applies them in their own dimensions.
//   * A `structure_block` marker is *not* a block write: its metadata names
//     the chest's loot table (`supply_chest`, `map_chest`, `treasure_chest`,
//     `chest`) and the writer appends the resolved `Items` block entity to the
//     chunk that owns the chest (`worldgen_loot.h`). The igloo's own
//     `handleDataMarker` additionally clears the marker cell to air, which the
//     writer does too. The swamp hut has no
//     block entity, but its witch and cat spawns are not modelled either
//     (there is no structure-entity path in the bridge; the block layout is
//     complete). Template block entities and entity data (the igloo's
//     villager, zombie villager, furnace, sign and brewing stand) are not
//     carried; the chests and their loot are.
//   * The swamp hut's **height** is vanilla's `updateAverageGroundHeight` over
//     the source chunk, computed once from the surface stage. Vanilla computes
//     it on the first chunk to post-process the piece; since the whole 7 x 9
//     footprint sits in the source chunk, that first chunk is the source chunk
//     in the served world too, so no seam choice is being invented here.
//
// Ordering: vanilla places structures inside `applyBiomeDecoration`, at the
// structure's own `GenerationStep`, with the chunk's decoration random. This
// bridge applies them after the feature stage, with a stream of its own — the
// same rule the feature stage already follows (`worldgen_feature.h`). The
// heights the placement reads are therefore the **surface** stage's heightmaps,
// not a partly decorated column's; that keeps a chunk-crossing piece's height
// identical from every target chunk, which is the property the stitched output
// depends on.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "world/chunk.h"
#include "world/worldgen_structure_piece_table.h"
#include "world/worldgen_structure_table.h"

namespace mc::world {

class WgClimateSampler;
class WgOverworldGenerator;

// The three `Heightmap.Types` the structure pieces read.
enum class WgStructureHeightmap : uint8_t {
  kWorldSurface,          // WORLD_SURFACE_WG: NOT_AIR
  kOceanFloor,            // OCEAN_FLOOR_WG: MATERIAL_MOTION_BLOCKING
  kMotionBlockingNoLeaves // MOTION_BLOCKING_NO_LEAVES
};

// `ChunkGenerator.getBaseHeight(x, z, type)`: the heightmap answer, i.e. one
// past the highest cell the type's predicate accepts. The runtime implements
// this over generated **surface** columns (see the ordering note above); a
// test may supply a fixed one.
class WgStructureHeights {
public:
  virtual ~WgStructureHeights() = default;
  virtual int32_t height(int32_t block_x, int32_t block_z, WgStructureHeightmap type) = 0;
};

// The noise-stage implementation of `WgStructureHeights`: a lazily filled
// 16 x 16 noise column per chunk, cached for the lifetime of the object. This
// is `ChunkGenerator.getBaseHeight`'s own stage (`iterateNoiseColumn`), before
// the surface rules can raise a badlands pillar or an iceberg above the noise
// surface. A piece's height is therefore a function of its source terrain
// alone, not of the chunk currently being decorated.
class WgGeneratedHeights final : public WgStructureHeights {
public:
  explicit WgGeneratedHeights(const WgOverworldGenerator& generator);
  ~WgGeneratedHeights() override;

  WgGeneratedHeights(const WgGeneratedHeights&) = delete;
  WgGeneratedHeights& operator=(const WgGeneratedHeights&) = delete;

  int32_t height(int32_t block_x, int32_t block_z, WgStructureHeightmap type) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The start `ShipwreckStructure.findGenerationPoint` produces for one source
// chunk, after `Structure.isValidBiome`. The piece position's y is vanilla's
// fixed 90 at this stage (the real y is chosen at placement); `x` and `z` are
// the source chunk's minimum block corner, as `ShipwreckPieces.addRandomPiece`
// is given. `box` is the template box for `rotation` and the (4, 0, 15) pivot,
// at that position.
struct WgShipwreckStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:shipwreck` or `_beached`
  const WgPieceTemplate* templ = nullptr;
  uint32_t rotation = 0; // 0 none, 1 clockwise 90, 2 180, 3 counterclockwise 90
  // The world seed the piece belongs to. The writer derives each chest's
  // loot-table seed from this and the chest position (worldgen_loot.h).
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

// `StructureTemplate.getBoundingBox(position, rotation, pivot, NONE)` for a
// shipwreck template, with the shipwreck pivot (4, 0, 15).
struct WgPieceBox {
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};
WgPieceBox wg_piece_box(const WgPieceTemplate& templ, uint32_t rotation, int32_t x, int32_t y,
                        int32_t z, uint8_t mirror = 0);

// `BlockState.rotate(Rotation)` for the properties the shipwreck palettes use:
// `facing`, `axis`, and the four fence connection booleans. Every other
// property (`half`, `shape`, `open`, `hinge`, `type`, `mode`, `waterlogged`,
// `powered`) is rotation-invariant in vanilla's own block classes. The input
// and output are the canonical sorted `k=v,k=v` form.
std::string wg_piece_rotate_properties(std::string_view properties, uint32_t rotation);

// `BlockState.mirror(Mirror)` for the same property families. `mirror` is 1
// for `LEFT_RIGHT` (reflect Z) and 2 for `FRONT_BACK` (reflect X); `NONE` is a
// no-op. The mansion templates need it before the rotation, exactly as
// `StructureTemplate.placeInWorld` applies `state.mirror(...).rotate(...)`.
std::string wg_piece_mirror_properties(std::string_view properties, uint8_t mirror);

// The generated template whose `name` matches, or nullptr.
const WgPieceTemplate* wg_piece_template_by_name(const char* name);

// `ShipwreckStructure.findGenerationPoint` + `isValidBiome` for one source
// chunk: the set's weighted member pick, `Rotation.getRandom` and
// `Util.getRandom` from a `WorldgenRandom.setLargeFeatureSeed(seed, x, z)`, the
// center column's height for the biome test, and the template box. `heights`
// answers the source chunk's surface heightmap. Returns an invalid start when
// the chunk is not this set's candidate, when the candidate fails its
// frequency/exclusion checks, or when neither member's biome set accepts the
// center's biome.
WgShipwreckStart wg_shipwreck_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights);

// `SwampHutStructure.findGenerationPoint` + `isValidBiome` for one source
// chunk: the set's candidate gate, the piece direction from the context
// random's first `nextInt(4)` (HORIZONTAL = north, east, south, west), the
// chunk-centre column's `WORLD_SURFACE_WG` height for the biome test, the
// `ScatteredFeaturePiece` box at (chunkMinX, 64, chunkMinZ), and the
// `updateAverageGroundHeight` answer over the box's footprint in the source
// chunk (`MOTION_BLOCKING_NO_LEAVES`). `ground_y` is the y the writer uses; the
// box fields stay at the initial y=64, as the server's piece does before its
// first post-process.
struct WgSwampHutStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:swamp_hut`
  uint8_t direction = 0;                  // 0 north, 1 east, 2 south, 3 west
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t x = 0;
  int32_t y = 64;
  int32_t z = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  int32_t ground_y = 0;
};

WgSwampHutStart wg_swamp_hut_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                   WgClimateSampler& sampler, WgStructureHeights& heights);

// `SwampHutPiece.postProcess`'s block half, clipped to `chunk`: the eight
// `generateBox` calls, the fences/air/pot/flower/crafting-table/cauldron
// placements, the four stair runs with their outer-corner shapes, and the four
// `fillColumnDown` oak-log columns. `start.ground_y` is the height
// `updateAverageGroundHeight` chose. The witch and cat spawns are not applied.
// Returns the number of cells whose state changed.
uint32_t wg_place_swamp_hut(Chunk& chunk, const WgSwampHutStart& start);

// `IglooPieces.addPieces`: one template piece per ladder segment, plus the
// laboratory bottom (only when the 50% draw succeeds) and the top. The writer
// applies them in vanilla's insertion order — bottom, ladders, top — because a
// later piece overwrites an earlier one where they overlap.
struct WgIglooPiece {
  const WgPieceTemplate* templ = nullptr;
  uint32_t rotation = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
};

struct WgIglooStart {
  static constexpr uint32_t kMaxPieces = 12;
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:igloo`
  int64_t world_seed = 0;
  uint32_t rotation = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t x = 0;
  int32_t y = 90; // `IglooPieces.GENERATION_HEIGHT`
  int32_t z = 0;
  uint32_t piece_count = 0;
  WgIglooPiece pieces[kMaxPieces];
};

WgIglooStart wg_igloo_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                            WgClimateSampler& sampler, WgStructureHeights& heights);

// `IglooPieces.IglooPiece.postProcess` for every piece, clipped to `chunk`.
// Each piece's template position is moved by `entrance height - 90 - 1` from
// the WORLD_SURFACE_WG column at its own entrance, and the top piece puts a
// snow block over its trapdoor when the cell below is neither air nor a
// ladder. Returns the number of cells whose state changed.
uint32_t wg_place_igloo(Chunk& chunk, const WgIglooStart& start, WgStructureHeights& heights);

// `BuriedTreasureStructure`: one chest at the chunk's (9, 9) column, found by
// walking down from `OCEAN_FLOOR_WG` to the first column whose block below is
// sandstone, stone, andesite, granite or diorite.
struct WgBuriedTreasureStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:buried_treasure`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t x = 0;
  int32_t z = 0;
};

WgBuriedTreasureStart wg_buried_treasure_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                               WgClimateSampler& sampler,
                                               WgStructureHeights& heights);

// `BuriedTreasurePieces.BuriedTreasurePiece.postProcess`: the downward scan,
// the eight surrounding cells' sand/stone fill, and the `chests/buried_treasure`
// chest at the found position. Returns the number of cells whose state changed
// (the chest block included).
uint32_t wg_place_buried_treasure(Chunk& chunk, const WgBuriedTreasureStart& start,
                                  WgStructureHeights& heights);

// What one applied chunk saw. `candidates` counts the source chunks of the
// four sets found in the scan window, `starts` the valid starts whose boxes
// intersect this chunk, `pieces` the starts that wrote at least one cell, and
// `blocks` the cells whose state changed. A box can intersect a chunk and still
// write nothing: the ship hull tapers, so a corner of the 28 x 9 box can be all
// margin.
struct WgStructureApplyStats {
  uint32_t candidates = 0;
  uint32_t starts = 0;
  uint32_t pieces = 0;
  uint32_t blocks = 0;
};

// `SinglePieceStructure`'s jungle temple: a 12 x 15 `ScatteredFeaturePiece`
// whose `updateAverageGroundHeight` chooses the y. The piece is the first
// hand-coded kind with a per-cell material selector (40% cobblestone, 60%
// mossy cobblestone) and a redstone/tripwire cellar, so its writer pins the
// selector's draw order as well as the block layout.
struct WgJungleTempleStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:jungle_pyramid`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint8_t direction = 0; // 0 north, 1 east, 2 south, 3 west
  int32_t x = 0;
  int32_t y = 64;
  int32_t z = 0;
  int32_t ground_y = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

WgJungleTempleStart wg_jungle_temple_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                           WgClimateSampler& sampler, WgStructureHeights& heights);

// `JungleTemplePiece.postProcess`: the stone shell, the air rooms, the
// cobblestone stairs, the two tripwire traps and their dispensers, the
// redstone cellar, the hidden chest and the main chest. The selector consumes
// one `nextFloat` per generated cell; the two chests and the two dispensers
// consume one `nextLong` each for their loot-table seeds. The writer keeps
// those draws in the same order as the server, so the same positional random
// stream produces the same cells.
uint32_t wg_place_jungle_temple(Chunk& chunk, const WgJungleTempleStart& start);

// `SinglePieceStructure`'s desert pyramid: a 21 x 21 hand-coded piece whose
// `updateHeightPositionToLowestGroundHeight` chooses the y. The four chests,
// the TNT cellar and the collapsed-roof sand are part of the piece writer.
struct WgDesertPyramidStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:desert_pyramid`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint8_t direction = 0; // 0 north, 1 east, 2 south, 3 west
  int32_t x = 0;
  int32_t y = 64;
  int32_t z = 0;
  int32_t ground_y = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

WgDesertPyramidStart wg_desert_pyramid_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                             WgClimateSampler& sampler,
                                             WgStructureHeights& heights);

// `DesertPyramidPiece.postProcess` plus `DesertPyramidStructure.afterPlace`:
// the pyramid body, the TNT cellar, the four `chests/desert_pyramid` chests, and
// the suspicious-sand placement (with its archaeology loot).
uint32_t wg_place_desert_pyramid(Chunk& chunk, const WgDesertPyramidStart& start,
                                 WgStructureHeights& heights);

// `MineshaftStructure` and `MineshaftPieces`: the room at the source chunk's
// (2, 2) corner, the recursive corridor/crossing/stairs assembly, and the
// hand-coded writers. The start is assembled with vanilla's context random and
// the set's weighted normal/mesa pick; each piece's writer uses a positional
// random seeded from its own bounding-box corner so a piece crossing a chunk
// border writes the same cells from every target chunk.
//
// The port has no minecart entity. `MineShaftCorridor.createChest` writes a
// rail and a `chest_minecart`; the writer keeps the loot draw and the
// `chests/abandoned_mineshaft` table, but stands a chest block in the
// minecart's cell so the contents are reachable. The probe records the vanilla
// minecart so the substitution is explicit in the parity dump.
enum class WgMineshaftPieceKind : uint8_t {
  kRoom = 0,
  kCorridor = 1,
  kCrossing = 2,
  kStairs = 3,
};

struct WgMineshaftPiece {
  WgMineshaftPieceKind kind = WgMineshaftPieceKind::kRoom;
  uint8_t type = 0;      // 0 normal oak, 1 mesa dark oak
  uint8_t direction = 0; // 0 north, 1 east, 2 south, 3 west; the room has none
  uint8_t two_floored = 0;
  uint8_t has_rails = 0;
  uint8_t spider_corridor = 0;
  uint16_t gen_depth = 0;
  uint16_t num_sections = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  // `MineShaftRoom.childEntranceBoxes`, in world coordinates. Empty for the
  // other piece kinds.
  std::vector<WgPieceBox> entrances;
};

struct WgMineshaftStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:mineshaft` or `_mesa`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint8_t type = 0;     // 0 normal, 1 mesa
  int32_t start_x = 0;  // stub position: chunk middle x
  int32_t start_y = 50; // stub position: 50 + the vertical adjustment
  int32_t start_z = 0;  // stub position: chunk minimum z
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  std::vector<WgMineshaftPiece> pieces;
};

// `MineshaftStructure.findGenerationPoint` + the set's weighted pick and
// `isValidBiome`: the room, recursive `addChildren`, the normal
// `moveBelowSeaLevel` or the mesa surface-height target, and the stub's biome
// gate. `heights` supplies the mesa target's surface column.
WgMineshaftStart wg_mineshaft_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights);

// Every intersecting piece's `postProcess`, clipped to `chunk`, in insertion
// order. Each piece uses `WgLegacyRandom(Mth.getSeed(piece.minX, piece.minY,
// piece.minZ))`. Returns the number of cells whose state changed.
uint32_t wg_place_mineshaft(Chunk& chunk, const WgMineshaftStart& start,
                            WgStructureHeights& heights, WgClimateSampler& sampler);

// `StrongholdStructure` and `StrongholdPieces`: the ring chunk's `StartPiece`
// (a `StairsDown`), its recursive small-door assembly, and the hand-coded
// pieces. This is the first applied family whose assembly is not a template or
// a simple bounded recursion: the retry loop rebuilds the whole start until a
// `PortalRoom` was placed, the piece weights are consumed without replacement,
// and the final `moveBelowSeaLevel` shifts every box together.
//
// The writer uses the same orientation transform as the other hand-coded
// pieces (`StructurePiece.getWorldX`/`getWorldZ`, then the state's mirror and
// rotation). Each piece gets `WgLegacyRandom(Mth.getSeed(box))`, the port's
// positional convention, so a piece that crosses a chunk border writes the
// same cells from every target chunk. The portal room's silverfish spawner is
// written as a block entity; its `setEntityId` consumes no random draws. The
// room crossing's basin is `water[level=0]`, the portal room's lava pools
// `lava[level=0]`, and chests use `StructurePiece.reorient`'s full-cube test
// (a slab or stair is not a solid neighbour).
enum class WgStrongholdPieceKind : uint8_t {
  kStart = 0,
  kStraight = 1,
  kPrisonHall = 2,
  kLeftTurn = 3,
  kRightTurn = 4,
  kRoomCrossing = 5,
  kStraightStairsDown = 6,
  kStairsDown = 7,
  kFiveCrossing = 8,
  kChestCorridor = 9,
  kLibrary = 10,
  kPortalRoom = 11,
  kFillerCorridor = 12,
};

struct WgStrongholdPiece {
  WgStrongholdPieceKind kind = WgStrongholdPieceKind::kStraight;
  uint8_t direction = 0;  // 0 north, 1 east, 2 south, 3 west
  uint8_t entry_door = 0; // 0 opening, 1 wood, 2 grates, 3 iron
  uint8_t gen_depth = 0;
  // `Straight.leftChild`/`rightChild`, `FiveCrossing.leftLow`/`leftHigh`/
  // `rightLow`/`rightHigh`, `RoomCrossing.type`, `Library.isTall`,
  // `StairsDown.isSource`, `FillerCorridor.steps`.
  uint8_t left = 0;
  uint8_t right = 0;
  uint8_t left_high = 0;
  uint8_t right_high = 0;
  uint8_t room_type = 0;
  uint8_t is_tall = 0;
  uint8_t is_source = 0;
  int32_t steps = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

struct WgStrongholdStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:stronghold`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  // How many `setLargeFeatureSeed(seed + tries)` attempts the start needed;
  // the retry stops at the first one whose `PortalRoom` was placed.
  uint32_t attempts = 0;
  // Index of the `PortalRoom` in `pieces`, or -1 when none was placed (the
  // start is then invalid and `attempts` stopped at its bound).
  int32_t portal_room = -1;
  // `StructureStart.getBoundingBox`: the pieces' union inflated by 12 on
  // every side, because the stronghold's terrain adaptation is not `none`,
  // not the union the pieces store.
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  std::vector<WgStrongholdPiece> pieces;
};

// `StrongholdStructure.generatePieces` + `isValidBiome`: the caller has already
// established that this chunk is one of the concentric-ring positions. The
// retry loop rebuilds the assembly until `PortalRoom` is present; `sampler`
// answers the generation-point biome gate at the chunk's minimum corner.
WgStrongholdStart wg_stronghold_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                      WgClimateSampler& sampler);

// Every intersecting piece's `postProcess`, in insertion order, clipped to
// `chunk`. Each piece uses `WgLegacyRandom(Mth.getSeed(piece.minX, piece.minY,
// piece.minZ))`. Returns the number of cells whose state changed.
uint32_t wg_place_stronghold(Chunk& chunk, const WgStrongholdStart& start);

// `WoodlandMansionStructure` and `WoodlandMansionPieces`: the mansion is a
// template-piece structure with its own 11 x 11 room grid rather than a jigsaw
// assembly. `MansionGrid` draws the base corridors and rooms, the identified
// 1x1/1x2/2x2 room graph and the optional third floor; `MansionPiecePlacer`
// then emits the entrance, outer walls, roofs, corridor floors, wall/door
// pieces and room templates in vanilla's insertion and random order. Every
// piece is a `TemplateStructurePiece` with a `Mirror` and `Rotation`; the
// writer mirrors then rotates each block state (the coordinates are transformed
// around the template position first) and applies the mansion's
// `BlockIgnoreProcessor.STRUCTURE_BLOCK` (the data-marker cell is cleared, not
// kept). Chest markers (`ChestWest`, `ChestEast`, `ChestNorth`, `ChestSouth`)
// create the chest at the marker cell with the rotated facing and the
// `chests/woodland_mansion` table; `Mage`, `Warrior` and `Group of Allays`
// markers are cleared but no mob is spawned (the bridge has no structure-entity
// path). `afterPlace` then places the cobblestone foundation under the
// structure's footprint, reading the target chunk's own post-piece cells.
struct WgMansionPiece {
  const WgPieceTemplate* templ = nullptr;
  uint8_t rotation = 0; // 0 none, 1 clockwise 90, 2 180, 3 counterclockwise 90
  uint8_t mirror = 0;   // 0 none, 1 left/right (Z), 2 front/back (X)
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
};

struct WgMansionStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:mansion`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  // `Rotation.getRandom` from the context's `setLargeFeatureSeed` stream.
  uint32_t rotation = 0;
  // `getLowestYIn5by5BoxOffset7Blocks`: chunk block (7, y, 7).
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  // `StructureStart.getBoundingBox`: the union of every piece's template box.
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  // Every generated `WoodlandMansionPiece`, in vanilla's insertion order.
  std::vector<WgMansionPiece> pieces;
};

// `WoodlandMansionStructure.findGenerationPoint` + `isValidBiome` for one
// source chunk: the rotation draw, the four `WORLD_SURFACE_WG` corner heights
// around the chunk's (7, 7) column, the y >= 60 gate, the start-biome test, and
// the full `MansionGrid` + `MansionPiecePlacer` assembly.
WgMansionStart wg_mansion_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                WgClimateSampler& sampler, WgStructureHeights& heights);

// Every piece whose template box intersects `chunk`, clipped to that chunk,
// followed by `WoodlandMansionStructure.afterPlace`'s cobblestone foundation.
// Returns the number of cells whose state changed.
uint32_t wg_place_mansion(Chunk& chunk, const WgMansionStart& start);

// `OceanMonumentStructure` and `OceanMonumentPieces`: the 49-room graph is
// assembled first (the source, core and shuffled room definitions with their
// door-closing pass), then the twelve piece kinds are written. The writer
// needs the current chunk's blocks, not just its heightmap: `generateWaterBox`
// reads each cell to decide between air and water, `generateBoxOnFillOnly`
// fills only cells that are already water, and `fillColumnDown` walks until it
// meets a non-replaceable cell. Those reads are confined to the target chunk
// (`StructurePiece.getBlock` answers air outside `chunkBB`), so the port reads
// `chunk` itself.
//
// The building's child pieces share the building's random stream: vanilla runs
// them from `MonumentBuilding.postProcess`, in insertion order, and skips a
// child whose box does not intersect the target chunk. A simple room's pillar
// draw and a top room's sponge cells therefore depend on which children reach
// this chunk, not on a per-piece positional seed. The writer keeps that order.
// The elder guardians' spawns are not modelled (the bridge has no structure-
// entity path); every block is.
enum class WgOceanMonumentPieceKind : uint8_t {
  kBuilding = 0,
  kPenthouse = 1,
  kWing = 2,
  kCore = 3,
  kDoubleYZ = 4,
  kDoubleXY = 5,
  kDoubleZ = 6,
  kDoubleX = 7,
  kDoubleY = 8,
  kSimpleTop = 9,
  kSimple = 10,
  kEntry = 11,
};

// One `OceanMonumentPieces.RoomDefinition`, with connections stored as room
// `index` values (`-1` for none). Opening bit `i` is vanilla's
// `Direction.get3DDataValue` order: down, up, north, south, west, east.
struct WgMonumentRoom {
  int32_t index = 0;
  uint8_t has_opening = 0;
  int16_t connections[6] = {-1, -1, -1, -1, -1, -1};
  uint8_t claimed = 0;
  uint8_t is_source = 0;
};

struct WgOceanMonumentPiece {
  WgOceanMonumentPieceKind kind = WgOceanMonumentPieceKind::kBuilding;
  uint8_t direction = 0;   // 0 north, 1 east, 2 south, 3 west
  uint8_t main_design = 0; // `SimpleRoom.mainDesign` / `WingRoom.mainDesign`
  // The piece's `RoomDefinition.index`, or -1 for the building, penthouse and
  // wings. The room row is in `WgOceanMonumentStart.rooms`.
  int32_t room_index = -1;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

struct WgOceanMonumentStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:monument`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  // `onTopOfChunkCenter`'s stub position: the chunk middle column's
  // `OCEAN_FLOOR_WG` first-free height.
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  // `StructureStart.getBoundingBox`: the building's own 58 x 23 x 58 box; the
  // monument's terrain adaptation is `none`, so there is no inflation.
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
  WgOceanMonumentPiece building;
  // `MonumentBuilding.childPieces`, in insertion order: entry, core, the room
  // pieces in the shuffled room-definition order, both wings, penthouse.
  std::vector<WgOceanMonumentPiece> pieces;
  // The room graph (46 grid rooms plus the roof/left/right specials), in grid
  // slot order; the specials carry indexes 1003, 1001 and 1002.
  std::vector<WgMonumentRoom> rooms;
};

// `OceanMonumentStructure.findGenerationPoint` + `isValidBiome` for one source
// chunk: the 59 x 59 x 59 surrounding-biome check at sea level, the chunk
// centre's ocean-floor height, and the building assembly. `heights` supplies
// the `OCEAN_FLOOR_WG` answer; the surrounding check uses `sampler` directly.
WgOceanMonumentStart wg_ocean_monument_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                             WgClimateSampler& sampler,
                                             WgStructureHeights& heights);

// `MonumentBuilding.postProcess` and every child piece, clipped to `chunk`. The
// building's positional random (`Mth.getSeed` of its box corner) is shared by
// all children in vanilla's order. Returns the number of cells whose state
// changed.
uint32_t wg_place_ocean_monument(Chunk& chunk, const WgOceanMonumentStart& start);

// `OceanRuinPieces.addPieces`: one warm/cold ruin, plus the cold biome's
// cracked/mossy layers and (large only) the cluster. Each piece carries its own
// integrity and the suspicious-block processor.
struct WgOceanRuinPiece {
  const WgPieceTemplate* templ = nullptr;
  uint32_t rotation = 0;
  int32_t x = 0;
  int32_t y = 90;
  int32_t z = 0;
  float integrity = 0.8f;
  uint8_t cold = 0;
  uint8_t large = 0;
};

struct WgOceanRuinStart {
  static constexpr uint32_t kMaxPieces = 12;
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:ocean_ruin_cold` / `_warm`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint8_t cold = 0;
  uint8_t large = 0;
  uint32_t piece_count = 0;
  WgOceanRuinPiece pieces[kMaxPieces];
};

WgOceanRuinStart wg_ocean_ruin_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                     WgClimateSampler& sampler, WgStructureHeights& heights);

// `OceanRuinPiece.postProcess`: snaps each piece to the ocean floor, applies the
// integrity rot and the capped suspicious-sand/gravel processor, and handles
// the `chest` and `drowned` data markers.
uint32_t wg_place_ocean_ruin(Chunk& chunk, const WgOceanRuinStart& start,
                             WgStructureHeights& heights);

// `RuinedPortalStructure.findGenerationPoint` + `RuinedPortalPiece`: the
// weighted setup, giant-portal draw, rotation/mirror, the suitable-y scan for
// the placement, and the netherrack/vines/leaves pass.
struct WgRuinedPortalStart {
  bool valid = false;
  const WgStructure* structure = nullptr; // one of `minecraft:ruined_portal*`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  const WgPieceTemplate* templ = nullptr;
  uint32_t rotation = 0;
  uint8_t mirror = 0; // 0 none, 1 front/back
  uint8_t placement = 0;
  uint8_t cold = 0;
  uint8_t air_pocket = 0;
  uint8_t overgrown = 0;
  uint8_t vines = 0;
  uint8_t blackstone = 0;
  float mossiness = 0.2f;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  int32_t pivot_x = 0;
  int32_t pivot_z = 0;
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t min_z = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t max_z = 0;
};

WgRuinedPortalStart wg_ruined_portal_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                           WgStructureHeights& heights);

uint32_t wg_place_ruined_portal(Chunk& chunk, const WgRuinedPortalStart& start,
                                WgStructureHeights& heights);

// One piece of a jigsaw structure: the template, its placement, the pool
// element's processor list and whether the element is terrain-matching.
struct WgJigsawPiece {
  const WgPieceTemplate* templ = nullptr; // first part, for the jigsaw pass
  const WgPoolElementRow* element = nullptr;
  uint32_t rotation = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  // The placed box, including the expansion-hack growth `JigsawPlacement`
  // applies. Recomputing it from the template loses that growth and can miss a
  // chunk the start touches.
  WgPieceBox box;
  uint16_t processor_list = 0xFFFF;
  uint8_t terrain_matching = 0;
  // End-city pieces use `BlockIgnoreProcessor.STRUCTURE_BLOCK` (overwrite
  // true), which writes the template's air cells; every jigsaw piece uses
  // STRUCTURE_AND_AIR and leaves them alone.
  uint8_t overwrite_air = 0;
};

struct WgJigsawStart {
  static constexpr uint32_t kMaxPieces = 768;
  bool valid = false;
  const WgStructure* structure = nullptr; // a village or `minecraft:pillager_outpost`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint32_t rotation = 0;
  uint32_t piece_count = 0;
  WgJigsawPiece pieces[kMaxPieces];
};

// `JigsawStructure.findGenerationPoint` for a village: starts at the chunk
// corner with `start_height` 0, projects the centre piece to
// `WORLD_SURFACE_WG`, then runs `JigsawPlacement` with the structure JSON's
// `size` (6) and `use_expansion_hack`.
WgJigsawStart wg_village_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights);

// The same `findGenerationPoint` for `minecraft:pillager_outpost`: one
// `base_plates` centre, size 7, the same 80-block centre bound.
WgJigsawStart wg_outpost_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights);

// `minecraft:ancient_city`: a size-7 jigsaw whose centre is anchored by the
// `minecraft:city_anchor` connector, starts at absolute y -27 with no heightmap
// projection, and has a 116-block centre bound. Its pieces carry the three
// degradation processor lists (block rot over `#ancient_city_replaceable`,
// the crack rules and `#features_cannot_replace` protection).
WgJigsawStart wg_ancient_city_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights);

// `minecraft:trail_ruins`: a size-7 graph over the tower/roads/buildings/decor
// pools, started at absolute y -15 and projected to `WORLD_SURFACE_WG`. Its
// three archaeology processor lists carry `capped` passes, the first applied
// family whose block writes depend on a whole-piece shuffle.
WgJigsawStart wg_trail_ruins_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                   WgClimateSampler& sampler, WgStructureHeights& heights);

// `minecraft:bastion_remnant`: a size-6 jigsaw over the bastion starts pool,
// absolute y 33, no heightmap projection and an 80-block centre bound. The
// nether biome gate is real (`skip_biome` is for probes, which open the server
// predicate the same way); the dimension bridge supplies a Nether sampler.
WgJigsawStart wg_bastion_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights,
                               bool skip_biome = false);

// `minecraft:end_city`: the recursive `EndCityPieces` tower/bridge graph. The
// templates are ordinary `TemplateStructurePiece`s (not jigsaw pools), so this
// is a direct port of the four section generators. The start is the chunk's
// (7, 7) column lowered over a 5 x 5 box; it is rejected below y 60.
WgJigsawStart wg_end_city_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                WgClimateSampler& sampler, WgStructureHeights& heights,
                                bool skip_biome = false);

// `PoolElementStructurePiece.postProcess` for every assembled jigsaw piece,
// clipped to `chunk`: the final jigsaw replacement, the rule processors, the
// `block_rot` integrity pass, the terrain-matching gravity processor and the
// structure block ignore.
uint32_t wg_place_jigsaw(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights);
uint32_t wg_place_village(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights);
uint32_t wg_place_outpost(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights);
uint32_t wg_place_ancient_city(Chunk& chunk, const WgJigsawStart& start,
                               WgStructureHeights& heights);
uint32_t wg_place_trail_ruins(Chunk& chunk, const WgJigsawStart& start,
                              WgStructureHeights& heights);

// `minecraft:fortress`: `NetherFortressStructure.generatePieces`. The fortress
// is hand-coded (no templates): a start bridge crossing at the source chunk's
// (2, 2) block, then weighted bridge/castle children until depth 30 or the
// pending list drains, followed by `moveInsideHeights(random, 48, 70)`. The
// nether biome predicate is not modelled here (this engine generates the
// overworld only); the assembly itself is dimension-independent, which is what
// the probe pins.
enum class WgFortressPieceType : uint8_t {
  kBridgeStraight,
  kBridgeCrossing,
  kRoomCrossing,
  kStairsRoom,
  kMonsterThrone,
  kCastleEntrance,
  kCastleSmallCorridor,
  kCastleSmallCorridorCrossing,
  kCastleSmallCorridorRightTurn,
  kCastleSmallCorridorLeftTurn,
  kCastleCorridorStairs,
  kCastleCorridorTBalcony,
  kCastleStalkRoom,
  kBridgeEndFiller,
};

struct WgFortressPiece {
  WgFortressPieceType type = WgFortressPieceType::kBridgeStraight;
  uint8_t direction = 0; // 0 north, 1 east, 2 south, 3 west
  uint8_t depth = 0;
  uint8_t needing_chest = 0;
  int32_t self_seed = 0; // BridgeEndFiller
  WgPieceBox box;
};

struct WgFortressStart {
  static constexpr uint32_t kMaxPieces = 768;
  bool valid = false;
  const WgStructure* structure = nullptr; // `minecraft:fortress`
  int64_t world_seed = 0;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint32_t piece_count = 0;
  WgFortressPiece pieces[kMaxPieces];
};

// `NetherFortressStructure.findGenerationPoint` + its recursive assembly. No
// heightmap is read; the source chunk's (2, 2) column is the start.
WgFortressStart wg_fortress_start(int64_t seed, int32_t chunk_x, int32_t chunk_z);

// The weighted member `minecraft:nether_complexes` picks for a source chunk.
// Vanilla draws this before building the member's start; a failed biome gate
// retries the remaining members, which the caller can reproduce by asking the
// start functions after this choice. A chunk that is not a structure chunk
// still answers according to the same draw, so callers must apply the set's
// gate (`wg_structure_is_chunk`) first.
enum class WgNetherComplexKind { kFortress, kBastion };
WgNetherComplexKind wg_nether_complex_kind(int64_t seed, int32_t chunk_x, int32_t chunk_z);

// Every fortress piece's `postProcess`, clipped to `chunk`. The bridge's
// chests use `minecraft:chests/nether_bridge`; the monster throne's spawner
// carries `minecraft:blaze`.
uint32_t wg_place_fortress(Chunk& chunk, const WgFortressStart& start);

// Applies every structure start whose box intersects `chunk` (x, z), using the
// generator's surface heights. Reads and writes only `chunk`'s own cells;
// deterministic for a given generator seed and chunk. Called by
// `fill_generated_decorated_chunk` after the feature stage.
WgStructureApplyStats apply_generated_structures(const WgOverworldGenerator& generator,
                                                 Chunk& chunk);

} // namespace mc::world
