#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// Tracks undo/redo history for core scene edits (spawn, delete, duplicate,
// transform, grouping). A "snapshot" is a serialized copy of the scene's
// objects + model groups, captured before each edit and pushed once the edit
// is committed. Each snapshot also remembers the selected objects (as indices into the
// scene's object list, which a snapshot restores in the same order) so undo/redo can restore the selection.
// History is capped (kMaxEntries entries, kMaxBytes of snapshots); the oldest steps are dropped first.
class UndoSystem {
public:
    // Records `snapshot` as the new undo step, clearing any redo history.
    // `selection` records which objects were selected at that point.
    void Push(std::string snapshot, std::vector<int> selection);

    // Records `snapshot` onto the *redo* stack without touching the undo stack.
    // Used when a unit edits pushes its pre-edit state for redo.
    void PushRedo(std::string snapshot, std::vector<int> selection);

    // Records `snapshot` onto the *undo* stack without clearing the redo stack.
    // Used by redo to save the current state so it can be undone back to later.
    void PushUndo(std::string snapshot, std::vector<int> selection);

    // Pop the top undo step and push the current one onto redo.
    bool CanUndo() const { return !undoStack.empty(); }
    bool CanRedo() const { return !redoStack.empty(); }

    // Returns the snapshot to restore. For Undo the returned snapshot is the
    // top of the undo stack (the state *before* the edit to revert); for Redo
    // it is the top of the redo stack. Caller must first push the *current*
    // scene onto the opposite stack before restoring the returned snapshot.
    const std::string& PeekUndo() const { return undoStack.back().bytes; }
    const std::string& PeekRedo() const { return redoStack.back().bytes; }
    const std::vector<int>& UndoSel() const { return undoStack.back().selection; }
    const std::vector<int>& RedoSel() const { return redoStack.back().selection; }

    void PopUndo() { undoStack.pop_back(); }
    void PopRedo() { redoStack.pop_back(); }

    // Reset all history (e.g. after opening a scene).
    void Clear();

private:
    struct Entry {
        std::string bytes;
        std::vector<int> selection;
    };
    static constexpr size_t kMaxEntries = 100;
    static constexpr size_t kMaxBytes = 256u * 1024u * 1024u;
    void Trim(std::deque<Entry>& stack);
    std::deque<Entry> undoStack;
    std::deque<Entry> redoStack;
};
