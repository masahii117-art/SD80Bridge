#include <windows.h>

#include <iostream>
#include <string>
#include <iomanip>
#include <cstdint>
#include <sstream>
#include <vector>
#include <memory>
#include <mutex>

#include <setupapi.h>
#include <winusb.h>
#include <initguid.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

#include <winrt/base.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Microsoft.Windows.Devices.Midi2.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Endpoints.Virtual.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Utilities.RuntimeInformation.h>

#include <winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp>

// ============================================================================
// Namespace aliases
// ============================================================================

namespace midi2 =
    winrt::Microsoft::Windows::Devices::Midi2;

namespace virt =
    winrt::Microsoft::Windows::Devices::Midi2::Endpoints::Virtual;

namespace runtime =
    winrt::Microsoft::Windows::Devices::Midi2::Utilities::RuntimeInformation;


// ============================================================================
// Diagnostic: print installed Windows MIDI Services SDK Runtime information
// ============================================================================

static void PrintMidiRuntimeInformation()
{
    try
    {
        auto version =
            runtime::MidiRuntimeInformation::GetInstalledVersion();

        auto architecture =
            runtime::MidiRuntimeInformation::GetInstalledArchitecture();

        auto releaseType =
            runtime::MidiRuntimeInformation::GetInstalledReleaseType();

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Windows MIDI Services Runtime Information"
            << std::endl
            << L"========================================"
            << std::endl;

        std::wcout
            << L"Runtime Version = "
            << version.Major()
            << L"."
            << version.Minor()
            << L"."
            << version.Patch()
            << L" build "
            << version.BuildNumber()
            << std::endl;

        std::wcout
            << L"Runtime PreviewSuffix = "
            << version.PreviewSuffix().c_str()
            << std::endl;

        std::wcout
            << L"Runtime Architecture = "
            << static_cast<int>(architecture)
            << std::endl;

        std::wcout
            << L"Runtime ReleaseType = "
            << static_cast<int>(releaseType)
            << std::endl;
    }
    catch (const winrt::hresult_error& ex)
    {
        std::wcerr
            << L"WARNING: Unable to query MIDI Runtime information."
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
    }
}


// ============================================================================
// SD-80 WinUSB output
//
// First bridge stage:
//   Virtual MIDI UMP Type 2
//       -> MIDI 1.0 bytes
//       -> USB-MIDI packet on Cable 0
//       -> SD-80 EP 0x01 OUT
//
// Cable 0 is intentionally fixed to PART A for this first integration test.
// The incoming MIDI channel is preserved.
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

        return true;
    }

    bool SendMidi1(
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
            cin,
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
    void CloseUnlocked()
    {
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
};


// ============================================================================
// Main
// ============================================================================

int main()
{
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

        // Diagnostic only. This does not change any MIDI configuration.
        PrintMidiRuntimeInformation();


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

        // Diagnostic only.
        // CreateOnlyUmpEndpoints is NOT set here.
        // We only display the value supplied by the SDK projection.
        std::wcout
            << L"CreateOnlyUmpEndpoints = "
            << (config.CreateOnlyUmpEndpoints()
                ? L"true"
                : L"false")
            << std::endl;


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

        block.GroupCount(1);

        block.Direction(
            midi2::MidiFunctionBlockDirection::Bidirectional
        );

        block.RepresentsMidi10Connection(
            midi2::MidiFunctionBlockRepresentsMidi10Connection::Not10
        );

        block.MaxSystemExclusive8Streams(0);

        block.MidiCIMessageVersionFormat(0);


        config.FunctionBlocks().Append(block);

        // Diagnostic only.
        std::wcout
            << L"FunctionBlock count = "
            << config.FunctionBlocks().Size()
            << std::endl;


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
        // Register MIDI MessageReceived handler
        // --------------------------------------------------------------------

        std::wcout
            << L"Registering MIDI MessageReceived handler..."
            << std::endl;


        auto messageReceivedToken =
            messageSource.MessageReceived(
                [sd80Output](
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

                            if (sd80Output->SendMidi1(
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
            << L"  ProductInstanceId = "
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
        // Disconnect endpoint connection
        // --------------------------------------------------------------------

        try
        {
            session.DisconnectEndpointConnection(
                connection.ConnectionId()
            );
        }
        catch (...)
        {
            // Ignore cleanup failure during application shutdown.
        }

        sd80Output->Close();

        return 0;
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