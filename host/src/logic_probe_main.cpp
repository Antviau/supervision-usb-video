#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include "logic_probe_analysis.h"
#include "logic_probe_test_signal.h"

static void win_check(bool ok, const char *action) {
    if (!ok) throw std::runtime_error(std::string(action) + " (Windows " + std::to_string(GetLastError()) + ")");
}

// Same WinUSB device identities and Zadig fallback as the video viewer.
static std::wstring device_path(const GUID &guid, bool filter_id) {
    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    for (DWORD n = 0;; ++n) {
        SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, n, &iface)) break;
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &size, nullptr);
        if (size == 0) continue;
        std::vector<uint8_t> bytes(size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(bytes.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, size, nullptr, &dev)) continue;
        if (filter_id) {
            DWORD type = 0, required = 0;
            SetupDiGetDeviceRegistryPropertyW(info, &dev, SPDRP_HARDWAREID, &type, nullptr, 0, &required);
            std::vector<uint8_t> ids(required + sizeof(wchar_t));
            if (!required || !SetupDiGetDeviceRegistryPropertyW(info, &dev, SPDRP_HARDWAREID,
                        &type, ids.data(), required, nullptr)) continue;
            bool match = false;
            for (const wchar_t *id = reinterpret_cast<const wchar_t *>(ids.data()); *id; id += wcslen(id) + 1)
                match |= std::wstring(id).find(L"VID_CAFE&PID_4020") != std::wstring::npos;
            if (!match) continue;
        }
        path = detail->DevicePath; break;
    }
    SetupDiDestroyDeviceInfoList(info);
    return path;
}

class ProbeUsb {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb_ = nullptr;
    std::array<uint8_t, 16384> read_buffer_{};
    size_t position_ = 0, available_ = 0;
public:
    ProbeUsb() {
        constexpr GUID custom = {0xA8B77A47,0x46EC,0x4DAD,{0x9F,0x56,0x8A,0x4B,0xF1,0x2D,0x0E,0xAE}};
        constexpr GUID generic = {0xA5DCBF10,0x6530,0x11D2,{0x90,0x1F,0x00,0xC0,0x4F,0xB9,0x51,0xED}};
        auto path = device_path(custom, false);
        if (path.empty()) path = device_path(generic, true);
        if (path.empty()) throw std::runtime_error("Pico introuvable.");
        // Exclusive access: do not compete with a running viewer on its endpoint.
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        win_check(file_ != INVALID_HANDLE_VALUE, "Ouvrir Pico (fermer le visualisateur)");
        if (!WinUsb_Initialize(file_, &usb_)) {
            const DWORD error = GetLastError(); CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            SetLastError(error); win_check(false, "WinUsb_Initialize");
        }
        ULONG timeout = 250;
        WinUsb_SetPipePolicy(usb_, SV_USB_EP_IN, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        WinUsb_SetPipePolicy(usb_, SV_USB_EP_OUT, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    }
    ~ProbeUsb() { if (usb_) WinUsb_Free(usb_); if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    void command(uint32_t id, uint32_t mode) {
        std::array<uint8_t, SV_PROBE_COMMAND_SIZE> command{};
        std::memcpy(command.data(), "SVPC", 4);
        sv_write_le32(command.data() + 4, id); sv_write_le32(command.data() + 8, mode);
        ULONG bytes = 0;
        win_check(WinUsb_WritePipe(usb_, SV_USB_EP_OUT, command.data(),
                  static_cast<ULONG>(command.size()), &bytes, nullptr) != FALSE, "Envoyer demande");
        if (bytes != command.size()) throw std::runtime_error("Demande USB incomplete.");
    }
    void read_exact(uint8_t *dst, size_t size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (size) {
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Aucune reponse complete du firmware de mesure.");
            if (position_ == available_) {
                ULONG got = 0;
                if (!WinUsb_ReadPipe(usb_, SV_USB_EP_IN, read_buffer_.data(),
                                   static_cast<ULONG>(read_buffer_.size()), &got, nullptr)) {
                    if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
                    win_check(false, "Lire mesure USB");
                }
                position_ = 0; available_ = got;
                if (got == 0) continue;
            }
            const size_t n = std::min(size, available_ - position_);
            std::memcpy(dst, read_buffer_.data() + position_, n);
            dst += n; size -= n; position_ += n;
        }
    }
    ProbeCapture capture(uint32_t id, uint32_t mode) {
        command(id, mode);
        ProbeCapture c;
        read_exact(c.header.data(), c.header.size());
        if (std::memcmp(c.header.data(), "SVW0", 4) != 0)
            throw std::runtime_error("Firmware video detecte, pas le firmware DIAGNOSTIC_CLOCK_LATCH.");
        if (sv_read_le16(c.header.data() + 4) != 1 ||
            sv_read_le16(c.header.data() + 6) != SV_PROBE_HEADER_SIZE ||
            sv_read_le32(c.header.data() + 8) != id)
            throw std::runtime_error("Reponse de mesure inattendue.");
        const uint32_t bytes = sv_read_le32(c.header.data() + 20);
        if (bytes > SV_PROBE_MAX_PAYLOAD) throw std::runtime_error("Mesure trop grande.");
        c.payload.resize(bytes);
        read_exact(c.payload.data(), c.payload.size());
        c.validate();
        return c;
    }
};

using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
static File open_file(const std::wstring &path, const wchar_t *mode) {
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), mode) || !f) throw std::runtime_error("Impossible d'ouvrir le fichier.");
    return File(f, std::fclose);
}
static void write_bytes(FILE *f, const void *data, size_t size) {
    if (std::fwrite(data, 1, size, f) != size) throw std::runtime_error("Ecriture de fichier incomplete.");
}
static void save_capture(const ProbeCapture &c, const std::wstring &path) {
    auto f = open_file(path, L"wbx"); // Never overwrite an existing diagnostic.
    write_bytes(f.get(), c.header.data(), c.header.size());
    write_bytes(f.get(), c.payload.data(), c.payload.size());
}
static ProbeCapture load_capture(const std::wstring &path) {
    auto f = open_file(path, L"rb");
    ProbeCapture c;
    if (std::fread(c.header.data(), 1, c.header.size(), f.get()) != c.header.size())
        throw std::runtime_error("En-tete incomplet.");
    const uint32_t bytes = sv_read_le32(c.header.data() + 20);
    if (bytes > SV_PROBE_MAX_PAYLOAD) throw std::runtime_error("Taille invalide.");
    c.payload.resize(bytes);
    if (std::fread(c.payload.data(), 1, bytes, f.get()) != bytes || std::fgetc(f.get()) != EOF)
        throw std::runtime_error("Taille du fichier incoherente.");
    c.validate(); return c;
}
static void export_vcd(const ProbeCapture &c, const std::wstring &path) {
    auto f = open_file(path, L"wbx");
    std::fprintf(f.get(), "$timescale 1ps $end\n$scope module supervision $end\n");
    for (uint32_t p = 0; p < c.pins; ++p)
        std::fprintf(f.get(), "$var wire 1 %c GP%u $end\n", 'A' + p, c.base + p);
    std::fprintf(f.get(), "$upscope $end\n$enddefinitions $end\n");
    uint32_t previous = UINT32_MAX;
    for (uint32_t i = 0; i < c.count; ++i) {
        const uint32_t value = c.sample(i);
        if (value == previous) continue;
        const uint64_t ps = (static_cast<uint64_t>(i) * 1000000000000ull + c.rate / 2u) / c.rate;
        std::fprintf(f.get(), "#%llu\n", static_cast<unsigned long long>(ps));
        for (uint32_t p = 0; p < c.pins; ++p) {
            if (previous == UINT32_MAX || ((value ^ previous) & (1u << p)))
                std::fprintf(f.get(), "%u%c\n", (value >> p) & 1u, 'A' + p);
        }
        previous = value;
    }
    if (std::ferror(f.get())) throw std::runtime_error("Ecriture VCD incomplete.");
}

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc < 3) {
            std::puts("--timing output.svw [captures=1]\n--data output.svw [captures=1]\n--analyze file.svw\nFermer le visualisateur. Firmware de mesure uniquement, console allumee.");
            return 2;
        }
        const std::wstring option = argv[1], path = argv[2];
        if (option == L"--selftest" && argc == 3) {
            const auto c = probe_test_signal(false, ProbeTestFault::extra_latch);
            save_capture(c, path);
            const auto loaded = load_capture(path);
            if (loaded.payload != c.payload || loaded.header != c.header)
                throw std::runtime_error("Round-trip fichier incorrect.");
            export_vcd(loaded, path + L".vcd");
            std::puts("SYNTHETIC SELFTEST ONLY -- no console measurement");
            std::printf("%s", probe_report(loaded, analyze_probe(loaded)).c_str()); return 0;
        }
        if (option == L"--analyze" && argc == 3) {
            const auto c = load_capture(path);
            std::printf("%s", probe_report(c, analyze_probe(c)).c_str()); return 0;
        }
        if (option != L"--timing" && option != L"--data") throw std::runtime_error("Option inconnue.");
        const int captures = argc == 4 ? std::stoi(argv[3]) : 1;
        if (argc > 4 || captures < 1 || captures > 100) throw std::runtime_error("Nombre de captures: 1..100.");
        ProbeUsb device;
        for (int i = 0; i < captures; ++i) {
            const auto c = device.capture(GetTickCount() + static_cast<uint32_t>(i),
                option == L"--timing" ? SV_PROBE_MODE_TIMING : SV_PROBE_MODE_DATA);
            const auto name = captures == 1 ? path : path + L"-" + std::to_wstring(i + 1) + L".svw";
            save_capture(c, name);
            export_vcd(c, name + L".vcd");
            const auto report = probe_report(c, analyze_probe(c));
            auto text = open_file(name + L".txt", L"wbx");
            write_bytes(text.get(), report.data(), report.size());
            std::printf("%s", report.c_str());
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "ERREUR: %s\n", e.what()); return 1;
    }
}
