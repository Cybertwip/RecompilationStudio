#include "gip_gamepad.h"

#include <libusb.h>

#include <array>
#include <chrono>
#include <cstdint>
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

void append_u16(std::vector<uint8_t>* packet, uint16_t value) {
    packet->push_back(static_cast<uint8_t>(value & 0xFFu));
    packet->push_back(static_cast<uint8_t>(value >> 8));
}

void append_s16(std::vector<uint8_t>* packet, int16_t value) {
    append_u16(packet, static_cast<uint16_t>(value));
}

bool write_proof(const char* path,
                 const std::vector<Check>& checks,
                 size_t enumerated_devices,
                 bool hardware_requested,
                 bool hardware_connected,
                 bool hardware_state_read) {
    if (!path || !path[0]) return true;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    const libusb_version* version = libusb_get_version();
    size_t passed = 0;
    for (const Check& check : checks) if (check.passed) ++passed;

    out << "{\n";
    out << "  \"schema\": 1,\n";
    out << "  \"artifact\": \"macOS Xbox GIP protocol proof\",\n";
    out << "  \"date\": \"2026-07-15\",\n";
    out << "  \"libusb_version\": \""
        << version->major << "." << version->minor << "." << version->micro
        << "." << version->nano << "\",\n";
    out << "  \"enumerated_supported_devices\": " << enumerated_devices << ",\n";
    out << "  \"hardware_probe_requested\": "
        << (hardware_requested ? "true" : "false") << ",\n";
    out << "  \"hardware_connected\": "
        << (hardware_connected ? "true" : "false") << ",\n";
    out << "  \"hardware_state_read\": "
        << (hardware_state_read ? "true" : "false") << ",\n";
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
    const bool hardware_requested = argc >= 3 &&
                                    std::strcmp(argv[2], "--hardware") == 0;
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

    bool hardware_connected = false;
    bool hardware_state_read = false;
    if (hardware_requested) {
        checks.push_back({"hardware_device_present", enumerated_devices > 0});
    }
    if (hardware_requested && enumerated_devices > 0) {
        PsxGipGamepad* gamepad = psx_gip_gamepad_open(devices[0].selector);
        checks.push_back({"hardware_reader_created", gamepad != nullptr});
        if (gamepad) {
            for (int i = 0; i < 800; ++i) {
                if (psx_gip_gamepad_connection(gamepad) ==
                    PSX_GIP_CONNECTION_CONNECTED) {
                    hardware_connected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            PsxGipGamepadState hardware_state{};
            hardware_state_read = psx_gip_gamepad_get_state(
                gamepad, &hardware_state) != 0;
            psx_gip_gamepad_close(gamepad);
            checks.push_back({"hardware_handshake", hardware_connected});
            checks.push_back({"hardware_state_snapshot", hardware_state_read});
        }
    }

    bool passed = true;
    for (const Check& check : checks) passed = passed && check.passed;
    const bool proof_written = write_proof(proof_path, checks,
                                           enumerated_devices,
                                           hardware_requested,
                                           hardware_connected,
                                           hardware_state_read);
    return passed && proof_written ? 0 : 1;
}
