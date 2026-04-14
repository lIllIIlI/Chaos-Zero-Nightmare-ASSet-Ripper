#include "SpineDictionary.h"
#include "DataPack.h"
#include "SCSPParser.h"
#include <algorithm>
#include <sstream>

namespace {
    std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    std::string get_extension(const std::string& name) {
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return "";
        return to_lower(name.substr(dot));
    }

    std::string strip_extension(const std::string& name) {
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return name;
        return name.substr(0, dot);
    }

    std::string get_directory(const std::string& path) {
        size_t sep = path.find_last_of('/');
        if (sep == std::string::npos) return "";
        return path.substr(0, sep);
    }

    std::string get_basename(const std::string& path) {
        size_t sep = path.find_last_of('/');
        if (sep == std::string::npos) return path;
        return path.substr(sep + 1);
    }

    std::string get_first_component(const std::string& path) {
        size_t sep = path.find('/');
        if (sep == std::string::npos) return path;
        return path.substr(0, sep);
    }

    std::string normalize_path(const std::string& path) {
        std::string result = path;
        for (auto& c : result) {
            if (c == '\\') c = '/';
        }
        return result;
    }
}

void SpineDictionary::Clear() {
    entries.clear();
    categories.clear();
    scsp_files.clear();
    atlas_files.clear();
    all_files.clear();
    built = false;
}

void SpineDictionary::CollectFiles(const Core::FileNode& node) {
    if (std::holds_alternative<Core::FileInfo>(node.data)) {
        std::string path = normalize_path(node.full_path);
        std::string ext = get_extension(node.name);
        all_files[path] = &node;

        if (ext == ".scsp") {
            scsp_files[path] = &node;
        } else if (ext == ".atlas") {
            atlas_files[path] = &node;
        }
    } else {
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        for (const auto& child : folder.children) {
            CollectFiles(child);
        }
    }
}

const Core::FileNode* SpineDictionary::FindSiblingByName(const std::string& scsp_path, const std::string& filename) {
    std::string dir = get_directory(scsp_path);
    std::string target = dir.empty() ? filename : dir + "/" + filename;
    auto it = all_files.find(normalize_path(target));
    if (it != all_files.end()) return it->second;
    return nullptr;
}

std::vector<std::string> SpineDictionary::ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data) {
    std::vector<std::string> textures;
    std::string content(atlas_data.begin(), atlas_data.end());
    std::istringstream ss(content);
    std::string line;
    bool after_blank = true;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty()) {
            after_blank = true;
            continue;
        }

        if (after_blank && !line.empty() && line[0] != ' ' && line[0] != '\t') {
            if (line.find('.') != std::string::npos) {
                textures.push_back(line);
            }
        }
        after_blank = false;
    }

    return textures;
}

void SpineDictionary::MatchEntries(DataPack& pack) {
    for (auto& [scsp_path, scsp_node] : scsp_files) {
        SpineEntry entry;
        entry.name = strip_extension(scsp_node->name);
        entry.scsp_node = scsp_node;
        entry.atlas_node = nullptr;

        // Use parent folder as display name, first component as category
        std::string norm_path = normalize_path(scsp_node->full_path);
        std::string dir = get_directory(norm_path);
        if (!dir.empty()) {
            entry.display_name = get_basename(dir); // parent folder name
            entry.category = get_first_component(norm_path);
        } else {
            entry.display_name = entry.name; // fallback to filename
            entry.category = "";
        }

        // Try to extract header info
        try {
            std::vector<uint8_t> data = pack.GetFileData(*scsp_node);
            if (!data.empty()) {
                SCSPParser::HeaderInfo hdr = SCSPParser::ExtractHeader(data);
                entry.width = hdr.width;
                entry.height = hdr.height;
                entry.version = hdr.version;
                entry.images_path = hdr.images_path;
                entry.hash = hdr.hash;
            }
        } catch (...) {}

        // Find matching atlas: same name in same directory
        std::string base_name = strip_extension(scsp_node->name);

        std::string atlas_candidate = dir.empty() ? base_name + ".atlas" : dir + "/" + base_name + ".atlas";
        auto atlas_it = atlas_files.find(normalize_path(atlas_candidate));
        if (atlas_it != atlas_files.end()) {
            entry.atlas_node = atlas_it->second;
        }

        // If not found, check if there's exactly one .atlas in the same directory
        if (!entry.atlas_node) {
            std::vector<const Core::FileNode*> dir_atlases;
            for (auto& [apath, anode] : atlas_files) {
                if (get_directory(apath) == dir) {
                    dir_atlases.push_back(anode);
                }
            }
            if (dir_atlases.size() == 1) {
                entry.atlas_node = dir_atlases[0];
            }
        }

        // Parse atlas to find texture image references
        if (entry.atlas_node) {
            try {
                std::vector<uint8_t> atlas_data = pack.GetFileData(*entry.atlas_node);
                if (!atlas_data.empty()) {
                    auto tex_names = ParseAtlasTextureNames(atlas_data);
                    for (const auto& tex_name : tex_names) {
                        const Core::FileNode* img = FindSiblingByName(
                            normalize_path(entry.atlas_node->full_path), tex_name);
                        if (img) {
                            entry.image_nodes.push_back(img);
                        }

                        if (!img && !entry.images_path.empty()) {
                            std::string img_path = normalize_path(entry.images_path);
                            if (!img_path.empty() && img_path.back() != '/') img_path += "/";
                            img_path += tex_name;
                            std::string scsp_dir = get_directory(scsp_path);
                            std::string full_img = scsp_dir.empty() ? img_path : scsp_dir + "/" + img_path;
                            auto it2 = all_files.find(normalize_path(full_img));
                            if (it2 != all_files.end()) {
                                entry.image_nodes.push_back(it2->second);
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        // If no atlas but we have images_path, try to find images directly
        if (entry.image_nodes.empty() && !entry.images_path.empty()) {
            std::string scsp_dir = get_directory(scsp_path);
            std::string img_dir = normalize_path(entry.images_path);
            std::string search_dir = scsp_dir.empty() ? img_dir : scsp_dir + "/" + img_dir;
            if (!search_dir.empty() && search_dir.back() == '/') search_dir.pop_back();

            for (auto& [fpath, fnode] : all_files) {
                if (get_directory(fpath) == search_dir) {
                    std::string ext = get_extension(fnode->name);
                    if (ext == ".sct" || ext == ".sct2" || ext == ".png" || ext == ".jpg") {
                        entry.image_nodes.push_back(fnode);
                    }
                }
            }
        }

        entries.push_back(std::move(entry));
    }

    // Sort entries by display_name
    std::sort(entries.begin(), entries.end(),
        [](const SpineEntry& a, const SpineEntry& b) { return a.display_name < b.display_name; });
}

void SpineDictionary::BuildCategories() {
    categories.clear();
    std::map<std::string, size_t> cat_map; // category name -> index in categories vector

    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& cat = entries[i].category;
        std::string cat_name = cat.empty() ? "(root)" : cat;
        auto it = cat_map.find(cat_name);
        if (it == cat_map.end()) {
            cat_map[cat_name] = categories.size();
            SpineCategory sc;
            sc.name = cat_name;
            sc.entry_indices.push_back(i);
            categories.push_back(std::move(sc));
        } else {
            categories[it->second].entry_indices.push_back(i);
        }
    }

    std::sort(categories.begin(), categories.end(),
        [](const SpineCategory& a, const SpineCategory& b) { return a.name < b.name; });
}

void SpineDictionary::Build(DataPack& pack, const Core::FileNode& root) {
    Clear();
    CollectFiles(root);
    MatchEntries(pack);
    BuildCategories();
    built = true;
}
