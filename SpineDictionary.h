#pragma once
#include "Core.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

class DataPack;

struct SpineEntry {
    std::string name;                    // skeleton name (from .scsp filename, minus extension)
    const Core::FileNode* scsp_node;     // the .scsp skeleton file
    const Core::FileNode* atlas_node;    // the matching .atlas file (if found)
    std::vector<const Core::FileNode*> image_nodes; // texture images referenced by atlas

    // Header info extracted from .scsp
    float width = 0;
    float height = 0;
    std::string version;
    std::string images_path;
    std::string hash;
    int bone_count = 0;
    int slot_count = 0;
    int skin_count = 0;
    int animation_count = 0;
};

class SpineDictionary {
public:
    void Build(DataPack& pack, const Core::FileNode& root);
    void Clear();

    const std::vector<SpineEntry>& GetEntries() const { return entries; }
    bool IsBuilt() const { return built; }

private:
    void CollectFiles(const Core::FileNode& node);
    void MatchEntries(DataPack& pack);
    const Core::FileNode* FindFileInTree(const Core::FileNode& root, const std::string& relative_path);
    std::vector<std::string> ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data);
    const Core::FileNode* FindSiblingByName(const std::string& scsp_path, const std::string& filename);

    std::vector<SpineEntry> entries;
    bool built = false;

    // Lookup tables built during scan
    std::map<std::string, const Core::FileNode*> scsp_files;  // full_path -> node
    std::map<std::string, const Core::FileNode*> atlas_files;
    std::map<std::string, const Core::FileNode*> all_files;    // for resolving image paths
};
