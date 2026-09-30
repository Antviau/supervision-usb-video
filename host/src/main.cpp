#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <winusb.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "lcd_field_reconstruction.h"
#include "capture_cadence.h"
#include "buffered_gdi_frame.h"

using Microsoft::WRL::ComPtr;

namespace {

// Must match the DeviceInterfaceGUIDs value in firmware/src/usb_descriptors.c.
constexpr GUID kDeviceInterfaceGuid = {
    0xA8B77A47, 0x46EC, 0x4DAD,
    {0x9F, 0x56, 0x8A, 0x4B, 0xF1, 0x2D, 0x0E, 0xAE}
};

// Generic USB device interface published by Windows. Zadig registers this
// interface even when it does not preserve our custom DeviceInterfaceGUIDs.
constexpr GUID kUsbDeviceInterfaceGuid = {
    0xA5DCBF10, 0x6530, 0x11D2,
    {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}
};

constexpr UINT kMessageFrame = WM_APP + 1;
constexpr UINT kMessageStatus = WM_APP + 2;
constexpr UINT_PTR kTitleTimer = 1;

struct Options {
    int scale = 4;
    std::wstring record_path;
    bool lcd_sum3 = SV_DEFAULT_LCD_SUM3 != 0;
    bool low_latency = SV_DEFAULT_LOW_LATENCY != 0;
    bool buffered_paint = SV_DEFAULT_BUFFERED_PAINT != 0;
    unsigned usb_read_bytes = SV_DEFAULT_LOW_LATENCY ? 1024u : 16384u;
    std::wstring timing_path;
};

struct Frame {
    uint32_t sequence = 0;
    uint64_t timestamp_us = 0;
    uint64_t received_host_us = 0;
    uint32_t source_rate_millihz = 0;
    uint32_t device_dropped = 0;
    uint32_t flags = 0;
    std::array<uint8_t, SV_FRAME_PAYLOAD_SIZE> pixels{};
};

struct DeviceStatus {
    uint64_t timestamp_us = 0;
    uint32_t captured_words = 0;
    uint32_t sequence = 0;
    uint32_t dropped_frames = 0;
    uint32_t gpio_levels = 0;
};

enum class PacketKind {
    stopped,
    frame,
    status,
};

std::wstring windows_error(const wchar_t *operation, DWORD error) {
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&message),
                   0, nullptr);
    std::wstring result(operation);
    result += L" (" + std::to_wstring(error) + L")";
    if (message != nullptr) {
        size_t length = wcslen(message);
        while (length != 0 &&
               (message[length - 1] == L'\r' || message[length - 1] == L'\n')) {
            message[--length] = L'\0';
        }
        result += L" : ";
        result += message;
        LocalFree(message);
    }
    return result;
}

std::wstring widen_ascii(const char *text) {
    return std::wstring(text, text + std::strlen(text));
}

class WindowsException final : public std::exception {
public:
    WindowsException(const wchar_t *operation, DWORD error)
        : wide_(windows_error(operation, error)) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide_.c_str(), -1,
                                                nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            narrow_.resize(static_cast<size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, wide_.c_str(), -1, narrow_.data(),
                                needed, nullptr, nullptr);
            narrow_.resize(static_cast<size_t>(needed - 1));
        }
    }

    const char *what() const noexcept override { return narrow_.c_str(); }
    const std::wstring &wide() const noexcept { return wide_; }

private:
    std::wstring wide_;
    std::string narrow_;
};

Options parse_options() {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) throw WindowsException(L"CommandLineToArgvW", GetLastError());
    std::unique_ptr<wchar_t *, decltype(&LocalFree)> arguments(argv, LocalFree);

    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto next = [&]() -> std::wstring {
            if (++index >= argc) {
                throw std::runtime_error("Valeur manquante sur la ligne de commande");
            }
            return argv[index];
        };
        if (argument == L"--scale") {
            options.scale = std::stoi(next());
        } else if (argument == L"--record") {
            options.record_path = next();
        } else if (argument == L"--lcd-sum3") {
            options.lcd_sum3 = true;
        } else if (argument == L"--lcd-pair") {
            options.lcd_sum3 = false;
        } else if (argument == L"--usb-read-bytes") {
            options.usb_read_bytes = static_cast<unsigned>(std::stoul(next()));
        } else if (argument == L"--timing-log") {
            options.timing_path = next();
        } else if (argument == L"--legacy-scheduling") {
            options.low_latency = false;
        } else if (argument == L"--direct-paint") {
            options.buffered_paint = false;
        } else {
            throw std::runtime_error(
                "Option inconnue. Options: --scale N, --record fichier.svf, --lcd-sum3, --lcd-pair, --usb-read-bytes N, --timing-log fichier.csv, --legacy-scheduling, --direct-paint");
        }
    }
    if (options.scale < 1 || options.scale > 16) {
        throw std::runtime_error("--scale doit etre compris entre 1 et 16");
    }
    if (options.usb_read_bytes < 64u || options.usb_read_bytes > 16384u ||
        options.usb_read_bytes % 64u != 0) {
        throw std::runtime_error("--usb-read-bytes doit etre un multiple de 64 entre 64 et 16384");
    }
    return options;
}

bool matches_capture_hardware_id(HDEVINFO info,
                                 const SP_DEVINFO_DATA &device_info) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(
        info, const_cast<SP_DEVINFO_DATA *>(&device_info), SPDRP_HARDWAREID,
        &type, nullptr, 0, &required);
    if (required == 0) return false;

    std::vector<uint8_t> storage(required + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(
            info, const_cast<SP_DEVINFO_DATA *>(&device_info),
            SPDRP_HARDWAREID, &type, storage.data(), required, nullptr)) {
        return false;
    }

    const wchar_t *id = reinterpret_cast<const wchar_t *>(storage.data());
    const wchar_t *end = reinterpret_cast<const wchar_t *>(
        storage.data() + required);
    while (id < end && *id != L'\0') {
        if (std::wstring(id).find(L"VID_CAFE&PID_4020") !=
            std::wstring::npos) {
            return true;
        }
        id += wcslen(id) + 1;
    }
    return false;
}

std::wstring find_device_path(const GUID &interface_guid,
                              bool filter_hardware_id) {
    HDEVINFO info = SetupDiGetClassDevsW(
        &interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return {};

    std::wstring result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &interface_guid, index,
                                         &interface_data)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &interface_data, nullptr, 0,
                                         &required, nullptr);
        if (required == 0) continue;

        std::vector<uint8_t> detail_storage(required);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(
            detail_storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device_info{};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(
                info, &interface_data, detail, required, nullptr,
                &device_info)) {
            continue;
        }
        if (!filter_hardware_id ||
            matches_capture_hardware_id(info, device_info)) {
            result = detail->DevicePath;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return result;
}

class UsbDevice {
public:
    UsbDevice() {
        std::wstring path = find_device_path(kDeviceInterfaceGuid, false);
        if (path.empty()) {
            path = find_device_path(kUsbDeviceInterfaceGuid, true);
        }
        if (path.empty()) throw std::runtime_error("Pico de capture introuvable");

        device_ = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (device_ == INVALID_HANDLE_VALUE) {
            throw WindowsException(L"CreateFileW(Pico)", GetLastError());
        }

        if (!WinUsb_Initialize(device_, &interface_)) {
            const DWORD error = GetLastError();
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
            throw WindowsException(L"WinUsb_Initialize", error);
        }

        ULONG timeout_ms = 250;
        if (!WinUsb_SetPipePolicy(interface_, SV_USB_EP_IN,
                                  PIPE_TRANSFER_TIMEOUT,
                                  sizeof(timeout_ms), &timeout_ms)) {
            const DWORD error = GetLastError();
            WinUsb_Free(interface_);
            interface_ = nullptr;
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
            throw WindowsException(L"WinUsb_SetPipePolicy", error);
        }
        UCHAR enabled = TRUE;
        WinUsb_SetPipePolicy(interface_, SV_USB_EP_IN, AUTO_CLEAR_STALL,
                             sizeof(enabled), &enabled);
    }

    UsbDevice(const UsbDevice &) = delete;
    UsbDevice &operator=(const UsbDevice &) = delete;

    ~UsbDevice() {
        if (interface_ != nullptr) WinUsb_Free(interface_);
        if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
    }

    size_t read(uint8_t *destination, size_t size) {
        ULONG transferred = 0;
        if (!WinUsb_ReadPipe(interface_, SV_USB_EP_IN, destination,
                             static_cast<ULONG>(size), &transferred, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) return 0;
            throw WindowsException(L"WinUsb_ReadPipe", error);
        }
        return transferred;
    }

private:
    HANDLE device_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE interface_ = nullptr;
};

class UsbStream {
public:
    explicit UsbStream(UsbDevice &device, unsigned read_bytes)
        : device_(device), read_bytes_(read_bytes) {}

    bool read_exact(uint8_t *destination, size_t size,
                    const std::atomic<bool> &stop) {
        size_t copied = 0;
        while (copied < size && !stop.load()) {
            if (position_ == available_) {
                available_ = device_.read(buffer_.data(), read_bytes_);
                position_ = 0;
                if (available_ == 0) continue;
            }
            const size_t chunk = (size - copied < available_ - position_)
                ? size - copied : available_ - position_;
            std::memcpy(destination + copied, buffer_.data() + position_, chunk);
            position_ += chunk;
            copied += chunk;
        }
        return copied == size;
    }

    PacketKind next_packet(Frame &frame, DeviceStatus &status,
                           std::array<uint8_t, SV_FRAME_HEADER_SIZE> &header,
                           const std::atomic<bool> &stop) {
        while (!stop.load()) {
            uint8_t prefix[4]{};
            do {
                if (!read_exact(prefix, 1, stop)) return PacketKind::stopped;
            } while (prefix[0] != 'S' && !stop.load());
            if (!read_exact(prefix + 1, 3, stop)) return PacketKind::stopped;
            if (prefix[1] != 'V' || prefix[3] != '0' ||
                (prefix[2] != 'F' && prefix[2] != 'S')) {
                continue;
            }

            if (prefix[2] == 'S') {
                std::array<uint8_t, SV_STATUS_SIZE> status_packet{};
                std::memcpy(status_packet.data(), prefix, sizeof(prefix));
                if (!read_exact(status_packet.data() + sizeof(prefix),
                                status_packet.size() - sizeof(prefix), stop)) {
                    return PacketKind::stopped;
                }
                if (sv_read_le16(status_packet.data() + SV_STATUS_VERSION_OFFSET) !=
                        SV_PROTOCOL_VERSION ||
                    sv_read_le16(status_packet.data() + SV_STATUS_SIZE_OFFSET) !=
                        SV_STATUS_SIZE) {
                    continue;
                }
                status.timestamp_us = sv_read_le64(
                    status_packet.data() + SV_STATUS_TIMESTAMP_OFFSET);
                status.captured_words = sv_read_le32(
                    status_packet.data() + SV_STATUS_CAPTURED_WORDS_OFFSET);
                status.sequence = sv_read_le32(
                    status_packet.data() + SV_STATUS_SEQUENCE_OFFSET);
                status.dropped_frames = sv_read_le32(
                    status_packet.data() + SV_STATUS_DROPPED_OFFSET);
                status.gpio_levels = sv_read_le32(
                    status_packet.data() + SV_STATUS_GPIO_OFFSET);
                return PacketKind::status;
            }

            std::memcpy(header.data(), prefix, sizeof(prefix));
            if (!read_exact(header.data() + sizeof(prefix),
                            header.size() - sizeof(prefix), stop)) {
                return PacketKind::stopped;
            }
            if (!valid_header(header)) continue;

            frame.sequence = sv_read_le32(header.data() + SV_HEADER_SEQUENCE_OFFSET);
            frame.timestamp_us = sv_read_le64(header.data() + SV_HEADER_TIMESTAMP_OFFSET);
            frame.source_rate_millihz = sv_read_le32(header.data() + SV_HEADER_RATE_OFFSET);
            frame.device_dropped = sv_read_le32(header.data() + SV_HEADER_DROPPED_OFFSET);
            frame.flags = sv_read_le32(header.data() + SV_HEADER_FLAGS_OFFSET);
            if (!read_exact(frame.pixels.data(), frame.pixels.size(), stop)) {
                return PacketKind::stopped;
            }
            return PacketKind::frame;
        }
        return PacketKind::stopped;
    }

private:
    static bool valid_header(
        const std::array<uint8_t, SV_FRAME_HEADER_SIZE> &header) {
        return sv_read_le16(header.data() + SV_HEADER_VERSION_OFFSET) ==
                   SV_PROTOCOL_VERSION &&
               sv_read_le16(header.data() + SV_HEADER_SIZE_OFFSET) ==
                   SV_FRAME_HEADER_SIZE &&
               sv_read_le16(header.data() + SV_HEADER_WIDTH_OFFSET) ==
                   SV_FRAME_WIDTH &&
               sv_read_le16(header.data() + SV_HEADER_HEIGHT_OFFSET) ==
                   SV_FRAME_HEIGHT &&
               sv_read_le32(header.data() + SV_HEADER_PAYLOAD_OFFSET) ==
                   SV_FRAME_PAYLOAD_SIZE;
    }

    UsbDevice &device_;
    std::array<uint8_t, 16384> buffer_{};
    unsigned read_bytes_;
    size_t position_ = 0;
    size_t available_ = 0;
};

class Recording {
public:
    explicit Recording(const std::wstring &path) {
        if (!path.empty()) {
            if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || file_ == nullptr) {
                throw std::runtime_error("Impossible de creer le fichier SVF");
            }
        }
    }

    ~Recording() {
        if (file_ != nullptr) std::fclose(file_);
    }

    void write(const std::array<uint8_t, SV_FRAME_HEADER_SIZE> &header,
               const Frame &frame) {
        if (file_ == nullptr) return;
        if (std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
            std::fwrite(frame.pixels.data(), 1, frame.pixels.size(), file_) !=
                frame.pixels.size()) {
            throw std::runtime_error("Echec d'ecriture du fichier SVF");
        }
    }

private:
    FILE *file_ = nullptr;
};

struct SharedCapture {
    std::mutex mutex;
    Frame latest;
    bool have_frame = false;
    bool connected = false;
    uint64_t received = 0;
    uint64_t sequence_gaps = 0;
    double receive_fps = 0.0;
    double source_fps = 0.0;
    double decoded_fps = 0.0;
    uint32_t captured_words = 0;
    uint32_t gpio_levels = 0;
    std::wstring status = L"Recherche du Pico de capture...";
};

uint64_t host_time_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Optional local measurements. Paint means GDI submission, NOT physical scanout
// or button-to-photon latency. Pico and Windows clocks are not synchronized.
class TimingLog {
public:
    explicit TimingLog(const std::wstring &path) {
        if (path.empty()) return;
        if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || file_ == nullptr)
            throw std::runtime_error("Impossible de creer le journal de timing");
        std::setvbuf(file_, nullptr, _IOFBF, 65536);
        std::fputs("event,host_us,sequence,pico_us,pico_dropped,display_ready\n", file_);
    }
    ~TimingLog() { if (file_) std::fclose(file_); }
    void write(const char *event, uint64_t time, const Frame &frame, bool ready) {
        if (!file_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::fprintf(file_, "%s,%llu,%u,%llu,%u,%u\n", event,
            static_cast<unsigned long long>(time), frame.sequence,
            static_cast<unsigned long long>(frame.timestamp_us),
            frame.device_dropped, ready ? 1u : 0u);
        if (++rows_ % 128u == 0) std::fflush(file_);
    }
private:
    FILE *file_ = nullptr;
    std::mutex mutex_;
    unsigned rows_ = 0;
};

class Application {
public:
    Application(HINSTANCE instance, Options options)
        : instance_(instance), options_(std::move(options)), timing_log_(options_.timing_path) {}

    int run() {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com_result)) throw std::runtime_error("Initialisation COM impossible");

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = L"SupervisionViewerWindow";
        if (!RegisterClassExW(&window_class)) {
            CoUninitialize();
            throw WindowsException(L"RegisterClassExW", GetLastError());
        }

        RECT rectangle{
            0,
            0,
            static_cast<LONG>(SV_FRAME_WIDTH * options_.scale),
            static_cast<LONG>(SV_FRAME_HEIGHT * options_.scale)};
        AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0);
        window_ = CreateWindowExW(
            0, window_class.lpszClassName, L"Supervision - recherche du Pico",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            CoUninitialize();
            throw WindowsException(L"CreateWindowExW", GetLastError());
        }

        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        SetTimer(window_, kTitleTimer, 250, nullptr);
        capture_thread_ = std::thread(&Application::capture_loop, this);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        stop_.store(true);
        if (capture_thread_.joinable()) capture_thread_.join();
        CoUninitialize();
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        Application *application = reinterpret_cast<Application *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            application = static_cast<Application *>(create->lpCreateParams);
            application->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(application));
        }
        if (application != nullptr) {
            return application->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_KEYDOWN:
            handle_key(static_cast<UINT>(wparam));
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wparam == kTitleTimer) update_title();
            return 0;
        case kMessageFrame:
            // Clear BEFORE reading latest: an arrival racing the snapshot can
            // schedule one more notification, never become stranded.
            frame_notification_pending_.store(false);
            consume_latest_frame();
            if (options_.low_latency) UpdateWindow(window_);
            return 0;
        case kMessageStatus:
            update_title();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            stop_.store(true);
            KillTimer(window_, kTitleTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
        }
    }

    void capture_loop() {
        try {
            Recording recording(options_.record_path);
            while (!stop_.load()) {
                try {
                    set_status(L"Recherche du Pico de capture...", false);
                    UsbDevice device;
                    UsbStream stream(device, options_.usb_read_bytes);
                    LcdFieldReconstruction reconstruction;
                    CaptureCadence cadence;
                    set_status(L"Pico connecte - attente du signal video...", true);

                    bool have_previous = false;
                    bool have_diagnostic = false;
                    uint32_t previous_sequence = 0;
                    uint32_t previous_captured_words = 0;
                    uint64_t interval_frames = 0;
                    uint64_t interval_decoded = 0;
                    auto interval_start = std::chrono::steady_clock::now();

                    while (!stop_.load()) {
                        Frame frame;
                        DeviceStatus device_status;
                        std::array<uint8_t, SV_FRAME_HEADER_SIZE> header{};
                        const PacketKind kind = stream.next_packet(
                            frame, device_status, header, stop_);
                        if (kind == PacketKind::stopped) break;
                        if (kind == PacketKind::status) {
                            const bool moving = have_diagnostic &&
                                device_status.captured_words != previous_captured_words;
                            previous_captured_words = device_status.captured_words;
                            have_diagnostic = true;

                            wchar_t diagnostic[256]{};
                            swprintf_s(
                                diagnostic,
                                L"Pico connecte - DMA %u (%s) - GPIO D0..D3/CLK/POL = %02X",
                                device_status.captured_words,
                                moving ? L"horloge active" : L"aucune progression",
                                device_status.gpio_levels & 0x3Fu);
                            {
                                std::lock_guard<std::mutex> lock(shared_.mutex);
                                shared_.connected = true;
                                shared_.captured_words = device_status.captured_words;
                                shared_.gpio_levels = device_status.gpio_levels;
                                shared_.status = diagnostic;
                            }
                            PostMessageW(window_, kMessageStatus, 0, 0);
                            continue;
                        }
                        frame.received_host_us = host_time_us();
                        recording.write(header, frame);
                        // Recording stays bit-for-bit raw, before processing.
                        const bool display_ready = !options_.lcd_sum3 ||
                            reconstruction.push(frame.sequence, frame.timestamp_us,
                                                frame.flags, frame.pixels);
                        if (options_.lcd_sum3 && display_ready) {
                            frame.flags &= ~SV_FRAME_FLAG_HIGH_FIELD_IS_MSB;
                        }
                        timing_log_.write("receive", frame.received_host_us, frame, display_ready);
                        const double source_fps = cadence.push(frame.sequence, frame.timestamp_us);

                        {
                            std::lock_guard<std::mutex> lock(shared_.mutex);
                            if (have_previous) {
                                const uint32_t distance = frame.sequence - previous_sequence;
                                if (distance > 1u && distance < 0x80000000u) {
                                    shared_.sequence_gaps += distance - 1u;
                                }
                            }
                            previous_sequence = frame.sequence;
                            have_previous = true;
                            if (display_ready) {
                                shared_.latest = frame;
                                shared_.have_frame = true;
                            }
                            shared_.connected = true;
                            ++shared_.received;
                            shared_.source_fps = source_fps;

                            ++interval_frames;
                            if (display_ready) ++interval_decoded;
                            const auto now = std::chrono::steady_clock::now();
                            const double seconds =
                                std::chrono::duration<double>(now - interval_start).count();
                            if (seconds >= 1.0) {
                                shared_.receive_fps = interval_frames / seconds;
                                shared_.decoded_fps = interval_decoded / seconds;
                                interval_frames = 0;
                                interval_decoded = 0;
                                interval_start = now;
                            }
                        }
                        if (display_ready && (!options_.low_latency ||
                            !frame_notification_pending_.exchange(true))) {
                            if (!PostMessageW(window_, kMessageFrame, 0, 0))
                                frame_notification_pending_.store(false);
                        }
                    }
                } catch (const WindowsException &error) {
                    set_status(error.wide(), false);
                } catch (const std::exception &error) {
                    set_status(widen_ascii(error.what()), false);
                }

                for (int tenth = 0; tenth < 10 && !stop_.load(); ++tenth) {
                    Sleep(100);
                }
            }
        } catch (const std::exception &error) {
            set_status(widen_ascii(error.what()), false);
        }
    }

    void set_status(std::wstring text, bool connected) {
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.status = std::move(text);
            shared_.connected = connected;
            if (!connected) {
                shared_.receive_fps = 0.0;
                shared_.source_fps = 0.0;
                shared_.decoded_fps = 0.0;
            }
        }
        if (window_ != nullptr) PostMessageW(window_, kMessageStatus, 0, 0);
    }

    void consume_latest_frame() {
        Frame frame;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (!shared_.have_frame) return;
            frame = shared_.latest;
        }

        // Several queued notifications can refer to the same latest frame.
        // Consuming it twice destroys the previous-frame relationship and
        // alternately applies/removes the motion filter on identical input.
        if (have_displayed_frame_ && frame.sequence == displayed_sequence_ &&
            frame.timestamp_us == current_frame_.timestamp_us) return;

        previous_frame_contiguous_ = false;
        if (have_displayed_frame_) {
            const uint32_t distance = frame.sequence - displayed_sequence_;
            if (distance > 1u && distance < 0x80000000u) {
                display_skips_ += distance - 1u;
            }
            previous_frame_ = current_frame_;
            previous_frame_contiguous_ = distance == 1u;
        }
        displayed_sequence_ = frame.sequence;
        current_frame_ = std::move(frame);
        have_displayed_frame_ = true;
        expand_current_frame();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void expand_current_frame() {
        const auto green = std::array<uint32_t, 4>{
            0xFFC7D6A3u, 0xFF91A873u, 0xFF526742u, 0xFF1D2B22u};
        const auto gray = std::array<uint32_t, 4>{
            0xFFEEEEEEu, 0xFFAAAAAAu, 0xFF5C5C5Cu, 0xFF111111u};
        const auto &palette = use_gray_ ? gray : green;
        std::array<uint32_t, 4> lane_sums{};

        for (size_t group = 0; group < current_frame_.pixels.size(); ++group) {
            const uint8_t packed = current_frame_.pixels[group];
            const uint8_t previous = previous_frame_.pixels[group];
            for (size_t pixel = 0; pixel < 4; ++pixel) {
                uint8_t value = static_cast<uint8_t>((packed >> (pixel * 2)) & 3u);

                // The LCD sends the two bits of a gray pixel in successive
                // fields, about 9.8 ms apart.  At a moving black/white edge,
                // one bit therefore belongs to the old position and the
                // other to the new position, producing a false gray trail.
                // A real gray pixel is stable from frame to frame; a transient
                // value between a stable endpoint and the next frame is not.
                // The low field is the newer field in the firmware's ordered
                // high/low pair, so duplicate its bit at those transition
                // pixels to display the newest edge without combing.
                if (motion_cleanup_ && previous_frame_contiguous_ &&
                    (current_frame_.flags & SV_FRAME_FLAG_HIGH_FIELD_IS_MSB) != 0 &&
                    (value == 1u || value == 2u)) {
                    const uint8_t previous_value = static_cast<uint8_t>(
                        (previous >> (pixel * 2)) & 3u);
                    if (previous_value == 0u || previous_value == 3u) {
                        value = (value & 1u) != 0u ? 3u : 0u;
                    }
                }
                if (swap_midtones_ && (value == 1u || value == 2u)) {
                    value = static_cast<uint8_t>(3u - value);
                }
                lane_sums[pixel] += value;
                image_[group * 4 + pixel] = palette[value];
            }
        }
        for (size_t lane = 0; lane < lane_means_.size(); ++lane) {
            lane_means_[lane] = static_cast<double>(lane_sums[lane]) /
                                current_frame_.pixels.size();
        }
    }

    void paint() {
        PAINTSTRUCT paint_structure{};
        HDC dc = BeginPaint(window_, &paint_structure);
        RECT client{};
        GetClientRect(window_, &client);
        bool frame_presented = false;
        if (have_displayed_frame_ && options_.buffered_paint) {
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width > 0 && height > 0) {
                frame_presented = presentation_surface_.ensure(width, height) &&
                    presentation_surface_.compose(image_.data(), SV_FRAME_WIDTH, SV_FRAME_HEIGHT) &&
                    presentation_surface_.present(dc);
                if (!frame_presented) {
                    ++paint_errors_;
                    timing_log_.write("paint_error", host_time_us(), current_frame_, false);
                }
            }
            // No screen clear, blank-image replacement or sleep on failure.
            // The next normal incoming frame retries in the same low-latency path.
        } else {
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

            if (have_displayed_frame_) {
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                const int side = width < height ? width : height;
                const int left = (width - side) / 2;
                const int top = (height - side) / 2;

                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = SV_FRAME_WIDTH;
                bitmap.bmiHeader.biHeight = -static_cast<LONG>(SV_FRAME_HEIGHT);
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchDIBits(dc, left, top, side, side, 0, 0, SV_FRAME_WIDTH,
                              SV_FRAME_HEIGHT, image_.data(), &bitmap,
                              DIB_RGB_COLORS, SRCCOPY);
                frame_presented = true;
            } else {
                std::wstring status;
                {
                    std::lock_guard<std::mutex> lock(shared_.mutex);
                    status = shared_.status;
                }
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(220, 220, 220));
                DrawTextW(dc, status.c_str(), -1, &client,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        EndPaint(window_, &paint_structure);
        if (frame_presented && (!have_painted_frame_ ||
            painted_sequence_ != current_frame_.sequence ||
            painted_timestamp_ != current_frame_.timestamp_us)) {
            const uint64_t now = host_time_us();
            timing_log_.write("paint", now, current_frame_, true);
            receive_to_paint_ms_ = (now - current_frame_.received_host_us) / 1000.0;
            have_painted_frame_ = true;
            painted_sequence_ = current_frame_.sequence;
            painted_timestamp_ = current_frame_.timestamp_us;
            ++interval_painted_;
            if (paint_interval_start_ == 0) paint_interval_start_ = now;
            if (now - paint_interval_start_ >= 1000000u) {
                paint_fps_ = interval_painted_ * 1000000.0 /
                             static_cast<double>(now - paint_interval_start_);
                interval_painted_ = 0;
                paint_interval_start_ = now;
            }
        }
    }

    void update_title() {
        bool connected = false;
        uint64_t received = 0;
        uint64_t gaps = 0;
        double fps = 0.0;
        double source_fps = 0.0, decoded_fps = 0.0;
        std::wstring status;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            connected = shared_.connected;
            received = shared_.received;
            gaps = shared_.sequence_gaps;
            fps = shared_.receive_fps;
            source_fps = shared_.source_fps;
            decoded_fps = shared_.decoded_fps;
            status = shared_.status;
        }

        wchar_t title[512]{};
        if (connected && have_displayed_frame_) {
            if (options_.low_latency || !options_.timing_path.empty()) {
                swprintf_s(title,
                    L"Supervision TIMING - source %.3f - USB %.2f - gris %.2f - paint %.2f fps - UI %.2f ms - seq %u - pertes Pico %u - trous %llu - read %u - %s",
                    source_fps, fps, decoded_fps, paint_fps_, receive_to_paint_ms_,
                    current_frame_.sequence, current_frame_.device_dropped,
                    static_cast<unsigned long long>(gaps), options_.usb_read_bytes,
                    options_.lcd_sum3 ? L"LCD SOMME 3 CHAMPS" : L"LCD PAIR");
            } else swprintf_s(title,
                L"Supervision - %.2f i/s - seq %u - pertes Pico %u - sequences manquantes %llu - voies %.2f/%.2f/%.2f/%.2f - %s%s",
                fps, current_frame_.sequence, current_frame_.device_dropped,
                static_cast<unsigned long long>(gaps),
                lane_means_[0], lane_means_[1], lane_means_[2], lane_means_[3],
                options_.lcd_sum3 ? L"LCD SOMME 3 CHAMPS" :
                    (motion_cleanup_ ? L"anti-trainee ON" : L"anti-trainee OFF"),
                options_.record_path.empty() ? L"" : L" - ENREGISTREMENT");
        } else {
            swprintf_s(title, L"Supervision - %s - trames recues %llu",
                       status.c_str(), static_cast<unsigned long long>(received));
        }
        if (options_.buffered_paint) {
            wchar_t paint_status[80]{};
            swprintf_s(paint_status, L" - buffered - GDI errors %llu",
                static_cast<unsigned long long>(paint_errors_));
            wcscat_s(title, paint_status);
        }
        SetWindowTextW(window_, title);
    }

    void handle_key(UINT key) {
        switch (key) {
        case VK_ESCAPE:
            DestroyWindow(window_);
            break;
        case VK_F11:
            toggle_fullscreen();
            break;
        case 'G':
            use_gray_ = !use_gray_;
            if (have_displayed_frame_) expand_current_frame();
            InvalidateRect(window_, nullptr, FALSE);
            break;
        case 'S':
            swap_midtones_ = !swap_midtones_;
            if (have_displayed_frame_) expand_current_frame();
            InvalidateRect(window_, nullptr, FALSE);
            break;
        case 'M':
            motion_cleanup_ = !motion_cleanup_;
            if (have_displayed_frame_) expand_current_frame();
            InvalidateRect(window_, nullptr, FALSE);
            update_title();
            break;
        case 'P':
            if (have_displayed_frame_) save_screenshot();
            break;
        default:
            break;
        }
    }

    void toggle_fullscreen() {
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        if ((style & WS_OVERLAPPEDWINDOW) != 0) {
            fullscreen_placement_.length = sizeof(fullscreen_placement_);
            GetWindowPlacement(window_, &fullscreen_placement_);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                            &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left,
                         monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left,
                         monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(window_, &fullscreen_placement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    void save_screenshot() {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t filename[MAX_PATH]{};
        swprintf_s(filename,
            L"Supervision-%04u%02u%02u-%02u%02u%02u-seq%08u.png",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
            time.wSecond, current_frame_.sequence);

        try {
            ComPtr<IWICImagingFactory> factory;
            HRESULT result = CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (FAILED(result)) throw std::runtime_error("WIC indisponible");

            ComPtr<IWICStream> stream;
            if (FAILED(factory->CreateStream(&stream)) ||
                FAILED(stream->InitializeFromFilename(filename, GENERIC_WRITE))) {
                throw std::runtime_error("Creation du PNG impossible");
            }
            ComPtr<IWICBitmapEncoder> encoder;
            if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                              &encoder)) ||
                FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
                throw std::runtime_error("Initialisation de l'encodeur PNG impossible");
            }
            ComPtr<IWICBitmapFrameEncode> encoded_frame;
            ComPtr<IPropertyBag2> properties;
            if (FAILED(encoder->CreateNewFrame(&encoded_frame, &properties)) ||
                FAILED(encoded_frame->Initialize(properties.Get())) ||
                FAILED(encoded_frame->SetSize(SV_FRAME_WIDTH, SV_FRAME_HEIGHT))) {
                throw std::runtime_error("Initialisation de l'image PNG impossible");
            }
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (FAILED(encoded_frame->SetPixelFormat(&format)) ||
                format != GUID_WICPixelFormat32bppBGRA ||
                FAILED(encoded_frame->WritePixels(
                    SV_FRAME_HEIGHT, SV_FRAME_WIDTH * sizeof(uint32_t),
                    static_cast<UINT>(image_.size() * sizeof(uint32_t)),
                    reinterpret_cast<BYTE *>(image_.data()))) ||
                FAILED(encoded_frame->Commit()) || FAILED(encoder->Commit())) {
                throw std::runtime_error("Ecriture du PNG impossible");
            }
            MessageBoxW(window_, filename, L"Capture PNG enregistree",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception &error) {
            const std::wstring message = widen_ascii(error.what());
            MessageBoxW(window_, message.c_str(), L"Erreur PNG",
                        MB_OK | MB_ICONERROR);
        }
    }

    HINSTANCE instance_ = nullptr;
    Options options_;
    TimingLog timing_log_;
    HWND window_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> frame_notification_pending_{false};
    std::thread capture_thread_;
    SharedCapture shared_;

    Frame current_frame_;
    BufferedGdiFrame presentation_surface_;
    uint64_t paint_errors_ = 0;
    Frame previous_frame_;
    std::array<uint32_t, SV_FRAME_WIDTH * SV_FRAME_HEIGHT> image_{};
    std::array<double, 4> lane_means_{};
    bool have_displayed_frame_ = false;
    bool use_gray_ = false;
    bool swap_midtones_ = false;
    bool motion_cleanup_ = true;
    bool previous_frame_contiguous_ = false;
    uint32_t displayed_sequence_ = 0;
    uint64_t display_skips_ = 0;
    bool have_painted_frame_ = false;
    uint32_t painted_sequence_ = 0;
    uint64_t painted_timestamp_ = 0, paint_interval_start_ = 0, interval_painted_ = 0;
    double paint_fps_ = 0, receive_to_paint_ms_ = 0;
    WINDOWPLACEMENT fullscreen_placement_{sizeof(WINDOWPLACEMENT)};
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Application application(instance, parse_options());
        return application.run();
    } catch (const WindowsException &error) {
        MessageBoxW(nullptr, error.wide().c_str(), L"Supervision Viewer",
                    MB_OK | MB_ICONERROR);
    } catch (const std::exception &error) {
        const std::wstring message = widen_ascii(error.what());
        MessageBoxW(nullptr, message.c_str(), L"Supervision Viewer",
                    MB_OK | MB_ICONERROR);
    }
    return 1;
}
