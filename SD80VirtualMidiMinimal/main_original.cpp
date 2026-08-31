#include <iostream>
#include <string>
#include <iomanip>
#include <cstdint>
#include <sstream>

#include <windows.h>

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

        config.CreateOnlyUmpEndpoints(true);

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
                [](
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
        // MIDI transmit test
        //
        // Send one MIDI 1.0 Note On as a 32-bit UMP immediately after
        // opening the device-side endpoint connection.
        //
        // 0x20903C64:
        //   Message Type = 0x2 (MIDI 1.0 Channel Voice)
        //   Group        = 0
        //   Channel      = 1
        //   Status       = Note On
        //   Note         = 60 (C4)
        //   Velocity     = 100
        // --------------------------------------------------------------------

        std::wcout
            << std::endl
            << L"========================================"
            << std::endl
            << L"Virtual MIDI TRANSMIT TEST"
            << std::endl
            << L"========================================"
            << std::endl;

        const std::uint32_t testWord = 0x20903C64;

        std::wcout
            << L"Sending test MIDI Note On..."
            << std::endl
            << L"  Word0 = 0x"
            << std::hex
            << std::uppercase
            << std::setw(8)
            << std::setfill(L'0')
            << testWord
            << std::dec
            << std::nouppercase
            << std::setfill(L' ')
            << std::endl;

        auto sendResult =
            connection.SendSingleMessageWords(
                0,
                testWord
            );

        if (midi2::MidiEndpointConnection::SendMessageSucceeded(sendResult))
        {
            std::wcout
                << L"  [TX Virtual MIDI] Test Note On sent successfully."
                << std::endl;
        }
        else
        {
            std::wcerr
                << L"  [TX Virtual MIDI] Test Note On FAILED. Result = 0x"
                << std::hex
                << static_cast<std::uint32_t>(sendResult)
                << std::dec
                << std::endl;
        }


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
        // Clean shutdown
        //
        // The previous diagnostic builds showed that
        // DisconnectEndpointConnection() could hang inside
        // MidiEndpointConnection::DeactivateMidiStream().
        //
        // The SDK lifetime model is:
        //   1. remove the MessageReceived handler
        //   2. remove the message-processing plugin
        //   3. release the additional message-source reference
        //   4. disconnect the endpoint through MidiSession
        //   5. close the MIDI session
        //
        // The virtual device is itself the message-processing plugin, so it
        // must be removed from the connection before the connection is
        // disconnected.
        // --------------------------------------------------------------------

        // --------------------------------------------------------------------
        // Diagnostic shutdown variant #2
        //
        // The previous diagnostic build skipped DisconnectEndpointConnection()
        // and MidiSession::Close(), but the program now hangs while removing
        // the MIDI MessageReceived handler.
        //
        // Therefore this build also skips explicit handler removal.
        // This is intentionally a diagnostic build only.
        // --------------------------------------------------------------------

        // --------------------------------------------------------------------
        // Diagnostic shutdown variant #5
        //
        // This version follows the documented MIDI Services lifetime order:
        //
        //   1. Remove MessageReceived handler.
        //   2. Remove the message-processing plugin.
        //   3. Release the extra message-source reference.
        //   4. Disconnect the endpoint through MidiSession.
        //   5. Release the connection.
        //   6. Release the virtual device.
        //   7. Release the MIDI session.
        //
        // The plugin ID is captured from AddMessageProcessingPlugin() and is
        // passed to RemoveMessageProcessingPlugin().
        //
        // Diagnostic only. Every operation is logged before and after the
        // call so the exact blocking operation can be identified.
        // --------------------------------------------------------------------

        std::wcout
            << L""
            << std::endl
            << L"========================================"
            << std::endl
            << L"DIAGNOSTIC ORDERED SHUTDOWN TEST"
            << std::endl
            << L"========================================"
            << std::endl;

        // [1] Remove the MessageReceived event handler.
        std::wcout
            << L"[1] Removing MIDI MessageReceived handler..."
            << std::flush;

        try
        {
            messageSource.MessageReceived(messageReceivedToken);

            std::wcout
                << L" done."
                << std::endl;
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wcout
                << L" FAILED. HRESULT = 0x"
                << std::hex << static_cast<uint32_t>(ex.code().value)
                << std::dec << std::endl;
        }
        catch (...)
        {
            std::wcout
                << L" FAILED. Unknown exception."
                << std::endl;
        }

        // [2] The SDK projection used by this project exposes
        // AddMessageProcessingPlugin() as void and does not provide a plugin
        // ID that can be passed to RemoveMessageProcessingPlugin().
        // Do not attempt to remove the plugin here. Release our local
        // virtualDevice reference and let the endpoint connection lifetime
        // own the registration until the connection is disconnected.
        std::wcout
            << L"[2] Releasing virtualDevice reference..."
            << std::flush;

        virtualDevice = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        // [3] Release the additional interface reference obtained from
        //     connection.as<IMidiMessageReceivedEventSource>().
        std::wcout
            << L"[3] Releasing messageSource object..."
            << std::flush;

        messageSource = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        // [4] Let MidiSession remove the connection from its connection map.
        std::wcout
            << L"[4] Disconnecting endpoint connection through MIDI session..."
            << std::endl
            << L"    Calling DisconnectEndpointConnection()..."
            << std::flush;

        try
        {
            auto connectionId = connection.ConnectionId();
            session.DisconnectEndpointConnection(connectionId);

            std::wcout
                << L" returned successfully."
                << std::endl;
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wcout
                << L" FAILED. HRESULT = 0x"
                << std::hex << static_cast<uint32_t>(ex.code().value)
                << std::dec << std::endl;
        }
        catch (...)
        {
            std::wcout
                << L" FAILED. Unknown exception."
                << std::endl;
        }

        // [5] Release our connection reference.
        std::wcout
            << L"[5] Releasing connection object..."
            << std::flush;

        connection = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        // [6] The virtualDevice reference was released in step [2].
        // The connection is now disconnected, so no additional release is needed.

        // [7] Finally release the MIDI session.
        std::wcout
            << L"[6] Releasing MIDI session..."
            << std::flush;

        session = nullptr;

        std::wcout
            << L" done."
            << std::endl;

        std::wcout
            << L"Ordered shutdown completed."
            << std::endl
            << L"Returning from main."
            << std::endl;

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