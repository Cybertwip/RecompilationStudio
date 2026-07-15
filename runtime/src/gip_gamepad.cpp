#include "gip_gamepad.h"

#ifndef PSX_HAVE_GIP_GAMEPAD
#error gip_gamepad.cpp requires PSX_HAVE_GIP_GAMEPAD and libusb-1.0
#endif

#include <libusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint16_t kPdpVendor = 0x0E6F;
constexpr uint8_t kGipInterface = 0;
constexpr uint8_t kGipCommandAnnounce = 0x02;
constexpr uint8_t kGipCommandGuide = 0x07;
constexpr uint8_t kGipCommandInput = 0x20;
constexpr uint8_t kGipOptionAck = 0x10;
constexpr size_t kReadBufferMinimum = 64;

struct ProductName {
    uint16_t vendor;
    uint16_t product;
    const char* name;
};

constexpr ProductName kProductNames[] = {
    {0x0E6F, 0x0139, "PDP Afterglow Prismatic (Xbox One)"},
    {0x0E6F, 0x0146, "PDP Xbox One Controller"},
    {0x0E6F, 0x0213, "PDP Xbox One Controller"},
    {0x0E6F, 0x02F1, "PDP Wired Controller for Xbox One"},
    {0x0E6F, 0x02F2, "PDP Wired Controller for Xbox One (Alt)"},
    {0x0E6F, 0x02A1, "PDP Afterglow Prismatic (v2)"},
    {0x0E6F, 0x0346, "PDP RC Xbox One"},
    {0x0E6F, 0x0446, "PDP Xbox One (v2)"},
    {0x045E, 0x02D1, "Microsoft Xbox One Controller"},
    {0x045E, 0x02DD, "Microsoft Xbox One Controller (FW 2015)"},
    {0x045E, 0x02E3, "Microsoft Xbox One Elite Controller"},
    {0x045E, 0x0B00, "Microsoft Xbox One Elite 2 Controller"},
    {0x045E, 0x0B12, "Microsoft Xbox Series X|S Controller"},
    {0x24C6, 0x541A, "PowerA Xbox One Controller"},
    {0x24C6, 0x542A, "PowerA Xbox One Spectra"},
};

constexpr uint8_t kInitPower[] = {
    0x05, 0x20, 0x00, 0x01, 0x00
};
constexpr uint8_t kInitLong[] = {
    0x05, 0x20, 0x00, 0x0F, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x55, 0x53
};
constexpr uint8_t kPdpLed[] = {
    0x0A, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14
};
constexpr uint8_t kPdpAuth1[] = {
    0x06, 0x20, 0x00, 0x02, 0x01, 0x00
};
constexpr uint8_t kPdpAuth2[] = {
    0x06, 0x20, 0x00, 0x02, 0x02, 0x00
};

bool is_supported_product(uint16_t vendor, uint16_t product) {
    if (vendor == kPdpVendor) return true;
    for (const ProductName& entry : kProductNames) {
        if (entry.vendor == vendor && entry.product == product) return true;
    }
    return false;
}

const char* product_name(uint16_t vendor, uint16_t product) {
    for (const ProductName& entry : kProductNames) {
        if (entry.vendor == vendor && entry.product == product) return entry.name;
    }
    return vendor == kPdpVendor ? "PDP Xbox GIP Controller"
                                : "Xbox GIP Controller";
}

void copy_text(char* out, size_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", text ? text : "");
}

std::string selector_for(libusb_device* device,
                         const libusb_device_descriptor& descriptor) {
    std::array<uint8_t, 8> ports{};
    const int port_count = libusb_get_port_numbers(
        device, ports.data(), static_cast<int>(ports.size()));

    char location[64] = {0};
    if (port_count > 0) {
        size_t used = 0;
        for (int i = 0; i < port_count && used + 5 < sizeof(location); ++i) {
            const int wrote = std::snprintf(location + used,
                                            sizeof(location) - used,
                                            i == 0 ? "%u" : ".%u",
                                            static_cast<unsigned>(ports[i]));
            if (wrote <= 0) break;
            used += static_cast<size_t>(wrote);
        }
    } else {
        std::snprintf(location, sizeof(location), "address-%03u",
                      static_cast<unsigned>(libusb_get_device_address(device)));
    }

    char selector[PSX_GIP_SELECTOR_CAPACITY] = {0};
    std::snprintf(selector, sizeof(selector), "gip:%04x:%04x:%03u:%s",
                  descriptor.idVendor, descriptor.idProduct,
                  static_cast<unsigned>(libusb_get_bus_number(device)),
                  location);
    return std::string(selector);
}

bool selector_vid_pid(const std::string& selector,
                      uint16_t* vendor,
                      uint16_t* product) {
    unsigned parsed_vendor = 0;
    unsigned parsed_product = 0;
    if (std::sscanf(selector.c_str(), "gip:%x:%x:",
                    &parsed_vendor, &parsed_product) != 2) {
        return false;
    }
    if (parsed_vendor > 0xFFFFu || parsed_product > 0xFFFFu) return false;
    if (vendor) *vendor = static_cast<uint16_t>(parsed_vendor);
    if (product) *product = static_cast<uint16_t>(parsed_product);
    return true;
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

int16_t read_s16(const uint8_t* p) {
    return static_cast<int16_t>(read_u16(p));
}

int16_t invert_axis(int16_t value) {
    return value == static_cast<int16_t>(-32768) ? static_cast<int16_t>(32767)
                                                  : static_cast<int16_t>(-value);
}

struct UsbConnection {
    libusb_device_handle* handle = nullptr;
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
    uint8_t transfer_type_in = 0;
    uint8_t transfer_type_out = 0;
    uint16_t max_packet_in = static_cast<uint16_t>(kReadBufferMinimum);
    bool claimed = false;
};

enum class ReadResult {
    Packet,
    Timeout,
    Error
};

void close_usb(UsbConnection* connection) {
    if (!connection) return;
    if (connection->handle) {
        if (connection->claimed) {
            libusb_release_interface(connection->handle, kGipInterface);
        }
        libusb_close(connection->handle);
    }
    *connection = UsbConnection{};
}

int transfer_in(UsbConnection& connection, uint8_t* data, int capacity,
                int* transferred, unsigned timeout_ms) {
    if (connection.transfer_type_in == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        return libusb_interrupt_transfer(connection.handle,
                                         connection.endpoint_in,
                                         data, capacity, transferred,
                                         timeout_ms);
    }
    if (connection.transfer_type_in == LIBUSB_TRANSFER_TYPE_BULK) {
        return libusb_bulk_transfer(connection.handle,
                                    connection.endpoint_in,
                                    data, capacity, transferred,
                                    timeout_ms);
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int transfer_out(UsbConnection& connection, const uint8_t* data, int size,
                 int* transferred, unsigned timeout_ms) {
    if (!data || size <= 0) return LIBUSB_ERROR_INVALID_PARAM;

    /* libusb's synchronous OUT API takes a mutable buffer. Passing our static
     * protocol packets through const_cast placed signed __TEXT,__const pages in
     * the IOKit transfer path. On hardened-runtime macOS builds that page can be
     * dirtied while the transfer is wired, and AMFI then terminates the process
     * with "Code Signature Invalid" on the next read. Always give libusb an
     * owned writable buffer; the wire bytes remain identical. */
    std::vector<uint8_t> transfer_data(data, data + size);
    if (connection.transfer_type_out == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        return libusb_interrupt_transfer(connection.handle,
                                         connection.endpoint_out,
                                         transfer_data.data(), size, transferred,
                                         timeout_ms);
    }
    if (connection.transfer_type_out == LIBUSB_TRANSFER_TYPE_BULK) {
        return libusb_bulk_transfer(connection.handle,
                                    connection.endpoint_out,
                                    transfer_data.data(), size, transferred,
                                    timeout_ms);
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

bool send_packet(UsbConnection& connection, const uint8_t* packet, size_t size) {
    if (!connection.handle || !connection.endpoint_out || !packet || size == 0 ||
        size > static_cast<size_t>(0x7FFFFFFF)) {
        return false;
    }
    int transferred = 0;
    const int rc = transfer_out(connection, packet, static_cast<int>(size),
                                &transferred, 200);
    return rc == LIBUSB_SUCCESS && transferred == static_cast<int>(size);
}

ReadResult read_packet(UsbConnection& connection, std::vector<uint8_t>* packet,
                       unsigned timeout_ms) {
    if (!packet || !connection.handle || !connection.endpoint_in) {
        return ReadResult::Error;
    }
    const size_t capacity = std::max(kReadBufferMinimum,
                                     static_cast<size_t>(connection.max_packet_in));
    packet->assign(capacity, 0);
    int transferred = 0;
    const int rc = transfer_in(connection, packet->data(),
                               static_cast<int>(packet->size()),
                               &transferred, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) {
        packet->clear();
        return ReadResult::Timeout;
    }
    if (rc == LIBUSB_ERROR_INTERRUPTED) {
        packet->clear();
        return ReadResult::Timeout;
    }
    if (rc != LIBUSB_SUCCESS || transferred <= 0) {
        packet->clear();
        return ReadResult::Error;
    }
    packet->resize(static_cast<size_t>(transferred));
    return ReadResult::Packet;
}

bool ack_if_needed(UsbConnection& connection,
                   const std::vector<uint8_t>& packet,
                   uint8_t* sequence) {
    if (!sequence) return false;
    uint8_t ack[13] = {0};
    const uint8_t next = static_cast<uint8_t>(*sequence + 1u);
    const size_t ack_size = psx_gip_gamepad_build_ack(
        next, packet.data(), packet.size(), ack);
    if (ack_size == 0) return true;
    if (!send_packet(connection, ack, ack_size)) return false;
    *sequence = next;
    return true;
}

bool stop_requested(const std::atomic<bool>* stop) {
    return stop && stop->load(std::memory_order_acquire);
}

void short_sleep(const std::atomic<bool>* stop, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds && !stop_requested(stop);
         elapsed += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min(10, milliseconds - elapsed)));
    }
}

bool drain_setup_packets(UsbConnection& connection,
                         int maximum_packets,
                         unsigned timeout_ms,
                         uint8_t* sequence,
                         const std::atomic<bool>* stop) {
    std::vector<uint8_t> packet;
    for (int i = 0; i < maximum_packets && !stop_requested(stop); ++i) {
        const ReadResult result = read_packet(connection, &packet, timeout_ms);
        if (result == ReadResult::Timeout) return true;
        if (result == ReadResult::Error) return false;
        if (!ack_if_needed(connection, packet, sequence)) return false;
    }
    return !stop_requested(stop);
}

bool initialize_gip(UsbConnection& connection, uint8_t* sequence,
                    const std::atomic<bool>* stop) {
    std::vector<uint8_t> packet;
    for (int i = 0; i < 50 && !stop_requested(stop); ++i) {
        const ReadResult result = read_packet(connection, &packet, 100);
        if (result == ReadResult::Error) return false;
        if (result == ReadResult::Timeout) continue;
        if (!ack_if_needed(connection, packet, sequence)) return false;
        if (packet.size() >= 4 && packet[0] == kGipCommandAnnounce) break;
    }
    if (stop_requested(stop)) return false;

    short_sleep(stop, 20);
    if (stop_requested(stop) ||
        !send_packet(connection, kInitPower, sizeof(kInitPower))) return false;
    short_sleep(stop, 50);
    if (!drain_setup_packets(connection, 30, 80, sequence, stop)) return false;

    if (stop_requested(stop) ||
        !send_packet(connection, kInitLong, sizeof(kInitLong))) return false;
    short_sleep(stop, 50);
    if (!drain_setup_packets(connection, 30, 80, sequence, stop)) return false;

    if (stop_requested(stop) ||
        !send_packet(connection, kPdpLed, sizeof(kPdpLed))) return false;
    short_sleep(stop, 30);
    if (stop_requested(stop) ||
        !send_packet(connection, kPdpAuth1, sizeof(kPdpAuth1))) return false;
    short_sleep(stop, 30);
    if (stop_requested(stop) ||
        !send_packet(connection, kPdpAuth2, sizeof(kPdpAuth2))) return false;
    short_sleep(stop, 30);

    return drain_setup_packets(connection, 30, 50, sequence, stop);
}

bool find_endpoints(const libusb_config_descriptor* config,
                    const libusb_interface_descriptor** chosen_interface,
                    uint8_t* endpoint_in,
                    uint8_t* transfer_type_in,
                    uint16_t* max_packet_in,
                    uint8_t* endpoint_out,
                    uint8_t* transfer_type_out) {
    if (!config || !chosen_interface || !endpoint_in || !transfer_type_in ||
        !max_packet_in || !endpoint_out || !transfer_type_out) {
        return false;
    }

    const libusb_interface_descriptor* fallback = nullptr;
    const libusb_interface_descriptor* selected = nullptr;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& interface = config->interface[i];
        for (int alt = 0; alt < interface.num_altsetting; ++alt) {
            const libusb_interface_descriptor& descriptor = interface.altsetting[alt];
            if (descriptor.bInterfaceNumber != kGipInterface) continue;
            if (!fallback) fallback = &descriptor;
            if (descriptor.bAlternateSetting == 0) selected = &descriptor;
        }
    }
    if (!selected) selected = fallback;
    if (!selected) return false;

    for (uint8_t i = 0; i < selected->bNumEndpoints; ++i) {
        const libusb_endpoint_descriptor& endpoint = selected->endpoint[i];
        const uint8_t transfer_type = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        if (transfer_type != LIBUSB_TRANSFER_TYPE_INTERRUPT &&
            transfer_type != LIBUSB_TRANSFER_TYPE_BULK) {
            continue;
        }
        if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
            if (!*endpoint_in) {
                *endpoint_in = endpoint.bEndpointAddress;
                *transfer_type_in = transfer_type;
                *max_packet_in = endpoint.wMaxPacketSize;
            }
        } else if (!*endpoint_out) {
            *endpoint_out = endpoint.bEndpointAddress;
            *transfer_type_out = transfer_type;
        }
    }

    *chosen_interface = selected;
    return *endpoint_in != 0 && *endpoint_out != 0;
}

bool open_usb(libusb_context* context, const std::string& selector,
              UsbConnection* connection) {
    if (!context || !connection) return false;

    uint16_t wanted_vendor = 0;
    uint16_t wanted_product = 0;
    const bool automatic = selector == "gip:auto";
    const bool has_wanted_product = !automatic &&
        selector_vid_pid(selector, &wanted_vendor, &wanted_product);

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0 || !devices) return false;

    libusb_device* exact = nullptr;
    libusb_device* product_fallback = nullptr;
    libusb_device* first_supported = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) continue;
        if (!is_supported_product(descriptor.idVendor, descriptor.idProduct)) continue;
        if (!first_supported) first_supported = devices[i];
        if (selector_for(devices[i], descriptor) == selector) exact = devices[i];
        if (has_wanted_product && descriptor.idVendor == wanted_vendor &&
            descriptor.idProduct == wanted_product && !product_fallback) {
            product_fallback = devices[i];
        }
    }

    libusb_device* selected = automatic ? first_supported
                                        : (exact ? exact : product_fallback);
    libusb_device_handle* handle = nullptr;
    const int open_rc = selected ? libusb_open(selected, &handle)
                                 : LIBUSB_ERROR_NO_DEVICE;
    if (open_rc != LIBUSB_SUCCESS || !handle) {
        libusb_free_device_list(devices, 1);
        return false;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);
    if (libusb_kernel_driver_active(handle, kGipInterface) == 1) {
        libusb_detach_kernel_driver(handle, kGipInterface);
    }

    libusb_config_descriptor* config = nullptr;
    int config_rc = libusb_get_active_config_descriptor(selected, &config);
    if (config_rc != LIBUSB_SUCCESS) {
        config_rc = libusb_get_config_descriptor(selected, 0, &config);
        if (config_rc == LIBUSB_SUCCESS && config) {
            int current_configuration = 0;
            if (libusb_get_configuration(handle, &current_configuration) != LIBUSB_SUCCESS ||
                current_configuration != config->bConfigurationValue) {
                if (libusb_set_configuration(handle, config->bConfigurationValue) !=
                    LIBUSB_SUCCESS) {
                    libusb_free_config_descriptor(config);
                    libusb_close(handle);
                    libusb_free_device_list(devices, 1);
                    return false;
                }
            }
        }
    }

    const libusb_interface_descriptor* interface_descriptor = nullptr;
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
    uint8_t transfer_type_in = 0;
    uint8_t transfer_type_out = 0;
    uint16_t max_packet_in = static_cast<uint16_t>(kReadBufferMinimum);
    const bool endpoints_ok = find_endpoints(
        config, &interface_descriptor,
        &endpoint_in, &transfer_type_in, &max_packet_in,
        &endpoint_out, &transfer_type_out);
    if (!endpoints_ok) {
        if (config) libusb_free_config_descriptor(config);
        libusb_close(handle);
        libusb_free_device_list(devices, 1);
        return false;
    }

    if (libusb_claim_interface(handle, kGipInterface) != LIBUSB_SUCCESS) {
        if (config) libusb_free_config_descriptor(config);
        libusb_close(handle);
        libusb_free_device_list(devices, 1);
        return false;
    }
    if (interface_descriptor->bAlternateSetting != 0 &&
        libusb_set_interface_alt_setting(handle, kGipInterface,
                                         interface_descriptor->bAlternateSetting) !=
            LIBUSB_SUCCESS) {
        libusb_release_interface(handle, kGipInterface);
        if (config) libusb_free_config_descriptor(config);
        libusb_close(handle);
        libusb_free_device_list(devices, 1);
        return false;
    }

    connection->handle = handle;
    connection->endpoint_in = endpoint_in;
    connection->endpoint_out = endpoint_out;
    connection->transfer_type_in = transfer_type_in;
    connection->transfer_type_out = transfer_type_out;
    connection->max_packet_in = std::max<uint16_t>(max_packet_in,
                                                    kReadBufferMinimum);
    connection->claimed = true;

    if (config) libusb_free_config_descriptor(config);
    libusb_free_device_list(devices, 1);
    return true;
}

void interruptible_sleep(const std::atomic<bool>& stop, int milliseconds) {
    const int slices = std::max(1, milliseconds / 10);
    for (int i = 0; i < slices && !stop.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

struct PsxGipGamepad {
    explicit PsxGipGamepad(std::string selected) : selector(std::move(selected)) {}

    std::string selector;
    std::atomic<bool> stop{false};
    std::atomic<PsxGipGamepadConnection> connection{PSX_GIP_CONNECTION_SEARCHING};
    mutable std::mutex state_mutex;
    PsxGipGamepadState state{};
    std::thread worker;
};

namespace {

void set_neutral(PsxGipGamepad* gamepad) {
    if (!gamepad) return;
    std::lock_guard<std::mutex> lock(gamepad->state_mutex);
    gamepad->state = PsxGipGamepadState{};
}

void gamepad_worker(PsxGipGamepad* gamepad) {
    libusb_context* context = nullptr;
    if (!gamepad || libusb_init(&context) != LIBUSB_SUCCESS || !context) {
        if (gamepad) {
            gamepad->connection.store(PSX_GIP_CONNECTION_UNAVAILABLE,
                                      std::memory_order_release);
        }
        return;
    }

    while (!gamepad->stop.load(std::memory_order_acquire)) {
        UsbConnection usb;
        gamepad->connection.store(PSX_GIP_CONNECTION_SEARCHING,
                                  std::memory_order_release);
        if (!open_usb(context, gamepad->selector, &usb)) {
            set_neutral(gamepad);
            interruptible_sleep(gamepad->stop, 500);
            continue;
        }

        gamepad->connection.store(PSX_GIP_CONNECTION_INITIALIZING,
                                  std::memory_order_release);
        uint8_t host_sequence = 0;
        if (!initialize_gip(usb, &host_sequence, &gamepad->stop)) {
            close_usb(&usb);
            set_neutral(gamepad);
            interruptible_sleep(gamepad->stop, 250);
            continue;
        }

        gamepad->connection.store(PSX_GIP_CONNECTION_CONNECTED,
                                  std::memory_order_release);
        std::vector<uint8_t> packet;
        while (!gamepad->stop.load(std::memory_order_acquire)) {
            const ReadResult result = read_packet(usb, &packet, 8);
            if (result == ReadResult::Timeout) continue;
            if (result == ReadResult::Error) break;
            if (!ack_if_needed(usb, packet, &host_sequence)) break;

            PsxGipGamepadState next{};
            {
                std::lock_guard<std::mutex> lock(gamepad->state_mutex);
                next = gamepad->state;
            }
            if (psx_gip_gamepad_parse_packet(packet.data(), packet.size(), &next) > 0) {
                std::lock_guard<std::mutex> lock(gamepad->state_mutex);
                gamepad->state = next;
            }
        }

        close_usb(&usb);
        set_neutral(gamepad);
    }

    gamepad->connection.store(PSX_GIP_CONNECTION_SEARCHING,
                              std::memory_order_release);
    libusb_exit(context);
}

}  // namespace

extern "C" size_t psx_gip_gamepad_enumerate(PsxGipGamepadInfo* out,
                                               size_t capacity) {
    libusb_context* context = nullptr;
    if (libusb_init(&context) != LIBUSB_SUCCESS || !context) return 0;

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0 || !devices) {
        libusb_exit(context);
        return 0;
    }

    size_t found = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) continue;
        if (!is_supported_product(descriptor.idVendor, descriptor.idProduct)) continue;

        if (out && found < capacity) {
            PsxGipGamepadInfo info{};
            const std::string selector = selector_for(devices[i], descriptor);
            copy_text(info.selector, sizeof(info.selector), selector.c_str());
            copy_text(info.name, sizeof(info.name),
                      product_name(descriptor.idVendor, descriptor.idProduct));
            info.vendor_id = descriptor.idVendor;
            info.product_id = descriptor.idProduct;
            info.bus = libusb_get_bus_number(devices[i]);
            info.address = libusb_get_device_address(devices[i]);
            out[found] = info;
        }
        ++found;
    }

    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return found;
}

extern "C" int psx_gip_gamepad_selector_supported(const char* selector) {
    if (!selector) return 0;
    if (std::strcmp(selector, "gip:auto") == 0) return 1;
    if (std::strncmp(selector, "gip:", 4) != 0) return 0;

    uint16_t vendor = 0;
    uint16_t product = 0;
    return selector_vid_pid(selector, &vendor, &product) &&
           is_supported_product(vendor, product);
}

extern "C" PsxGipGamepad* psx_gip_gamepad_open(const char* selector) {
    if (!psx_gip_gamepad_selector_supported(selector)) return nullptr;
    PsxGipGamepad* gamepad = new (std::nothrow) PsxGipGamepad(selector);
    if (!gamepad) return nullptr;
    try {
        gamepad->worker = std::thread(gamepad_worker, gamepad);
    } catch (...) {
        delete gamepad;
        return nullptr;
    }
    return gamepad;
}

extern "C" void psx_gip_gamepad_close(PsxGipGamepad* gamepad) {
    if (!gamepad) return;
    gamepad->stop.store(true, std::memory_order_release);
    if (gamepad->worker.joinable()) gamepad->worker.join();
    delete gamepad;
}

extern "C" PsxGipGamepadConnection psx_gip_gamepad_connection(
    const PsxGipGamepad* gamepad) {
    if (!gamepad) return PSX_GIP_CONNECTION_UNAVAILABLE;
    return gamepad->connection.load(std::memory_order_acquire);
}

extern "C" int psx_gip_gamepad_get_state(const PsxGipGamepad* gamepad,
                                            PsxGipGamepadState* out) {
    if (!gamepad || !out) return 0;
    {
        std::lock_guard<std::mutex> lock(gamepad->state_mutex);
        *out = gamepad->state;
    }
    return gamepad->connection.load(std::memory_order_acquire) ==
           PSX_GIP_CONNECTION_CONNECTED;
}

extern "C" size_t psx_gip_gamepad_build_ack(uint8_t host_sequence,
                                               const uint8_t* packet,
                                               size_t packet_size,
                                               uint8_t out_ack[13]) {
    if (!packet || packet_size < 4 || !out_ack ||
        (packet[1] & kGipOptionAck) == 0) {
        return 0;
    }
    const uint8_t ack[13] = {
        0x01, 0x20, host_sequence, 0x09,
        0x00, packet[0], packet[1], packet[2], packet[3],
        0x00, 0x00, 0x00, 0x00
    };
    std::memcpy(out_ack, ack, sizeof(ack));
    return sizeof(ack);
}

extern "C" int psx_gip_gamepad_parse_packet(const uint8_t* packet,
                                               size_t packet_size,
                                               PsxGipGamepadState* state) {
    if (!packet || packet_size < 4 || !state) return -1;

    if (packet[0] == kGipCommandInput) {
        if (packet_size < 18 || packet[3] < 14) return -1;
        const uint8_t saved_guide = state->guide;
        state->buttons = read_u16(packet + 4);
        state->left_trigger = read_u16(packet + 6);
        state->right_trigger = read_u16(packet + 8);
        state->left_x = read_s16(packet + 10);
        state->left_y = invert_axis(read_s16(packet + 12));
        state->right_x = read_s16(packet + 14);
        state->right_y = invert_axis(read_s16(packet + 16));
        state->guide = saved_guide;
        return 1;
    }

    if (packet[0] == kGipCommandGuide) {
        if (packet_size < 5 || packet[3] < 1) return -1;
        state->guide = (packet[4] & 0x01u) ? 1u : 0u;
        return 1;
    }

    return 0;
}
