#include <iostream>
#include <string>
#include <iomanip>
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

#include <windows.h>

#include <setupapi.h>
#include <winusb.h>
#include <initguid.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "advapi32.lib")

#include <winrt/base.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Microsoft.Windows.Devices.Midi2.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Endpoints.Virtual.h>

#include <winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp>

// ============================================================================
// MIDI SysEx debug helpers
// ============================================================================

static void PrintHexBytes(
    wchar_t const* prefix,
    std::vector<BYTE> const& bytes)
{
    std::wcout
        << prefix
        << L"Length = "
        << bytes.size()
        << L" bytes"
        << std::endl
        << L"  ";

    for (BYTE b : bytes)
    {
        std::wcout
            << std::uppercase
            << std::hex
            << std::setw(2)
            << std::setfill(L'0')
            << static_cast<unsigned int>(b)
            << L" "
            << std::dec
            << std::setfill(L' ');
    }

    std::wcout << std::endl;
}

// ============================================================================
// Namespace aliases
// ============================================================================

namespace midi2 =
    winrt::Microsoft::Windows::Devices::Midi2;

namespace virt =
    winrt::Microsoft::Windows::Devices::Midi2::Endpoints::Virtual;

// ============================================================================
// SD-80 WinUSB bridge
//
// Virtual MIDI UMP Type 2 -> MIDI 1.0 -> USB-MIDI -> SD-80
// Cable 0 is fixed to PART A; the incoming MIDI channel is preserved.
// ============================================================================

DEFINE_GUID(
    GUID_SD80_WINUSB,
    0x7d8e5f32,
    0x6b31,
    0x4f1d,
    0x9c,
    0x52,
    0x3a,
    0x7e,
    0x8b,
    0x1d,
    0x42,
    0x01
);

static constexpr UCHAR SD80_EP_OUT = 0x01;
static constexpr UCHAR SD80_EP_IN = 0x81;

static std::wstring FindSD80DevicePath()
{
    HDEVINFO deviceInfoSet =
        SetupDiGetClassDevsW(
            &GUID_SD80_WINUSB,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        std::wcerr
            << L"[SD-80] SetupDiGetClassDevsW failed. Error = "
            << GetLastError()
            << std::endl;
        return {};
    }

    std::wstring result;

    for (DWORD index = 0;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(
            deviceInfoSet,
            nullptr,
            &GUID_SD80_WINUSB,
            index,
            &interfaceData))
        {
            const DWORD error = GetLastError();

            if (error == ERROR_NO_MORE_ITEMS)
            {
                break;
            }

            std::wcerr
                << L"[SD-80] SetupDiEnumDeviceInterfaces failed. Error = "
                << error
                << std::endl;
            break;
        }

        DWORD requiredSize = 0;

        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            nullptr,
            0,
            &requiredSize,
            nullptr);

        if (requiredSize == 0)
        {
            continue;
        }

        std::vector<BYTE> buffer(requiredSize);

        auto detailData =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
                buffer.data());

        detailData->cbSize =
            sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            detailData,
            requiredSize,
            nullptr,
            nullptr))
        {
            std::wcerr
                << L"[SD-80] SetupDiGetDeviceInterfaceDetailW failed. Error = "
                << GetLastError()
                << std::endl;
            continue;
        }

        result = detailData->DevicePath;
        break;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return result;
}

class SD80UsbOutput
{
public:
    bool Open()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_winusb != nullptr)
        {
            return true;
        }

        const std::wstring devicePath = FindSD80DevicePath();

        if (devicePath.empty())
        {
            std::wcerr
                << L"[SD-80] WinUSB device interface not found."
                << std::endl;
            return false;
        }

        std::wcout
            << L"[SD-80] WinUSB device interface found:" << std::endl
            << L"         " << devicePath << std::endl;

        m_deviceHandle =
            CreateFileW(
                devicePath.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr);

        if (m_deviceHandle == INVALID_HANDLE_VALUE)
        {
            m_deviceHandle = INVALID_HANDLE_VALUE;

            std::wcerr
                << L"[SD-80] CreateFileW failed. Error = "
                << GetLastError()
                << std::endl;
            return false;
        }

        std::wcout
            << L"[SD-80] CreateFile : OK"
            << std::endl;

        if (!WinUsb_Initialize(
            m_deviceHandle,
            &m_winusb))
        {
            std::wcerr
                << L"[SD-80] WinUsb_Initialize failed. Error = "
                << GetLastError()
                << std::endl;

            CloseHandle(m_deviceHandle);
            m_deviceHandle = INVALID_HANDLE_VALUE;
            m_winusb = nullptr;
            return false;
        }

        std::wcout
            << L"[SD-80] WinUsb_Initialize : OK"
            << std::endl;

        USB_INTERFACE_DESCRIPTOR interfaceDescriptor{};

        if (!WinUsb_QueryInterfaceSettings(
            m_winusb,
            0,
            &interfaceDescriptor))
        {
            std::wcerr
                << L"[SD-80] WinUsb_QueryInterfaceSettings failed. Error = "
                << GetLastError()
                << std::endl;

            CloseUnlocked();
            return false;
        }

        std::wcout
            << L"[SD-80] Interface = "
            << static_cast<unsigned int>(
                interfaceDescriptor.bInterfaceNumber)
            << L", Endpoints = "
            << static_cast<unsigned int>(
                interfaceDescriptor.bNumEndpoints)
            << std::endl;

        if (!WinUsb_SetCurrentAlternateSetting(m_winusb, 0))
        {
            std::wcerr
                << L"[SD-80] WinUsb_SetCurrentAlternateSetting(0) failed. Error = "
                << GetLastError()
                << std::endl;

            CloseUnlocked();
            return false;
        }

        std::wcout
            << L"[SD-80] Alternate Setting 0 selected."
            << std::endl;

        // Start USB-MIDI IN receive diagnostics only.  This stage deliberately
        // does not forward anything to Virtual MIDI yet.  It only reads the
        // confirmed SD-80 IN endpoint (0x81) and prints the raw USB-MIDI data.
        StartReceiveThread();

        return true;
    }

    // ------------------------------------------------------------------------
    // Send a complete MIDI 1.0 SysEx byte stream to the SD-80.
    //
    // The SD-80 uses standard USB-MIDI event packets:
    //   CIN 0x4 = SysEx start / continue (3 bytes)
    //   CIN 0x5 = SysEx end with 1 byte
    //   CIN 0x6 = SysEx end with 2 bytes
    //   CIN 0x7 = SysEx end with 3 bytes
    //
    // This is deliberately kept separate from SendMidi1(), so the existing
    // Note/CC/Program/Pitch-Bend path is unchanged.
    // ------------------------------------------------------------------------
    bool SendSysEx(
        BYTE cable,
        std::vector<BYTE> const& bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_winusb == nullptr)
        {
            std::wcerr
                << L"[SD-80] SysEx send requested but WinUSB is not open."
                << std::endl;
            return false;
        }

        if (bytes.size() < 2 ||
            bytes.front() != 0xF0 ||
            bytes.back() != 0xF7)
        {
            std::wcerr
                << L"[SD-80] Invalid SysEx: expected F0 ... F7."
                << std::endl;
            return false;
        }

        std::wcout
            << L"[TX SD-80 SysEx] Cable = "
            << static_cast<unsigned int>(cable)
            << L", Length = "
            << bytes.size()
            << L" bytes"
            << std::endl;

        size_t offset = 0;

        while (offset < bytes.size())
        {
            const size_t remaining = bytes.size() - offset;

            BYTE cin = 0;
            size_t count = 0;

            if (remaining > 3)
            {
                cin = 0x04;
                count = 3;
            }
            else
            {
                count = remaining;

                switch (count)
                {
                case 1: cin = 0x05; break;
                case 2: cin = 0x06; break;
                case 3: cin = 0x07; break;
                default:
                    return false;
                }
            }

            BYTE packet[4] =
            {
                static_cast<BYTE>(((cable & 0x0F) << 4) | cin),
                0,
                0,
                0
            };

            for (size_t i = 0; i < count; ++i)
            {
                packet[1 + i] = bytes[offset + i];
            }

            ULONG transferred = 0;

            const BOOL ok =
                WinUsb_WritePipe(
                    m_winusb,
                    SD80_EP_OUT,
                    packet,
                    sizeof(packet),
                    &transferred,
                    nullptr);

            if (!ok || transferred != sizeof(packet))
            {
                std::wcerr
                    << L"[SD-80] SysEx WinUsb_WritePipe FAILED. Error = "
                    << GetLastError()
                    << L", transferred = "
                    << transferred
                    << std::endl;
                return false;
            }

            std::wcout
                << L"  [TX SD-80 SysEx USB] "
                << std::uppercase
                << std::hex
                << std::setw(2) << std::setfill(L'0')
                << static_cast<unsigned int>(packet[0]) << L" "
                << std::setw(2)
                << static_cast<unsigned int>(packet[1]) << L" "
                << std::setw(2)
                << static_cast<unsigned int>(packet[2]) << L" "
                << std::setw(2)
                << static_cast<unsigned int>(packet[3])
                << std::dec
                << std::setfill(L' ')
                << std::endl;

            offset += count;
        }

        std::wcout
            << L"  SD-80 SysEx TX: OK"
            << std::endl;

        return true;
    }

    bool SendMidi1(
        BYTE cable,
        BYTE status,
        BYTE data1,
        BYTE data2)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_winusb == nullptr)
        {
            std::wcerr
                << L"[SD-80] Send requested but WinUSB is not open."
                << std::endl;
            return false;
        }

        const BYTE statusClass =
            static_cast<BYTE>(status & 0xF0);

        BYTE cin = 0;

        switch (statusClass)
        {
        case 0x80: cin = 0x08; break;
        case 0x90: cin = 0x09; break;
        case 0xA0: cin = 0x0A; break;
        case 0xB0: cin = 0x0B; break;
        case 0xC0: cin = 0x0C; break;
        case 0xD0: cin = 0x0D; break;
        case 0xE0: cin = 0x0E; break;
        default:
            std::wcerr
                << L"[SD-80] Unsupported MIDI status 0x"
                << std::hex
                << static_cast<unsigned int>(status)
                << std::dec
                << std::endl;
            return false;
        }

        // Cable 0 = SD-80 PART A for this first integration test.
        BYTE packet[4] =
        {
            static_cast<BYTE>(((cable & 0x0F) << 4) | (cin & 0x0F)),
            status,
            static_cast<BYTE>(data1 & 0x7F),
            static_cast<BYTE>(data2 & 0x7F)
        };

        ULONG transferred = 0;

        const BOOL ok =
            WinUsb_WritePipe(
                m_winusb,
                SD80_EP_OUT,
                packet,
                sizeof(packet),
                &transferred,
                nullptr);

        if (!ok)
        {
            std::wcerr
                << L"[SD-80] WinUsb_WritePipe FAILED. Error = "
                << GetLastError()
                << std::endl;
            return false;
        }

        std::wcout
            << L"[TX SD-80] USB-MIDI = "
            << std::uppercase
            << std::hex
            << std::setw(2) << std::setfill(L'0')
            << static_cast<unsigned int>(packet[0]) << L" "
            << std::setw(2)
            << static_cast<unsigned int>(packet[1]) << L" "
            << std::setw(2)
            << static_cast<unsigned int>(packet[2]) << L" "
            << std::setw(2)
            << static_cast<unsigned int>(packet[3])
            << std::dec
            << std::setfill(L' ')
            << L" ("
            << transferred
            << L" bytes)"
            << std::endl;

        return transferred == sizeof(packet);
    }

    void SetReceiveUmpCallback(
        std::function<void(std::vector<std::uint32_t> const&)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_receiveUmpCallback = std::move(callback);
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CloseUnlocked();
    }

    ~SD80UsbOutput()
    {
        Close();
    }

private:
    void StartReceiveThread()
    {
        if (m_rxThread.joinable())
        {
            return;
        }

        m_rxStop.store(false);

        std::wcout
            << L"[SD-80 RX] Starting USB-MIDI receive thread..."
            << std::endl
            << L"[SD-80 RX] IN PipeId = 0x81"
            << std::endl;

        m_rxThread = std::thread(
            [this]()
            {
                ReceiveLoop();
            });
    }

    void StopReceiveThread()
    {
        if (!m_rxThread.joinable())
        {
            return;
        }

        m_rxStop.store(true);

        if (m_winusb != nullptr)
        {
            // Cancel a pending WinUSB IN transfer so the receive thread can
            // leave WinUsb_ReadPipe() and join cleanly.
            WinUsb_AbortPipe(
                m_winusb,
                SD80_EP_IN);
        }

        m_rxThread.join();

        std::wcout
            << L"[SD-80 RX] USB-MIDI receive thread stopped."
            << std::endl;
    }

    void ReceiveLoop()
    {
        std::array<BYTE, 64> buffer{};

        while (!m_rxStop.load())
        {
            OVERLAPPED overlapped{};

            overlapped.hEvent =
                CreateEventW(
                    nullptr,
                    TRUE,
                    FALSE,
                    nullptr);

            if (overlapped.hEvent == nullptr)
            {
                std::wcerr
                    << L"[SD-80 RX] CreateEvent failed. Error = "
                    << GetLastError()
                    << std::endl;
                break;
            }

            ULONG transferred = 0;

            const BOOL ok =
                WinUsb_ReadPipe(
                    m_winusb,
                    SD80_EP_IN,
                    buffer.data(),
                    static_cast<ULONG>(buffer.size()),
                    &transferred,
                    &overlapped);

            if (!ok)
            {
                const DWORD error = GetLastError();

                if (error != ERROR_IO_PENDING)
                {
                    CloseHandle(overlapped.hEvent);

                    if (!m_rxStop.load())
                    {
                        std::wcerr
                            << L"[SD-80 RX] WinUsb_ReadPipe FAILED. Error = "
                            << error
                            << std::endl;
                    }

                    break;
                }

                const DWORD waitResult =
                    WaitForSingleObject(
                        overlapped.hEvent,
                        INFINITE);

                if (waitResult != WAIT_OBJECT_0)
                {
                    CloseHandle(overlapped.hEvent);
                    if (!m_rxStop.load())
                    {
                        std::wcerr
                            << L"[SD-80 RX] WaitForSingleObject FAILED. Error = "
                            << GetLastError()
                            << std::endl;
                    }
                    break;
                }

                if (!GetOverlappedResult(
                    m_deviceHandle,
                    &overlapped,
                    &transferred,
                    FALSE))
                {
                    const DWORD error = GetLastError();
                    CloseHandle(overlapped.hEvent);

                    if (!m_rxStop.load() &&
                        error != ERROR_OPERATION_ABORTED)
                    {
                        std::wcerr
                            << L"[SD-80 RX] GetOverlappedResult FAILED. Error = "
                            << error
                            << std::endl;
                    }

                    if (m_rxStop.load())
                    {
                        break;
                    }

                    continue;
                }
            }

            CloseHandle(overlapped.hEvent);

            if (m_rxStop.load())
            {
                break;
            }

            if (transferred == 0)
            {
                continue;
            }

            std::wcout
                << L"[RX SD-80 USB] Bytes = "
                << transferred
                << std::endl
                << L"  Raw: ";

            for (ULONG i = 0; i < transferred; ++i)
            {
                std::wcout
                    << std::uppercase
                    << std::hex
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<unsigned int>(buffer[i])
                    << L" "
                    << std::dec
                    << std::setfill(L' ');
            }

            std::wcout << std::endl;

            // USB-MIDI event packets are four bytes. Ignore empty/invalid
            // packets (CIN 0x0) produced by the SD-80 IN endpoint while idle.
            // Valid MIDI 1.0 packets are converted to UMP and forwarded to
            // the corresponding Virtual MIDI Group.
            for (ULONG offset = 0;
                offset + 4 <= transferred;
                offset += 4)
            {
                const BYTE packet0 = buffer[offset + 0];
                const BYTE cable =
                    static_cast<BYTE>((packet0 >> 4) & 0x0F);
                const BYTE cin =
                    static_cast<BYTE>(packet0 & 0x0F);
                const BYTE b1 = buffer[offset + 1];
                const BYTE b2 = buffer[offset + 2];
                const BYTE b3 = buffer[offset + 3];

                if (cin == 0x00)
                {
                    continue;
                }

                std::wcout
                    << L"  [RX SD-80 USB-MIDI] "
                    << std::uppercase
                    << std::hex
                    << std::setw(2) << std::setfill(L'0')
                    << static_cast<unsigned int>(packet0) << L" "
                    << std::setw(2) << static_cast<unsigned int>(b1) << L" "
                    << std::setw(2) << static_cast<unsigned int>(b2) << L" "
                    << std::setw(2) << static_cast<unsigned int>(b3)
                    << std::dec
                    << std::setfill(L' ')
                    << L"  Cable="
                    << static_cast<unsigned int>(cable)
                    << L" CIN=0x"
                    << std::hex << std::uppercase << std::setw(1)
                    << static_cast<unsigned int>(cin)
                    << std::dec
                    << std::endl;

                // The SD-80 physical MIDI IN 1 was experimentally confirmed
                // to appear as USB-MIDI Cable 2. For this bridge, USB Cable 2
                // maps to Virtual MIDI Group 2. Other cables are preserved as
                // their same-numbered groups.
                const BYTE group = cable;
                std::vector<std::uint32_t> ump;

                switch (cin)
                {
                case 0x08: // Note Off
                case 0x09: // Note On
                case 0x0A: // Poly Pressure
                case 0x0B: // Control Change
                case 0x0E: // Pitch Bend
                {
                    const std::uint32_t word0 =
                        (0x2u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        (static_cast<std::uint32_t>(b1) << 16) |
                        ((static_cast<std::uint32_t>(b2) & 0x7Fu) << 8) |
                        (static_cast<std::uint32_t>(b3) & 0x7Fu);
                    ump.push_back(word0);
                }
                break;

                case 0x0C: // Program Change
                case 0x0D: // Channel Pressure
                {
                    const std::uint32_t word0 =
                        (0x2u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        (static_cast<std::uint32_t>(b1) << 16) |
                        ((static_cast<std::uint32_t>(b2) & 0x7Fu) << 8);
                    ump.push_back(word0);
                }
                break;

                case 0x02: // System Common, 2 bytes
                {
                    const std::uint32_t word0 =
                        (0x1u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        (static_cast<std::uint32_t>(b1) << 16) |
                        ((static_cast<std::uint32_t>(b2) & 0x7Fu) << 8);
                    ump.push_back(word0);
                }
                break;

                case 0x03: // System Common, 3 bytes
                {
                    const std::uint32_t word0 =
                        (0x1u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        (static_cast<std::uint32_t>(b1) << 16) |
                        ((static_cast<std::uint32_t>(b2) & 0x7Fu) << 8) |
                        (static_cast<std::uint32_t>(b3) & 0x7Fu);
                    ump.push_back(word0);
                }
                break;

                case 0x04: // SysEx7 start/continue, 3 bytes
                case 0x05: // SysEx7 end, 1 byte
                case 0x06: // SysEx7 end, 2 bytes
                case 0x07: // SysEx7 end, 3 bytes
                {
                    BYTE sysexStatus = 0;
                    BYTE byteCount = 0;

                    if (cin == 0x04)
                    {
                        sysexStatus = (b1 == 0xF0) ? 1 : 2;
                        byteCount = 3;
                    }
                    else
                    {
                        sysexStatus = 3;
                        byteCount = static_cast<BYTE>(cin - 0x04);
                    }

                    const std::uint32_t word0 =
                        (0x3u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        ((static_cast<std::uint32_t>(sysexStatus) & 0x0Fu) << 20) |
                        ((static_cast<std::uint32_t>(byteCount) & 0x0Fu) << 16);

                    const std::uint32_t word1 =
                        (static_cast<std::uint32_t>(b1) << 24) |
                        (static_cast<std::uint32_t>(b2) << 16) |
                        (static_cast<std::uint32_t>(b3) << 8);

                    ump.push_back(word0);
                    ump.push_back(word1);
                }
                break;

                case 0x0F: // Single-byte system/realtime message
                {
                    const std::uint32_t word0 =
                        (0x1u << 28) |
                        ((static_cast<std::uint32_t>(group) & 0x0Fu) << 24) |
                        (static_cast<std::uint32_t>(b1) << 16);
                    ump.push_back(word0);
                }
                break;

                default:
                    std::wcout
                        << L"  [RX SD-80 USB-MIDI] Unsupported CIN=0x"
                        << std::hex << std::uppercase
                        << static_cast<unsigned int>(cin)
                        << std::dec << std::endl;
                    break;
                }

                if (!ump.empty())
                {
                    std::function<void(std::vector<std::uint32_t> const&)> callback;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        callback = m_receiveUmpCallback;
                    }

                    if (callback)
                    {
                        callback(ump);
                    }
                }
            }

            if ((transferred % 4) != 0)
            {
                std::wcout
                    << L"  [RX SD-80 USB-MIDI] WARNING: "
                    << L"transfer length is not a multiple of 4."
                    << std::endl;
            }
        }
    }

    void CloseUnlocked()
    {
        StopReceiveThread();

        if (m_winusb != nullptr)
        {
            WinUsb_Free(m_winusb);
            m_winusb = nullptr;
        }

        if (m_deviceHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_deviceHandle);
            m_deviceHandle = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE m_deviceHandle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE m_winusb = nullptr;
    std::mutex m_mutex;
    std::function<void(std::vector<std::uint32_t> const&)> m_receiveUmpCallback;
    std::atomic<bool> m_rxStop{ false };
    std::thread m_rxThread;
};

static bool EnableSeDebugPrivilege()
{
    HANDLE token = nullptr;

    if (!OpenProcessToken(
        GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        &token))
    {
        std::wcerr
            << L"[MidiSrv] OpenProcessToken failed. Error = "
            << GetLastError()
            << std::endl;

        return false;
    }

    TOKEN_PRIVILEGES tp{};
    LUID luid{};

    if (!LookupPrivilegeValueW(
        nullptr,
        SE_DEBUG_NAME,
        &luid))
    {
        std::wcerr
            << L"[MidiSrv] LookupPrivilegeValueW failed. Error = "
            << GetLastError()
            << std::endl;

        CloseHandle(token);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(
        token,
        FALSE,
        &tp,
        sizeof(tp),
        nullptr,
        nullptr))
    {
        std::wcerr
            << L"[MidiSrv] AdjustTokenPrivileges failed. Error = "
            << GetLastError()
            << std::endl;

        CloseHandle(token);
        return false;
    }

    DWORD error = GetLastError();

    CloseHandle(token);

    if (error == ERROR_NOT_ALL_ASSIGNED)
    {
        std::wcerr
            << L"[MidiSrv] SeDebugPrivilege was not assigned."
            << std::endl;

        return false;
    }

    std::wcout
        << L"[MidiSrv] SeDebugPrivilege enabled."
        << std::endl;

    return true;
}

// ============================================================================
// MIDI Services service reset workaround
// ============================================================================
static bool ResetMidiSrv()
{
    std::wcout << std::endl
        << L"========================================" << std::endl
        << L"Resetting MIDI Services (MidiSrv)..." << std::endl
        << L"========================================" << std::endl;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
    {
        std::wcerr << L"[MidiSrv] OpenSCManagerW failed. Error = "
            << GetLastError() << std::endl;
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        scm,
        L"MidiSrv",
        SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START);

    if (!service)
    {
        std::wcerr << L"[MidiSrv] OpenServiceW failed. Error = "
            << GetLastError() << std::endl;
        CloseServiceHandle(scm);
        return false;
    }

    auto QueryStatus = [&]() -> SERVICE_STATUS_PROCESS
        {
            SERVICE_STATUS_PROCESS status{};
            DWORD bytesNeeded = 0;

            if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytesNeeded))
            {
                status.dwCurrentState = 0;
            }

            return status;
        };

    auto PrintState = [](DWORD state)
        {
            switch (state)
            {
            case SERVICE_STOPPED:       std::wcout << L"STOPPED"; break;
            case SERVICE_START_PENDING: std::wcout << L"START_PENDING"; break;
            case SERVICE_STOP_PENDING:  std::wcout << L"STOP_PENDING"; break;
            case SERVICE_RUNNING:       std::wcout << L"RUNNING"; break;
            default:                    std::wcout << L"STATE=" << state; break;
            }

            std::wcout << std::endl;
        };

    // Normal service stop can complete quickly.  A timeout is the known
    // MidiSrv failure mode, so a forced termination is retained as a
    // deliberate fallback rather than removing the workaround.
    const DWORD pollMs = 250;
    const DWORD stopTimeoutMs = 10000;

    // After a forced termination, do not immediately start MidiSrv again.
    // Give SCM enough time to finish the terminated service instance and
    // apply any configured recovery action before deciding that STOPPED is
    // stable.
    const DWORD forcedTerminateWaitMs = 5000;

    // Require several consecutive STOPPED observations.  This is intentionally
    // stronger than a single QueryServiceStatusEx() result because the failure
    // we are working around occurs during repeated MidiSrv starts.
    const int stableStoppedSamples = 8;

    // Starting the service is followed by the same kind of stable-state check.
    const DWORD startTimeoutMs = 10000;
    const int stableRunningSamples = 4;

    SERVICE_STATUS_PROCESS status = QueryStatus();

    std::wcout << L"[MidiSrv] Current state = ";
    PrintState(status.dwCurrentState);

    bool forcedTerminationOccurred = false;

    // ------------------------------------------------------------------------
    // Stop MidiSrv completely.
    // ------------------------------------------------------------------------
    if (status.dwCurrentState != SERVICE_STOPPED)
    {
        std::wcout << L"[MidiSrv] Stopping service..." << std::endl;

        SERVICE_STATUS serviceStatus{};

        if (!ControlService(
            service,
            SERVICE_CONTROL_STOP,
            &serviceStatus))
        {
            const DWORD error = GetLastError();

            if (error != ERROR_SERVICE_NOT_ACTIVE)
            {
                std::wcerr
                    << L"[MidiSrv] ControlService(STOP) failed. Error = "
                    << error << std::endl;
            }
        }
    }

    DWORD elapsed = 0;

    for (;;)
    {
        status = QueryStatus();

        if (status.dwCurrentState == SERVICE_STOPPED)
        {
            std::wcout << L"[MidiSrv] Service stopped." << std::endl;
            break;
        }

        if (elapsed >= stopTimeoutMs)
        {
            if (status.dwProcessId == 0)
            {
                std::wcerr
                    << L"[MidiSrv] Stop timeout but no service PID is available."
                    << std::endl;

                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return false;
            }

            std::wcout
                << L"[MidiSrv] Stop timeout. Attempting forced termination..."
                << std::endl
                << L"[MidiSrv] Service PID = "
                << status.dwProcessId
                << std::endl;

            // Enable debug privilege before opening MidiSrv for termination.
            if (!EnableSeDebugPrivilege())
            {
                std::wcerr
                    << L"[MidiSrv] Failed to enable SeDebugPrivilege."
                    << std::endl;

                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return false;
            }

            HANDLE process = OpenProcess(
                PROCESS_TERMINATE | SYNCHRONIZE,
                FALSE,
                status.dwProcessId);

            if (!process)
            {
                std::wcerr
                    << L"[MidiSrv] OpenProcess failed. Error = "
                    << GetLastError()
                    << std::endl;

                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return false;
            }

            if (!TerminateProcess(process, 0))
            {
                const DWORD error = GetLastError();

                std::wcerr
                    << L"[MidiSrv] TerminateProcess failed. Error = "
                    << error
                    << std::endl;

                CloseHandle(process);
                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return false;
            }

            forcedTerminationOccurred = true;

            // Wait for the actual process object to terminate.  This is
            // separate from the SCM service state.
            WaitForSingleObject(process, 5000);
            CloseHandle(process);

            std::wcout
                << L"[MidiSrv] Forced termination requested."
                << std::endl;

            // Do not immediately call StartService().  First allow SCM and
            // MidiSrv recovery handling to finish.
            std::wcout
                << L"[MidiSrv] Waiting after forced termination..."
                << std::endl;

            Sleep(forcedTerminateWaitMs);
            break;
        }

        Sleep(pollMs);
        elapsed += pollMs;
    }

    // ------------------------------------------------------------------------
    // Establish a genuinely stable STOPPED state.
    //
    // If SCM automatically starts MidiSrv again, stop that new instance and
    // begin the stability check again.  We require multiple consecutive
    // STOPPED observations so that an automatic recovery restart cannot race
    // with our StartService() call.
    // ------------------------------------------------------------------------
    int stoppedSamples = 0;

    for (;;)
    {
        status = QueryStatus();

        if (status.dwCurrentState == SERVICE_STOPPED)
        {
            ++stoppedSamples;

            if (stoppedSamples >= stableStoppedSamples)
            {
                std::wcout
                    << L"[MidiSrv] Service is stably STOPPED."
                    << std::endl;
                break;
            }
        }
        else
        {
            stoppedSamples = 0;

            if (status.dwCurrentState == SERVICE_RUNNING)
            {
                std::wcout
                    << L"[MidiSrv] MidiSrv restarted automatically."
                    << std::endl
                    << L"[MidiSrv] New service PID = "
                    << status.dwProcessId
                    << std::endl;

                SERVICE_STATUS serviceStatus{};

                if (!ControlService(
                    service,
                    SERVICE_CONTROL_STOP,
                    &serviceStatus))
                {
                    const DWORD error = GetLastError();

                    if (error != ERROR_SERVICE_NOT_ACTIVE)
                    {
                        std::wcerr
                            << L"[MidiSrv] ControlService(STOP) after automatic "
                            L"restart failed. Error = "
                            << error
                            << std::endl;
                    }
                }
            }
        }

        Sleep(pollMs);
    }

    // ------------------------------------------------------------------------
    // Start a fresh MidiSrv instance.
    // ------------------------------------------------------------------------
    std::wcout << L"[MidiSrv] Starting service..." << std::endl;

    if (!StartServiceW(service, 0, nullptr))
    {
        const DWORD error = GetLastError();

        if (error != ERROR_SERVICE_ALREADY_RUNNING)
        {
            std::wcerr
                << L"[MidiSrv] StartServiceW failed. Error = "
                << error
                << std::endl;

            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }
    }

    // ------------------------------------------------------------------------
    // Wait for RUNNING and require several consecutive RUNNING observations.
    // This is especially important after a forced termination.
    // ------------------------------------------------------------------------
    elapsed = 0;
    int runningSamples = 0;

    for (;;)
    {
        status = QueryStatus();

        if (status.dwCurrentState == SERVICE_RUNNING)
        {
            ++runningSamples;

            if (runningSamples >= stableRunningSamples)
            {
                std::wcout
                    << L"[MidiSrv] Service is RUNNING."
                    << std::endl;
                break;
            }
        }
        else
        {
            runningSamples = 0;
        }

        if (elapsed >= startTimeoutMs)
        {
            std::wcerr
                << L"[MidiSrv] Start timeout. Final state = ";
            PrintState(status.dwCurrentState);

            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }

        Sleep(pollMs);
        elapsed += pollMs;
    }

    // Keep the diagnostic explicit: this tells us whether this invocation
    // used the known forced-termination workaround.
    if (forcedTerminationOccurred)
    {
        std::wcout
            << L"[MidiSrv] Reset completed after forced termination."
            << std::endl;
    }
    else
    {
        std::wcout
            << L"[MidiSrv] Reset completed after normal service stop."
            << std::endl;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return true;
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    // Temporary MidiSrv workaround: reset the service before initializing
    // Windows MIDI Services or creating the Virtual MIDI endpoint.
    if (!ResetMidiSrv())
    {
        std::wcerr << L"ERROR: MidiSrv reset failed. Cannot continue safely." << std::endl;
        return 5;
    }

    try
    {
        // --------------------------------------------------------------------
        // Initialize WinRT
        // --------------------------------------------------------------------

        winrt::init_apartment();

        std::wcout
            << L"SD80VirtualMidi starting..."
            << std::endl;

        // --------------------------------------------------------------------
        // Initialize Windows MIDI Services SDK Runtime
        // --------------------------------------------------------------------

        std::wcout
            << L"Creating MIDI Services SDK initializer..."
            << std::endl;

        ::Microsoft::Windows::Devices::Midi2::Initialization::
            MidiDesktopAppSdkInitializer midiInitializer;

        std::wcout
            << L"Initializing MIDI Services SDK Runtime..."
            << std::endl;

        if (!midiInitializer.InitializeSdkRuntime())
        {
            std::wcerr
                << L"ERROR: InitializeSdkRuntime() FAILED."
                << std::endl;

            return 20;
        }

        std::wcout
            << L"MIDI Services SDK Runtime initialized."
            << std::endl;

        // --------------------------------------------------------------------
        // Check MIDI Services installation
        // --------------------------------------------------------------------

        std::wcout
            << L"Checking MIDI Services installation..."
            << std::endl;

        if (!midiInitializer.IsServiceInstalled())
        {
            std::wcerr
                << L"ERROR: Windows MIDI Services is not installed."
                << std::endl;

            return 21;
        }

        std::wcout
            << L"Windows MIDI Services is installed."
            << std::endl;

        // --------------------------------------------------------------------
        // Ensure MIDI Services is available
        // --------------------------------------------------------------------

        std::wcout
            << L"Ensuring MIDI Services is available..."
            << std::endl;

        if (!midiInitializer.EnsureServiceAvailable())
        {
            std::wcerr
                << L"ERROR: EnsureServiceAvailable() FAILED."
                << std::endl;

            return 22;
        }

        std::wcout
            << L"MIDI Services is available."
            << std::endl;

        // --------------------------------------------------------------------
        // Check Virtual MIDI Device transport
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Checking Virtual MIDI Device transport..."
            << std::endl
            << L"========================================"
            << std::endl;

        bool transportAvailable = false;

        try
        {
            transportAvailable =
                virt::MidiVirtualDeviceManager::
                IsTransportAvailable();
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wcerr
                << L"Virtual MIDI Device transport check EXCEPTION"
                << std::endl;

            std::wcerr
                << L"HRESULT = 0x"
                << std::hex
                << static_cast<unsigned long>(ex.code().value)
                << std::dec
                << std::endl;

            std::wcerr
                << L"Message = "
                << ex.message().c_str()
                << std::endl;

            return 30;
        }

        std::wcout
            << L"Virtual MIDI Device transport available = "
            << (transportAvailable
                ? L"true"
                : L"false")
            << std::endl;

        if (!transportAvailable)
        {
            std::wcerr
                << L"ERROR: Virtual MIDI Device transport is not available."
                << std::endl;

            return 31;
        }

        // --------------------------------------------------------------------
        // Open SD-80 WinUSB output
        // --------------------------------------------------------------------

        auto sd80Output = std::make_shared<SD80UsbOutput>();

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Opening SD-80 USB output..."
            << std::endl
            << L"========================================"
            << std::endl;

        if (!sd80Output->Open())
        {
            std::wcerr
                << L"ERROR: SD-80 USB output could not be opened."
                << std::endl;

            return 33;
        }

        std::wcout
            << L"SD-80 USB output ready."
            << std::endl;

        // --------------------------------------------------------------------
        // Get Virtual MIDI Device transport ID
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Getting Virtual MIDI Device transport ID..."
            << std::endl;

        winrt::guid transportId{};

        try
        {
            transportId =
                virt::MidiVirtualDeviceManager::
                TransportId();
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wcerr
                << L"TransportId() EXCEPTION"
                << std::endl;

            std::wcerr
                << L"HRESULT = 0x"
                << std::hex
                << static_cast<unsigned long>(ex.code().value)
                << std::dec
                << std::endl;

            std::wcerr
                << L"Message = "
                << ex.message().c_str()
                << std::endl;

            return 32;
        }

        std::wcout
            << L"Virtual MIDI Device Transport ID = "
            << winrt::to_hstring(transportId).c_str()
            << std::endl;

        // --------------------------------------------------------------------
        // Create a unique ProductInstanceId
        //
        // Windows MIDI Services requires the ProductInstanceId to be unique
        // among currently running Virtual UMP Devices.
        //
        // We use the current process ID so that every running instance gets
        // a different software device ID.
        // --------------------------------------------------------------------

        const DWORD processId =
            ::GetCurrentProcessId();

        std::wstringstream productInstanceIdStream;

        productInstanceIdStream
            << L"SD80BridgeVM"
            << processId;

        const std::wstring productInstanceId =
            productInstanceIdStream.str();

        std::wcout
            << std::endl
            << L"Virtual MIDI ProductInstanceId:"
            << std::endl
            << L"  "
            << productInstanceId
            << std::endl;

        // --------------------------------------------------------------------
        // Create declared endpoint information
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Creating virtual MIDI device configuration..."
            << std::endl;

        midi2::MidiDeclaredEndpointInfo declaredEndpointInfo{};

        declaredEndpointInfo.Name =
            L"SD-80 Bridge Virtual MIDI";

        declaredEndpointInfo.ProductInstanceId =
            productInstanceId;

        declaredEndpointInfo.SpecificationVersionMajor =
            1;

        declaredEndpointInfo.SpecificationVersionMinor =
            1;

        declaredEndpointInfo.SupportsMidi10Protocol =
            true;

        declaredEndpointInfo.SupportsMidi20Protocol =
            true;

        declaredEndpointInfo.SupportsReceivingJitterReductionTimestamps =
            false;

        declaredEndpointInfo.SupportsSendingJitterReductionTimestamps =
            false;

        declaredEndpointInfo.HasStaticFunctionBlocks =
            true;

        // --------------------------------------------------------------------
        // Device identity
        // --------------------------------------------------------------------

        midi2::MidiDeclaredDeviceIdentity declaredDeviceIdentity{};

        // --------------------------------------------------------------------
        // User supplied information
        // --------------------------------------------------------------------

        midi2::MidiEndpointUserSuppliedInfo userSuppliedInfo{};

        userSuppliedInfo.Name =
            L"SD-80 Bridge Virtual MIDI";

        userSuppliedInfo.Description =
            L"Virtual MIDI endpoint for Roland SD-80 Bridge";

        // --------------------------------------------------------------------
        // Create virtual MIDI device configuration
        // --------------------------------------------------------------------

        virt::MidiVirtualDeviceCreationConfig config(
            L"SD-80 Bridge Virtual MIDI",
            L"Virtual MIDI endpoint for Roland SD-80 Bridge",
            L"masahii",
            declaredEndpointInfo,
            declaredDeviceIdentity,
            userSuppliedInfo
        );

        config.CreateOnlyUmpEndpoints(false);

        // --------------------------------------------------------------------
        // Create MIDI Function Block
        //
        // FirstGroupIndex() is intentionally not used because it is not
        // present in the C++/WinRT projection generated by the installed
        // SDK version 1.0.17-rc.4.25 on this PC.
        // --------------------------------------------------------------------

        midi2::MidiFunctionBlock block{};

        block.Number(0);

        block.Name(
            L"SD-80 Bridge"
        );

        block.IsActive(true);

        block.UIHint(
            midi2::MidiFunctionBlockUIHint::Sender
        );

        block.GroupCount(4);

        block.Direction(
            midi2::MidiFunctionBlockDirection::Bidirectional
        );

        block.RepresentsMidi10Connection(
            midi2::MidiFunctionBlockRepresentsMidi10Connection::Not10
        );

        block.MaxSystemExclusive8Streams(0);

        block.MidiCIMessageVersionFormat(0);

        config.FunctionBlocks().Append(block);

        std::wcout
            << L"Virtual MIDI device configuration created."
            << std::endl;

        // --------------------------------------------------------------------
        // Create MIDI Session
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Creating MIDI Session..."
            << std::endl;

        auto session =
            midi2::MidiSession::Create(
                config.Name()
            );

        if (session == nullptr)
        {
            std::wcerr
                << L"ERROR: MidiSession::Create() returned null."
                << std::endl;

            return 40;
        }

        std::wcout
            << L"MIDI Session created."
            << std::endl;

        // --------------------------------------------------------------------
        // Create Virtual MIDI Device
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Creating Virtual MIDI Device..."
            << std::endl;

        auto virtualDevice =
            virt::MidiVirtualDeviceManager::
            CreateVirtualDevice(
                config
            );

        if (virtualDevice == nullptr)
        {
            std::wcerr
                << L"ERROR: CreateVirtualDevice() returned null."
                << std::endl;

            return 41;
        }

        std::wcout
            << L"Virtual MIDI Device created."
            << std::endl;

        // --------------------------------------------------------------------
        // Get device-side Endpoint Device ID
        // --------------------------------------------------------------------

        auto endpointDeviceId =
            virtualDevice.DeviceEndpointDeviceId();
        std::wcout
            << L"Virtual MIDI device-side EndpointDeviceId:"
            << std::endl
            << L"  "
            << endpointDeviceId.c_str()
            << std::endl;

        // --------------------------------------------------------------------
        // Create endpoint connection
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Creating endpoint connection..."
            << std::endl;

        auto connection =
            session.CreateEndpointConnection(
                endpointDeviceId
            );

        if (connection == nullptr)
        {
            std::wcerr
                << L"ERROR: CreateEndpointConnection() returned null."
                << std::endl;

            return 42;
        }

        std::wcout
            << L"Endpoint connection created."
            << std::endl;

        // --------------------------------------------------------------------
        // Get MIDI message received event source
        // --------------------------------------------------------------------

        auto messageSource =
            connection.as<
            midi2::IMidiMessageReceivedEventSource
            >();

        // --------------------------------------------------------------------
        // Route SD-80 USB IN MIDI back into the corresponding Virtual MIDI Group.
        // SD-80 physical MIDI IN 1 was measured as USB-MIDI Cable 2, so Cable 2
        // is returned to Group 2. The same-number mapping is retained for the
        // other USB cables.
        // --------------------------------------------------------------------

        sd80Output->SetReceiveUmpCallback(
            [connection](std::vector<std::uint32_t> const& words)
            {
                if (words.empty() || words.size() > 4)
                {
                    return;
                }

                auto timestamp =
                    midi2::MidiClock::TimestampConstantSendImmediately();

                midi2::MidiSendMessageResults result =
                    midi2::MidiSendMessageResults::Failed;

                switch (words.size())
                {
                case 1:
                    result = connection.SendSingleMessageWords(
                        timestamp, words[0]);
                    break;
                case 2:
                    result = connection.SendSingleMessageWords(
                        timestamp, words[0], words[1]);
                    break;
                case 3:
                    result = connection.SendSingleMessageWords(
                        timestamp, words[0], words[1], words[2]);
                    break;
                case 4:
                    result = connection.SendSingleMessageWords(
                        timestamp, words[0], words[1], words[2], words[3]);
                    break;
                default:
                    return;
                }

                std::wcout
                    << L"  [ROUTE] SD-80 USB IN Cable -> Virtual MIDI Group"
                    << std::endl;

                if (midi2::MidiEndpointConnection::SendMessageSucceeded(result))
                {
                    std::wcout
                        << L"  [ROUTE] SD-80 -> Virtual MIDI : OK"
                        << std::endl;
                }
                else
                {
                    std::wcerr
                        << L"  [ROUTE] SD-80 -> Virtual MIDI : FAILED result=0x"
                        << std::hex << std::uppercase
                        << static_cast<std::uint32_t>(result)
                        << std::dec
                        << std::endl;
                }
            }
        );

        // --------------------------------------------------------------------
        // Register MIDI MessageReceived handler
        // --------------------------------------------------------------------

        std::wcout
            << L"Registering MIDI MessageReceived handler..."
            << std::endl;

        // --------------------------------------------------------------------
        // SysEx 7 assembly state.
        //
        // A MIDI 1.0 SysEx can arrive as multiple 64-bit UMP Data Messages.
        // Keep one byte buffer per UMP Group so Group 0-3 remain independent.
        // --------------------------------------------------------------------

        auto sysexBuffers =
            std::make_shared<
            std::array<std::vector<BYTE>, 16>
            >();

        auto sysexActive =
            std::make_shared<
            std::array<bool, 16>
            >();

        sysexActive->fill(false);

        auto messageReceivedToken =
            messageSource.MessageReceived(
                [sd80Output, sysexBuffers, sysexActive](
                    winrt::Windows::Foundation::IInspectable const&,
                    midi2::MidiMessageReceivedEventArgs const& args
                    )
                {
                    try
                    {
                        // ----------------------------------------------------
                        // FillWords() requires writable references.
                        // word0 therefore must NOT be const.
                        // ----------------------------------------------------

                        std::uint32_t word0 =
                            args.PeekFirstWord();

                        std::uint32_t word1 = 0;
                        std::uint32_t word2 = 0;
                        std::uint32_t word3 = 0;

                        auto wordCount =
                            args.FillWords(
                                word0,
                                word1,
                                word2,
                                word3
                            );

                        // ----------------------------------------------------
                        // Display received UMP
                        // ----------------------------------------------------

                        std::wcout
                            << std::endl
                            << L"[RX Virtual MIDI]"
                            << std::endl;

                        std::wcout
                            << L"  WordCount = "
                            << wordCount
                            << std::endl;

                        std::wcout
                            << L"  Word0     = 0x"
                            << std::hex
                            << std::uppercase
                            << std::setw(8)
                            << std::setfill(L'0')
                            << word0
                            << std::dec
                            << std::nouppercase
                            << std::setfill(L' ')
                            << std::endl;

                        if (wordCount >= 2)
                        {
                            std::wcout
                                << L"  Word1     = 0x"
                                << std::hex
                                << std::uppercase
                                << std::setw(8)
                                << std::setfill(L'0')
                                << word1
                                << std::dec
                                << std::nouppercase
                                << std::setfill(L' ')
                                << std::endl;
                        }

                        if (wordCount >= 3)
                        {
                            std::wcout
                                << L"  Word2     = 0x"
                                << std::hex
                                << std::uppercase
                                << std::setw(8)
                                << std::setfill(L'0')
                                << word2
                                << std::dec
                                << std::nouppercase
                                << std::setfill(L' ')
                                << std::endl;
                        }

                        if (wordCount >= 4)
                        {
                            std::wcout
                                << L"  Word3     = 0x"
                                << std::hex
                                << std::uppercase
                                << std::setw(8)
                                << std::setfill(L'0')
                                << word3
                                << std::dec
                                << std::nouppercase
                                << std::setfill(L' ')
                                << std::endl;
                        }

                        // ----------------------------------------------------
                        // Decode UMP message type
                        // ----------------------------------------------------

                        const std::uint32_t messageType =
                            (word0 >> 28) & 0x0F;

                        std::wcout
                            << L"  MessageType = 0x"
                            << std::hex
                            << messageType
                            << std::dec
                            << std::endl;

                        // ----------------------------------------------------
                        // MIDI 1.0 System Exclusive (SysEx 7)
                        //
                        // UMP Message Type = 0x3 (Data Message 64)
                        //
                        // Each UMP carries up to 6 SysEx bytes. The four status
                        // values are:
                        //   0 = complete
                        //   1 = start
                        //   2 = continue
                        //   3 = end
                        //
                        // We assemble the MIDI 1.0 byte stream first, log it,
                        // and then send the complete F0 ... F7 stream to the
                        // corresponding SD-80 USB-MIDI cable.
                        // ----------------------------------------------------

                        if (messageType == 0x3 && wordCount >= 2)
                        {
                            const std::uint32_t group =
                                (word0 >> 24) & 0x0F;

                            const std::uint32_t sysexStatus =
                                (word0 >> 20) & 0x0F;

                            const std::uint32_t byteCount =
                                (word0 >> 16) & 0x0F;

                            std::vector<BYTE> payload;

                            const std::uint32_t payloadWords[2] =
                            {
                                word0,
                                word1
                            };

                            // First two bytes are in word0 bits 15..0.
                            if (byteCount >= 1)
                            {
                                payload.push_back(
                                    static_cast<BYTE>((word0 >> 8) & 0xFF)
                                );
                            }

                            if (byteCount >= 2)
                            {
                                payload.push_back(
                                    static_cast<BYTE>(word0 & 0xFF)
                                );
                            }

                            // Remaining four bytes are in word1.
                            if (byteCount >= 3)
                            {
                                payload.push_back(
                                    static_cast<BYTE>((word1 >> 24) & 0xFF)
                                );
                            }

                            if (byteCount >= 4)
                            {
                                payload.push_back(
                                    static_cast<BYTE>((word1 >> 16) & 0xFF)
                                );
                            }

                            if (byteCount >= 5)
                            {
                                payload.push_back(
                                    static_cast<BYTE>((word1 >> 8) & 0xFF)
                                );
                            }

                            if (byteCount >= 6)
                            {
                                payload.push_back(
                                    static_cast<BYTE>(word1 & 0xFF)
                                );
                            }

                            std::wcout
                                << L"  [RX SysEx7 UMP]"
                                << std::endl
                                << L"    Group      = "
                                << group
                                << std::endl
                                << L"    Status     = "
                                << sysexStatus
                                << std::endl
                                << L"    ByteCount  = "
                                << byteCount
                                << std::endl;

                            PrintHexBytes(
                                L"    Payload: ",
                                payload
                            );

                            if (group < sysexBuffers->size())
                            {
                                auto& buffer =
                                    (*sysexBuffers)[group];

                                switch (sysexStatus)
                                {
                                case 0: // Complete
                                    buffer.clear();
                                    buffer.insert(
                                        buffer.end(),
                                        payload.begin(),
                                        payload.end()
                                    );

                                    (*sysexActive)[group] = false;

                                    std::wcout
                                        << L"  [SysEx] COMPLETE"
                                        << std::endl;

                                    PrintHexBytes(
                                        L"  [RX SysEx complete] ",
                                        buffer
                                    );

                                    // UMP SysEx7 payloads do not contain the MIDI 1.0
                                    // framing bytes F0/F7.  The complete UMP therefore
                                    // contains only the SysEx data bytes (for example
                                    // 7D 01 02 03 04).  Reconstruct the MIDI 1.0 stream
                                    // before sending it to the SD-80 USB-MIDI endpoint.
                                    if (!buffer.empty())
                                    {
                                        std::vector<BYTE> framed;
                                        framed.reserve(buffer.size() + 2);
                                        framed.push_back(0xF0);
                                        framed.insert(
                                            framed.end(),
                                            buffer.begin(),
                                            buffer.end()
                                        );
                                        framed.push_back(0xF7);

                                        PrintHexBytes(
                                            L"  [TX MIDI 1.0 SysEx] ",
                                            framed
                                        );

                                        if (sd80Output->SendSysEx(
                                            static_cast<BYTE>(group),
                                            framed))
                                        {
                                            std::wcout
                                                << L"  [ROUTE] SysEx Group "
                                                << group
                                                << L" -> SD-80 Cable "
                                                << group
                                                << L" : OK"
                                                << std::endl;
                                        }
                                        else
                                        {
                                            std::wcerr
                                                << L"  [ROUTE] SysEx Group "
                                                << group
                                                << L" -> SD-80 Cable "
                                                << group
                                                << L" : FAILED"
                                                << std::endl;
                                        }
                                    }
                                    else
                                    {
                                        std::wcerr
                                            << L"  [SysEx] COMPLETE packet contained "
                                            << L"no data bytes."
                                            << std::endl;
                                    }
                                    break;

                                case 1: // Start
                                    buffer.clear();
                                    buffer.insert(
                                        buffer.end(),
                                        payload.begin(),
                                        payload.end()
                                    );

                                    (*sysexActive)[group] = true;

                                    std::wcout
                                        << L"  [SysEx] START"
                                        << std::endl;
                                    break;

                                case 2: // Continue
                                    if (!(*sysexActive)[group])
                                    {
                                        std::wcerr
                                            << L"  [SysEx] CONTINUE without "
                                            L"active START; buffer reset."
                                            << std::endl;
                                        buffer.clear();
                                    }

                                    buffer.insert(
                                        buffer.end(),
                                        payload.begin(),
                                        payload.end()
                                    );

                                    (*sysexActive)[group] = true;

                                    std::wcout
                                        << L"  [SysEx] CONTINUE, assembled = "
                                        << buffer.size()
                                        << L" bytes"
                                        << std::endl;
                                    break;

                                case 3: // End
                                    if (!(*sysexActive)[group])
                                    {
                                        std::wcerr
                                            << L"  [SysEx] END without "
                                            L"active START; buffer reset."
                                            << std::endl;
                                        buffer.clear();
                                    }

                                    buffer.insert(
                                        buffer.end(),
                                        payload.begin(),
                                        payload.end()
                                    );

                                    (*sysexActive)[group] = false;

                                    std::wcout
                                        << L"  [SysEx] END"
                                        << std::endl;

                                    PrintHexBytes(
                                        L"  [RX SysEx complete] ",
                                        buffer
                                    );

                                    // As with UMP COMPLETE, an UMP SysEx7 END
                                    // packet contains only SysEx data bytes.  Add
                                    // the MIDI 1.0 F0/F7 framing before USB output.
                                    if (!buffer.empty())
                                    {
                                        std::vector<BYTE> framed;
                                        framed.reserve(buffer.size() + 2);
                                        framed.push_back(0xF0);
                                        framed.insert(
                                            framed.end(),
                                            buffer.begin(),
                                            buffer.end()
                                        );
                                        framed.push_back(0xF7);

                                        PrintHexBytes(
                                            L"  [TX MIDI 1.0 SysEx] ",
                                            framed
                                        );

                                        if (sd80Output->SendSysEx(
                                            static_cast<BYTE>(group),
                                            framed))
                                        {
                                            std::wcout
                                                << L"  [ROUTE] SysEx Group "
                                                << group
                                                << L" -> SD-80 Cable "
                                                << group
                                                << L" : OK"
                                                << std::endl;
                                        }
                                        else
                                        {
                                            std::wcerr
                                                << L"  [ROUTE] SysEx Group "
                                                << group
                                                << L" -> SD-80 Cable "
                                                << group
                                                << L" : FAILED"
                                                << std::endl;
                                        }
                                    }
                                    else
                                    {
                                        std::wcerr
                                            << L"  [SysEx] END result contained "
                                            L"no data bytes."
                                            << std::endl;
                                    }
                                    break;

                                default:
                                    std::wcerr
                                        << L"  [SysEx] Unknown status = "
                                        << sysexStatus
                                        << std::endl;
                                    break;
                                }
                            }

                            // Do not interpret SysEx7 as a channel voice message.
                            return;
                        }

                        // ----------------------------------------------------
                        // MIDI 1.0 Channel Voice Message
                        //
                        // UMP Message Type = 0x2
                        // ----------------------------------------------------

                        if (messageType == 0x2)
                        {
                            const std::uint32_t group =
                                (word0 >> 24) & 0x0F;

                            const std::uint32_t statusByte =
                                (word0 >> 16) & 0xFF;

                            const std::uint32_t channel =
                                statusByte & 0x0F;

                            const std::uint32_t status =
                                statusByte & 0xF0;

                            const std::uint32_t data1 =
                                (word0 >> 8) & 0x7F;

                            const std::uint32_t data2 =
                                word0 & 0x7F;

                            std::wcout
                                << L"  Group       = "
                                << group
                                << std::endl;

                            std::wcout
                                << L"  Channel     = "
                                << (channel + 1)
                                << std::endl;

                            std::wcout
                                << L"  Status      = 0x"
                                << std::hex
                                << status
                                << std::dec
                                << std::endl;

                            std::wcout
                                << L"  Data1       = "
                                << data1
                                << std::endl;

                            std::wcout
                                << L"  Data2       = "
                                << data2
                                << std::endl;

                            if (status == 0x90 && data2 != 0)
                            {
                                std::wcout
                                    << L"  MIDI = Note ON"
                                    << std::endl;
                            }
                            else if (
                                status == 0x80 ||
                                (status == 0x90 && data2 == 0)
                                )
                            {
                                std::wcout
                                    << L"  MIDI = Note OFF"
                                    << std::endl;
                            }
                            else if (status == 0xB0)
                            {
                                std::wcout
                                    << L"  MIDI = Control Change"
                                    << std::endl;
                            }
                            else if (status == 0xC0)
                            {
                                std::wcout
                                    << L"  MIDI = Program Change"
                                    << std::endl;
                            }
                            else if (status == 0xE0)
                            {
                                std::wcout
                                    << L"  MIDI = Pitch Bend"
                                    << std::endl;
                            }

                            // ----------------------------------------------------
                            // First SD-80 bridge stage
                            // ----------------------------------------------------
                            // Preserve the MIDI status/channel and send the
                            // decoded MIDI 1.0 message to SD-80 Cable 0 (PART A).
                            // ----------------------------------------------------

                            std::wcout
                                << L"  [ROUTE] Group "
                                << group
                                << L" -> SD-80 Cable "
                                << group
                                << std::endl;

                            if (sd80Output->SendMidi1(
                                static_cast<BYTE>(group),
                                static_cast<BYTE>(statusByte),
                                static_cast<BYTE>(data1),
                                static_cast<BYTE>(data2)))
                            {
                                std::wcout
                                    << L"  SD-80 TX: OK"
                                    << std::endl;
                            }
                            else
                            {
                                std::wcerr
                                    << L"  SD-80 TX: FAILED"
                                    << std::endl;
                            }
                        }
                    }
                    catch (const winrt::hresult_error& ex)
                    {
                        std::wcerr
                            << L"[RX Virtual MIDI] Handler WinRT error: 0x"
                            << std::hex
                            << static_cast<unsigned long>(
                                ex.code().value
                                )
                            << std::dec
                            << std::endl;

                        std::wcerr
                            << L"Message: "
                            << ex.message().c_str()
                            << std::endl;
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr
                            << "[RX Virtual MIDI] Handler exception: "
                            << ex.what()
                            << std::endl;
                    }
                }
            );

        std::wcout
            << L"MIDI MessageReceived handler registered."
            << std::endl;

        // --------------------------------------------------------------------
        // Add Virtual MIDI Device message processing plugin
        // --------------------------------------------------------------------

        std::wcout
            << L"Adding Virtual MIDI Device message processing plugin..."
            << std::endl;

        connection.AddMessageProcessingPlugin(
            virtualDevice
        );

        std::wcout
            << L"Virtual MIDI Device plugin added."
            << std::endl;

        // --------------------------------------------------------------------
        // Open endpoint connection
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"Opening endpoint connection..."
            << std::endl;

        connection.Open();

        // Get client-side Endpoint Device ID after Open()
        auto associationId =
            virtualDevice.AssociationId();

        std::wcout
            << L"AssociationId:"
            << std::endl
            << winrt::to_hstring(associationId).c_str()
            << std::endl;

        auto clientEndpointDeviceId =
            virt::MidiVirtualDeviceManager::
            GetAssociatedClientEndpointDeviceId(
                associationId
            );

        std::wcout
            << L"Client-side EndpointDeviceId:"
            << std::endl
            << clientEndpointDeviceId.c_str()
            << std::endl;

        std::wcout
            << L"Endpoint connection opened."
            << std::endl;

        // --------------------------------------------------------------------
        // Success
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Virtual MIDI Device test OK"
            << std::endl
            << L"========================================"
            << std::endl;

        std::wcout
            << std::endl
            << L"Virtual MIDI Device:"
            << std::endl
            << L"  Name = SD-80 Bridge Virtual MIDI"
            << std::endl;

        std::wcout
            << L"  Product Instance Id = "
            << productInstanceId
            << std::endl;

        std::wcout
            << L"  EndpointDeviceId = "
            << endpointDeviceId.c_str()
            << std::endl;

        std::wcout
            << std::endl
            << L"SD-80 USB device:"
            << std::endl
            << L"  VID = 0x0582"
            << std::endl
            << L"  PID = 0x0029"
            << std::endl;

        // --------------------------------------------------------------------
        // MIDI receive test
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Virtual MIDI RECEIVE TEST"
            << std::endl
            << L"========================================"
            << std::endl;

        std::wcout
            << L"Waiting for MIDI messages..."
            << std::endl;

        std::wcout
            << L"Send MIDI from another application."
            << std::endl;

        std::wcout
            << L"Note On / Note Off / CC / Program Change"
            << std::endl;

        std::wcout
            << L"will be displayed here."
            << std::endl;

        std::wcout
            << std::endl
            << L"Press ENTER to exit."
            << std::endl;

        // --------------------------------------------------------------------
        // Wait
        // --------------------------------------------------------------------

        std::wstring line;

        std::getline(
            std::wcin,
            line
        );

        // --------------------------------------------------------------------
        // Remove event handler
        // --------------------------------------------------------------------

        try
        {
            messageSource.MessageReceived(
                messageReceivedToken
            );
        }
        catch (...)
        {
            // Ignore cleanup failure.
        }

        // --------------------------------------------------------------------
        // SHUTDOWN
        // --------------------------------------------------------------------
        //
        // Windows MIDI Services 1.0.17-rc.4.25 currently hangs in the
        // EndpointConnection/MidiSession cleanup path on this system.
        //
        // An explicit DisconnectEndpointConnection() was already confirmed
        // to block. The subsequent automatic WinRT destruction path was also
        // confirmed to enter MidiSession::Close() /
        // MidiEndpointConnection::DeactivateMidiStream() and remain there.
        //
        // Therefore the bridge deliberately avoids the SDK disconnect call
        // here. The event handler is removed first, the SD-80 WinUSB handle
        // is closed, and then the process is terminated explicitly so that
        // the hanging MIDI SDK destructors are not entered.
        //
        // This is a workaround for the MIDI Services cleanup hang. It does
        // not claim that the underlying Microsoft MIDI Services issue is fixed.
        // --------------------------------------------------------------------

        std::wcout
            << L"[SHUTDOWN] DisconnectEndpointConnection skipped"
            << std::endl;

        sd80Output->Close();

        std::wcout
            << L"[SHUTDOWN] SD-80 USB output closed"
            << std::endl;

        std::wcout
            << L"[SHUTDOWN] Exiting process before MIDI SDK destructors"
            << std::endl;

        std::wcout.flush();
        std::wcerr.flush();
        std::cout.flush();
        std::cerr.flush();

        ::ExitProcess(0);
    }

    // ------------------------------------------------------------------------
    // WinRT exception
    // ------------------------------------------------------------------------

    catch (const winrt::hresult_error& ex)
    {
        std::wcerr
            << L"WinRT error: 0x"
            << std::hex
            << static_cast<unsigned long>(ex.code().value)
            << std::dec
            << std::endl;

        std::wcerr
            << L"Message: "
            << ex.message().c_str()
            << std::endl;

        return 10;
    }

    // ------------------------------------------------------------------------
    // Standard C++ exception
    // ------------------------------------------------------------------------

    catch (const std::exception& ex)
    {
        std::cerr
            << "C++ exception: "
            << ex.what()
            << std::endl;

        return 11;
    }
}