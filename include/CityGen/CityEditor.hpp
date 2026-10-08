#pragma once

#include <cstdint>
#include <vector>

#include "City.hpp"

namespace city {

    enum class CityTool : int { Select = 0, PaintBlock = 1, InsertBuilding = 2, DrawRoad = 3, ElevateRoad = 4, Districts = 5 };

    // Selection/drag state used by the viewport node editor and the ImGui panel.
    struct CityEditorState {
        City* activeCity = nullptr;
        int selectedNode = -1;
        int selectedEdge = -1;
        int hoveredNode = -1;
        uint64_t selectedBlockId = 0;    // 0 = none; building overrides are keyed by (blockId, slot)
        int selectedBuildingSlot = -1;

        // Viewport node dragging.
        bool draggingNode = false;
        int dragNode = -1;
        Vector2 dragStartNodePos{};
        bool dragMoved = false;
        bool dragBlocked = false;   // last drag/nudge step was stopped to keep roads from crossing

        // Active tool + block-paint state.
        CityTool tool = CityTool::Select;
        BlockKind paintKind = BlockKind::Park;
        int hoveredBlock = -1;        // index into City::GetBlocks() under the cursor (paint tool)
        bool paintUndoPushed = false;
        bool painting = false;        // left button held while painting (one undo step per stroke)

        // Road draw tool: click (drag to bend) to start, click to place the end;
        // the end then becomes the next start (chained). Esc/right-click stops.
        bool roadActive = false;       // a start point is placed
        bool roadDragging = false;     // start press held: dragging out the tangent handle
        bool roadHasHandle = false;
        bool roadErase = false;        // delete-road mode (Ctrl also does this)
        Vector2 roadStart{};
        Vector2 roadHandle{};          // quadratic Bezier control point
        Vector2 roadSnapPos{};         // cursor snapped to a node/road
        bool roadSnapValid = false;
        bool roadPreviewOk = false;
        std::vector<Vector2> roadPreview; // sampled curve to the cursor
        float roadHeight = 0.0f;       // height of the point being placed (PgUp/PgDn); a snapped node keeps its own
        float roadEndH = 0.0f;         // height at the cursor end (snapped node's own height, else roadHeight)
        float roadStartH = 0.0f;       // height at the placed start point (for the preview ramp)

        // Elevate tool: drag a node up/down to change its road height.
        bool elevDragging = false;
        int elevNode = -1;
        float elevStartH = 0.0f;
        float elevMouseY = 0.0f;
        bool elevMoved = false;
        float gradePercent = 4.0f;     // "Set grade" value in the road panel

        // Building insert tool.
        int insertShape = 0;            // kBuildingBox / kBuildingGable / kBuildingTower
        bool insertNoCollision = false; // bypass placement rules: place anywhere, even on roads/overlapping
        bool insertSnap = true;        // snap to the nearest road (Alt inverts while held)
        float insertW = 11.0f, insertD = 11.0f, insertH = 13.6f;
        float insertAngle = 0.0f;      // free-placement rotation (Q/E)
        PlacedBuilding insertPreview{};
        bool insertHasPreview = false;
        bool insertPreviewOk = false;

        // District tool: click a marker to select/drag it, click ground to place a new one, or paint blocks into the selected district.
        int districtSel = -1;
        bool districtDragging = false;
        bool districtPaintMode = false;   // false = move/place markers, true = paint blocks into the selected district
        int districtNewKind = 0;          // DistrictKind for newly placed districts
        float districtAutoPeak = 3.0f;
        Vector2 districtDragLast{};
        Vector2 districtDragOffset{};     // marker position minus the grabbed point on the drag plane

        // Keyboard nudge step (Shift scales it down for fine control).
        float nudgeStep = 1.0f;

        // ImGui panel.
        bool panelOpen = false;
        CityParams prevParams{}; // last snapshot of the active city's params
    };

    CityEditorState& GetCityEditorState();
    void InitCityEditor();

    void SetActiveCity(City* c);
    bool IsCityEditorActive();

    // Runs every interaction-manager update BEFORE object picking. Returns true
    // when the viewport left-button down/press should be consumed by the editor
    // (node grabbed, city selected, road click) so it never leaks into picking.
    bool UpdateCityEditor(Engine& engine, Camera3D& camera);

    // ImGui panel drawn by ui::DrawImGuiFrame.
    void DrawCityEditorPanel();

} // namespace city