#pragma once

#include "Terrain.hpp"
#include "TerrainTypes.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace terrain {

// Central, ordered registry of every terrain::Terrain currently in the scene.
//
// It owns the single terrain.terrain file format:
//
//     TerrainFileHeader { magic "FLYTERR1", version, terrainCount }
//     for each terrain, in slot order (1-based):
//         TerrainBlockHeader { mark "TER", terrainIndex, nameLen, dataSize }
//         name bytes (UTF-8)          <- authoritative Explorer name
//         data bytes                  <- Terrain::WriteToStream() payload
//
// The TERRAIN# slot is PURE ordering metadata. When a terrain is deleted, all
// later slots renumber downward (TERRAIN3 -> TERRAIN2 ...) but the Explorer
// name inside each block never changes, so Flyscript/physics/editor references
// (which all key off the Explorer name) remain valid. The file is regenerated
// with the new numbering whenever the set of terrains changes.
class TerrainRegistry {
public:
    // Add a terrain to the end of the slot order.
    void Register(Terrain* terrain) {
        if (!terrain) return;
        if (std::find(terrains.begin(), terrains.end(), terrain) != terrains.end()) return;
        terrains.push_back(terrain);
    }

    // Remove a terrain and renumber all later slots downward. Explorer names
    // are untouched, so references stay stable.
    void Unregister(Terrain* terrain) {
        terrains.erase(std::remove(terrains.begin(), terrains.end(), terrain), terrains.end());
    }

    bool Contains(Terrain* terrain) const {
        return std::find(terrains.begin(), terrains.end(), terrain) != terrains.end();
    }

    const std::vector<Terrain*>& GetTerrains() const { return terrains; }
    std::vector<Terrain*>& GetTerrains() { return terrains; }
    size_t Count() const { return terrains.size(); }

    // 1-based slot of a terrain (TERRAIN#), or 0 if not registered.
    int SlotOf(Terrain* terrain) const {
        auto it = std::find(terrains.begin(), terrains.end(), terrain);
        return it == terrains.end() ? 0 : (int)(it - terrains.begin()) + 1;
    }

    // Authoritative lookup by Explorer name (what Flyscript/associations use).
    Terrain* GetByName(const std::string& name) const {
        for (auto* t : terrains) {
            if (t && t->GetName() == name) return t;
        }
        return nullptr;
    }

    bool NameExists(const std::string& name) const {
        return GetByName(name) != nullptr;
    }

    // Produce a name that doesn't collide with any registered terrain (or any
    // optional extra "taken" names). Used to block duplicates on insert/rename.
    std::string MakeUniqueName(const std::string& base,
                               const std::vector<std::string>* extraTaken = nullptr) const {
        auto taken = [&](const std::string& n) {
            if (NameExists(n)) return true;
            if (extraTaken) {
                for (const auto& e : *extraTaken) if (e == n) return true;
            }
            return false;
        };
        if (!taken(base)) return base;
        for (int i = 2; i < 100000; i++) {
            std::string candidate = base + " (" + std::to_string(i) + ")";
            if (!taken(candidate)) return candidate;
        }
        return base + " (" + std::to_string(rand()) + ")";
    }

    // ---------------------------------------------------------------------
    // terrain.terrain file I/O
    // ---------------------------------------------------------------------

    // Write every registered terrain to `path` as a series of TERRAIN# blocks.
    bool WriteFile(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        TerrainFileHeader fileHeader;
        fileHeader.terrainCount = (uint32_t)terrains.size();
        out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));

        for (size_t i = 0; i < terrains.size(); i++) {
            const Terrain* t = terrains[i];
            if (!t) continue;

            const std::string& name = t->GetName();
            std::ostringstream payload(std::ios::binary);
            if (!t->WriteToStream(payload)) return false;
            std::string data = payload.str();

            TerrainBlockHeader bh;
            bh.terrainIndex = (uint32_t)(i + 1);
            bh.nameLen = (uint32_t)name.size();
            bh.dataSize = (uint32_t)data.size();

            out.write(reinterpret_cast<const char*>(&bh), sizeof(bh));
            out.write(name.data(), (std::streamsize)name.size());
            out.write(data.data(), (std::streamsize)data.size());
        }
        return out.good();
    }

    // Read `path` and rebuild every terrain through `create`, which must
    // construct, name, and add the terrain to the engine (returning the raw
    // pointer). The registry sets its Transform/layers/chunks from the file and
    // registers it into the slot order. Returns the number of terrains loaded.
    int ReadFile(const std::string& path, const std::function<Terrain*()>& create) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return 0;

        TerrainFileHeader fileHeader;
        in.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
        if (!in) return 0;
        if (memcmp(fileHeader.magic, "FLYTERR1", 8) != 0) return 0;

        int loaded = 0;
        for (uint32_t i = 0; i < fileHeader.terrainCount; i++) {
            TerrainBlockHeader bh;
            in.read(reinterpret_cast<char*>(&bh), sizeof(bh));
            if (!in) return loaded;

            std::string name(bh.nameLen, '\0');
            if (bh.nameLen) in.read(&name[0], (std::streamsize)bh.nameLen);

            std::string data(bh.dataSize, '\0');
            if (bh.dataSize) in.read(&data[0], (std::streamsize)bh.dataSize);
            if (!in) return loaded;

            Terrain* t = create();
            if (!t) return loaded;
            t->SetName(name);

            std::istringstream payload(data, std::ios::binary);
            // On read, chunk records are matched by (gx,gz), so the empty-name
            // is fine; blocks don't need an explicit order beyond slot framing.
            t->ReadFromStream(payload);

            Register(t);
            loaded++;
        }
        return loaded;
    }

    // Singleton accessor (engine has a single active registry).
    static TerrainRegistry& Get() {
        static TerrainRegistry instance;
        return instance;
    }

private:
    TerrainRegistry() = default;
    std::vector<Terrain*> terrains;
};

// Convenience free functions mirroring the registry singleton.
inline TerrainRegistry& GetTerrainRegistry() { return TerrainRegistry::Get(); }

} // namespace terrain
