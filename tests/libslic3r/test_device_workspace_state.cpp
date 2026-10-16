#include "libslic3r/DeviceWorkspaceState.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace Slic3r;

TEST_CASE("Combined Device workspace has stable page identities", "[DeviceWorkspace]")
{
    CHECK(static_cast<unsigned char>(DeviceWorkspacePage::SnapmakerControl) == 0);
    CHECK(static_cast<unsigned char>(DeviceWorkspacePage::LanCamera) == 1);
}

TEST_CASE("LAN camera runs only while selected and visible", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.update_authoritative({"printer-a", "U1", "192.168.10.25", true});
    state.set_outer_active(true);

    state.select_page(DeviceWorkspacePage::LanCamera);
    CHECK(state.should_run(DeviceWorkspacePage::LanCamera));

    state.set_outer_active(false);
    CHECK_FALSE(state.should_run(DeviceWorkspacePage::LanCamera));
}

TEST_CASE("Offline state never starts a direct page", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.set_outer_active(true);
    state.select_page(DeviceWorkspacePage::LanCamera);
    state.update_authoritative({"printer-a", "U1", "192.168.10.25", false});

    CHECK_FALSE(state.endpoint_available());
    CHECK_FALSE(state.should_run(DeviceWorkspacePage::LanCamera));
}

TEST_CASE("Printer switching invalidates the direct camera before replacement", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.update_authoritative({"printer-a", "U1 A", "192.168.10.25", true});
    state.set_outer_active(true);
    state.select_page(DeviceWorkspacePage::LanCamera);
    const auto old_generation = state.generation();

    const auto update = state.update_authoritative({"printer-b", "U1 B", "192.168.10.26", true});
    CHECK(update.identity_changed);
    CHECK(update.endpoint_changed);
    CHECK(update.direct_pages_invalidated);
    CHECK(state.generation() > old_generation);
    CHECK(state.should_run(DeviceWorkspacePage::LanCamera));
    CHECK(state.endpoint().printer_id == "printer-b");
}

TEST_CASE("Endpoint changes invalidate a running LAN page even when printer identity is stable", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.update_authoritative({"printer-a", "U1", "192.168.10.25", true});
    state.set_outer_active(true);
    state.select_page(DeviceWorkspacePage::LanCamera);
    REQUIRE(state.should_run(DeviceWorkspacePage::LanCamera));
    const auto old_generation = state.generation();

    const auto update = state.update_authoritative({"printer-a", "U1", "192.168.10.26", true});
    CHECK_FALSE(update.identity_changed);
    CHECK(update.endpoint_changed);
    CHECK(update.direct_pages_invalidated);
    CHECK(state.generation() > old_generation);
    CHECK(state.should_run(DeviceWorkspacePage::LanCamera));
    CHECK(state.endpoint().host == "192.168.10.26");
}

TEST_CASE("Disconnect invalidates every LAN page and cannot leave one running", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.update_authoritative({"printer-a", "U1", "192.168.10.25", true});
    state.set_outer_active(true);
    state.select_page(DeviceWorkspacePage::LanCamera);
    REQUIRE(state.should_run(DeviceWorkspacePage::LanCamera));
    const auto old_generation = state.generation();

    const auto update = state.update_authoritative({"printer-a", "U1", "192.168.10.25", false});
    CHECK_FALSE(update.identity_changed);
    CHECK_FALSE(update.endpoint_changed);
    CHECK(update.availability_changed);
    CHECK(update.direct_pages_invalidated);
    CHECK(state.generation() > old_generation);
    CHECK_FALSE(state.should_run(DeviceWorkspacePage::LanCamera));
}

TEST_CASE("Reload suspension can recreate only the selected direct page", "[DeviceWorkspace]")
{
    DeviceWorkspaceState state;
    state.update_authoritative({"printer-a", "U1", "192.168.20.30", true});
    state.set_outer_active(true);
    state.select_page(DeviceWorkspacePage::LanCamera);
    REQUIRE(state.should_run(DeviceWorkspacePage::LanCamera));

    state.suspend_direct_pages();
    CHECK_FALSE(state.should_run(DeviceWorkspacePage::LanCamera));
    state.set_outer_active(true);
    CHECK(state.should_run(DeviceWorkspacePage::LanCamera));
}

TEST_CASE("Endpoint policy is LAN-only and does not accept URL-shaped input", "[DeviceWorkspace]")
{
    CHECK(DeviceWorkspaceState::is_safe_lan_host("10.0.0.5"));
    CHECK(DeviceWorkspaceState::is_safe_lan_host("172.20.4.9"));
    CHECK(DeviceWorkspaceState::is_safe_lan_host("192.168.1.12"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("printer.local"));
    CHECK(DeviceWorkspaceState::is_safe_lan_host("fd12::25"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("8.8.8.8"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("https://192.168.1.12"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("192.168.1.12/path"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("fd-not-an-ip::1"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("fd12:12345::1"));
    CHECK_FALSE(DeviceWorkspaceState::is_safe_lan_host("fd12::1::2"));
    CHECK(DeviceWorkspaceState::http_origin("fd12::25") == "http://[fd12::25]");
}

TEST_CASE("Connection addresses yield only validated private LAN hosts", "[DeviceWorkspace]")
{
    CHECK(DeviceWorkspaceState::lan_host_from_address("192.168.1.12:7125") == "192.168.1.12");
    CHECK(DeviceWorkspaceState::lan_host_from_address("[fd12::25]:80") == "fd12::25");
    CHECK(DeviceWorkspaceState::lan_host_from_address("10.0.0.5") == "10.0.0.5");
    CHECK(DeviceWorkspaceState::lan_host_from_address("8.8.8.8:80").empty());
    CHECK(DeviceWorkspaceState::lan_host_from_address("192.168.1.12:0").empty());
    CHECK(DeviceWorkspaceState::lan_host_from_address("192.168.1.12:not-a-port").empty());
    CHECK(DeviceWorkspaceState::lan_host_from_address("[fd12::25]:70000").empty());
    CHECK(DeviceWorkspaceState::lan_host_from_address("user@192.168.1.12:80").empty());
}
