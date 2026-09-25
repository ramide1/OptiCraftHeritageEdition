#pragma once

// -----------------------------------------------------------------------------
// 3DS world tuning
// -----------------------------------------------------------------------------
// The 3DS takes the desktop branch of PlatformGameTuning.h on purpose (it
// shares the desktop WorldRenderer/RenderGlobal path) and then overrides the
// handful of desktop *assumptions* the ARM11 cannot honour. Same shape as the
// Wii's WiiWorldTuning.h/WiiFrameTuning.h pair, kept in one file because every
// knob here traces to the same 2026-09-25 debug.log: a world entry that
// started at 53 fps, spent its whole mesh budget on a 2312-slot renderer grid,
// and slid to 1 fps within seconds:
//
//   [3ds.perf] frames=534 fps=53 ...                 (menu / loading)
//   [3ds.perf] frames=481 fps=48 ... 458 fps=45 ... 330 fps=33
//   [3ds.perf] frames=212 fps=21 frame=65/9400ms ...
//   [3ds.perf] frames=15  fps=1  frame=708/1444ms tick=238/347ms
//   [MC][W][render] 3ds: draw with 21096 vertices exceeds one arena ...
//
// The desktop defaults it inherited: render distance FAR with
// ofRenderDistanceFine=128 -- RenderGlobal derives its grid from the fine
// distance, so that is a 17 x 8 x 17 grid (2312 sections, the very figure the
// Wii's tuning header calls unaffordable at 729 MHz) -- plus
// PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME=99 with no wall-clock mesh budget,
// and a 128-block synchronous preload (289 chunk columns before the first
// frame). None of that is a deliberate choice for this hardware; it is simply
// the only branch of the table the 3DS had not claimed yet.

// TINY, the PS2's own default (Ps2CoreTuning.h). GameSettingsBackend_3DS pins
// both Cycle and Clamp to this value exactly like the PS2 backend, so this is
// the whole render-distance surface: what boots is what plays.
#undef  PLATFORM_DEFAULT_RENDER_DISTANCE
#define PLATFORM_DEFAULT_RENDER_DISTANCE          3

// The lever the renderer grid actually reads on this path. RenderGlobal sizes
// its grid from Config::getRenderDistanceFine(), GameSettings clamps that
// fine value to PLATFORM_VISIBLE_CHUNK_RADIUS * 16 (GameSettingsBackend_3DS),
// and EntityRenderer draws the fog edge at the same figure -- so radius 2
// pins grid, fog and the fine slider to 32 blocks, 5 columns wide
// (2*32/16+1). Coherent with TINY above: the coarse table maps TINY to
// 32 << (3 - 3) = 32 blocks.
#undef  PLATFORM_VISIBLE_CHUNK_RADIUS
#define PLATFORM_VISIBLE_CHUNK_RADIUS              2

// Moving vertical renderer window, the PS2's 5x3x5 idea at TINY proportions.
// The desktop branch of RenderGlobal::markRenderersForNewPosition() centers
// this window on the player generically (the guarded-edge variant is the
// PS2/WII one), so 5 sections = 80 blocks tall against TINY's 32-block
// horizontal reach and the grid drops from 5 x 8 x 5 = 200 slots to 125.
#undef  PLATFORM_VERTICAL_CHUNK_COUNT
#define PLATFORM_VERTICAL_CHUNK_COUNT              5
#undef  PLATFORM_CENTER_VERTICAL_RENDERERS
#define PLATFORM_CENTER_VERTICAL_RENDERERS         1

// Preload radius, in blocks. The desktop's 128 steps 16 blocks at a time over
// [-128, 128] on both axes -- 17 x 17 = 289 chunk columns generated
// SYNCHRONOUSLY on the loading screen (the Wii tuning header's own analysis
// of the same default). 32 keeps startup to 5 x 5 columns and streams the
// rest behind the mesh budget below.
#undef  PLATFORM_PRELOAD_RADIUS_BLOCKS
#define PLATFORM_PRELOAD_RADIUS_BLOCKS             32

// Mesh governor: one updateRenderers() step per frame under a wall-clock
// budget, instead of the vanilla retry loop that re-calls updateRenderers
// until the same total work is done -- the loop is the 9400 ms frame in the
// log once the ceiling above it is 99 updates with no clock. Both halves have
// to switch together; see the PLATFORM_MESH_BUDGET comment in
// PlatformGameTuning.h.
#undef  PLATFORM_MESH_BUDGET
#define PLATFORM_MESH_BUDGET                       1
#undef  PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME
#define PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME    8
#undef  PLATFORM_CHUNK_BUILD_BUDGET_MS
#define PLATFORM_CHUNK_BUILD_BUDGET_MS             5

// Edit-latency lane (Wii values): without it a block break queues behind the
// streaming budget above and the edited section only re-meshes once the
// player moves.
#undef  PLATFORM_URGENT_MESH_DISTANCE_SQ
#define PLATFORM_URGENT_MESH_DISTANCE_SQ           1024.0f
#undef  PLATFORM_URGENT_MESH_BUDGET_MS
#define PLATFORM_URGENT_MESH_BUDGET_MS             10

// Resident chunk cache: TINY is 5 columns wide (radius 2), and the rule the
// Wii header states is visible radius + 1 -- a WorldRenderer outside the
// cache meshes through an EmptyChunk and reads as a black terrain hole. The
// unload ring sits one chunk beyond so walking a boundary does not churn.
#undef  PLATFORM_CHUNK_CACHE_RADIUS
#define PLATFORM_CHUNK_CACHE_RADIUS                3
#undef  PLATFORM_CHUNK_UNLOAD_RADIUS
#define PLATFORM_CHUNK_UNLOAD_RADIUS                4
#undef  PLATFORM_CHUNK_MAP_RESERVE
#define PLATFORM_CHUNK_MAP_RESERVE                 81

// Entity simulation normally requires every chunk in a 32-block radius (5x5
// columns); the resident cache is 7x7, so keep one chunk of margin.
#undef  PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS
#define PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS  16

// Lighting, the Wii values: the desktop queue is effectively unbounded
// (1,000,000 jobs, 5-entry merge scan) and the flood fill is what makes a
// streamed chunk publish expensive. 3 ms of a ~33 ms frame with a 256-job
// backstop; the wide merge scan and hard cap keep the backlog from growing
// for as long as the player keeps walking.
#undef  PLATFORM_LIGHTING_UPDATES_PER_FRAME
#define PLATFORM_LIGHTING_UPDATES_PER_FRAME        256
#undef  PLATFORM_LIGHTING_INTERACTIVE_QUEUE_MAX
#define PLATFORM_LIGHTING_INTERACTIVE_QUEUE_MAX    256
#undef  PLATFORM_LIGHTING_INTERACTIVE_BURST
#define PLATFORM_LIGHTING_INTERACTIVE_BURST        2048
#undef  PLATFORM_LIGHTING_BUDGET_US
#define PLATFORM_LIGHTING_BUDGET_US                3000
#undef  PLATFORM_LIGHTING_MERGE_SCAN
#define PLATFORM_LIGHTING_MERGE_SCAN               96
#undef  PLATFORM_LIGHTING_QUEUE_HARD_CAP
#define PLATFORM_LIGHTING_QUEUE_HARD_CAP           8192

// Random ticks: 25 columns at radius 2 instead of the desktop's 81-chunk
// sweep, a quarter of the per-chunk probe rate, round-robin across 6 columns
// per world tick.
#undef  PLATFORM_RANDOM_TICK_CHUNK_RADIUS
#define PLATFORM_RANDOM_TICK_CHUNK_RADIUS          2
#undef  PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK
#define PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK      10
#undef  PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK
#define PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK       6

// Entity CPU guardrails (Wii values, radii scaled to TINY's 32-block reach):
// entities beyond the terrain distance stand on ground that is not drawn, so
// skip their model submission and their push scans; cap live mobs and A* work
// so a spawn pass cannot eat the tick.
#undef  PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE
#define PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE      1
#undef  PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS
#define PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS       32.0f
#undef  PLATFORM_LIMIT_ENTITY_PUSH_COLLISIONS
#define PLATFORM_LIMIT_ENTITY_PUSH_COLLISIONS      1
#undef  PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS
#define PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS 32.0f
#undef  PLATFORM_MAX_LIVE_MOBS
#define PLATFORM_MAX_LIVE_MOBS                     8
#undef  PLATFORM_PATHFIND_BUDGET_PER_TICK
#define PLATFORM_PATHFIND_BUDGET_PER_TICK          2
#undef  PLATFORM_PATHFIND_MAX_NODES
#define PLATFORM_PATHFIND_MAX_NODES                160
#undef  PLATFORM_MOB_SPAWN_INTERVAL_TICKS
#define PLATFORM_MOB_SPAWN_INTERVAL_TICKS          8
#undef  PLATFORM_MOB_SPAWN_Y_BAND
#define PLATFORM_MOB_SPAWN_Y_BAND                  24
