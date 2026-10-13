#ifndef __GLTF_SCENE_H__
#define __GLTF_SCENE_H__

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "json/json.hpp"

// A binary glTF (.glb) read into world-space nodes (src/gltf_scene.cpp): every node of the default scene with its name,
// world matrix and, for a mesh node, its triangles transformed to world space. Units and axes are left as glTF has them
// (meters, y up, right-handed).
namespace rush2::gltf {
    struct Primitive {
        std::vector<std::array<float, 3>> pos, nrm;   // world space; nrm is unit length ({0, 1, 0} when the mesh has none)
        std::vector<std::array<float, 2>> uv;          // TEXCOORD_0, or 0
        std::vector<std::array<float, 4>> col;         // COLOR_0 (0-1) when the mesh has it, else empty
        std::vector<uint32_t> tris;                    // 3 per triangle, counter-clockwise from the front (a mirroring
                                                       // node matrix is undone)
        int material = -1;
    };

    struct Node {
        std::string name;
        int parent = -1;
        std::array<float, 16> world{};                 // column-major
        std::vector<Primitive> primitives;             // mesh nodes only
        size_t loose_edges = 0;                        // line primitives (mode 1-3) skipped
        nlohmann::json extras;                         // the node's custom properties, then its mesh's
        // Translation, and the world matrix's columns (axes with scale).
        std::array<float, 3> position() const { return { world[12], world[13], world[14] }; }
        std::array<float, 3> axis(int a) const { return { world[a * 4], world[a * 4 + 1], world[a * 4 + 2] }; }
    };

    struct Material {
        std::string name;
        int image = -1;                                // base color texture's image, or -1
        std::array<float, 4> color = { 1, 1, 1, 1 };   // baseColorFactor
        std::string alpha_mode = "OPAQUE";             // OPAQUE, MASK or BLEND
        float alpha_cutoff = 0.5f;
        bool double_sided = false;
        int wrap_s = 10497, wrap_t = 10497;            // the texture's sampler (10497 repeat, 33648 mirror, 33071 clamp)
    };

    struct Image {
        std::string name, mime;
        std::vector<uint8_t> data;                     // encoded (PNG, JPEG)
    };

    struct Scene {
        std::string generator;
        std::vector<Node> nodes;                       // in glTF node order (nodes outside the scene are left out)
        std::vector<Material> materials;
        std::vector<Image> images;
    };

    // False with a message in error when bytes isn't a usable .glb.
    bool read_glb(const std::vector<uint8_t>& bytes, Scene& scene, std::string& error);
}

#endif
