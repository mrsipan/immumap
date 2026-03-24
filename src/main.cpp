#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <variant>
#include <vector>
#include <bit>
#include <memory>
#include <sstream>

namespace py = pybind11;

// --- Data Structures ---
struct Node;
using NodePtr = std::shared_ptr<const Node>;
struct Leaf { py::object key; py::object value; };
using Entry = std::variant<Leaf, NodePtr>;

enum class NodeType { Bitmap, Collision };

struct Node {
    NodeType type;
    uint32_t bitmap = 0;
    std::vector<Entry> children;

    static NodePtr make_bitmap(uint32_t bmp, std::vector<Entry> ch) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Bitmap; n->bitmap = bmp; n->children = std::move(ch);
        return n;
    }

    static NodePtr make_collision(std::vector<Entry> ch) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Collision; n->children = std::move(ch);
        return n;
    }
};

// --- Recursive Helper ---
std::pair<NodePtr, bool> set_rec(NodePtr node, size_t hash, py::object key, py::object val, int shift) {
    if (node->type == NodeType::Collision) {
        auto next_ch = node->children;
        for (auto& entry : next_ch) {
            if (std::get<Leaf>(entry).key.equal(key)) {
                entry = Leaf{key, val};
                return {Node::make_collision(std::move(next_ch)), false};
            }
        }
        next_ch.push_back(Leaf{key, val});
        return {Node::make_collision(std::move(next_ch)), true};
    }

    uint32_t bit = 1u << ((hash >> shift) & 0x1f);
    uint32_t idx = std::popcount(node->bitmap & (bit - 1));

    if (node->bitmap & bit) {
        auto& target = node->children[idx];
        if (auto* leaf = std::get_if<Leaf>(&target)) {
            if (leaf->key.equal(key)) {
                auto next_ch = node->children;
                next_ch[idx] = Leaf{key, val};
                return {Node::make_bitmap(node->bitmap, std::move(next_ch)), false};
            } else {
                size_t old_hash = py::hash(leaf->key);
                if (old_hash == hash) return {Node::make_collision({*leaf, Leaf{key, val}}), true};
                auto [sub, _] = set_rec(Node::make_bitmap(0, {}), old_hash, leaf->key, leaf->value, shift + 5);
                auto [final_sub, added] = set_rec(sub, hash, key, val, shift + 5);
                auto next_ch = node->children;
                next_ch[idx] = final_sub;
                return {Node::make_bitmap(node->bitmap, std::move(next_ch)), true};
            }
        } else {
            auto [next_sub, added] = set_rec(std::get<NodePtr>(target), hash, key, val, shift + 5);
            auto next_ch = node->children;
            next_ch[idx] = next_sub;
            return {Node::make_bitmap(node->bitmap, std::move(next_ch)), added};
        }
    } else {
        auto next_ch = node->children;
        next_ch.insert(next_ch.begin() + idx, Leaf{key, val});
        return {Node::make_bitmap(node->bitmap | bit, std::move(next_ch)), true};
    }
}

// --- Map Class ---
class Map {
    NodePtr root;
    size_t _size;

public:
    struct Iter {
        std::vector<std::pair<NodePtr, size_t>> stack;
        Iter(NodePtr r) { if (r) stack.push_back({r, 0}); }
        py::object next() {
            while (!stack.empty()) {
                auto& [node, idx] = stack.back();
                if (idx >= node->children.size()) { stack.pop_back(); continue; }
                auto& entry = node->children[idx++];
                if (auto* leaf = std::get_if<Leaf>(&entry)) return leaf->key;
                stack.push_back({std::get<NodePtr>(entry), 0});
            }
            throw py::stop_iteration();
        }
    };

    Map() : root(Node::make_bitmap(0, {})), _size(0) {}
    Map(NodePtr r, size_t s) : root(r), _size(s) {}

    NodePtr get_root() const { return root; }
    size_t size() const { return _size; }

    // Internal find logic that returns a pointer (safe)
    const py::object* find(py::object key) const {
        size_t h = py::hash(key);
        int shift = 0;
        NodePtr curr = root;
        while (curr) {
            if (curr->type == NodeType::Collision) {
                for (auto& e : curr->children)
                    if (std::get<Leaf>(e).key.equal(key)) return &std::get<Leaf>(e).value;
                return nullptr;
            }
            uint32_t bit = 1u << ((h >> shift) & 0x1f);
            if (!(curr->bitmap & bit)) return nullptr;
            auto& entry = curr->children[std::popcount(curr->bitmap & (bit - 1))];
            if (auto* leaf = std::get_if<Leaf>(&entry)) {
                if (leaf->key.equal(key)) return &leaf->value;
                return nullptr;
            }
            curr = std::get<NodePtr>(entry);
            shift += 5;
        }
        return nullptr;
    }

    std::shared_ptr<Map> set(py::object k, py::object v) const {
        auto res = set_rec(root, py::hash(k), k, v, 0);
        return std::make_shared<Map>(res.first, _size + (res.second ? 1 : 0));
    }

    std::string repr() const {
        std::stringstream ss;
        ss << "Map({";
        Iter it(root);
        bool first = true;
        try {
            while (true) {
                py::object k = it.next();
                const py::object* v = find(k);
                if (!first) ss << ", ";
                ss << py::repr(k) << ": " << py::repr(*v);
                first = false;
            }
        } catch (const py::stop_iteration&) {}
        ss << "})";
        return ss.str();
    }
};

// --- Module ---
PYBIND11_MODULE(_core, m) {
    py::class_<Map::Iter>(m, "MapIterator")
        .def("__next__", &Map::Iter::next)
        .def("__iter__", [](Map::Iter &it) -> Map::Iter& { return it; });

    py::class_<Map, std::shared_ptr<Map>>(m, "Map")
        .def(py::init<>())
        .def("set", &Map::set)
        .def("get", [](const Map& m, py::object key, py::object def_val) {
            const py::object* val = m.find(key);
            return val ? *val : def_val;
        }, py::arg("key"), py::arg("default") = py::none())
        .def("__getitem__", [](const Map& m, py::object key) {
            const py::object* val = m.find(key);
            if (!val) throw py::key_error(py::str(key));
            return *val;
        })
        .def("__contains__", [](const Map& m, py::object key) {
            return m.find(key) != nullptr;
        })
        .def("__len__", &Map::size)
        .def("__repr__", &Map::repr)
        .def("__iter__", [](const Map& m) { return Map::Iter(m.get_root()); }, py::keep_alive<0, 1>());
}
