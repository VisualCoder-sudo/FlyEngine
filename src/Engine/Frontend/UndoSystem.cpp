#include "../../../include/Engine/Backend/UndoSystem.hpp"

#include <utility>

void UndoSystem::Push(std::string snapshot, std::vector<std::string> selectionNames) {
    undoStack.push_back(Entry{ std::move(snapshot), std::move(selectionNames) });
    redoStack.clear();
}

void UndoSystem::PushRedo(std::string snapshot, std::vector<std::string> selectionNames) {
    redoStack.push_back(Entry{ std::move(snapshot), std::move(selectionNames) });
}

void UndoSystem::PushUndo(std::string snapshot, std::vector<std::string> selectionNames) {
    undoStack.push_back(Entry{ std::move(snapshot), std::move(selectionNames) });
}

void UndoSystem::Clear() {
    undoStack.clear();
    redoStack.clear();
}
