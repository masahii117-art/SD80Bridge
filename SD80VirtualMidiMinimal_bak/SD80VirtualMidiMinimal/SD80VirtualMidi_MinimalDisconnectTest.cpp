#include <iostream>
#include <string>
#include <sstream>
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Microsoft.Windows.Devices.Midi2.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Endpoints.Virtual.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Utilities.RuntimeInformation.h>

#include <winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp>

namespace midi2 = winrt::Microsoft::Windows::Devices::Midi2;
namespace virt = winrt::Microsoft::Windows::Devices::Midi2::Endpoints::Virtual;
namespace runtime = winrt::Microsoft::Windows::Devices::Midi2::Utilities::RuntimeInformation;

int main()
{
    try
    {
        std::wcout << L"SD80VirtualMidi Minimal Disconnect Test starting..." << std::endl;

        winrt::init_apartment();

        std::wcout << L"Creating MIDI Services SDK initializer..." << std::endl;
        midi2::MidiDesktopAppSdkInitializer midiInitializer;

        std::wcout << L"Initializing MIDI Services SDK Runtime..." << std::endl;
        if (!midiInitializer.InitializeSdkRuntime())
        {
            std::wcerr << L"ERROR: InitializeSdkRuntime() failed." << std::endl;
            return 1;
        }

        std::wcout << L"MIDI Services SDK Runtime initialized." << std::endl;

        try
        {
            auto version = runtime::MidiRuntimeInformation::GetInstalledVersion();

            std::wcout
                << L"Runtime = "
                << version.Major() << L"."
                << version.Minor() << L"."
                << version.Patch()
                << L" build "
                << version.BuildNumber()
                << L" "
                << version.PreviewSuffix().c_str()
                << std::endl;
        }
        catch (...)
        {
            std::wcout << L"Runtime version query unavailable." << std::endl;
        }

        if (!midiInitializer.IsServiceInstalled())
        {
            std::wcerr << L"ERROR: Windows MIDI Services is not installed." << std::endl;
            return 2;
        }

        if (!midiInitializer.EnsureServiceAvailable())
        {
            std::wcerr << L"ERROR: MIDI Service is not available." << std::endl;
            return 3;
        }

        if (!virt::MidiVirtualDeviceManager::IsTransportAvailable())
        {
            std::wcerr << L"ERROR: Virtual MIDI transport is not available." << std::endl;
            return 4;
        }

        std::wcout
            << L"Virtual MIDI transport available."
            << std::endl;

        // Use a unique ProductInstanceId, just as the main bridge does.
        std::wstringstream id;
        id << L"MinimalVM" << GetCurrentProcessId();

        midi2::MidiDeclaredEndpointInfo endpointInfo{};
        endpointInfo.Name(L"SD-80 Bridge Minimal Test");
        endpointInfo.ProductInstanceId(id.str());
        endpointInfo.SpecificationVersionMajor(1);
        endpointInfo.SpecificationVersionMinor(1);
        endpointInfo.SupportsMidi10Protocol(true);
        endpointInfo.SupportsMidi20Protocol(true);
        endpointInfo.SupportsReceivingJitterReductionTimestamps(false);
        endpointInfo.SupportsSendingJitterReductionTimestamps(false);
        endpointInfo.HasStaticFunctionBlocks(true);

        midi2::MidiDeclaredDeviceIdentity deviceIdentity{};

        midi2::MidiEndpointUserSuppliedInfo userInfo{};
        userInfo.Name(L"SD-80 Bridge Minimal Test");
        userInfo.Description(L"Minimal Virtual MIDI disconnect test");

        virt::MidiVirtualDeviceCreationConfig config(
            L"SD-80 Bridge Minimal Test",
            L"Minimal Virtual MIDI disconnect test",
            L"masahii",
            endpointInfo,
            deviceIdentity,
            userInfo);

        midi2::MidiFunctionBlock block{};
        block.Number(0);
        block.Name(L"Minimal Test");
        block.IsActive(true);
        block.UIHint(midi2::MidiFunctionBlockUIHint::Sender);
        block.GroupCount(1);
        block.Direction(midi2::MidiFunctionBlockDirection::Bidirectional);
        block.RepresentsMidi10Connection(
            midi2::MidiFunctionBlockRepresentsMidi10Connection::Not10);
        block.MaxSystemExclusive8Streams(0);
        block.MidiCIMessageVersionFormat(0);

        config.FunctionBlocks().Append(block);

        // Match the existing SD80VirtualMidi configuration.
        config.CreateOnlyUmpEndpoints(true);

        std::wcout << L"Creating MIDI Session..." << std::endl;
        auto session = midi2::MidiSession::Create(config.Name());

        if (session == nullptr)
        {
            std::wcerr << L"ERROR: MidiSession::Create() returned null." << std::endl;
            return 5;
        }

        std::wcout << L"MIDI Session created." << std::endl;

        std::wcout << L"Creating Virtual MIDI Device..." << std::endl;
        auto virtualDevice =
            virt::MidiVirtualDeviceManager::CreateVirtualDevice(config);

        if (virtualDevice == nullptr)
        {
            std::wcerr << L"ERROR: CreateVirtualDevice() returned null." << std::endl;
            return 6;
        }

        std::wcout
            << L"Virtual MIDI Device created."
            << std::endl
            << L"EndpointDeviceId = "
            << virtualDevice.DeviceEndpointDeviceId().c_str()
            << std::endl;

        std::wcout << L"Creating endpoint connection..." << std::endl;
        auto connection =
            session.CreateEndpointConnection(
                virtualDevice.DeviceEndpointDeviceId());

        std::wcout << L"Endpoint connection created." << std::endl;

        // This is part of Microsoft's documented Virtual Device pattern.
        std::wcout
            << L"Adding Virtual MIDI Device message processing plugin..."
            << std::endl;

        connection.AddMessageProcessingPlugin(virtualDevice);

        std::wcout
            << L"Plugin added."
            << std::endl;

        std::wcout << L"Opening endpoint connection..." << std::endl;
        connection.Open();

        std::wcout
            << L"Endpoint connection opened."
            << std::endl;

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"MINIMAL DISCONNECT TEST"
            << std::endl
            << L"========================================"
            << std::endl
            << L"No SD-80 USB access."
            << std::endl
            << L"No MessageReceived handler."
            << std::endl
            << L"No MIDI messages sent."
            << std::endl
            << L"No additional message-source object."
            << std::endl
            << L"Press ENTER to disconnect and exit."
            << std::endl;

        std::wstring dummy;
        std::getline(std::wcin, dummy);

        std::wcout
            << L"[1] Disconnecting endpoint connection through MIDI session..."
            << std::endl
            << L"    Calling DisconnectEndpointConnection()..."
            << std::flush;

        auto connectionId = connection.ConnectionId();
        session.DisconnectEndpointConnection(connectionId);

        std::wcout
            << L" returned successfully."
            << std::endl;

        std::wcout
            << L"[2] Releasing connection..."
            << std::flush;

        connection = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        std::wcout
            << L"[3] Releasing virtual device..."
            << std::flush;

        virtualDevice = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        std::wcout
            << L"[4] Releasing MIDI session..."
            << std::flush;

        session = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        std::wcout
            << L"MINIMAL DISCONNECT TEST PASSED."
            << std::endl;

        return 0;
    }
    catch (const winrt::hresult_error& ex)
    {
        std::wcerr
            << L"WinRT exception: HRESULT=0x"
            << std::hex
            << static_cast<unsigned long>(ex.code().value)
            << std::dec
            << L" Message="
            << ex.message().c_str()
            << std::endl;
        return 100;
    }
    catch (const std::exception& ex)
    {
        std::wcerr
            << L"std::exception: "
            << ex.what()
            << std::endl;
        return 101;
    }
    catch (...)
    {
        std::wcerr
            << L"Unknown exception."
            << std::endl;
        return 102;
    }
}
