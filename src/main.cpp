#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <variant>
#include <vector>
#include <bit>
#include <memory>
#include <sstream>
#include <iomanip>

namespace py = pybind11;

// 1. Forward Declarations
struct Node;
using NodePtr = std::shared_ptr<const Node>;
struct Leaf { py::object key; py::object value; };
using Entry = std::variant<Leaf, NodePtr>;

enum class NodeType { Bitmap, Collision };

// 2. The Node Structure
struct Node {
    NodeType type;
    uint32_t bitmap = 0;
    std::vector<Entry> children;
    bool frozen = true;

    static NodePtr make_bitmap(uint32_t bmp, std::vector<Entry> ch, bool f = true) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Bitmap; n->bitmap = bmp; n->children = std::move(ch); n->frozen = f;
        return n;
    }

    static NodePtr make_collision(std::vector<Entry> ch, bool f = true) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Collision; n->children = std::move(ch); n->frozen = f;
        return n;
    }
};

// 3. Helper for JSON values
std::string py_to_json(py::handle obj) {
    if (obj.is_none()) return "null";
    if (py::isinstance<py::bool_>(obj)) return obj.cast<bool>() ? "true" : "false";
    if (py::isinstance<py::str>(obj)) {
        std::stringstream ss;
        ss << std::quoted(obj.cast<std::string>());
        return ss.str();
    }
    if (py::isinstance<py::int_>(obj) || py::isinstance<py::float_>(obj)) {
        return py::str(obj).cast<std::string>();
    }
    auto json_module = py::module_::import("json");
    return json_module.attr("dumps")(obj).cast<std::string>();
}

// 4. Core Recursive Logic
std::pair<NodePtr, bool> set_rec(NodePtr node, size_t hash, py::object key, py::object val, int shift, bool mutation) {
    std::shared_ptr<Node> target;
    if (!node->frozen) {
        target = std::const_pointer_cast<Node>(node);
    } else {
        target = std::make_shared<Node>(*node);
        target->frozen = !mutation;
    }

    if (target->type == NodeType::Collision) {
        for (auto& entry : target->children) {
            if (std::get<Leaf>(entry).key.equal(key)) {
                entry = Leaf{key, val};
                return {target, false};
            }
        }
        target->children.push_back(Leaf{key, val});
        return {target, true};
    }

    uint32_t bit = 1u << ((hash >> shift) & 0x1f);
    uint32_t idx = std::popcount(target->bitmap & (bit - 1));

    if (target->bitmap & bit) {
        auto& child_entry = target->children[idx];
        if (auto* leaf = std::get_if<Leaf>(&child_entry)) {
            if (leaf->key.equal(key)) {
                child_entry = Leaf{key, val};
                return {target, false};
            } else {
                size_t old_hash = py::hash(leaf->key);
                if (old_hash == hash) {
                    child_entry = Node::make_collision({*leaf, Leaf{key, val}}, !mutation);
                    return {target, true};
                }
                auto sub_empty = Node::make_bitmap(0, {}, !mutation);
                auto [s1, _] = set_rec(sub_empty, old_hash, leaf->key, leaf->value, shift + 5, mutation);
                auto [s2, added] = set_rec(s1, hash, key, val, shift + 5, mutation);
                child_entry = s2;
                return {target, true};
            }
        } else {
            auto [next_sub, added] = set_rec(std::get<NodePtr>(child_entry), hash, key, val, shift + 5, mutation);
            child_entry = next_sub;
            return {target, added};
        }
    } else {
        target->bitmap |= bit;
        target->children.insert(target->children.begin() + idx, Leaf{key, val});
        return {target, true};
    }
}

// 5. Map Class Forward Decl
class Map;

// 6. Mutation Class
class MapMutation {
    NodePtr root;
    size_t _size;
    bool active;
public:
    MapMutation(NodePtr r, size_t s) : root(r), _size(s), active(true) {
        auto clone = std::make_shared<Node>(*root);
        clone->frozen = false;
        root = clone;
    }
    void set(py::object k, py::object v) {
        if (!active) throw py::value_error("Mutation finished");
        auto res = set_rec(root, py::hash(k), k, v, 0, true);
        root = res.first;
        if (res.second) _size++;
    }
    std::shared_ptr<Map> finish();
};

// 7. Final Map Class
class Map : public std::enable_shared_from_this<Map> {
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

    const py::object* find(py::object key) const {
        size_t h = py::hash(key);
        int shift = 0;
        NodePtr curr = root;
        while (curr) {
            if (curr->type == NodeType::Collision) {
                for (auto& e : curr->children) if (std::get<Leaf>(e).key.equal(key)) return &std::get<Leaf>(e).value;
                return nullptr;
            }
            uint32_t bit = 1u << ((h >> shift) & 0x1f);
            if (!(curr->bitmap & bit)) return nullptr;
            auto& entry = curr->children[std::popcount(curr->bitmap & (bit - 1))];
            if (auto* leaf = std::get_if<Leaf>(&entry)) return leaf->key.equal(key) ? &leaf->value : nullptr;
            curr = std::get<NodePtr>(entry);
            shift += 5;
        }
        return nullptr;
    }

    std::shared_ptr<Map> update(py::dict other) const {
        auto mut = std::make_shared<MapMutation>(root, _size);
        for (auto item : other) {
            mut->set(py::reinterpret_borrow<py::object>(item.first),
                     py::reinterpret_borrow<py::object>(item.second));
        }
        return mut->finish();
    }

    std::shared_ptr<Map> set(py::object k, py::object v) const {
        auto res = set_rec(root, py::hash(k), k, v, 0, false);
        return std::make_shared<Map>(res.first, _size + (res.second ? 1 : 0));
    }

    std::string to_json() const {
        std::stringstream ss; ss << "{";
        Iter it(root); bool first = true;
        try {
            while (true) {
                py::object k = it.next(); const py::object* v = find(k);
                if (!first) ss << ",";
                if (py::isinstance<py::str>(k)) ss << std::quoted(k.cast<std::string>());
                else ss << std::quoted(py::str(k).cast<std::string>());
                ss << ":" << py_to_json(*v); first = false;
            }
        } catch (const py::stop_iteration&) {}
        ss << "}"; return ss.str();
    }

    std::string repr() const {
        std::stringstream ss; ss << "Map({";
        Iter it(root); bool first = true;
        try {
            while (true) {
                py::object k = it.next(); const py::object* v = find(k);
                if (!first) ss << ", ";
                ss << py::repr(k) << ": " << py::repr(*v); first = false;
            }
        } catch (const py::stop_iteration&) {}
        ss << "})"; return ss.str();
    }
};

// 8. Definition for MapMutation::finish
std::shared_ptr<Map> MapMutation::finish() {
    active = false;
    return std::make_shared<Map>(root, _size);
}

// 9. Pybind11 Module Block
PYBIND11_MODULE(_core, m) {
    py::class_<Map::Iter>(m, "MapIterator")
        .def("__next__", &Map::Iter::next)
        .def("__iter__", [](Map::Iter &it) -> Map::Iter& { return it; });

    py::class_<MapMutation, std::shared_ptr<MapMutation>>(m, "MapMutation")
        .def("set", &MapMutation::set)
        .def("finish", &MapMutation::finish);

    py::class_<Map, std::shared_ptr<Map>>(m, "Map")
        .def(py::init<>())
        .def("set", &Map::set)
        .def("update", &Map::update)
        .def("to_json", &Map::to_json)
        .def("mutate", [](std::shared_ptr<Map> self) { return std::make_shared<MapMutation>(self->get_root(), self->size()); })
        .def("get", [](const Map& m, py::object k, py::object d) { const py::object* v = m.find(k); return v ? *v : d; }, py::arg("key"), py::arg("default") = py::none())
        .def("__getitem__", [](const Map& m, py::object k) { const py::object* v = m.find(k); if (!v) throw py::key_error(py::str(k)); return *v; })
        .def("__contains__", [](const Map& m, py::object k) { return m.find(k) != nullptr; })
        .def("__len__", &Map::size)
        .def("__repr__", &Map::repr)
        .def("__iter__", [](const Map& m) { return Map::Iter(m.get_root()); }, py::keep_alive<0, 1>());
}
