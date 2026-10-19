#include "../../../include/Engine/Backend/UndoSystem.hpp"

#include <utility>

void UndoSystem::Push(std::string snapshot, std::vector<int> selection) {
    undoStack.push_back(Entry{ std::move(snapshot), std::move(selection) });
    Trim(undoStack);
    redoStack.clear();
}

void UndoSystem::PushRedo(std::string snapshot, std::vector<int> selection) {
    redoStack.push_back(Entry{ std::move(snapshot), std::move(selection) });
    Trim(redoStack);
}

void UndoSystem::PushUndo(std::string snapshot, std::vector<int> selection) {
    undoStack.push_back(Entry{ std::move(snapshot), std::move(selection) });
    Trim(undoStack);
}

// Drops the oldest entries until the stack fits the entry and byte caps (the newest entry always stays).
void UndoSystem::Trim(std::deque<Entry>& stack) {
    size_t bytes = 0;
    for (const Entry& e : stack) bytes += e.bytes.size();
    while (stack.size() > kMaxEntries || (stack.size() > 1 && bytes > kMaxBytes)) {
        bytes -= stack.front().bytes.size();
        stack.pop_front();
    }
}

void UndoSystem::Clear() {
    undoStack.clear();
    redoStack.clear();
}
