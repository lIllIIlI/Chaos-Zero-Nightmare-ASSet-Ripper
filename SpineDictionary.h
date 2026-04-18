#pragma once
#include "Core.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

class DataPack;

struct SpineEntry {
    std::string name;                    // raw .scsp filename minus extension
    std::string display_name;            // parent folder name (e.g. "e_terra_underlab_1")
    std::string category;                // top-level folder (e.g. "spine")
    const Core::FileNode* scsp_node;     // the .scsp skeleton file
    const Core::FileNode* atlas_node;    // the matching .atlas file (if found)
    std::vector<const Core::FileNode*> image_nodes; // texture images referenced by atlas

    // Header info extracted from .scsp
    float width = 0;
    float height = 0;
    std::string version;
    std::string images_path;
    std::string hash;
};

struct SpineCategory {
    std::string name;
    std::vector<size_t> entry_indices; // indices into flat entries vector
};

class SpineDictionary {
public:
    void Build(DataPack& pack, const Core::FileNode& root);
    void Clear();

    const std::vector<SpineEntry>& GetEntries() const { return entries; }
    const std::vector<SpineCategory>& GetCategories() const { return categories; }
    bool IsBuilt() const { return built; }

private:
    void CollectFiles(const Core::FileNode& node);
    void MatchEntries(DataPack& pack);
    void BuildCategories();
    std::vector<std::string> ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data);
    const Core::FileNode* FindSiblingByName(const std::string& scsp_path, const std::string& filename);

    std::vector<SpineEntry> entries;
    std::vector<SpineCategory> categories;
    bool built = false;

    // Lookup tables built during scan
    std::map<std::string, const Core::FileNode*> scsp_files;
    std::map<std::string, const Core::FileNode*> atlas_files;
    std::map<std::string, const Core::FileNode*> all_files;
};
