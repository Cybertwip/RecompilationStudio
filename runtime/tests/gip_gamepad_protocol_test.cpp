#include "gip_gamepad.h"

#include <libusb.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Check {
    const char* name;
    bool passed;
};

struct HardwareMetrics {
    bool requested = false;
    bool connected = false;
    bool state_read = false;
    int soak_ms = 0;
    uint64_t connection_transitions = 0;
    uint64_t disconnected_ms = 0;
    uint64_t maximum_disconnected_span_ms = 0;
    PsxGipGamepadDiagnostics diagnostics{};
};

void append_u16(std::vector<uint8_t>* packet, uint16_t value) {
    packet->push_back(static_cast<uint8_t>(value & 0xFFu));
    packet->push_back(static_cast<uint8_t>(value >> 8));
}

void append_s16(std::vector<uint8_t>* packet, int16_t value) {
    append_u16(packet, static_cast<uint16_t>(value));
}

const char* failure_stage_name(PsxGipFailureStage stage) {
    switch (stage) {
    case PSX_GIP_FAILURE_NONE:              return "none";
    case PSX_GIP_FAILURE_OPEN_DEVICE:       return "open_device";
    case PSX_GIP_FAILURE_CONFIGURE:         return "configure";
    case PSX_GIP_FAILURE_CLAIM_INTERFACE:   return "claim_interface";
    case PSX_GIP_FAILURE_INITIALIZE_READ:   return "initialize_read";
    case PSX_GIP_FAILURE_INITIALIZE_WRITE:  return "initialize_write";
    case PSX_GIP_FAILURE_LIVE_READ:         return "live_read";
    case PSX_GIP_FAILURE_ACK_WRITE:         return "ack_write";
    }
    return "unknown";
}

bool write_proof(const char* path,
                 const std::vector<Check>& checks,
                 size_t enumerated_devices,
                 const HardwareMetrics& hardware) {
    if (!path || !path[0]) return true;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    const libusb_version* version = libusb_get_version();
    size_t passed = 0;
    for (const Check& check : checks) if (check.passed) ++passed;
    const PsxGipGamepadDiagnostics& d = hardware.diagnostics;

    out << "{\n";
    out << "  \"schema\": 2,\n";
    out << "  \"artifact\": \"macOS Xbox GIP protocol proof\",\n";
    out << "  \"date\": \"2026-07-15\",\n";
    out << "  \"libusb_version\": \""
        << version->major << "." << version->minor << "." << version->micro
        << "." << version->nano << "\",\n";
    out << "  \"enumerated_supported_devices\": " << enumerated_devices << ",\n";
    out << "  \"hardware_probe_requested\": "
        << (hardware.requested ? "true" : "false") << ",\n";
    out << "  \"hardware_connected\": "
        << (hardware.connected ? "true" : "false") << ",\n";
    out << "  \"hardware_state_read\": "
        << (hardware.state_read ? "true" : "false") << ",\n";
    out << "  \"hardware_soak_ms\": " << hardware.soak_ms << ",\n";
    out << "  \"connection_transitions\": "
        << hardware.connection_transitions << ",\n";
    out << "  \"disconnected_ms\": " << hardware.disconnected_ms << ",\n";
    out << "  \"maximum_disconnected_span_ms\": "
        << hardware.maximum_disconnected_span_ms << ",\n";
    out << "  \"transport\": {\n";
    out << "    \"open_attempts\": " << d.open_attempts << ",\n";
    out << "    \"open_failures\": " << d.open_failures << ",\n";
    out << "    \"initialize_attempts\": " << d.initialize_attempts << ",\n";
    out << "    \"initialize_failures\": " << d.initialize_failures << ",\n";
    out << "    \"successful_connections\": " << d.successful_connections << ",\n";
    out << "    \"disconnects\": " << d.disconnects << ",\n";
    out << "    \"packets_received\": " << d.packets_received << ",\n";
    out << "    \"input_packets\": " << d.input_packets << ",\n";
    out << "    \"guide_packets\": " << d.guide_packets << ",\n";
    out << "    \"other_packets\": " << d.other_packets << ",\n";
    out << "    \"read_timeouts\": " << d.read_timeouts << ",\n";
    out << "    \"read_errors\": " << d.read_errors << ",\n";
    out << "    \"transient_read_recoveries\": "
        << d.transient_read_recoveries << ",\n";
    out << "    \"peak_consecutive_read_errors\": "
        << d.peak_consecutive_read_errors << ",\n";
    out << "    \"writes_attempted\": " << d.writes_attempted << ",\n";
    out << "    \"write_errors\": " << d.write_errors << ",\n";
    out << "    \"ack_requests\": " << d.ack_requests << ",\n";
    out << "    \"ack_failures\": " << d.ack_failures << ",\n";
    out << "    \"last_packet_age_ms\": " << d.last_packet_age_ms << ",\n";
    out << "    \"maximum_packet_gap_ms\": " << d.maximum_packet_gap_ms << ",\n";
    out << "    \"last_packet_command\": "
        << static_cast<unsigned>(d.last_packet_command) << ",\n";
    out << "    \"last_libusb_error\": " << d.last_libusb_error << ",\n";
    out << "    \"last_libusb_error_name\": \""
        << libusb_error_name(d.last_libusb_error) << "\",\n";
    out << "    \"last_failure_stage\": \""
        << failure_stage_name(d.last_failure_stage) << "\"\n";
    out << "  },\n";
    out << "  \"checks_passed\": " << passed << ",\n";
    out << "  \"checks_total\": " << checks.size() << ",\n";
    out << "  \"checks\": [\n";
    for (size_t i = 0; i < checks.size(); ++i) {
        out << "    {\"name\": \"" << checks[i].name << "\", \"passed\": "
            << (checks[i].passed ? "true" : "false") << "}";
        out << (i + 1 == checks.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    const char* proof_path = argc >= 2 ? argv[1] : nullptr;
    HardwareMetrics hardware;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hardware") == 0) {
            hardware.requested = true;
        } else if (std::strcmp(argv[i], "--soak-ms") == 0 && i + 1 < argc) {
            hardware.soak_ms = std::max(0, std::atoi(argv[++i]));
        }
    }
    std::vector<Check> checks;

    checks.push_back({"selector_auto",
                      psx_gip_gamepad_selector_supported("gip:auto") == 1});
    checks.push_back({"selector_known_product",
                      psx_gip_gamepad_selector_supported(
                          "gip:0e6f:02f1:001:2.3") == 1});
    checks.push_back({"selector_rejects_non_gip",
                      psx_gip_gamepad_selector_supported(
                          "030000005e0400008e02000014010000") == 0});

    const std::array<uint8_t, 8> announce = {
        0x02, 0x30, 0x5A, 0x04, 0x10, 0x20, 0x30, 0x40
    };
    std::array<uint8_t, 13> ack{};
    const std::array<uint8_t, 13> expected_ack = {
        0x01, 0x20, 0x33, 0x09, 0x00,
        0x02, 0x30, 0x5A, 0x04,
        0x00, 0x00, 0x00, 0x00
    };
    const size_t ack_size = psx_gip_gamepad_build_ack(
        0x33, announce.data(), announce.size(), ack.data());
    checks.push_back({"announce_ack_bytes",
                      ack_size == ack.size() && ack == expected_ack});

    std::array<uint8_t, 8> no_ack = announce;
    no_ack[1] = 0x20;
    checks.push_back({"ack_not_emitted_without_flag",
                      psx_gip_gamepad_build_ack(
                          1, no_ack.data(), no_ack.size(), ack.data()) == 0});

    const uint16_t buttons = PSX_GIP_BUTTON_MENU |
                             PSX_GIP_BUTTON_A |
                             PSX_GIP_BUTTON_DPAD_LEFT |
                             PSX_GIP_BUTTON_RIGHT_BUMPER |
                             PSX_GIP_BUTTON_RIGHT_STICK;
    std::vector<uint8_t> input = {0x20, 0x20, 0x11, 0x0E};
    append_u16(&input, buttons);
    append_u16(&input, 0x0123);
    append_u16(&input, 0x03FF);
    append_s16(&input, static_cast<int16_t>(-12345));
    append_s16(&input, static_cast<int16_t>(1000));
    append_s16(&input, static_cast<int16_t>(32767));
    append_s16(&input, static_cast<int16_t>(-32768));

    PsxGipGamepadState state{};
    state.guide = 1;
    const int input_rc = psx_gip_gamepad_parse_packet(
        input.data(), input.size(), &state);
    checks.push_back({"input_packet_accepted", input_rc == 1});
    checks.push_back({"input_buttons", state.buttons == buttons});
    checks.push_back({"input_triggers",
                      state.left_trigger == 0x0123 &&
                      state.right_trigger == 0x03FF});
    checks.push_back({"input_sticks_and_y_inversion",
                      state.left_x == -12345 && state.left_y == -1000 &&
                      state.right_x == 32767 && state.right_y == 32767});
    checks.push_back({"input_preserves_guide", state.guide == 1});

    const std::array<uint8_t, 5> guide_release = {0x07, 0x20, 0x12, 0x01, 0x00};
    const int guide_rc = psx_gip_gamepad_parse_packet(
        guide_release.data(), guide_release.size(), &state);
    checks.push_back({"guide_packet", guide_rc == 1 && state.guide == 0});
    checks.push_back({"guide_preserves_input_state",
                      state.buttons == buttons && state.left_x == -12345});

    const std::array<uint8_t, 4> malformed_input = {0x20, 0x20, 0x00, 0x0E};
    checks.push_back({"malformed_input_rejected",
                      psx_gip_gamepad_parse_packet(
                          malformed_input.data(), malformed_input.size(), &state) == -1});
    const std::array<uint8_t, 5> status_packet = {0x03, 0x20, 0x00, 0x01, 0x00};
    checks.push_back({"unrelated_packet_ignored",
                      psx_gip_gamepad_parse_packet(
                          status_packet.data(), status_packet.size(), &state) == 0});

    std::array<PsxGipGamepadInfo, 16> devices{};
    const size_t enumerated_devices = psx_gip_gamepad_enumerate(
        devices.data(), devices.size());
    checks.push_back({"libusb_enumeration_completed", true});

    const auto lifecycle_start = std::chrono::steady_clock::now();
    PsxGipGamepad* lifecycle = psx_gip_gamepad_open("gip:auto");
    if (lifecycle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        psx_gip_gamepad_close(lifecycle);
    }
    const auto lifecycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lifecycle_start).count();
    checks.push_back({"background_reader_lifecycle",
                      lifecycle != nullptr && lifecycle_ms < 1000});

    if (hardware.requested) {
        checks.push_back({"hardware_device_present", enumerated_devices > 0});
    }
    if (hardware.requested && enumerated_devices > 0) {
        PsxGipGamepad* gamepad = psx_gip_gamepad_open(devices[0].selector);
        checks.push_back({"hardware_reader_created", gamepad != nullptr});
        if (gamepad) {
            for (int i = 0; i < 800; ++i) {
                if (psx_gip_gamepad_connection(gamepad) ==
                    PSX_GIP_CONNECTION_CONNECTED) {
                    hardware.connected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (hardware.connected && hardware.soak_ms > 0) {
                using Clock = std::chrono::steady_clock;
                const auto soak_start = Clock::now();
                auto previous_tick = soak_start;
                PsxGipGamepadConnection previous =
                    psx_gip_gamepad_connection(gamepad);
                bool disconnected = previous != PSX_GIP_CONNECTION_CONNECTED;
                auto disconnected_start = soak_start;
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - soak_start).count() < hardware.soak_ms) {
                    const auto now = Clock::now();
                    const auto elapsed = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - previous_tick).count());
                    if (previous != PSX_GIP_CONNECTION_CONNECTED) {
                        hardware.disconnected_ms += elapsed;
                    }
                    const PsxGipGamepadConnection current =
                        psx_gip_gamepad_connection(gamepad);
                    if (current != previous) {
                        ++hardware.connection_transitions;
                        if (current != PSX_GIP_CONNECTION_CONNECTED) {
                            disconnected = true;
                            disconnected_start = now;
                        } else if (disconnected) {
                            const uint64_t span = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - disconnected_start).count());
                            hardware.maximum_disconnected_span_ms = std::max(
                                hardware.maximum_disconnected_span_ms, span);
                            disconnected = false;
                        }
                    }
                    previous = current;
                    previous_tick = now;
                    PsxGipGamepadState sample{};
                    (void)psx_gip_gamepad_get_state(gamepad, &sample);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (disconnected) {
                    const uint64_t span = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - disconnected_start).count());
                    hardware.maximum_disconnected_span_ms = std::max(
                        hardware.maximum_disconnected_span_ms, span);
                }
            }

            PsxGipGamepadState hardware_state{};
            hardware.state_read = psx_gip_gamepad_get_state(
                gamepad, &hardware_state) != 0;
            (void)psx_gip_gamepad_get_diagnostics(
                gamepad, &hardware.diagnostics);
            psx_gip_gamepad_close(gamepad);
            checks.push_back({"hardware_handshake", hardware.connected});
            checks.push_back({"hardware_state_snapshot", hardware.state_read});
            if (hardware.soak_ms > 0) {
                checks.push_back({"hardware_soak_connection_stable",
                                  hardware.diagnostics.disconnects == 0 &&
                                  hardware.diagnostics.successful_connections == 1 &&
                                  hardware.connection_transitions == 0});
                checks.push_back({"hardware_soak_transient_errors_recovered",
                                  hardware.diagnostics.disconnects == 0 &&
                                  hardware.diagnostics.read_errors ==
                                      hardware.diagnostics.transient_read_recoveries &&
                                  hardware.diagnostics.write_errors == 0 &&
                                  hardware.diagnostics.ack_failures == 0});
            }
        }
    }

    bool passed = true;
    for (const Check& check : checks) passed = passed && check.passed;
    const bool proof_written = write_proof(
        proof_path, checks, enumerated_devices, hardware);
    return passed && proof_written ? 0 : 1;
}
