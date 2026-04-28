#include <catch2/catch.hpp>
#include "platform/linux/playback/PipeWireManager.h"
#include "platform/linux/playback/PipeWireBackend.h"
#include "platform/linux/playback/detail/PipeWireShared.h"
#include "core/backend/IAudioBackend.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace app::playback;
using namespace app::playback::detail;
using namespace app::core::backend;
using namespace app::core;

namespace {
    struct DummySinkGuard {
        PwThreadLoopPtr threadLoop;
        PwContextPtr context;
        PwCorePtr core;
        PwProxyPtr<::pw_proxy> proxy;

        DummySinkGuard() {
            ensurePipeWireInit();
            threadLoop.reset(::pw_thread_loop_new("TestSinkLoop", nullptr));
            if (!threadLoop) return;

            context.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoop.get()), nullptr, 0));
            if (!context) return;

            if (::pw_thread_loop_start(threadLoop.get()) < 0) return;

            ::pw_thread_loop_lock(threadLoop.get());
            core.reset(::pw_context_connect(context.get(), nullptr, 0));
            if (core) {
                ::pw_properties* props = ::pw_properties_new(
                    "factory.name", "support.null-audio-sink",
                    "node.name", "rs-test-dummy-sink",
                    "media.class", "Audio/Sink",
                    "object.linger", "false",
                    nullptr);
                
                // Create node via adapter factory
                void* p = ::pw_core_create_object(core.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
                proxy.reset(static_cast<::pw_proxy*>(p));
                ::pw_properties_free(props);
                
                // Sync to ensure it's processed
                ::pw_core_sync(core.get(), PW_ID_CORE, 0);
            }
            ::pw_thread_loop_unlock(threadLoop.get());
        }

        ~DummySinkGuard() {
            if (threadLoop) {
                ::pw_thread_loop_lock(threadLoop.get());
                proxy.reset(); // Destroy proxy while lock is held
                ::pw_thread_loop_unlock(threadLoop.get());
                ::pw_thread_loop_stop(threadLoop.get());
            }
        }

        bool isValid() const { return proxy != nullptr; }
    };
}

TEST_CASE("PipeWireManager - Integration with Real Daemon via API", "[integration][pipewire]")
{
    // Check if PipeWire is available
    ensurePipeWireInit();
    
    // Quick check if we can connect to a daemon
    {
        auto* loop = ::pw_main_loop_new(nullptr);
        auto* context = ::pw_context_new(::pw_main_loop_get_loop(loop), nullptr, 0);
        auto* core = ::pw_context_connect(context, nullptr, 0);
        if (!core) {
            ::pw_context_destroy(context);
            ::pw_main_loop_destroy(loop);
            std::cout << "Skipping PipeWire integration test: Daemon not running" << std::endl;
            return;
        }
        ::pw_core_disconnect(core);
        ::pw_context_destroy(context);
        ::pw_main_loop_destroy(loop);
    }

    DummySinkGuard sinkGuard;
    if (!sinkGuard.isValid()) {
        std::cout << "Skipping PipeWire integration test: Failed to create dummy sink" << std::endl;
        return;
    }

    PipeWireManager manager;

    SECTION("Enumeration finds the dummy sink")
    {
        bool found = false;
        // Wait a bit for PipeWire to propagate the new node to the manager's registry
        for (int i = 0; i < 20; ++i) {
            auto devices = manager.enumerateDevices();
            for (auto const& d : devices) {
                if (d.displayName == "rs-test-dummy-sink" || d.id == "rs-test-dummy-sink") {
                    found = true;
                    break;
                }
            }
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        REQUIRE(found);
    }

    SECTION("Graph subscription shows full path to sink")
    {
        AudioDevice dummyDevice;
        // Wait a bit for PipeWire to propagate the new node
        for (int i = 0; i < 20; ++i) {
            auto devices = manager.enumerateDevices();
            for (auto const& d : devices) {
                if (d.displayName == "rs-test-dummy-sink" || d.id == "rs-test-dummy-sink") {
                    dummyDevice = d;
                    break;
                }
            }
            if (!dummyDevice.id.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        REQUIRE(!dummyDevice.id.empty());

        auto backend = manager.createBackend(dummyDevice);
        AudioFormat format{.sampleRate = 48000, .channels = 2, .bitDepth = 16, .isFloat = false};

        std::string routeAnchor;
        AudioRenderCallbacks cb{};
        cb.userData = &routeAnchor;
        cb.onRouteReady = [](void* data, std::string_view anchor) noexcept {
            *static_cast<std::string*>(data) = anchor;
        };
        // Provide dummy callbacks to avoid SEGV when backend calls them
        cb.readPcm = [](void*, std::span<std::byte> output) noexcept -> std::size_t {
            if (output.data()) memset(output.data(), 0, output.size());
            return output.size();
        };
        cb.onPositionAdvanced = [](void*, std::uint32_t) noexcept {};

        REQUIRE(backend->open(format, cb));
        backend->start();

        // Wait for route anchor (asynchronous)
        for (int i = 0; i < 100 && routeAnchor.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(!routeAnchor.empty());

        AudioGraph receivedGraph;
        bool graphReceived = false;
        auto sub = manager.subscribeGraph(routeAnchor, [&](AudioGraph const& g) {
            receivedGraph = g;
            graphReceived = true;
        });

        // Wait for graph update
        for (int i = 0; i < 100 && !graphReceived; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(graphReceived);

        // Verify topological structure
        bool hasStream = false;
        bool hasSink = false;
        for (auto const& node : receivedGraph.nodes) {
            if (node.type == AudioNodeType::Stream) hasStream = true;
            if (node.type == AudioNodeType::Sink) hasSink = true;
        }

        CHECK(hasStream);
        CHECK(hasSink);
        // There should be at least one link from stream to sink
        CHECK(!receivedGraph.links.empty());

        // Check our synthesized node logic (from fix)
        auto streamIt = std::ranges::find_if(receivedGraph.nodes, [](auto const& n) {
            return n.type == AudioNodeType::Stream;
        });
        REQUIRE(streamIt != receivedGraph.nodes.end());
        CHECK(streamIt->id == routeAnchor);
        CHECK(streamIt->name == "RockStudio Playback");

        backend->stop();
        backend->close();
    }
}
