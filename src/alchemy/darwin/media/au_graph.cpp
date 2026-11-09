#include "alchemy/darwin/media/media.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// AUGraph (<AudioToolbox/AUGraph.h>) over the audio units Alchemy has (audio_units.cpp: Apple's three output
// units), and MusicDeviceMIDIEvent / MusicDeviceSysEx.
// - Nodes keep their component description; AUGraphOpen (or AUGraphAddNode on an open graph) makes each node's unit
//   with AudioComponentFindNext and AudioComponentInstanceNew. A description no component here matches (a DLS
//   synth, an effect, a mixer) fails the open with kAUGraphErr_InvalidAudioUnit and leaves the graph closed.
// - Connections and node input callbacks are kept, and made when the graph initializes (or at once on an
//   initialized graph): a connection is kAudioUnitProperty_MakeConnection on the destination unit (output units
//   take none, so the error it answers is the initialize's), a callback kAudioUnitProperty_SetRenderCallback on
//   the node's input. AUGraphInitialize then initializes every unit; AUGraphStart and AUGraphStop start and stop
//   the output node's unit (kAudioUnitType_Output). Changes take effect at once, so AUGraphUpdate has nothing left.
// - No unit here is a music device: MusicDeviceMIDIEvent and MusicDeviceSysEx answer badComponentSelector for an
//   instance (the call is not one it has) and badComponentInstance without one.
namespace alchemy::gui::fill {
namespace {

// AUGraph.h, AUComponent.h, MacErrors.h.
constexpr std::int32_t no_err = 0, param_err = -50, node_not_found = -10860, invalid_connection = -10861, output_node_err = -10862,
    cannot_do_in_current_context = -10863, invalid_audio_unit = -10864, bad_component_selector = static_cast<std::int32_t>(0x80008002U),
    bad_component_instance = static_cast<std::int32_t>(0x80008001U);
constexpr std::uint32_t output_type = fourcc("auou");   // kAudioUnitType_Output
constexpr std::uint32_t prop_make_connection = 1, prop_set_render_callback = 23, scope_input = 1;

using Description = std::array<std::uint8_t, 20>;   // AudioComponentDescription

struct Node {
    std::int32_t id{};
    Description description{};
    Address unit{};
};
struct Connection {
    std::int32_t source{};
    std::uint32_t source_bus{};
    std::int32_t destination{};
    std::uint32_t destination_bus{};
};
struct Callback {
    std::int32_t node{};
    std::uint32_t bus{};
    Address proc{}, refcon{};
};
struct Graph {
    std::recursive_mutex mutex;   // held across the audio unit calls, which may call back
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<Callback> callbacks;
    std::int32_t next{1};
    bool open{}, initialized{}, running{};
};

class AUGraphs final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<Graph> graph(Address handle) {
        std::lock_guard lock(mutex_);
        const auto found = graphs_.find(handle);
        return found == graphs_.end() ? nullptr : found->second;
    }
    static Node* node(Graph& g, std::int32_t id) {
        const auto found = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return n.id == id; });
        return found == g.nodes.end() ? nullptr : &*found;
    }
    std::int32_t make_unit(Node& n);
    void dispose_unit(Node& n);
    std::int32_t connect(Graph& g, const Connection& c);
    std::int32_t set_callback(Graph& g, const Callback& c);
    std::int32_t open(Graph& g);
    void close(Graph& g);
    std::int32_t initialize(Graph& g);
    void uninitialize(Graph& g);
    Node* output_node(Graph& g);

    std::mutex mutex_;
    std::map<Address, std::shared_ptr<Graph>> graphs_;
};

std::int32_t AUGraphs::make_unit(Node& n) {
    auto& m = process.calling_engine();
    const auto description = process.allocate_guest(20, 8), out = process.allocate_guest(8, 8);
    m.write_bytes(description, n.description);
    const auto component = call(audio_unit, "_AudioComponentFindNext", {0, description});
    std::int32_t result = invalid_audio_unit;
    if (component) {
        m.write64(out, 0);
        result = static_cast<std::int32_t>(call(audio_unit, "_AudioComponentInstanceNew", {component, out}));
        if (!result) n.unit = m.read64(out);
    }
    process.release_guest(description);
    process.release_guest(out);
    return result;
}

void AUGraphs::dispose_unit(Node& n) {
    if (const auto unit = std::exchange(n.unit, 0)) static_cast<void>(call(audio_unit, "_AudioComponentInstanceDispose", {unit}));
}

// kAudioUnitProperty_MakeConnection on the destination: AudioUnitConnection {sourceAudioUnit, sourceOutputNumber, destInputNumber}.
std::int32_t AUGraphs::connect(Graph& g, const Connection& c) {
    const auto* source = node(g, c.source);
    const auto* destination = node(g, c.destination);
    if (!source || !destination || !source->unit || !destination->unit) return invalid_connection;
    auto& m = process.calling_engine();
    const auto value = process.allocate_guest(16, 8);
    m.write64(value, source->unit);
    m.write32(value + 8, c.source_bus);
    m.write32(value + 12, c.destination_bus);
    const auto result = static_cast<std::int32_t>(call(audio_unit, "_AudioUnitSetProperty", {destination->unit, prop_make_connection, scope_input,
                                                                                              c.destination_bus, value, 16}));
    process.release_guest(value);
    return result;
}

// kAudioUnitProperty_SetRenderCallback on the node's input: AURenderCallbackStruct {inputProc, inputProcRefCon}.
std::int32_t AUGraphs::set_callback(Graph& g, const Callback& c) {
    const auto* n = node(g, c.node);
    if (!n || !n->unit) return node_not_found;
    auto& m = process.calling_engine();
    const auto value = process.allocate_guest(16, 8);
    m.write64(value, c.proc);
    m.write64(value + 8, c.refcon);
    const auto result = static_cast<std::int32_t>(call(audio_unit, "_AudioUnitSetProperty", {n->unit, prop_set_render_callback, scope_input, c.bus,
                                                                                              value, 16}));
    process.release_guest(value);
    return result;
}

std::int32_t AUGraphs::open(Graph& g) {
    if (g.open) return no_err;
    for (auto& n : g.nodes)
        if (const auto result = make_unit(n)) {
            for (auto& made : g.nodes) dispose_unit(made);
            return result;
        }
    g.open = true;
    return no_err;
}

void AUGraphs::uninitialize(Graph& g) {
    if (g.running)
        if (auto* out = output_node(g); out && out->unit) static_cast<void>(call(audio_unit, "_AudioOutputUnitStop", {out->unit}));
    g.running = false;
    if (!g.initialized) return;
    for (auto& n : g.nodes)
        if (n.unit) static_cast<void>(call(audio_unit, "_AudioUnitUninitialize", {n.unit}));
    g.initialized = false;
}

void AUGraphs::close(Graph& g) {
    uninitialize(g);
    for (auto& n : g.nodes) dispose_unit(n);
    g.open = false;
}

std::int32_t AUGraphs::initialize(Graph& g) {
    if (!g.open) return cannot_do_in_current_context;
    if (g.initialized) return no_err;
    for (const auto& c : g.connections)
        if (const auto result = connect(g, c)) return result;
    for (const auto& c : g.callbacks)
        if (const auto result = set_callback(g, c)) return result;
    for (auto& n : g.nodes)
        if (const auto result = static_cast<std::int32_t>(call(audio_unit, "_AudioUnitInitialize", {n.unit}))) {
            for (auto& done : g.nodes) {
                if (&done == &n) break;
                static_cast<void>(call(audio_unit, "_AudioUnitUninitialize", {done.unit}));
            }
            return result;
        }
    g.initialized = true;
    return no_err;
}

Node* AUGraphs::output_node(Graph& g) {
    for (auto& n : g.nodes) {
        std::uint32_t type{};
        std::memcpy(&type, n.description.data(), 4);
        if (type == output_type) return &n;
    }
    return nullptr;
}

void AUGraphs::install() {
    // The calls on a graph: the graph (or paramErr for none), under its lock.
    const auto with_graph = [this](const char* name, std::function<std::int32_t(Graph&, CpuState&, GuestMemory&)> body) {
        raw(audio_toolbox, name, [this, body](CpuState& s, GuestMemory& m) {
            const auto g = graph(s.x[0]);
            if (!g) { s.x[0] = status(param_err); return; }
            std::lock_guard lock(g->mutex);
            s.x[0] = status(body(*g, s, m));
        });
    };
    // NewAUGraph(AUGraph *outGraph)
    raw(audio_toolbox, "_NewAUGraph", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(param_err); return; }
        const auto handle = process.allocate_guest(16, 16);
        m.fill_bytes(handle, 0, 16);
        {
            std::lock_guard lock(mutex_);
            graphs_[handle] = std::make_shared<Graph>();
        }
        m.write64(s.x[0], handle);
        s.x[0] = status(no_err);
    });
    raw(audio_toolbox, "_DisposeAUGraph", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Graph> gone;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = graphs_.find(s.x[0]); found != graphs_.end()) {
                gone = std::move(found->second);
                graphs_.erase(found);
            }
        }
        if (!gone) { s.x[0] = status(param_err); return; }
        {
            std::lock_guard lock(gone->mutex);
            close(*gone);
        }
        process.release_guest(s.x[0]);
        s.x[0] = status(no_err);
    });
    // AUGraphAddNode(graph, const AudioComponentDescription *, AUNode *outNode)
    with_graph("_AUGraphAddNode", [this](Graph& g, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1] || !s.x[2]) return param_err;
        Node n;
        n.id = g.next++;
        m.read_bytes(s.x[1], n.description);
        if (g.open)
            if (const auto result = make_unit(n)) return result;
        g.nodes.push_back(n);
        m.write32(s.x[2], static_cast<std::uint32_t>(n.id));
        return no_err;
    });
    // AUGraphRemoveNode(graph, node): with its connections and callbacks.
    with_graph("_AUGraphRemoveNode", [this](Graph& g, CpuState& s, GuestMemory&) -> std::int32_t {
        const auto id = static_cast<std::int32_t>(s.x[1]);
        auto* n = node(g, id);
        if (!n) return node_not_found;
        if (n->unit && g.initialized) static_cast<void>(call(audio_unit, "_AudioUnitUninitialize", {n->unit}));
        dispose_unit(*n);
        std::erase_if(g.nodes, [&](const Node& e) { return e.id == id; });
        std::erase_if(g.connections, [&](const Connection& c) { return c.source == id || c.destination == id; });
        std::erase_if(g.callbacks, [&](const Callback& c) { return c.node == id; });
        return no_err;
    });
    with_graph("_AUGraphGetNodeCount", [](Graph& g, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return param_err;
        m.write32(s.x[1], static_cast<std::uint32_t>(g.nodes.size()));
        return no_err;
    });
    // AUGraphGetIndNode(graph, index, AUNode *outNode)
    with_graph("_AUGraphGetIndNode", [](Graph& g, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[2]) return param_err;
        if (s.x[1] >= g.nodes.size()) return node_not_found;
        m.write32(s.x[2], static_cast<std::uint32_t>(g.nodes[static_cast<std::size_t>(s.x[1])].id));
        return no_err;
    });
    // AUGraphNodeInfo(graph, node, AudioComponentDescription *outDescription, AudioUnit *outAudioUnit): the unit once open.
    with_graph("_AUGraphNodeInfo", [](Graph& g, CpuState& s, GuestMemory& m) -> std::int32_t {
        const auto* n = node(g, static_cast<std::int32_t>(s.x[1]));
        if (!n) return node_not_found;
        if (s.x[2]) m.write_bytes(s.x[2], n->description);
        if (s.x[3]) m.write64(s.x[3], n->unit);
        return no_err;
    });
    with_graph("_AUGraphOpen", [this](Graph& g, CpuState&, GuestMemory&) { return open(g); });
    with_graph("_AUGraphClose", [this](Graph& g, CpuState&, GuestMemory&) { close(g); return no_err; });
    with_graph("_AUGraphInitialize", [this](Graph& g, CpuState&, GuestMemory&) { return initialize(g); });
    with_graph("_AUGraphUninitialize", [this](Graph& g, CpuState&, GuestMemory&) { uninitialize(g); return no_err; });
    // AUGraphConnectNodeInput(graph, sourceNode, sourceOutputNumber, destNode, destInputNumber)
    with_graph("_AUGraphConnectNodeInput", [this](Graph& g, CpuState& s, GuestMemory&) -> std::int32_t {
        const Connection c{static_cast<std::int32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), static_cast<std::int32_t>(s.x[3]),
                           static_cast<std::uint32_t>(s.x[4])};
        if (!node(g, c.source) || !node(g, c.destination)) return node_not_found;
        if (c.source == c.destination) return invalid_connection;
        if (g.initialized)
            if (const auto result = connect(g, c)) return result;
        std::erase_if(g.connections, [&](const Connection& e) { return e.destination == c.destination && e.destination_bus == c.destination_bus; });
        g.connections.push_back(c);
        return no_err;
    });
    // AUGraphDisconnectNodeInput(graph, destNode, destInputNumber)
    with_graph("_AUGraphDisconnectNodeInput", [](Graph& g, CpuState& s, GuestMemory&) -> std::int32_t {
        const auto destination = static_cast<std::int32_t>(s.x[1]);
        const auto bus = static_cast<std::uint32_t>(s.x[2]);
        if (!node(g, destination)) return node_not_found;
        std::erase_if(g.connections, [&](const Connection& e) { return e.destination == destination && e.destination_bus == bus; });
        return no_err;
    });
    with_graph("_AUGraphClearConnections", [](Graph& g, CpuState&, GuestMemory&) {
        g.connections.clear();
        g.callbacks.clear();
        return no_err;
    });
    // AUGraphSetNodeInputCallback(graph, node, inputNumber, const AURenderCallbackStruct *)
    with_graph("_AUGraphSetNodeInputCallback", [this](Graph& g, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[3]) return param_err;
        Callback c{static_cast<std::int32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), m.read64(s.x[3]), m.read64(s.x[3] + 8)};
        if (!node(g, c.node)) return node_not_found;
        if (g.initialized)
            if (const auto result = set_callback(g, c)) return result;
        std::erase_if(g.callbacks, [&](const Callback& e) { return e.node == c.node && e.bus == c.bus; });
        g.callbacks.push_back(c);
        return no_err;
    });
    // AUGraphUpdate(graph, Boolean *outIsUpdated): every change took effect when it was made.
    with_graph("_AUGraphUpdate", [](Graph&, CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write8(s.x[1], 1);
        return no_err;
    });
    with_graph("_AUGraphStart", [this](Graph& g, CpuState&, GuestMemory&) -> std::int32_t {
        if (!g.initialized) return cannot_do_in_current_context;
        auto* out = output_node(g);
        if (!out || !out->unit) return output_node_err;
        if (const auto result = static_cast<std::int32_t>(call(audio_unit, "_AudioOutputUnitStart", {out->unit}))) return result;
        g.running = true;
        return no_err;
    });
    with_graph("_AUGraphStop", [this](Graph& g, CpuState&, GuestMemory&) -> std::int32_t {
        if (auto* out = output_node(g); out && out->unit && g.running) static_cast<void>(call(audio_unit, "_AudioOutputUnitStop", {out->unit}));
        g.running = false;
        return no_err;
    });
    for (const auto& [name, flag] : std::array<std::pair<const char*, bool Graph::*>, 3>{{
             {"_AUGraphIsOpen", &Graph::open}, {"_AUGraphIsInitialized", &Graph::initialized}, {"_AUGraphIsRunning", &Graph::running}}})
        with_graph(name, [flag](Graph& g, CpuState& s, GuestMemory& m) {
            if (!s.x[1]) return param_err;
            m.write8(s.x[1], g.*flag ? 1 : 0);
            return no_err;
        });
    // AUGraphGetCPULoad / GetMaxCPULoad(graph, Float32 *): the render load is not measured; 0.
    for (const auto* name : {"_AUGraphGetCPULoad", "_AUGraphGetMaxCPULoad"})
        with_graph(name, [](Graph&, CpuState& s, GuestMemory& m) {
            if (!s.x[1]) return param_err;
            m.write32(s.x[1], 0);
            return no_err;
        });
    // MusicDeviceMIDIEvent(unit, status, data1, data2, offsetSampleFrame), MusicDeviceSysEx(unit, data, length): no music device.
    for (const auto* name : {"_MusicDeviceMIDIEvent", "_MusicDeviceSysEx"})
        raw(audio_toolbox, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(s.x[0] ? bad_component_selector : bad_component_instance); });
}

} // namespace

void install_au_graph(InstallContext& context) { std::make_shared<AUGraphs>(context)->install(); }

} // namespace alchemy::gui::fill
