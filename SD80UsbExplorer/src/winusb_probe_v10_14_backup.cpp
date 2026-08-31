#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <SetupAPI.h>
#include <winusb.h>
#include <initguid.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <map>
#include <cstring>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")


// ============================================================
// v10.0 runtime configuration
//
// Normal mode keeps startup output focused on the MIDI bridge.
// Define SD80BRIDGE_VERBOSE_STARTUP=1 to enable descriptor analysis.
// ============================================================

#ifndef SD80BRIDGE_VERBOSE_STARTUP
#define SD80BRIDGE_VERBOSE_STARTUP 0
#endif

// Runtime MIDI detail logging.
// 0 = concise normal-operation log.
// 1 = existing detailed MIDI parser/routing log.
#ifndef SD80BRIDGE_VERBOSE_MIDI
#define SD80BRIDGE_VERBOSE_MIDI 0
#endif

// ============================================================
// USB-MIDI packet sender
// ============================================================

static bool SendMidiPacket(
    WINUSB_INTERFACE_HANDLE usbHandle,
    UCHAR pipeId,
    const BYTE packet[4])
{
    ULONG transferred = 0;

    BYTE buffer[4];

    memcpy(
        buffer,
        packet,
        sizeof(buffer));

#if SD80BRIDGE_VERBOSE_MIDI
    printf("[INFO ] Sending USB-MIDI packet:");

    for (int i = 0; i < 4; ++i)
    {
        printf(
            " %02X",
            buffer[i]);
    }

    printf("\n");

#endif
    BOOL ok =
        WinUsb_WritePipe(
            usbHandle,
            pipeId,
            buffer,
            sizeof(buffer),
            &transferred,
            nullptr);

    if (!ok)
    {
        DWORD err =
            GetLastError();

        printf(
            "[ERROR] WinUsb_WritePipe failed: "
            "GetLastError=%lu (0x%08lX)\n",
            err,
            err);

        return false;
    }

    printf(
        "[INFO ] WinUsb_WritePipe OK: "
        "%lu bytes transferred\n",
        transferred);

    return transferred == sizeof(buffer);
}


// ============================================================
// SD80 WinUSB Device Interface GUID
// ============================================================

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


// ============================================================
// Constants
// ============================================================

static constexpr UCHAR SD80_EP_OUT = 0x01;
static constexpr UCHAR SD80_EP_IN = 0x81;

static constexpr ULONG SD80_PACKET_SIZE = 64;

static constexpr DWORD
SD80_READ_TIMEOUT_MS = 1000;

static constexpr int
SD80_READ_COUNT = 5;


// USB descriptor types

static constexpr UCHAR USB_DESCRIPTOR_TYPE_DEVICE =
0x01;

static constexpr UCHAR USB_DESCRIPTOR_TYPE_CONFIGURATION =
0x02;

static constexpr UCHAR USB_DESCRIPTOR_TYPE_INTERFACE =
0x04;

static constexpr UCHAR USB_DESCRIPTOR_TYPE_ENDPOINT =
0x05;

static constexpr UCHAR USB_DESCRIPTOR_TYPE_CS_INTERFACE =
0x24;

static constexpr UCHAR USB_DESCRIPTOR_TYPE_CS_ENDPOINT =
0x25;


// USB MIDI Streaming class

static constexpr UCHAR USB_CLASS_AUDIO =
0x01;

static constexpr UCHAR USB_SUBCLASS_MIDI_STREAMING =
0x03;


// MIDI class-specific interface subtypes

static constexpr UCHAR MIDI_MS_HEADER =
0x01;

static constexpr UCHAR MIDI_MIDI_IN_JACK =
0x02;

static constexpr UCHAR MIDI_MIDI_OUT_JACK =
0x03;


// MIDI Jack types

static constexpr UCHAR MIDI_JACK_EMBEDDED =
0x01;

static constexpr UCHAR MIDI_JACK_EXTERNAL =
0x02;


// MIDI class-specific endpoint subtype

static constexpr UCHAR MIDI_CS_EP_GENERAL =
0x01;


// ============================================================
// Error
// ============================================================

static void PrintLastError(
    const wchar_t* operation)
{
    DWORD error =
        GetLastError();

    std::wcout
        << L"[ERROR] "
        << operation
        << L" failed. Error = "
        << error
        << std::endl;
}


// ============================================================
// HEX dump
// ============================================================

static void DumpHex(
    const BYTE* data,
    ULONG length)
{
    for (ULONG i = 0;
        i < length;
        ++i)
    {
        if ((i % 16) == 0)
        {
            std::wcout
                << L"  "
                << std::hex
                << std::uppercase
                << std::setw(4)
                << std::setfill(L'0')
                << i
                << L": ";
        }

        std::wcout
            << std::setw(2)
            << static_cast<unsigned int>(
                data[i])
            << L' ';

        if ((i % 16) == 15 ||
            i == length - 1)
        {
            std::wcout
                << std::dec
                << std::setfill(L' ')
                << std::endl;
        }
    }
}


// ============================================================
// USB MIDI Jack information
// ============================================================

struct MidiJackInfo
{
    UCHAR jackId = 0;
    UCHAR jackType = 0;

    UCHAR sourceJackId = 0;
    UCHAR sourcePin = 0;

    bool hasSource = false;
};


// ============================================================
// USB MIDI Endpoint information
// ============================================================

struct MidiEndpointInfo
{
    UCHAR endpointAddress = 0;

    std::vector<UCHAR> embeddedJackIds;
};


// ============================================================
// USB MIDI Descriptor parser
// ============================================================

static void ParseSD80MidiDescriptors(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================\n"
        << L"SD-80 USB MIDI DESCRIPTOR ANALYSIS\n"
        << L"========================================\n";


    // --------------------------------------------------------
    // Read first 9 bytes of Configuration Descriptor
    // --------------------------------------------------------

    BYTE configHeader[9]{};

    ULONG headerLength = 0;

    if (!WinUsb_GetDescriptor(
        winusbHandle,
        USB_DESCRIPTOR_TYPE_CONFIGURATION,
        0,
        0,
        configHeader,
        sizeof(configHeader),
        &headerLength))
    {
        PrintLastError(
            L"WinUsb_GetDescriptor(Configuration header)");

        return;
    }


    if (headerLength < 9)
    {
        std::wcout
            << L"[ERROR] Configuration descriptor is too short."
            << std::endl;

        return;
    }


    USHORT totalLength =
        static_cast<USHORT>(
            configHeader[2] |
            (configHeader[3] << 8));


    std::wcout
        << L"[INFO ] Configuration Descriptor Length = "
        << totalLength
        << L" bytes"
        << std::endl;


    if (totalLength < 9 ||
        totalLength > 4096)
    {
        std::wcout
            << L"[ERROR] Invalid configuration descriptor length."
            << std::endl;

        return;
    }


    // --------------------------------------------------------
    // Read complete Configuration Descriptor
    // --------------------------------------------------------

    std::vector<BYTE> descriptor(
        totalLength);

    ULONG actualLength = 0;

    if (!WinUsb_GetDescriptor(
        winusbHandle,
        USB_DESCRIPTOR_TYPE_CONFIGURATION,
        0,
        0,
        descriptor.data(),
        totalLength,
        &actualLength))
    {
        PrintLastError(
            L"WinUsb_GetDescriptor(Configuration)");

        return;
    }


    if (actualLength < 9)
    {
        std::wcout
            << L"[ERROR] Failed to obtain complete descriptor."
            << std::endl;

        return;
    }


    descriptor.resize(
        actualLength);


    std::wcout
        << L"[INFO ] Configuration Descriptor read: "
        << actualLength
        << L" bytes"
        << std::endl;


    // --------------------------------------------------------
    // Descriptor parser state
    // --------------------------------------------------------

    int currentInterfaceNumber = -1;
    int currentAlternateSetting = -1;

    bool midiStreamingInterface = false;

    std::vector<MidiJackInfo> jackList;
    std::vector<MidiEndpointInfo> endpointList;


    UCHAR currentEndpointAddress = 0;


    // --------------------------------------------------------
    // Parse descriptor chain
    // --------------------------------------------------------

    size_t offset = 0;

    while (offset + 2 <= descriptor.size())
    {
        UCHAR length =
            descriptor[offset];

        UCHAR type =
            descriptor[offset + 1];


        if (length < 2)
        {
            std::wcout
                << L"[WARNING] Invalid descriptor length at offset "
                << offset
                << std::endl;

            break;
        }


        if (offset + length >
            descriptor.size())
        {
            std::wcout
                << L"[WARNING] Descriptor exceeds buffer."
                << std::endl;

            break;
        }


        const BYTE* d =
            &descriptor[offset];


        // ----------------------------------------------------
        // Standard Interface Descriptor
        // ----------------------------------------------------

        if (type ==
            USB_DESCRIPTOR_TYPE_INTERFACE &&
            length >= 9)
        {
            currentInterfaceNumber =
                d[2];

            currentAlternateSetting =
                d[3];

            UCHAR interfaceClass =
                d[5];

            UCHAR interfaceSubClass =
                d[6];

            midiStreamingInterface =
                (interfaceSubClass ==
                    USB_SUBCLASS_MIDI_STREAMING);

            std::wcout
                << L"\n[INTERFACE]"
                << L" Number="
                << static_cast<unsigned int>(
                    d[2])
                << L" Alt="
                << static_cast<unsigned int>(
                    d[3])
                << L" Class=0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<unsigned int>(
                    d[5])
                << L" SubClass=0x"
                << std::setw(2)
                << static_cast<unsigned int>(
                    d[6])
                << std::dec
                << std::setfill(L' ')
                << std::endl;


            if (midiStreamingInterface)
            {
                std::wcout
                    << L"  -> MIDI Streaming Interface"
                    << std::endl;
            }
        }


        // ----------------------------------------------------
        // Standard Endpoint Descriptor
        // ----------------------------------------------------

        else if (
            type ==
            USB_DESCRIPTOR_TYPE_ENDPOINT &&
            length >= 7)
        {
            currentEndpointAddress =
                d[2];

            UCHAR attributes =
                d[3];

            USHORT maxPacket =
                static_cast<USHORT>(
                    d[4] |
                    (d[5] << 8));

            std::wcout
                << L"\n[ENDPOINT]"
                << L" Address=0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<unsigned int>(
                    currentEndpointAddress)
                << L" Attributes=0x"
                << std::setw(2)
                << static_cast<unsigned int>(
                    attributes)
                << L" MaxPacket="
                << std::dec
                << maxPacket
                << std::endl;


            bool endpointAlreadyExists = false;

            for (const auto& existingEndpoint :
                endpointList)
            {
                if (existingEndpoint.endpointAddress ==
                    currentEndpointAddress)
                {
                    endpointAlreadyExists = true;
                    break;
                }
            }

            if (!endpointAlreadyExists)
            {
                MidiEndpointInfo info{};

                info.endpointAddress =
                    currentEndpointAddress;

                endpointList.push_back(
                    info);
            }
        }


        // ----------------------------------------------------
        // Class-specific MIDI Interface Descriptor
        // ----------------------------------------------------

        else if (
            type ==
            USB_DESCRIPTOR_TYPE_CS_INTERFACE &&
            midiStreamingInterface &&
            length >= 3)
        {
            UCHAR subtype =
                d[2];


            // ------------------------------------------------
            // MIDI IN Jack
            // ------------------------------------------------

            if (subtype ==
                MIDI_MIDI_IN_JACK &&
                length >= 6)
            {
                MidiJackInfo jack{};

                jack.jackId =
                    d[4];

                jack.jackType =
                    d[3];


                jackList.push_back(
                    jack);


                std::wcout
                    << L"\n[MIDI IN JACK]"
                    << L" ID=0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<unsigned int>(
                        jack.jackId)
                    << L" Type=";


                if (jack.jackType ==
                    MIDI_JACK_EMBEDDED)
                {
                    std::wcout
                        << L"Embedded";
                }
                else if (
                    jack.jackType ==
                    MIDI_JACK_EXTERNAL)
                {
                    std::wcout
                        << L"External";
                }
                else
                {
                    std::wcout
                        << L"0x"
                        << std::setw(2)
                        << static_cast<unsigned int>(
                            jack.jackType);
                }


                std::wcout
                    << std::dec
                    << std::setfill(L' ')
                    << std::endl;
            }


            // ------------------------------------------------
            // MIDI OUT Jack
            // ------------------------------------------------

            else if (
                subtype ==
                MIDI_MIDI_OUT_JACK &&
                length >= 8)
            {
                MidiJackInfo jack{};

                jack.jackId =
                    d[4];

                jack.jackType =
                    d[3];

                jack.sourceJackId =
                    d[6];

                jack.sourcePin =
                    d[7];

                jack.hasSource =
                    true;


                jackList.push_back(
                    jack);


                std::wcout
                    << L"\n[MIDI OUT JACK]"
                    << L" ID=0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<unsigned int>(
                        jack.jackId)
                    << L" Type=";


                if (jack.jackType ==
                    MIDI_JACK_EMBEDDED)
                {
                    std::wcout
                        << L"Embedded";
                }
                else if (
                    jack.jackType ==
                    MIDI_JACK_EXTERNAL)
                {
                    std::wcout
                        << L"External";
                }
                else
                {
                    std::wcout
                        << L"0x"
                        << std::setw(2)
                        << static_cast<unsigned int>(
                            jack.jackType);
                }


                std::wcout
                    << L" Source=0x"
                    << std::setw(2)
                    << static_cast<unsigned int>(
                        jack.sourceJackId)
                    << L":"
                    << std::setw(2)
                    << static_cast<unsigned int>(
                        jack.sourcePin)
                    << std::dec
                    << std::setfill(L' ')
                    << std::endl;
            }
        }


        // ----------------------------------------------------
        // Class-specific MIDI Endpoint Descriptor
        // ----------------------------------------------------

        else if (
            type ==
            USB_DESCRIPTOR_TYPE_CS_ENDPOINT &&
            midiStreamingInterface &&
            length >= 4)
        {
            UCHAR subtype =
                d[2];


            if (subtype ==
                MIDI_CS_EP_GENERAL)
            {
                UCHAR jackCount =
                    d[3];


                std::wcout
                    << L"\n[CS ENDPOINT]"
                    << L" EP=0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<unsigned int>(
                        currentEndpointAddress)
                    << L" Embedded MIDI Jack Count="
                    << std::dec
                    << static_cast<unsigned int>(
                        jackCount)
                    << std::endl;


                if (length >=
                    static_cast<UCHAR>(
                        4 + jackCount))
                {
                    MidiEndpointInfo* info =
                        nullptr;


                    for (auto& endpoint :
                        endpointList)
                    {
                        if (endpoint.endpointAddress ==
                            currentEndpointAddress)
                        {
                            info =
                                &endpoint;

                            break;
                        }
                    }


                    if (info != nullptr)
                    {
                        for (UCHAR i = 0;
                            i < jackCount;
                            ++i)
                        {
                            UCHAR jackId =
                                d[4 + i];

                            bool jackAlreadyListed = false;

                            for (const auto existingJackId :
                                info->embeddedJackIds)
                            {
                                if (existingJackId == jackId)
                                {
                                    jackAlreadyListed = true;
                                    break;
                                }
                            }

                            if (!jackAlreadyListed)
                            {
                                info->embeddedJackIds
                                    .push_back(
                                        jackId);
                            }

                            std::wcout
                                << L"  Embedded Jack "
                                << static_cast<unsigned int>(
                                    i)
                                << L" = 0x"
                                << std::hex
                                << std::uppercase
                                << std::setw(2)
                                << std::setfill(L'0')
                                << static_cast<unsigned int>(
                                    jackId)
                                << std::dec
                                << std::setfill(L' ')
                                << std::endl;
                        }
                    }
                }
            }
        }


        offset += length;
    }


    // ========================================================
    // Summary
    // ========================================================

    std::wcout
        << L"\n========================================\n"
        << L"USB MIDI TOPOLOGY SUMMARY\n"
        << L"========================================\n";


    // --------------------------------------------------------
    // Jack summary
    // --------------------------------------------------------

    std::wcout
        << L"\nMIDI Jacks\n"
        << L"----------\n";


    for (const auto& jack :
        jackList)
    {
        std::wcout
            << L"Jack 0x"
            << std::hex
            << std::uppercase
            << std::setw(2)
            << std::setfill(L'0')
            << static_cast<unsigned int>(
                jack.jackId)
            << std::dec
            << std::setfill(L' ')
            << L" : ";


        if (jack.jackType ==
            MIDI_JACK_EMBEDDED)
        {
            std::wcout
                << L"Embedded";
        }
        else if (
            jack.jackType ==
            MIDI_JACK_EXTERNAL)
        {
            std::wcout
                << L"External";
        }
        else
        {
            std::wcout
                << L"Unknown";
        }


        if (jack.hasSource)
        {
            std::wcout
                << L" <- Jack 0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<unsigned int>(
                    jack.sourceJackId)
                << L":"
                << std::setw(2)
                << static_cast<unsigned int>(
                    jack.sourcePin)
                << std::dec
                << std::setfill(L' ');
        }


        std::wcout
            << std::endl;
    }


    // --------------------------------------------------------
    // Endpoint / Cable summary
    // --------------------------------------------------------

    std::wcout
        << L"\nMIDI Endpoints\n"
        << L"--------------\n";


    for (const auto& endpoint :
        endpointList)
    {
        std::wcout
            << L"EP 0x"
            << std::hex
            << std::uppercase
            << std::setw(2)
            << std::setfill(L'0')
            << static_cast<unsigned int>(
                endpoint.endpointAddress)
            << std::dec
            << std::setfill(L' ');


        if ((endpoint.endpointAddress &
            0x80) != 0)
        {
            std::wcout
                << L" IN";
        }
        else
        {
            std::wcout
                << L" OUT";
        }


        std::wcout
            << L"\n";


        for (size_t i = 0;
            i < endpoint.embeddedJackIds.size();
            ++i)
        {
            std::wcout
                << L"  Cable "
                << i
                << L" -> Jack 0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<unsigned int>(
                    endpoint.embeddedJackIds[i])
                << std::dec
                << std::setfill(L' ')
                << std::endl;
        }
    }



    // --------------------------------------------------------
    // Derived Cable <-> Jack relationship
    // --------------------------------------------------------
    //
    // This is intentionally a descriptor-only analysis.
    // It does NOT send MIDI data and does NOT change routing.
    //
    // For each MIDI OUT/IN endpoint, show:
    //   USB Cable -> Embedded Jack
    // and, when available:
    //   Embedded Jack -> Source Jack:Pin
    //
    // This lets us compare the USB cable numbers with the
    // actual MIDI Jack topology reported by the SD-80.
    // --------------------------------------------------------

    std::wcout
        << L"\n========================================\n"
        << L"USB MIDI CABLE / JACK RELATIONSHIP"
        << L"\n========================================\n";

    for (const auto& endpoint : endpointList)
    {
        const bool isIn =
            (endpoint.endpointAddress & 0x80) != 0;

        const bool isOut =
            !isIn;

        if (!isIn && !isOut)
            continue;

        std::wcout
            << L"\nEP 0x"
            << std::hex
            << std::uppercase
            << std::setw(2)
            << std::setfill(L'0')
            << static_cast<unsigned int>(
                endpoint.endpointAddress)
            << std::dec
            << std::setfill(L' ');

        if (isIn)
        {
            std::wcout
                << L" IN"
                << L" (USB-MIDI OUT data from device)";
        }
        else
        {
            std::wcout
                << L" OUT"
                << L" (USB-MIDI IN data to device)";
        }

        std::wcout << std::endl;

        for (size_t cable = 0;
            cable < endpoint.embeddedJackIds.size();
            ++cable)
        {
            const BYTE jackId =
                endpoint.embeddedJackIds[cable];

            std::wcout
                << L"  Cable "
                << cable
                << L" -> Embedded Jack 0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<unsigned int>(jackId)
                << std::dec
                << std::setfill(L' ');

            bool found = false;

            for (const auto& jack : jackList)
            {
                if (jack.jackId != jackId)
                    continue;

                found = true;

                if (jack.jackType ==
                    MIDI_JACK_EMBEDDED)
                {
                    std::wcout
                        << L" [Embedded]";
                }
                else if (
                    jack.jackType ==
                    MIDI_JACK_EXTERNAL)
                {
                    std::wcout
                        << L" [External]";
                }
                else
                {
                    std::wcout
                        << L" [Type=0x"
                        << std::hex
                        << std::uppercase
                        << std::setw(2)
                        << std::setfill(L'0')
                        << static_cast<unsigned int>(
                            jack.jackType)
                        << std::dec
                        << std::setfill(L' ')
                        << L"]";
                }

                if (jack.hasSource)
                {
                    std::wcout
                        << L" Source=0x"
                        << std::hex
                        << std::uppercase
                        << std::setw(2)
                        << std::setfill(L'0')
                        << static_cast<unsigned int>(
                            jack.sourceJackId)
                        << L":"
                        << std::setw(2)
                        << static_cast<unsigned int>(
                            jack.sourcePin)
                        << std::dec
                        << std::setfill(L' ');
                }

                break;
            }

            if (!found)
            {
                std::wcout
                    << L" [Jack descriptor not found]";
            }

            std::wcout << std::endl;
        }
    }

    std::wcout
        << L"\n========================================\n"
        << L"USB MIDI CABLE / JACK RELATIONSHIP COMPLETED"
        << L"\n========================================\n";

    std::wcout
        << L"\n========================================\n"
        << L"IMPORTANT DESCRIPTOR NOTE"
        << L"\n========================================\n"
        << L"USB-MIDI Cable numbers identify positions in the"
        << L" class-specific endpoint descriptor.\n"
        << L"A MIDI OUT Jack SourceID/SourcePin describes"
        << L" the Jack feeding that OUT Jack.\n"
        << L"These fields do not by themselves prove which"
        << L" SD-80 internal Part produces sound.\n"
        << L"\n"
        << L"Do not infer Cable 2/3 audio routing from the"
        << L" Cable number alone.\n"
        << std::endl;

    std::wcout
        << L"\n========================================\n"
        << L"Descriptor analysis completed."
        << L"\n========================================\n"
        << std::endl;
}


// ============================================================
// Find SD-80 device interface
// ============================================================

static std::wstring FindSD80DevicePath()
{
    HDEVINFO deviceInfoSet =
        SetupDiGetClassDevsW(
            &GUID_SD80_WINUSB,
            nullptr,
            nullptr,
            DIGCF_PRESENT |
            DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet ==
        INVALID_HANDLE_VALUE)
    {
        PrintLastError(
            L"SetupDiGetClassDevsW");

        return {};
    }


    std::wstring result;


    for (DWORD index = 0;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA
            interfaceData{};

        interfaceData.cbSize =
            sizeof(
                SP_DEVICE_INTERFACE_DATA);


        if (!SetupDiEnumDeviceInterfaces(
            deviceInfoSet,
            nullptr,
            &GUID_SD80_WINUSB,
            index,
            &interfaceData))
        {
            DWORD error =
                GetLastError();

            if (error ==
                ERROR_NO_MORE_ITEMS)
            {
                break;
            }

            PrintLastError(
                L"SetupDiEnumDeviceInterfaces");

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


        std::vector<BYTE> buffer(
            requiredSize);


        auto detailData =
            reinterpret_cast<
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
                buffer.data());


        detailData->cbSize =
            sizeof(
                SP_DEVICE_INTERFACE_DETAIL_DATA_W);


        if (!SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            detailData,
            requiredSize,
            nullptr,
            nullptr))
        {
            PrintLastError(
                L"SetupDiGetDeviceInterfaceDetailW");

            continue;
        }


        result =
            detailData->DevicePath;

        break;
    }


    SetupDiDestroyDeviceInfoList(
        deviceInfoSet);


    return result;
}


// ============================================================
// Read one IN packet
//
// No OUT transfer is performed here.
// ============================================================

static bool ReadOnePacket(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle,
    int packetNumber)
{
    BYTE buffer[
        SD80_PACKET_SIZE]{};

        ULONG bytesRead = 0;


        OVERLAPPED overlapped{};


        overlapped.hEvent =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nullptr);


        if (!overlapped.hEvent)
        {
            PrintLastError(
                L"CreateEventW");

            return false;
        }


        std::wcout
            << L"\n  [READ "
            << packetNumber
            << L"] Waiting on EP 0x81..."
            << std::endl;


        BOOL result =
            WinUsb_ReadPipe(
                winusbHandle,
                SD80_EP_IN,
                buffer,
                sizeof(buffer),
                &bytesRead,
                &overlapped);


        if (result)
        {
            std::wcout
                << L"  [READ "
                << packetNumber
                << L"] Completed immediately: "
                << bytesRead
                << L" bytes"
                << std::endl;


            if (bytesRead > 0)
            {
                DumpHex(
                    buffer,
                    bytesRead);
            }


            CloseHandle(
                overlapped.hEvent);

            return true;
        }


        DWORD error =
            GetLastError();


        if (error != ERROR_IO_PENDING)
        {
            std::wcout
                << L"  [READ "
                << packetNumber
                << L"] WinUsb_ReadPipe error = "
                << error
                << std::endl;


            CloseHandle(
                overlapped.hEvent);

            return false;
        }


        DWORD waitResult =
            WaitForSingleObject(
                overlapped.hEvent,
                SD80_READ_TIMEOUT_MS);


        if (waitResult == WAIT_TIMEOUT)
        {
            std::wcout
                << L"  [READ "
                << packetNumber
                << L"] Timeout after "
                << SD80_READ_TIMEOUT_MS
                << L" ms"
                << std::endl;


            if (!CancelIoEx(
                deviceHandle,
                &overlapped))
            {
                DWORD cancelError =
                    GetLastError();

                if (cancelError !=
                    ERROR_NOT_FOUND)
                {
                    std::wcout
                        << L"  [WARNING] CancelIoEx error = "
                        << cancelError
                        << std::endl;
                }
            }


            WaitForSingleObject(
                overlapped.hEvent,
                1000);


            CloseHandle(
                overlapped.hEvent);

            return true;
        }


        if (waitResult != WAIT_OBJECT_0)
        {
            std::wcout
                << L"  [READ "
                << packetNumber
                << L"] Wait failed."
                << std::endl;


            CloseHandle(
                overlapped.hEvent);

            return false;
        }


        if (!WinUsb_GetOverlappedResult(
            winusbHandle,
            &overlapped,
            &bytesRead,
            FALSE))
        {
            DWORD completionError =
                GetLastError();


            if (completionError ==
                ERROR_OPERATION_ABORTED)
            {
                std::wcout
                    << L"  [READ "
                    << packetNumber
                    << L"] Operation aborted."
                    << std::endl;
            }
            else
            {
                std::wcout
                    << L"  [READ "
                    << packetNumber
                    << L"] Completion error = "
                    << completionError
                    << std::endl;
            }


            CloseHandle(
                overlapped.hEvent);

            return false;
        }


        std::wcout
            << L"  [READ "
            << packetNumber
            << L"] Completed: "
            << bytesRead
            << L" bytes"
            << std::endl;


        if (bytesRead > 0)
        {
            DumpHex(
                buffer,
                bytesRead);
        }


        CloseHandle(
            overlapped.hEvent);


        return true;
}


// ============================================================
// Wait for MIDI input
// ============================================================

static bool WaitForMidiInput(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle,
    DWORD timeoutMs)
{
    BYTE buffer[64]{};
    ULONG bytesRead = 0;


    OVERLAPPED ov{};

    ov.hEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);


    if (!ov.hEvent)
    {
        PrintLastError(
            L"CreateEventW");

        return false;
    }


    std::wcout
        << L"\n========================================\n"
        << L"SD-80 MIDI IN Monitor\n"
        << L"EP 0x81 / 10 second wait\n"
        << L"========================================\n"
        << std::endl;


    BOOL ok =
        WinUsb_ReadPipe(
            winusbHandle,
            SD80_EP_IN,
            buffer,
            sizeof(buffer),
            &bytesRead,
            &ov);


    if (ok)
    {
        std::wcout
            << L"[RX] Immediate completion: "
            << bytesRead
            << L" bytes\n";


        if (bytesRead)
        {
            DumpHex(
                buffer,
                bytesRead);
        }


        CloseHandle(
            ov.hEvent);

        return true;
    }


    DWORD error =
        GetLastError();


    if (error != ERROR_IO_PENDING)
    {
        std::wcout
            << L"[ERROR] WinUsb_ReadPipe = "
            << error
            << std::endl;


        CloseHandle(
            ov.hEvent);

        return false;
    }


    std::wcout
        << L"[INFO] Read pending.\n"
        << L"[INFO] Now send MIDI into SD-80...\n"
        << std::endl;


    DWORD waitResult =
        WaitForSingleObject(
            ov.hEvent,
            timeoutMs);


    if (waitResult == WAIT_OBJECT_0)
    {
        if (!GetOverlappedResult(
            deviceHandle,
            &ov,
            &bytesRead,
            FALSE))
        {
            PrintLastError(
                L"GetOverlappedResult");


            CloseHandle(
                ov.hEvent);

            return false;
        }


        std::wcout
            << L"\n[RX] "
            << bytesRead
            << L" bytes received\n";


        if (bytesRead)
        {
            DumpHex(
                buffer,
                bytesRead);
        }


        CloseHandle(
            ov.hEvent);

        return true;
    }


    if (waitResult == WAIT_TIMEOUT)
    {
        std::wcout
            << L"\n[INFO] No MIDI data received within "
            << timeoutMs
            << L" ms.\n";


        CancelIoEx(
            deviceHandle,
            &ov);


        WaitForSingleObject(
            ov.hEvent,
            1000);


        CloseHandle(
            ov.hEvent);

        return true;
    }


    std::wcout
        << L"[ERROR] WaitForSingleObject failed.\n";


    CancelIoEx(
        deviceHandle,
        &ov);


    WaitForSingleObject(
        ov.hEvent,
        1000);


    CloseHandle(
        ov.hEvent);


    return false;
}


// ============================================================
// Test MIDI OUT -> SD-80 -> USB IN
// ============================================================

static bool TestMidiOutToUsbIn(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    constexpr DWORD READ_TIMEOUT_MS = 3000;


    BYTE rxBuffer[64]{};
    ULONG bytesRead = 0;


    OVERLAPPED overlapped{};


    overlapped.hEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);


    if (!overlapped.hEvent)
    {
        PrintLastError(
            L"CreateEventW");

        return false;
    }


    std::wcout
        << L"\n========================================"
        << std::endl;


    std::wcout
        << L"USB MIDI OUT -> USB IN TEST"
        << std::endl;


    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // Start EP 0x81 read FIRST
    // --------------------------------------------------------

    std::wcout
        << L"[INFO ] Starting EP 0x81 read..."
        << std::endl;


    BOOL readResult =
        WinUsb_ReadPipe(
            winusbHandle,
            SD80_EP_IN,
            rxBuffer,
            sizeof(rxBuffer),
            &bytesRead,
            &overlapped);


    if (readResult)
    {
        std::wcout
            << L"[INFO ] EP 0x81 completed immediately: "
            << bytesRead
            << L" bytes"
            << std::endl;


        if (bytesRead > 0)
        {
            DumpHex(
                rxBuffer,
                bytesRead);
        }


        CloseHandle(
            overlapped.hEvent);

        return true;
    }


    DWORD readError =
        GetLastError();


    if (readError != ERROR_IO_PENDING)
    {
        std::wcout
            << L"[ERROR] WinUsb_ReadPipe failed: "
            << readError
            << L" (0x"
            << std::hex
            << readError
            << std::dec
            << L")"
            << std::endl;


        CloseHandle(
            overlapped.hEvent);

        return false;
    }


    std::wcout
        << L"[INFO ] EP 0x81 read is pending."
        << std::endl;


    Sleep(100);


    // --------------------------------------------------------
    // Send Note On
    // --------------------------------------------------------

    const BYTE noteOn[4] =
    {
        0x09,
        0x90,
        0x3C,
        0x7F
    };


    std::wcout
        << L"[INFO ] Sending Cable 0 Note On..."
        << std::endl;


    bool writeOK =
        SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOn);


    if (!writeOK)
    {
        std::wcout
            << L"[ERROR] Note On transmission failed."
            << std::endl;


        CancelIoEx(
            deviceHandle,
            &overlapped);


        WaitForSingleObject(
            overlapped.hEvent,
            1000);


        CloseHandle(
            overlapped.hEvent);

        return false;
    }


    // --------------------------------------------------------
    // Wait for possible USB IN response
    // --------------------------------------------------------

    std::wcout
        << L"[INFO ] Waiting for EP 0x81 response..."
        << std::endl;


    DWORD waitResult =
        WaitForSingleObject(
            overlapped.hEvent,
            READ_TIMEOUT_MS);


    if (waitResult == WAIT_OBJECT_0)
    {
        if (!WinUsb_GetOverlappedResult(
            winusbHandle,
            &overlapped,
            &bytesRead,
            FALSE))
        {
            DWORD error =
                GetLastError();


            std::wcout
                << L"[ERROR] "
                L"WinUsb_GetOverlappedResult failed: "
                << error
                << std::endl;


            CloseHandle(
                overlapped.hEvent);

            return false;
        }


        std::wcout
            << L"[RX] EP 0x81 received "
            << bytesRead
            << L" bytes"
            << std::endl;


        if (bytesRead > 0)
        {
            DumpHex(
                rxBuffer,
                bytesRead);
        }
        else
        {
            std::wcout
                << L"[RX] Zero bytes."
                << std::endl;
        }
    }
    else if (waitResult == WAIT_TIMEOUT)
    {
        std::wcout
            << L"[INFO ] No EP 0x81 response within "
            << READ_TIMEOUT_MS
            << L" ms."
            << std::endl;


        CancelIoEx(
            deviceHandle,
            &overlapped);


        WaitForSingleObject(
            overlapped.hEvent,
            1000);
    }
    else
    {
        std::wcout
            << L"[ERROR] WaitForSingleObject failed: "
            << GetLastError()
            << std::endl;


        CancelIoEx(
            deviceHandle,
            &overlapped);


        WaitForSingleObject(
            overlapped.hEvent,
            1000);


        CloseHandle(
            overlapped.hEvent);

        return false;
    }


    // --------------------------------------------------------
    // Send Note Off
    // --------------------------------------------------------

    const BYTE noteOff[4] =
    {
        0x08,
        0x80,
        0x3C,
        0x00
    };


    std::wcout
        << L"[INFO ] Sending Cable 0 Note Off..."
        << std::endl;


    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOff);


    CloseHandle(
        overlapped.hEvent);


    std::wcout
        << L"[INFO ] USB MIDI OUT -> USB IN test completed."
        << std::endl;


    return true;
}

// ============================================================
// Set SD-80 Native Mode Tone
//
// cable      : USB-MIDI Cable Number (0-3)
// channel    : MIDI Channel (1-16)
// bankMSB    : CC#0  Bank Select MSB (0-127)
// bankLSB    : CC#32 Bank Select LSB (0-127)
// program    : Program Change (0-127)
//
// USB-MIDI:
//   CC#0       = CIN 0xB
//   CC#32      = CIN 0xB
//   Program    = CIN 0xC
//
// Example:
//   Cable 0 / Channel 1
//   Classical Set / Variation 0 / Program 9
//
//   0B B0 60 00
//   0B B0 20 00
//   0C C0 09 00
// ============================================================


static bool SetSD80Tone(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    int cable,
    int channel,
    int bankMSB,
    int bankLSB,
    int program)
{
    // --------------------------------------------------------
    // Parameter validation
    // --------------------------------------------------------

    if (cable < 0 || cable > 15)
    {
        std::wcout
            << L"[ERROR] Invalid USB-MIDI cable: "
            << cable
            << std::endl;

        return false;
    }

    if (channel < 1 || channel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI channel: "
            << channel
            << std::endl;

        return false;
    }

    if (bankMSB < 0 || bankMSB > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Bank MSB: "
            << bankMSB
            << std::endl;

        return false;
    }

    if (bankLSB < 0 || bankLSB > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Bank LSB: "
            << bankLSB
            << std::endl;

        return false;
    }

    if (program < 0 || program > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Program: "
            << program
            << std::endl;

        return false;
    }


    // --------------------------------------------------------
    // MIDI status bytes
    //
    // MIDI Channel 1 = 0xB0 / 0xC0
    // MIDI Channel 2 = 0xB1 / 0xC1
    // ...
    // MIDI Channel 16 = 0xBF / 0xCF
    // --------------------------------------------------------

    const BYTE midiChannel =
        static_cast<BYTE>(
            channel - 1);


    const BYTE controlChangeStatus =
        static_cast<BYTE>(
            0xB0 | midiChannel);


    const BYTE programChangeStatus =
        static_cast<BYTE>(
            0xC0 | midiChannel);


    // --------------------------------------------------------
    // USB-MIDI Code Index Numbers
    // --------------------------------------------------------

    const BYTE cinControlChange =
        0x0B;

    const BYTE cinProgramChange =
        0x0C;


    // --------------------------------------------------------
    // USB-MIDI Cable + CIN
    // --------------------------------------------------------

    const BYTE cableCinCC =
        static_cast<BYTE>(
            (cable << 4) |
            cinControlChange);


    const BYTE cableCinPC =
        static_cast<BYTE>(
            (cable << 4) |
            cinProgramChange);


    // --------------------------------------------------------
    // CC#0 - Bank Select MSB
    // --------------------------------------------------------

    const BYTE bankSelectMSB[4] =
    {
        cableCinCC,
        controlChangeStatus,
        0x00,
        static_cast<BYTE>(
            bankMSB)
    };


    // --------------------------------------------------------
    // CC#32 - Bank Select LSB
    // --------------------------------------------------------

    const BYTE bankSelectLSB[4] =
    {
        cableCinCC,
        controlChangeStatus,
        0x20,
        static_cast<BYTE>(
            bankLSB)
    };


    // --------------------------------------------------------
    // Program Change
    // --------------------------------------------------------

    const BYTE programChange[4] =
    {
        cableCinPC,
        programChangeStatus,
        static_cast<BYTE>(
            program),
        0x00
    };


    std::wcout
        << L"\n[INFO ] SetSD80Tone"
        << std::endl;

    std::wcout
        << L"        Cable     = "
        << cable
        << std::endl;

    std::wcout
        << L"        MIDI Ch   = "
        << channel
        << std::endl;

    std::wcout
        << L"        Bank MSB  = "
        << bankMSB
        << std::endl;

    std::wcout
        << L"        Bank LSB  = "
        << bankLSB
        << std::endl;

    std::wcout
        << L"        Program   = "
        << program
        << std::endl;


    // --------------------------------------------------------
    // Send Bank Select MSB
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankSelectMSB))
    {
        return false;
    }


    // Give SD-80 time to process the message.

    Sleep(20);


    // --------------------------------------------------------
    // Send Bank Select LSB
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankSelectLSB))
    {
        return false;
    }


    Sleep(20);


    // --------------------------------------------------------
    // Send Program Change
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        programChange))
    {
        return false;
    }


    Sleep(50);


    std::wcout
        << L"[INFO ] SD-80 tone selection completed."
        << std::endl;


    return true;
}

// ============================================================
// Set SD-80 GS Mode Tone
//
// cable    : USB-MIDI Cable (0-15)
// channel  : MIDI Channel (1-16)
// bankMSB  : CC#0  (0-127)
// bankLSB  : CC#32 (0-127)
// program  : Program Change (0-127)
//
// GS Mode
// ============================================================

static bool SetSD80GSTone(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    int cable,
    int channel,
    int bankMSB,
    int bankLSB,
    int program)
{
    if (cable < 0 || cable > 15)
    {
        std::wcout
            << L"[ERROR] Invalid Cable = "
            << cable
            << std::endl;

        return false;
    }

    if (channel < 1 || channel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI Channel = "
            << channel
            << std::endl;

        return false;
    }

    if (bankMSB < 0 || bankMSB > 127 ||
        bankLSB < 0 || bankLSB > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Bank value."
            << std::endl;

        return false;
    }

    if (program < 0 || program > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Program = "
            << program
            << std::endl;

        return false;
    }


    // MIDI channel is zero-based internally.
    const BYTE midiChannel =
        static_cast<BYTE>(channel - 1);


    const BYTE controlChangeStatus =
        static_cast<BYTE>(
            0xB0 | midiChannel);


    const BYTE programChangeStatus =
        static_cast<BYTE>(
            0xC0 | midiChannel);


    // USB-MIDI CIN
    const BYTE cinCC = 0x0B;
    const BYTE cinPC = 0x0C;


    // Cable number is stored in the upper nibble.
    const BYTE cableCC =
        static_cast<BYTE>(
            (cable << 4) | cinCC);


    const BYTE cablePC =
        static_cast<BYTE>(
            (cable << 4) | cinPC);


    // --------------------------------------------------------
    // CC#0 - Bank Select MSB
    // --------------------------------------------------------

    const BYTE packetBankMSB[4] =
    {
        cableCC,
        controlChangeStatus,
        0x00,
        static_cast<BYTE>(bankMSB)
    };


    // --------------------------------------------------------
    // CC#32 - Bank Select LSB
    // --------------------------------------------------------

    const BYTE packetBankLSB[4] =
    {
        cableCC,
        controlChangeStatus,
        0x20,
        static_cast<BYTE>(bankLSB)
    };


    // --------------------------------------------------------
    // Program Change
    // --------------------------------------------------------

    const BYTE packetProgram[4] =
    {
        cablePC,
        programChangeStatus,
        static_cast<BYTE>(program),
        0x00
    };


    std::wcout
        << L"\n[INFO ] SetSD80GSTone"
        << std::endl;

    std::wcout
        << L"        Cable    = "
        << cable
        << std::endl;

    std::wcout
        << L"        Channel  = "
        << channel
        << std::endl;

    std::wcout
        << L"        Bank MSB = "
        << bankMSB
        << std::endl;

    std::wcout
        << L"        Bank LSB = "
        << bankLSB
        << std::endl;

    std::wcout
        << L"        Program  = "
        << program
        << std::endl;


    // --------------------------------------------------------
    // Send Bank Select MSB
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        packetBankMSB))
    {
        return false;
    }

    Sleep(50);


    // --------------------------------------------------------
    // Send Bank Select LSB
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        packetBankLSB))
    {
        return false;
    }

    Sleep(50);


    // --------------------------------------------------------
    // Send Program Change
    // --------------------------------------------------------

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        packetProgram))
    {
        return false;
    }

    Sleep(100);


    return true;
}


// ============================================================
// SD-80 Part Tone Selection
//
// partGroup:
//   'A' -> USB MIDI Cable 0 -> PART A
//   'B' -> USB MIDI Cable 1 -> PART B
//
// channel:
//   1～16
//
// bankMSB / bankLSB:
//   Bank Select MSB / LSB
//
// program:
//   Program Change 0～127
// ============================================================

static bool SetSD80PartTone(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    char partGroup,
    int channel,
    int bankMSB,
    int bankLSB,
    int program)
{
    int cable = -1;

    if (partGroup == 'A' || partGroup == 'a')
    {
        cable = 0;
    }
    else if (partGroup == 'B' || partGroup == 'b')
    {
        cable = 1;
    }
    else
    {
        std::wcout
            << L"[ERROR] Invalid SD-80 Part Group."
            << std::endl;

        return false;
    }


    if (channel < 1 || channel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI Channel."
            << std::endl;

        return false;
    }


    if (bankMSB < 0 || bankMSB > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Bank MSB."
            << std::endl;

        return false;
    }


    if (bankLSB < 0 || bankLSB > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Bank LSB."
            << std::endl;

        return false;
    }


    if (program < 0 || program > 127)
    {
        std::wcout
            << L"[ERROR] Invalid Program."
            << std::endl;

        return false;
    }


    std::wcout
        << L"\n[INFO ] SetSD80PartTone"
        << std::endl;

    std::wcout
        << L"        Part Group = "
        << partGroup
        << std::endl;

    std::wcout
        << L"        Cable      = "
        << cable
        << std::endl;

    std::wcout
        << L"        Channel    = "
        << channel
        << std::endl;

    std::wcout
        << L"        Bank MSB   = "
        << bankMSB
        << std::endl;

    std::wcout
        << L"        Bank LSB   = "
        << bankLSB
        << std::endl;

    std::wcout
        << L"        Program    = "
        << program
        << std::endl;


    // --------------------------------------------------------
    // Bank Select MSB
    // Control Change 0
    // --------------------------------------------------------

    BYTE bankMSBPacket[4] =
    {
        static_cast<BYTE>(
            (cable << 4) | 0x0B),

        static_cast<BYTE>(
            0xB0 | (channel - 1)),

        0x00,

        static_cast<BYTE>(
            bankMSB)
    };


    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankMSBPacket))
    {
        return false;
    }


    // --------------------------------------------------------
    // Bank Select LSB
    // Control Change 32
    // --------------------------------------------------------

    BYTE bankLSBPacket[4] =
    {
        static_cast<BYTE>(
            (cable << 4) | 0x0B),

        static_cast<BYTE>(
            0xB0 | (channel - 1)),

        0x20,

        static_cast<BYTE>(
            bankLSB)
    };


    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankLSBPacket))
    {
        return false;
    }


    // --------------------------------------------------------
    // Program Change
    // --------------------------------------------------------

    BYTE programPacket[4] =
    {
        static_cast<BYTE>(
            (cable << 4) | 0x0C),

        static_cast<BYTE>(
            0xC0 | (channel - 1)),

        static_cast<BYTE>(
            program),

        0x00
    };


    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        programPacket))
    {
        return false;
    }


    return true;
}

// ============================================================
// Send MIDI message to SD-80 PART A / PART B
//
// partGroup:
//   'A' -> Cable 0
//   'B' -> Cable 1
//
// status:
//   Normal MIDI status byte, e.g.
//     0x90 = Note On
//     0x80 = Note Off
//     0xB0 = Control Change
//     0xC0 = Program Change
//
// data1 / data2:
//   MIDI data bytes
// ============================================================

static bool SendSD80Midi(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    char partGroup,
    int channel,
    BYTE status,
    BYTE data1,
    BYTE data2,
    int outputCable = -1)
{
    int cable = outputCable;

    if (cable < 0)
    {
        if (partGroup == 'A' || partGroup == 'a')
        {
            cable = 0;
        }
        else if (partGroup == 'B' || partGroup == 'b')
        {
            cable = 1;
        }
        else
        {
            std::wcout
                << L"[ERROR] Invalid SD-80 Part Group."
                << std::endl;

            return false;
        }
    }

    if (cable < 0 || cable > 15)
    {
        std::wcout
            << L"[ERROR] Invalid SD-80 Output Cable."
            << std::endl;

        return false;
    }


    if (channel < 1 || channel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI Channel."
            << std::endl;

        return false;
    }


    // MIDI channel is zero-based in the status byte.
    BYTE midiStatus =
        static_cast<BYTE>(
            (status & 0xF0) |
            ((channel - 1) & 0x0F));


    // Determine USB-MIDI CIN from MIDI status.
    BYTE cin = 0x00;

    switch (status & 0xF0)
    {
    case 0x80:
        cin = 0x08;     // Note Off
        break;

    case 0x90:
        cin = 0x09;     // Note On
        break;

    case 0xA0:
        cin = 0x0A;     // Polyphonic Key Pressure
        break;

    case 0xB0:
        cin = 0x0B;     // Control Change
        break;

    case 0xC0:
        cin = 0x0C;     // Program Change
        break;

    case 0xD0:
        cin = 0x0D;     // Channel Pressure
        break;

    case 0xE0:
        cin = 0x0E;     // Pitch Bend
        break;

    default:
        std::wcout
            << L"[ERROR] Unsupported MIDI status."
            << std::endl;

        return false;
    }


    BYTE packet[4] =
    {
        static_cast<BYTE>(
            (cable << 4) | cin),

        midiStatus,

        data1,
        data2
    };


#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] SendSD80Midi"
        << std::endl;

    std::wcout
        << L"        Part Group = "
        << partGroup
        << std::endl;

    std::wcout
        << L"        Cable      = "
        << cable
        << std::endl;

    std::wcout
        << L"        Channel    = "
        << channel
        << std::endl;
#endif


    return SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        packet);
}

// ============================================================
// Route MIDI message to SD-80
//
// partGroup:
//   'A' -> PART A -> Cable 0
//   'B' -> PART B -> Cable 1
//
// This function is the routing layer between
// an incoming MIDI message and SendSD80Midi().
// The incoming USB-MIDI cable is carried separately by MidiMessage;
// the existing Part A / Part B routing remains unchanged.
// ============================================================

static bool RouteMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    char partGroup,
    int channel,
    BYTE status,
    BYTE data1,
    BYTE data2)
{
#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] RouteMidiToSD80"
        << std::endl;

    std::wcout
        << L"        Part Group = "
        << partGroup
        << std::endl;

    std::wcout
        << L"        Channel    = "
        << channel
        << std::endl;

    std::wcout
        << L"        Status     = 0x"
        << std::hex
        << static_cast<int>(status)
        << std::dec
        << std::endl;

    std::wcout
        << L"        Data1      = 0x"
        << std::hex
        << static_cast<int>(data1)
        << std::dec
        << std::endl;

    std::wcout
        << L"        Data2      = 0x"
        << std::hex
        << static_cast<int>(data2)
        << std::dec
        << std::endl;
#endif


    return SendSD80Midi(
        winusbHandle,
        partGroup,
        channel,
        status,
        data1,
        data2,
        -1);
}


// ============================================================
// SD-80 MIDI Routing Configuration
//
// Normal MIDI Channel 1～16 can be routed to either:
//
//   PART A
// or
//   PART B
//
// The MIDI channel number itself is preserved.
// ============================================================


struct SD80MidiRouting
{
    BYTE inputCable = 0;
    BYTE outputCable = 0;

    int inputChannel = 0;
    int outputChannel = 0;

    char partGroup = 0;
};

// ============================================================
// SD-80 Tone Setting
//
// Bank Select + Program Change configuration associated with a
// logical SD-80 routing destination.
//
// The existing SetSD80Tone() function remains the low-level USB-MIDI
// sender. This structure lets higher layers describe a tone without
// mixing tone data into the cable/channel routing object.
// ============================================================

struct SD80ToneSetting
{
    int bankMSB = 0;
    int bankLSB = 0;
    int program = 0;
};

static bool SetSD80ToneForRouting(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    const SD80ToneSetting& tone)
{
    if (routing.outputCable > 15)
    {
        std::wcout
            << L"[ERROR] Invalid routing Output Cable."
            << std::endl;

        return false;
    }

    if (routing.outputChannel < 1 ||
        routing.outputChannel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid routing Output Channel."
            << std::endl;

        return false;
    }

    std::wcout
        << L"\n[INFO ] SetSD80ToneForRouting"
        << std::endl;

    std::wcout
        << L"        Part Group     = "
        << routing.partGroup
        << std::endl;

    std::wcout
        << L"        Output Cable   = "
        << static_cast<int>(routing.outputCable)
        << std::endl;

    std::wcout
        << L"        Output Channel = "
        << routing.outputChannel
        << std::endl;

    std::wcout
        << L"        Bank MSB       = "
        << tone.bankMSB
        << std::endl;

    std::wcout
        << L"        Bank LSB       = "
        << tone.bankLSB
        << std::endl;

    std::wcout
        << L"        Program        = "
        << tone.program
        << std::endl;

    return SetSD80Tone(
        winusbHandle,
        static_cast<int>(routing.outputCable),
        routing.outputChannel,
        tone.bankMSB,
        tone.bankLSB,
        tone.program);
}


// ============================================================
// Part Tone Configuration
//
// This layer answers:
//   WHAT tone belongs to each logical Part?
//   WHICH MIDI channel should be used when applying it?
//
// Timing/policy is controlled separately by
// SD80BRIDGE_APPLY_PART_TONES_ON_STARTUP (default 0).
//
// Current confirmed routing:
//   PART A -> Output Cable 0
//   PART B -> Output Cable 1
//
// The first version only defines the configuration. It does not
// change tones unless the startup macro is explicitly enabled.
// ============================================================

struct SD80PartToneConfig
{
    char partGroup = 0;

    int outputChannel = 0;

    SD80ToneSetting tone{};
};

static const SD80PartToneConfig g_partToneConfig[] =
{
    {
        'A',
        2,
        { 0, 0, 8 }
    },

    {
        'B',
        2,
        { 0, 0, 9 }
    }
};

static const SD80PartToneConfig* FindPartToneConfig(
    char partGroup)
{
    for (const auto& config : g_partToneConfig)
    {
        if (config.partGroup == partGroup)
        {
            return &config;
        }
    }

    return nullptr;
}

static bool ApplyConfiguredPartTone(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    char partGroup)
{
    const SD80PartToneConfig* config =
        FindPartToneConfig(partGroup);

    if (config == nullptr)
    {
        std::wcout
            << L"[ERROR] No tone configuration for Part Group = "
            << partGroup
            << std::endl;

        return false;
    }

    SD80MidiRouting routing{};

    if (config->partGroup == 'A' ||
        config->partGroup == 'a')
    {
        routing.inputCable = 2;
        routing.inputChannel = config->outputChannel;
        routing.partGroup = 'A';
        routing.outputCable = 0;
        routing.outputChannel = config->outputChannel;
    }
    else
    {
        routing.inputCable = 3;
        routing.inputChannel = config->outputChannel;
        routing.partGroup = 'B';
        routing.outputCable = 1;
        routing.outputChannel = config->outputChannel;
    }

    std::wcout
        << L"\n[INFO ] Applying configured Part tone"
        << std::endl;

    return SetSD80ToneForRouting(
        winusbHandle,
        routing,
        config->tone);
}

static bool ApplyConfiguredPartTonesOnStartup(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 PART TONE STARTUP CONFIGURATION"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    if (!ApplyConfiguredPartTone(
        winusbHandle,
        'A'))
    {
        return false;
    }

    if (!ApplyConfiguredPartTone(
        winusbHandle,
        'B'))
    {
        return false;
    }

    return true;
}

#if SD80BRIDGE_TONE_TEST

// ============================================================
// One-shot SD-80 Tone Selection Test
//
// Target:
//   Part B
//   Output Cable   = 1
//   Output Channel = 2
//
// Tone:
//   Bank MSB = 0
//   Bank LSB = 0
//   Program = 9
//
// This test sends only tone-selection messages.
// It does not send a Note On/Off.
// ============================================================

static bool RunOneShotToneTest(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing{};

    routing.inputCable = 3;
    routing.inputChannel = 2;
    routing.partGroup = 'B';
    routing.outputCable = 1;
    routing.outputChannel = 2;

    SD80ToneSetting tone{};
    tone.bankMSB = 0;
    tone.bankLSB = 0;
    tone.program = 9;

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 ONE-SHOT TONE TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    return SetSD80ToneForRouting(
        winusbHandle,
        routing,
        tone);
}

#endif // SD80BRIDGE_TONE_TEST


#if SD80BRIDGE_TONE_COMPARE_TEST

// ============================================================
// Program 8 / 9 / 10 comparison test
//
// Target:
//   Part B / Output Cable 1 / Channel 2
//
// Each Program Change is sent once, with Bank MSB/LSB = 0.
// No Note On/Off is generated.
// The test pauses for ENTER between programs so the user can
// listen to and compare each resulting tone.
// ============================================================

static bool RunProgramCompareTest(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing{};

    routing.inputCable = 3;
    routing.inputChannel = 2;
    routing.partGroup = 'B';
    routing.outputCable = 1;
    routing.outputChannel = 2;

    const int programs[] =
    {
        8,
        9,
        10
    };

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 PROGRAM 8 / 9 / 10 AUDITION TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    for (const int program : programs)
    {
        SD80ToneSetting tone{};
        tone.bankMSB = 0;
        tone.bankLSB = 0;
        tone.program = program;

        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Program = "
            << program
            << std::endl;

        std::wcout
            << L"Press ENTER to set tone and play C4..."
            << std::endl;

        std::cin.get();

        if (!SetSD80ToneForRouting(
            winusbHandle,
            routing,
            tone))
        {
            std::wcout
                << L"[ERROR] Program Change failed."
                << std::endl;

            return false;
        }

        // Note On: Cable 1 / Channel 2 / C4 / Velocity 100
        const BYTE noteOn[4] =
        {
            0x19,
            0x91,
            0x3C,
            0x64
        };

        // Note Off: Cable 1 / Channel 2 / C4
        const BYTE noteOff[4] =
        {
            0x18,
            0x81,
            0x3C,
            0x00
        };

        std::wcout
            << L"[INFO ] Playing C4..."
            << std::endl;

        if (!SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOn))
        {
            std::wcout
                << L"[ERROR] Note On failed."
                << std::endl;

            return false;
        }

        Sleep(1000);

        if (!SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOff))
        {
            std::wcout
                << L"[ERROR] Note Off failed."
                << std::endl;

            return false;
        }

        std::wcout
            << L"[INFO ] C4 playback completed."
            << std::endl;

        if (program != programs[2])
        {
            std::wcout
                << L"Press ENTER for the next program..."
                << std::endl;

            std::cin.get();
        }
    }

    std::wcout
        << L"\n[INFO ] Program 8 / 9 / 10 audition test completed."
        << std::endl;

    return true;
}

#endif // SD80BRIDGE_TONE_COMPARE_TEST





// ============================================================
// Resolve USB-MIDI IN Cable to the SD-80 logical destination
//
//   Cable 0 -> SD-80 PART A
//   Cable 1 -> SD-80 PART B
//   Cable 2 -> SD-80 MIDI IN 1 -> PART A
//   Cable 3 -> SD-80 MIDI IN 2 -> PART B
//
// This applies only to incoming USB-MIDI routing.
// Existing Cable 0/1/2/3 output tests remain unchanged.
// ============================================================

static bool ResolveSD80RoutingForInputCable(
    BYTE cable,
    int inputChannel,
    SD80MidiRouting& routing)
{
    if (inputChannel < 1 ||
        inputChannel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI input Channel = "
            << inputChannel
            << std::endl;

        return false;
    }

    routing = {};
    routing.inputCable = cable;
    routing.inputChannel = inputChannel;
    routing.outputChannel = inputChannel;

    switch (cable)
    {
    case 0:
    case 2:
        routing.partGroup = 'A';
        routing.outputCable = 0;
        return true;

    case 1:
    case 3:
        routing.partGroup = 'B';
        routing.outputCable = 1;
        return true;

    default:
        std::wcout
            << L"[ERROR] Unsupported SD-80 USB input Cable = "
            << static_cast<int>(cable)
            << std::endl;

        return false;
    }
}


// Forward declaration of the existing Cable-independent routing function.
static bool RouteNormalMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    int inputChannel,
    BYTE status,
    BYTE data1,
    BYTE data2);

static bool RouteNormalMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    BYTE status,
    BYTE data1,
    BYTE data2)
{
#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] RouteNormalMidiToSD80"
        << std::endl;

    std::wcout
        << L"        Input Cable   = "
        << static_cast<int>(routing.inputCable)
        << std::endl;

    std::wcout
        << L"        Input Channel = "
        << routing.inputChannel
        << std::endl;

    std::wcout
        << L"        Part Group    = "
        << routing.partGroup
        << std::endl;

    std::wcout
        << L"        Output Cable  = "
        << static_cast<int>(routing.outputCable)
        << std::endl;

    std::wcout
        << L"        Output Channel= "
        << routing.outputChannel
        << std::endl;
#endif

    return RouteNormalMidiToSD80(
        winusbHandle,
        routing,
        routing.inputChannel,
        status,
        data1,
        data2);
}





// ============================================================
// Route normal MIDI Channel 1～16 to SD-80
//
// inputChannel:
//   1～16
//
// routing.partGroup:
//   'A' -> PART A
//   'B' -> PART B
// ============================================================

static bool RouteNormalMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    int inputChannel,
    BYTE status,
    BYTE data1,
    BYTE data2)
{
    if (inputChannel < 1 ||
        inputChannel > 16)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI Channel."
            << std::endl;

        return false;
    }


    if (routing.partGroup != 'A' &&
        routing.partGroup != 'a' &&
        routing.partGroup != 'B' &&
        routing.partGroup != 'b')
    {
        std::wcout
            << L"[ERROR] Invalid SD-80 Part Group."
            << std::endl;

        return false;
    }


#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] RouteNormalMidiToSD80"
        << std::endl;

    std::wcout
        << L"        Input Channel = "
        << inputChannel
        << std::endl;

    std::wcout
        << L"        Part Group    = "
        << routing.partGroup
        << std::endl;
#endif


    return RouteMidiToSD80(
        winusbHandle,
        routing.partGroup,
        inputChannel,
        status,
        data1,
        data2);
}


// ============================================================
// Process incoming MIDI message
//
// MIDI message format:
//
//   status  data1  data2
//
// Channel Voice messages:
//
//   0x8n Note Off
//   0x9n Note On
//   0xAn Poly Pressure
//   0xBn Control Change
//   0xCn Program Change
//   0xDn Channel Pressure
//   0xEn Pitch Bend
//
// n = MIDI channel - 1
// ============================================================

// ============================================================
// Incoming MIDI message
// ============================================================

struct MidiMessage
{
    BYTE cable;
    BYTE status;
    BYTE data1;
    BYTE data2;
};





static bool ProcessMidiMessage(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    BYTE cable,
    BYTE status,
    BYTE data1,
    BYTE data2)
{
    // --------------------------------------------------------
    // Reject non-MIDI status bytes
    // --------------------------------------------------------

    if (status < 0x80)
    {
        std::wcout
            << L"[ERROR] Invalid MIDI status byte."
            << std::endl;

        return false;
    }


    // --------------------------------------------------------
    // System messages are not routed here.
    //
    // 0xF0～0xFF are MIDI System messages.
    // --------------------------------------------------------

    if ((status & 0xF0) == 0xF0)
    {
        std::wcout
            << L"[INFO ] System MIDI message ignored."
            << std::endl;

        return false;
    }


    // --------------------------------------------------------
    // Extract MIDI Channel
    //
    // status low nibble:
    //
    //   0 = Channel 1
    //   1 = Channel 2
    //   ...
    //   F = Channel 16
    // --------------------------------------------------------

    const int midiChannel =
        static_cast<int>(status & 0x0F) + 1;


    // --------------------------------------------------------
    // Display received MIDI message
    // --------------------------------------------------------

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] ProcessMidiMessage"
        << std::endl;

    std::wcout
        << L"        Cable   = "
        << static_cast<int>(cable)
        << std::endl;




    std::wcout
        << L"        Status  = 0x"
        << std::hex
        << static_cast<int>(status)
        << std::dec
        << std::endl;

    std::wcout
        << L"        Data1   = 0x"
        << std::hex
        << static_cast<int>(data1)
        << std::dec
        << std::endl;

    std::wcout
        << L"        Data2   = 0x"
        << std::hex
        << static_cast<int>(data2)
        << std::dec
        << std::endl;

    std::wcout
        << L"        Channel = "
        << midiChannel
        << std::endl;
#endif


    // --------------------------------------------------------
    // Route to selected SD-80 Part
    // --------------------------------------------------------

    return RouteNormalMidiToSD80(
        winusbHandle,
        routing,
        status,
        data1,
        data2);
}

// ============================================================
// Parse one USB-MIDI packet
//
// USB-MIDI packet:
//
//   Byte 0 : Cable Number + CIN
//   Byte 1 : MIDI Status
//   Byte 2 : MIDI Data 1
//   Byte 3 : MIDI Data 2
// ============================================================

static bool ParseUsbMidiPacket(
    const BYTE packet[4],
    MidiMessage& message)
{
    const BYTE header = packet[0];

    const BYTE cable =
        static_cast<BYTE>(header >> 4);



    const BYTE cin =
        static_cast<BYTE>(header & 0x0F);


#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] ParseUsbMidiPacket"
        << std::endl;

    std::wcout
        << L"        Cable  = "
        << static_cast<int>(cable)
        << std::endl;

    std::wcout
        << L"        CIN    = 0x"
        << std::hex
        << static_cast<int>(cin)
        << std::dec
        << std::endl;
#endif


    // USB-MIDI CIN 8 = Note Off
    // USB-MIDI CIN 9 = Note On
    // USB-MIDI CIN A = Poly Key Pressure
    // USB-MIDI CIN B = Control Change
    // USB-MIDI CIN C = Program Change
    // USB-MIDI CIN D = Channel Pressure
    // USB-MIDI CIN E = Pitch Bend

    if (cin < 0x8 || cin > 0xE)
    {
        std::wcout
            << L"[INFO ] Unsupported USB-MIDI CIN."
            << std::endl;

        return false;
    }


    message.status = packet[1];
    message.data1 = packet[2];
    message.data2 = packet[3];
    message.cable = cable;

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"        Status = 0x"
        << std::hex
        << static_cast<int>(message.status)
        << std::endl;

    std::wcout
        << L"        Data1  = 0x"
        << static_cast<int>(message.data1)
        << std::endl;

    std::wcout
        << L"        Data2  = 0x"
        << static_cast<int>(message.data2)
        << std::dec
        << std::endl;
#endif


    return true;
}






// ============================================================
// Process an incoming MIDI message
//
// This is the input-layer entry point.
//
// Later, a real MIDI device can call this function.
// For now, TestMidiInput() will call it.
// ============================================================

static bool HandleIncomingMidiMessage(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const SD80MidiRouting& routing,
    const MidiMessage& message)
{
#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] HandleIncomingMidiMessage"
        << std::endl;

    std::wcout
        << L"        Cable  = "
        << static_cast<int>(message.cable)
        << std::endl;


    std::wcout
        << L"        Status = 0x"
        << std::hex
        << static_cast<int>(message.status)
        << std::dec
        << std::endl;
#endif


    return ProcessMidiMessage(
        winusbHandle,
        routing,
        message.cable,
        message.status,
        message.data1,
        message.data2);
}

// ============================================================
// Handle incoming USB-MIDI message using the actual USB Cable
//
// Normal runtime routing:
//   Cable 0 -> PART A -> output Cable 0
//   Cable 1 -> PART B -> output Cable 1
//   Cable 2 -> MIDI IN 1 -> PART A -> output Cable 0
//   Cable 3 -> MIDI IN 2 -> PART B -> output Cable 1
//
// This overload resolves the Part Group from MidiMessage.cable,
// then uses the existing 3-argument handler unchanged.
// ============================================================

static bool HandleIncomingMidiMessage(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const MidiMessage& message)
{
    const int inputChannel =
        static_cast<int>(message.status & 0x0F) + 1;

    SD80MidiRouting routing{};

    if (!ResolveSD80RoutingForInputCable(
        message.cable,
        inputChannel,
        routing))
    {
        return false;
    }

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"\n[INFO ] Resolved SD-80 Routing"
        << std::endl;

    std::wcout
        << L"        Input Cable   = "
        << static_cast<int>(routing.inputCable)
        << std::endl;

    std::wcout
        << L"        Input Channel = "
        << routing.inputChannel
        << std::endl;

    std::wcout
        << L"        Part Group    = "
        << routing.partGroup
        << std::endl;

    std::wcout
        << L"        Output Cable  = "
        << static_cast<int>(routing.outputCable)
        << std::endl;

    std::wcout
        << L"        Output Channel= "
        << routing.outputChannel
        << std::endl;

#endif
    return HandleIncomingMidiMessage(
        winusbHandle,
        routing,
        message);
}


// ============================================================
// ============================================================
// DIAGNOSTIC / TEST FUNCTIONS
//
// Disabled by default. Define SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS=1
// in project preprocessor definitions to compile them again.
// ============================================================

#ifndef SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS
#define SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS 0
#endif

#ifndef SD80BRIDGE_TONE_TEST
#define SD80BRIDGE_TONE_TEST 0
#endif

#ifndef SD80BRIDGE_TONE_COMPARE_TEST
#define SD80BRIDGE_TONE_COMPARE_TEST 0
#endif

// Apply configured Part A/B tones automatically at startup.
// Keep this disabled until the desired Part tone configuration is confirmed.
#ifndef SD80BRIDGE_APPLY_PART_TONES_ON_STARTUP
#define SD80BRIDGE_APPLY_PART_TONES_ON_STARTUP 1
#endif

#if SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS

// Test USB-MIDI packet parser
// ============================================================

static void TestParseUsbMidiPacket()
{
    const BYTE noteOn[4] =
    {
        0x09,
        0x90,
        0x3C,
        0x7F
    };


    MidiMessage message{};


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI PACKET PARSER TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    if (ParseUsbMidiPacket(
        noteOn,
        message))
    {
        std::wcout
            << L"\n[INFO ] USB-MIDI packet parsed successfully."
            << std::endl;
    }
    else
    {
        std::wcout
            << L"\n[ERROR] USB-MIDI packet parse failed."
            << std::endl;
    }
}

// ============================================================
// Test USB-MIDI packet -> MIDI input processing
// ============================================================

static void TestUsbMidiToMidiInput(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing =
    {
        'A'
    };

    const BYTE noteOn[4] =
    {
        0x09,
        0x90,
        0x3C,
        0x7F
    };

    const BYTE noteOff[4] =
    {
        0x08,
        0x80,
        0x3C,
        0x00
    };


    MidiMessage message{};


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI -> MIDI INPUT TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // Note On
    // --------------------------------------------------------

    std::wcout
        << L"\n[TEST] USB-MIDI Note On"
        << std::endl;

    if (!ParseUsbMidiPacket(
        noteOn,
        message))
    {
        std::wcout
            << L"[ERROR] Note On parse failed."
            << std::endl;

        return;
    }


    std::wcout
        << L"[INFO ] Passing parsed MIDI message to input layer..."
        << std::endl;


    HandleIncomingMidiMessage(
        winusbHandle,
        routing,
        message);


    Sleep(1000);


    // --------------------------------------------------------
    // Note Off
    // --------------------------------------------------------

    std::wcout
        << L"\n[TEST] USB-MIDI Note Off"
        << std::endl;

    if (!ParseUsbMidiPacket(
        noteOff,
        message))
    {
        std::wcout
            << L"[ERROR] Note Off parse failed."
            << std::endl;

        return;
    }


    std::wcout
        << L"[INFO ] Passing parsed MIDI message to input layer..."
        << std::endl;


    HandleIncomingMidiMessage(
        winusbHandle,
        routing,
        message);


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI -> MIDI INPUT TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}


// ============================================================
// Test reading USB-MIDI IN from SD-80 EP 0x81
//
// Uses OVERLAPPED I/O so the read can be cancelled after
// a real 10-second timeout.
//
// This test ONLY reads and displays received bytes.
// It does NOT route the received MIDI data.
// ============================================================

static void TestReadUsbMidiIn(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    UNREFERENCED_PARAMETER(deviceHandle);

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI IN READ TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    BYTE buffer[64] = {};

    ULONG bytesTransferred = 0;


    // --------------------------------------------------------
    // Create an OVERLAPPED event
    // --------------------------------------------------------

    OVERLAPPED overlapped{};

    overlapped.hEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);


    if (overlapped.hEvent == nullptr)
    {
        std::wcout
            << L"[ERROR] CreateEventW failed."
            << std::endl;

        return;
    }


    std::wcout
        << L"[INFO ] Reading EP 0x81..."
        << std::endl;

    std::wcout
        << L"[INFO ] Waiting up to 10 seconds..."
        << std::endl;


    // --------------------------------------------------------
    // Start asynchronous WinUSB read
    // --------------------------------------------------------

    BOOL result =
        WinUsb_ReadPipe(
            winusbHandle,
            0x81,
            buffer,
            sizeof(buffer),
            &bytesTransferred,
            &overlapped);


    if (!result)
    {
        DWORD error =
            GetLastError();


        if (error != ERROR_IO_PENDING)
        {
            std::wcout
                << L"[ERROR] WinUsb_ReadPipe failed."
                << std::endl;

            std::wcout
                << L"[ERROR] Error code = "
                << error
                << std::endl;

            CloseHandle(
                overlapped.hEvent);

            return;
        }


        // ----------------------------------------------------
        // Wait for data, but no longer than 10 seconds
        // ----------------------------------------------------

        DWORD waitResult =
            WaitForSingleObject(
                overlapped.hEvent,
                10000);


        if (waitResult == WAIT_TIMEOUT)
        {
            std::wcout
                << L"[INFO ] 10-second timeout."
                << std::endl;


            // Cancel the pending WinUSB request.
            WinUsb_AbortPipe(
                winusbHandle,
                0x81);


            CloseHandle(
                overlapped.hEvent);

            return;
        }


        if (waitResult != WAIT_OBJECT_0)
        {
            std::wcout
                << L"[ERROR] WaitForSingleObject failed."
                << std::endl;


            WinUsb_AbortPipe(
                winusbHandle,
                0x81);


            CloseHandle(
                overlapped.hEvent);

            return;
        }
    }


    // --------------------------------------------------------
    // Obtain final transfer result
    // --------------------------------------------------------

    if (!WinUsb_GetOverlappedResult(
        winusbHandle,
        &overlapped,
        &bytesTransferred,
        FALSE))
    {
        DWORD error =
            GetLastError();


        std::wcout
            << L"[ERROR] WinUsb_GetOverlappedResult failed."
            << std::endl;

        std::wcout
            << L"[ERROR] Error code = "
            << error
            << std::endl;


        CloseHandle(
            overlapped.hEvent);

        return;
    }


    CloseHandle(
        overlapped.hEvent);


    // --------------------------------------------------------
    // Display received data
    // --------------------------------------------------------

    std::wcout
        << L"[INFO ] WinUsb_ReadPipe OK."
        << std::endl;

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"[INFO ] Bytes received = "
        << bytesTransferred
        << std::endl;

#endif

    if (bytesTransferred == 0)
    {
        std::wcout
            << L"[INFO ] No data received."
            << std::endl;

        return;
    }


    std::wcout
        << L"[INFO ] Received data:"
        << std::endl;


    for (ULONG i = 0;
        i < bytesTransferred;
        ++i)
    {
        std::wcout
            << std::hex
            << std::uppercase
            << static_cast<int>(buffer[i])
            << L" ";

        if ((i + 1) % 16 == 0)
        {
            std::wcout
                << std::endl;
        }
    }


    std::wcout
        << std::dec
        << std::nouppercase
        << std::endl;


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI IN READ TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}

// ============================================================
// Test Cable 0 / Cable 1 with MIDI Channel 2
//
// Cable 0 / Channel 2 -> 09 91 3C 7F
// Cable 1 / Channel 2 -> 19 91 3C 7F
//
// This test sends Note On / Note Off directly.
// No Program Change is sent.
// ============================================================

static void TestCable0Cable1Channel2(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE 0 / CABLE 1 / CABLE 2 - CHANNEL 2 TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // Cable 0 / Channel 2
    // --------------------------------------------------------

    const BYTE noteOnCable0[4] =
    {
        0x09,       // Cable 0 / CIN 9
        0x91,       // Note On / MIDI Channel 2
        0x30,       // C4
        0x61        // Velocity
    };

    const BYTE noteOffCable0[4] =
    {
        0x08,       // Cable 0 / CIN 8
        0x81,       // Note Off / MIDI Channel 2
        0x30,
        0x00
    };


    std::wcout
        << L"\n[TEST] Cable 0 / Channel 2"
        << std::endl;

    std::wcout
        << L"Packet : 09 91 30 61"
        << std::endl;

    std::wcout
        << L"Press ENTER to play Cable 0..."
        << std::endl;

    std::wstring dummy;
    std::getline(std::wcin, dummy);


    std::wcout
        << L"[INFO] Sending Cable 0 Note On..."
        << std::endl;

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOnCable0))
    {
        std::wcout
            << L"[ERROR] Cable 0 Note On failed."
            << std::endl;

        return;
    }

    Sleep(1000);


    std::wcout
        << L"[INFO] Sending Cable 0 Note Off..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOffCable0);

    Sleep(500);


    // --------------------------------------------------------
    // Cable 1 / Channel 2
    // --------------------------------------------------------

    const BYTE noteOnCable1[4] =
    {
        0x19,       // Cable 1 / CIN 9
        0x91,       // Note On / MIDI Channel 2
        0x3C,       // C4
        0x7F        // Velocity
    };

    const BYTE noteOffCable1[4] =
    {
        0x18,       // Cable 1 / CIN 8
        0x81,       // Note Off / MIDI Channel 2
        0x3C,
        0x00
    };


    std::wcout
        << L"\n[TEST] Cable 1 / Channel 2"
        << std::endl;

    std::wcout
        << L"Packet : 19 91 3C 7F"
        << std::endl;

    std::wcout
        << L"Press ENTER to play Cable 1..."
        << std::endl;

    std::getline(std::wcin, dummy);


    std::wcout
        << L"[INFO] Sending Cable 1 Note On..."
        << std::endl;

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOnCable1))
    {
        std::wcout
            << L"[ERROR] Cable 1 Note On failed."
            << std::endl;

        return;
    }

    Sleep(1000);


    std::wcout
        << L"[INFO] Sending Cable 1 Note Off..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOffCable1);

    Sleep(500);


    // --------------------------------------------------------
    // Cable 2 / Channel 2
    // --------------------------------------------------------

    const BYTE noteOnCable2[4] =
    {
        0x29,       // Cable 2 / CIN 9
        0x91,       // Note On / MIDI Channel 2
        0x30,       // 実際にSD-80から受信したNote
        0x66        // 実際にSD-80から受信したVelocity;
    };

    const BYTE noteOffCable2[4] =
    {
        0x28,       // Cable 2 / CIN 8
        0x81,       // Note Off / MIDI Channel 2
        0x30,
        0x00
    };


    std::wcout
        << L"\n[TEST] Cable 2 / Channel 2"
        << std::endl;

    std::wcout
        << L"Packet : 29 91 3C 7F"
        << std::endl;

    std::wcout
        << L"Press ENTER to play Cable 2..."
        << std::endl;

    std::getline(std::wcin, dummy);


    std::wcout
        << L"[INFO] Sending Cable 2 Note On..."
        << std::endl;

    if (!SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOnCable2))
    {
        std::wcout
            << L"[ERROR] Cable 2 Note On failed."
            << std::endl;

        return;
    }

    Sleep(1000);


    std::wcout
        << L"[INFO] Sending Cable 2 Note Off..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOffCable2);

    Sleep(500);


    // --------------------------------------------------------
    // Completed
    // --------------------------------------------------------

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE 0 / CABLE 1 / CABLE 2 - CHANNEL 2 TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}




// ============================================================
// Cable 0-3 / Channel 2 output test
//
// Sends the same Note On/Off to Cable 0, 1, 2, and 3.
// Each cable waits for ENTER before sending so the user can
// identify exactly which output cable produces sound.
//
// Note = 0x30
// MIDI Channel = 2
// ============================================================

static void TestCable0To3Channel2(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wstring dummy;

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE 0 / 1 / 2 / 3 - CHANNEL 2 OUTPUT TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    const BYTE note = 0x30;
    const BYTE velocity = 0x66;

    for (BYTE cable = 0; cable < 4; ++cable)
    {
        const BYTE noteOn[4] =
        {
            static_cast<BYTE>((cable << 4) | 0x09),
            0x91,
            note,
            velocity
        };

        const BYTE noteOff[4] =
        {
            static_cast<BYTE>((cable << 4) | 0x08),
            0x81,
            note,
            0x00
        };

        std::wcout
            << L"\n[TEST] Cable "
            << static_cast<int>(cable)
            << L" / Channel 2"
            << std::endl;

        std::wcout
            << L"  Note On  : ";

        for (int i = 0; i < 4; ++i)
        {
            std::wcout
                << std::hex
                << std::uppercase
                << static_cast<int>(noteOn[i])
                << (i < 3 ? L" " : L"");
        }

        std::wcout
            << std::dec
            << std::endl;

        std::wcout
            << L"  Press ENTER to send this packet..."
            << std::endl;

        std::getline(std::wcin, dummy);

        std::wcout
            << L"[INFO] Sending Cable "
            << static_cast<int>(cable)
            << L" Note On..."
            << std::endl;

        if (!SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOn))
        {
            std::wcout
                << L"[ERROR] Cable "
                << static_cast<int>(cable)
                << L" Note On failed."
                << std::endl;

            return;
        }

        Sleep(1000);

        std::wcout
            << L"[INFO] Sending Cable "
            << static_cast<int>(cable)
            << L" Note Off..."
            << std::endl;

        if (!SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOff))
        {
            std::wcout
                << L"[ERROR] Cable "
                << static_cast<int>(cable)
                << L" Note Off failed."
                << std::endl;

            return;
        }

        Sleep(500);

        std::wcout
            << L"[RESULT] Cable "
            << static_cast<int>(cable)
            << L" test completed."
            << std::endl;
    }

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE 0 / 1 / 2 / 3 - CHANNEL 2 OUTPUT TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}

// ============================================================
// Parse received USB-MIDI IN packets
//
// Reads one 64-byte USB-MIDI transfer and parses every
// complete 4-byte USB-MIDI packet.
//
// This test does NOT send anything back to the SD-80.
// ============================================================

static void TestParseReceivedUsbMidi(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    UNREFERENCED_PARAMETER(deviceHandle);

    BYTE buffer[64] = {};
    ULONG bytesTransferred = 0;

    OVERLAPPED overlapped{};

    overlapped.hEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);

    if (overlapped.hEvent == nullptr)
    {
        std::wcout
            << L"[ERROR] CreateEventW failed."
            << std::endl;

        return;
    }

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI IN PARSE TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    std::wcout
        << L"[INFO ] Waiting for MIDI input on EP 0x81..."
        << std::endl;

    std::wcout
        << L"[INFO ] Press one key on the MIDI keyboard."
        << std::endl;

    BOOL result =
        WinUsb_ReadPipe(
            winusbHandle,
            0x81,
            buffer,
            sizeof(buffer),
            &bytesTransferred,
            &overlapped);

    if (!result)
    {
        DWORD error =
            GetLastError();

        if (error != ERROR_IO_PENDING)
        {
            std::wcout
                << L"[ERROR] WinUsb_ReadPipe failed."
                << std::endl;

            std::wcout
                << L"[ERROR] Error code = "
                << error
                << std::endl;

            CloseHandle(
                overlapped.hEvent);

            return;
        }

        DWORD waitResult =
            WaitForSingleObject(
                overlapped.hEvent,
                10000);

        if (waitResult == WAIT_TIMEOUT)
        {
            std::wcout
                << L"[INFO ] 10-second timeout."
                << std::endl;

            WinUsb_AbortPipe(
                winusbHandle,
                0x81);

            CloseHandle(
                overlapped.hEvent);

            return;
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            std::wcout
                << L"[ERROR] WaitForSingleObject failed."
                << std::endl;

            WinUsb_AbortPipe(
                winusbHandle,
                0x81);

            CloseHandle(
                overlapped.hEvent);

            return;
        }
    }

    if (!WinUsb_GetOverlappedResult(
        winusbHandle,
        &overlapped,
        &bytesTransferred,
        FALSE))
    {
        DWORD error =
            GetLastError();

        std::wcout
            << L"[ERROR] WinUsb_GetOverlappedResult failed."
            << std::endl;

        std::wcout
            << L"[ERROR] Error code = "
            << error
            << std::endl;

        CloseHandle(
            overlapped.hEvent);

        return;
    }

    CloseHandle(
        overlapped.hEvent);

    std::wcout
        << L"[INFO ] Bytes received = "
        << bytesTransferred
        << std::endl;

    if (bytesTransferred == 0)
    {
        std::wcout
            << L"[INFO ] No data received."
            << std::endl;

        return;
    }

    // --------------------------------------------------------
    // Parse complete USB-MIDI packets
    // --------------------------------------------------------

    const ULONG packetCount =
        bytesTransferred / 4;

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"[INFO ] USB-MIDI packet count = "
        << packetCount
        << std::endl;

#endif
    for (ULONG i = 0;
        i < packetCount;
        ++i)
    {
        const BYTE* packet =
            &buffer[i * 4];

        MidiMessage message{};

#if SD80BRIDGE_VERBOSE_MIDI
        std::wcout
            << L"\n[USB-MIDI PACKET "
            << (i + 1)
            << L"]"
            << std::endl;

        std::wcout
            << L"  Raw      : "
            << std::hex
            << std::uppercase
            << static_cast<int>(packet[0])
            << L" "
            << static_cast<int>(packet[1])
            << L" "
            << static_cast<int>(packet[2])
            << L" "
            << static_cast<int>(packet[3])
            << std::dec
            << std::nouppercase
            << std::endl;

#endif
        if (!ParseUsbMidiPacket(
            packet,
            message))
        {
#if SD80BRIDGE_VERBOSE_MIDI
            std::wcout
                << L"  [INFO ] Packet ignored."
                << std::endl;
#endif

            continue;
        }

#if !SD80BRIDGE_VERBOSE_MIDI
        std::wcout
            << L"[MIDI] Cable="
            << static_cast<int>(message.cable)
            << L" Ch="
            << (static_cast<int>(message.status & 0x0F) + 1)
            << L" Status=0x"
            << std::hex
            << static_cast<int>(message.status)
            << L" Data1=0x"
            << static_cast<int>(message.data1)
            << L" Data2=0x"
            << static_cast<int>(message.data2)
            << std::dec
            << std::endl;
#endif

        const int midiChannel =
            (message.status & 0x0F) + 1;

        const int cable =
            static_cast<int>(message.cable);

#if SD80BRIDGE_VERBOSE_MIDI
        std::wcout
            << L"  MIDI Channel = "
            << midiChannel
            << std::endl;

        std::wcout
            << L"  USB Cable    = "
            << cable
            << std::endl;

#endif

        // --------------------------------------------------------
        // Pass the received MIDI message to the MIDI input layer.
        //
        // IMPORTANT:
        // This is the first real connection between
        //
        //   SD-80 EP 0x81
        //        ↓
        //   USB-MIDI parser
        //        ↓
        //   MIDI input layer
        //
        // No special re-routing is performed here.
        // The existing input-layer routing is used.
        // --------------------------------------------------------

#if SD80BRIDGE_VERBOSE_MIDI
        std::wcout
            << L"  [INFO ] Passing received MIDI to input layer..."
            << std::endl;

#endif
        if (!HandleIncomingMidiMessage(
            winusbHandle,
            message))
        {
            std::wcout
                << L"  [INFO ] MIDI message was not routed."
                << std::endl;
        }
    }

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI IN PARSE TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}




// ============================================================
#endif // SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS


// v9.3 - Continuous USB-MIDI IN receive loop
//
// Keeps EP 0x81 armed continuously.
// Each received USB-MIDI transfer is parsed and passed to
// the existing MIDI input / SD-80 routing layer.
//
// Existing v9.2 one-shot test remains unchanged above.
// ============================================================

static bool RunContinuousUsbMidiIn(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    UNREFERENCED_PARAMETER(deviceHandle);

    BYTE buffer[64] = {};

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"USB-MIDI INPUT RUNNING v10.14"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    std::wcout
        << L"[INFO ] Continuous MIDI input on EP 0x81."
        << std::endl;

    std::wcout
        << L"[INFO ] Press MIDI keys normally."
        << std::endl;

    std::wcout
        << L"[INFO ] Stop the program with Ctrl+C."
        << std::endl;

    for (;;)
    {
        ULONG bytesTransferred = 0;

        OVERLAPPED overlapped{};

        overlapped.hEvent =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nullptr);

        if (overlapped.hEvent == nullptr)
        {
            std::wcout
                << L"[ERROR] CreateEventW failed."
                << std::endl;

            return false;
        }

        ZeroMemory(
            buffer,
            sizeof(buffer));

        BOOL result =
            WinUsb_ReadPipe(
                winusbHandle,
                0x81,
                buffer,
                sizeof(buffer),
                &bytesTransferred,
                &overlapped);

        if (!result)
        {
            DWORD error =
                GetLastError();

            if (error != ERROR_IO_PENDING)
            {
                std::wcout
                    << L"[ERROR] WinUsb_ReadPipe failed: "
                    << error
                    << std::endl;

                CloseHandle(
                    overlapped.hEvent);

                return false;
            }

            DWORD waitResult =
                WaitForSingleObject(
                    overlapped.hEvent,
                    1000);

            if (waitResult == WAIT_TIMEOUT)
            {
                // No MIDI arrived during this interval.
                // Abort this pending read and immediately arm EP 0x81 again.
                WinUsb_AbortPipe(
                    winusbHandle,
                    0x81);

                WaitForSingleObject(
                    overlapped.hEvent,
                    1000);

                CloseHandle(
                    overlapped.hEvent);

                continue;
            }

            if (waitResult != WAIT_OBJECT_0)
            {
                std::wcout
                    << L"[ERROR] WaitForSingleObject failed."
                    << std::endl;

                WinUsb_AbortPipe(
                    winusbHandle,
                    0x81);

                WaitForSingleObject(
                    overlapped.hEvent,
                    1000);

                CloseHandle(
                    overlapped.hEvent);

                return false;
            }
        }

        if (!WinUsb_GetOverlappedResult(
            winusbHandle,
            &overlapped,
            &bytesTransferred,
            FALSE))
        {
            DWORD error =
                GetLastError();

            std::wcout
                << L"[ERROR] WinUsb_GetOverlappedResult failed: "
                << error
                << std::endl;

            CloseHandle(
                overlapped.hEvent);

            return false;
        }

        CloseHandle(
            overlapped.hEvent);

        if (bytesTransferred == 0)
        {
            continue;
        }

        const ULONG packetCount =
            bytesTransferred / 4;

        std::wcout
            << L"\n[RX ] Bytes received = "
            << bytesTransferred
            << L", USB-MIDI packets = "
            << packetCount
            << std::endl;

        for (ULONG i = 0;
            i < packetCount;
            ++i)
        {
            const BYTE* packet =
                &buffer[i * 4];

            // Empty USB-MIDI slots are padding in the 64-byte transfer.
            // Ignore them before any logging or parsing.
            if (packet[0] == 0x00 &&
                packet[1] == 0x00 &&
                packet[2] == 0x00 &&
                packet[3] == 0x00)
            {
                continue;
            }

            MidiMessage message{};

            std::wcout
                << L"\n[USB-MIDI PACKET "
                << (i + 1)
                << L"]"
                << std::endl;

            std::wcout
                << L"  Raw      : "
                << std::hex
                << std::uppercase
                << static_cast<int>(packet[0])
                << L" "
                << static_cast<int>(packet[1])
                << L" "
                << static_cast<int>(packet[2])
                << L" "
                << static_cast<int>(packet[3])
                << std::dec
                << std::nouppercase
                << std::endl;

            if (!ParseUsbMidiPacket(
                packet,
                message))
            {
                std::wcout
                    << L"  [INFO ] Packet ignored."
                    << std::endl;

                continue;
            }

            const int midiChannel =
                (message.status & 0x0F) + 1;

            std::wcout
                << L"  MIDI Channel = "
                << midiChannel
                << std::endl;

            std::wcout
                << L"  USB Cable    = "
                << static_cast<int>(message.cable)
                << std::endl;

            std::wcout
                << L"  [INFO ] Passing received MIDI to input layer..."
                << std::endl;


#if !SD80BRIDGE_VERBOSE_MIDI
            std::wcout
                << L"[MIDI] Cable="
                << static_cast<int>(message.cable)
                << L" Ch="
                << (static_cast<int>(message.status & 0x0F) + 1)
                << L" Status=0x"
                << std::hex
                << static_cast<int>(message.status)
                << L" Data1=0x"
                << static_cast<int>(message.data1)
                << L" Data2=0x"
                << static_cast<int>(message.data2)
                << std::dec
                << std::endl;
#endif

            if (!HandleIncomingMidiMessage(
                winusbHandle,
                message))
            {
                std::wcout
                    << L"  [INFO ] MIDI message was not routed."
                    << std::endl;
            }
        }
    }
}


// ============================================================
#if SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS

// Test MIDI input layer
//
// Simulates MIDI messages coming from an external MIDI device.
//
// Channel 1 / Note On
// Channel 1 / Note Off
// ============================================================

static void TestMidiInput(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing =
    {
        'A'
    };


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"MIDI INPUT LAYER TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // Hand-built test packets use Cable 0.
    MidiMessage noteOn =
    {
        0,
        0x90,
        0x3C,
        0x7F
    };


    MidiMessage noteOff =
    {
        0,
        0x80,
        0x3C,
        0x00
    };


    std::wcout
        << L"\n[TEST] Channel 1 / Note On"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    HandleIncomingMidiMessage(
        winusbHandle,
        routing,
        noteOn);


    Sleep(1000);


    std::wcout
        << L"\n[TEST] Channel 1 / Note Off"
        << std::endl;


    HandleIncomingMidiMessage(
        winusbHandle,
        routing,
        noteOff);


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"MIDI INPUT LAYER TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}



// ============================================================
// Test ProcessMidiMessage()
//
// MIDI Channel 1～4を使ってC4を発音する。
// Status byte:
//
//   0x90 = Note On  / Channel 1
//   0x91 = Note On  / Channel 2
//   0x92 = Note On  / Channel 3
//   0x93 = Note On  / Channel 4
// ============================================================

static void TestProcessMidiMessage(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing =
    {
        'A'
    };

    const BYTE statuses[4] =
    {
        0x90,
        0x91,
        0x92,
        0x93
    };


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"PROCESS MIDI MESSAGE TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const BYTE status = statuses[i];

        const int expectedChannel = i + 1;


        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Status = 0x"
            << std::hex
            << static_cast<int>(status)
            << std::dec
            << std::endl;

        std::wcout
            << L"Expected MIDI Channel = "
            << expectedChannel
            << std::endl;

        std::wcout
            << L"Part Group = A"
            << std::endl;

        std::wcout
            << L"Press ENTER to send C4..."
            << std::endl;

        std::cin.get();


        // ----------------------------------------------------
        // Note On
        // ----------------------------------------------------

        ProcessMidiMessage(
            winusbHandle,
            routing,
            0,
            status,
            0x3C,
            0x7F);

        Sleep(1000);


        // ----------------------------------------------------
        // Note Off
        //
        // Same MIDI channel, status = 0x80 + channel
        // ----------------------------------------------------

        const BYTE noteOffStatus =
            static_cast<BYTE>(
                0x80 + i);


        ProcessMidiMessage(
            winusbHandle,
            routing,
            0,
            noteOffStatus,
            0x3C,
            0x00);

        Sleep(500);
    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"PROCESS MIDI MESSAGE TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}


// ============================================================
// Test RouteNormalMidiToSD80()
//
// Normal MIDI Channel 1～4 -> PART A
// ============================================================

static void TestRouteNormalMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    SD80MidiRouting routing =
    {
        'B'
    };

    const int channels[4] =
    {
        1,
        2,
        3,
        4
    };

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"NORMAL MIDI -> SD-80 PART TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const int channel = channels[i];

        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Input Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"Part Group    = "
            << routing.partGroup
            << std::endl;

        std::wcout
            << L"Press ENTER to play C4..."
            << std::endl;

        std::cin.get();


        // Note On
        RouteNormalMidiToSD80(
            winusbHandle,
            routing,
            channel,
            0x90,
            0x3C,
            0x7F);

        Sleep(1000);


        // Note Off
        RouteNormalMidiToSD80(
            winusbHandle,
            routing,
            channel,
            0x80,
            0x3C,
            0x00);

        Sleep(500);
    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"NORMAL MIDI -> SD-80 PART TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}


// ============================================================
// MIDI Channel -> SD-80 Part routing
//
// Input Channel 1～16:
//     -> PART A, same Channel
//
// Input Channel 17～32:
//     -> PART B, Channel 1～16
//
// Example:
//     Input Ch 1  -> A01
//     Input Ch 2  -> A02
//     Input Ch 16 -> A16
//
//     Input Ch 17 -> B01
//     Input Ch 18 -> B02
//     Input Ch 32 -> B16
// ============================================================

static bool RouteChannelToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle,
    int inputChannel,
    BYTE status,
    BYTE data1,
    BYTE data2)
{
    char partGroup;
    int sd80Channel;


    if (inputChannel >= 1 &&
        inputChannel <= 16)
    {
        // ----------------------------------------------------
        // Input Channel 1～16 -> PART A
        // ----------------------------------------------------

        partGroup = 'A';
        sd80Channel = inputChannel;
    }
    else if (inputChannel >= 17 &&
        inputChannel <= 32)
    {
        // ----------------------------------------------------
        // Input Channel 17～32 -> PART B
        // ----------------------------------------------------

        partGroup = 'B';
        sd80Channel = inputChannel - 16;
    }
    else
    {
        std::wcout
            << L"[ERROR] Invalid input MIDI Channel."
            << std::endl;

        return false;
    }


    std::wcout
        << L"\n[INFO ] RouteChannelToSD80"
        << std::endl;

    std::wcout
        << L"        Input Channel = "
        << inputChannel
        << std::endl;

    std::wcout
        << L"        Part Group    = "
        << partGroup
        << std::endl;

    std::wcout
        << L"        SD-80 Channel = "
        << sd80Channel
        << std::endl;


    return RouteMidiToSD80(
        winusbHandle,
        partGroup,
        sd80Channel,
        status,
        data1,
        data2);
}

// ============================================================
// Test RouteChannelToSD80()
//
// Input Channel 1 -> PART A / Channel 1
// Input Channel 2 -> PART A / Channel 2
// Input Channel 3 -> PART A / Channel 3
// Input Channel 4 -> PART A / Channel 4
//
// C4 is played on each channel.
// ============================================================

static void TestRouteChannelToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    const int channels[4] =
    {
        17,
        18,
        19,
        20
    };


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CHANNEL -> SD-80 PART ROUTING TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const int channel = channels[i];


        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Input Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"Expected Part = A"
            << std::endl;

        std::wcout
            << L"Expected SD-80 Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"Press ENTER to play C4..."
            << std::endl;

        std::cin.get();


        // Note On
        RouteChannelToSD80(
            winusbHandle,
            channel,
            0x90,
            0x3C,
            0x7F);


        Sleep(1000);


        // Note Off
        RouteChannelToSD80(
            winusbHandle,
            channel,
            0x80,
            0x3C,
            0x00);


        Sleep(500);
    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CHANNEL -> SD-80 PART ROUTING TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}





// ============================================================
// Test RouteMidiToSD80()
// A01 = Piano
// B01 = Glockenspiel
// ============================================================

static void TestRouteMidiToSD80(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"ROUTE MIDI TO SD-80 TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // PART A / Channel 1 / C4
    // --------------------------------------------------------

    std::wcout
        << L"\n[A01] C4"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    RouteMidiToSD80(
        winusbHandle,
        'A',
        1,
        0x90,
        0x3C,
        0x7F);

    Sleep(1000);


    RouteMidiToSD80(
        winusbHandle,
        'A',
        1,
        0x80,
        0x3C,
        0x00);

    Sleep(500);


    // --------------------------------------------------------
    // PART B / Channel 1 / C4
    // --------------------------------------------------------

    std::wcout
        << L"\n[B01] C4"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    RouteMidiToSD80(
        winusbHandle,
        'B',
        1,
        0x90,
        0x3C,
        0x7F);

    Sleep(1000);


    RouteMidiToSD80(
        winusbHandle,
        'B',
        1,
        0x80,
        0x3C,
        0x00);


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"ROUTE MIDI TO SD-80 TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}



// ============================================================
// Test SendSD80Midi()
// A01 and B01 are played independently.
// ============================================================

static void TestSendSD80Midi(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SEND SD-80 MIDI TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // A01 / Channel 1 / C4 Note On
    // --------------------------------------------------------

    std::wcout
        << L"\n[A01] C4 Note On"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    SendSD80Midi(
        winusbHandle,
        'A',
        1,
        0x90,
        0x3C,
        0x7F);

    Sleep(1000);


    // A01 / Note Off

    SendSD80Midi(
        winusbHandle,
        'A',
        1,
        0x80,
        0x3C,
        0x00);

    Sleep(500);


    // --------------------------------------------------------
    // B01 / Channel 1 / C4 Note On
    // --------------------------------------------------------

    std::wcout
        << L"\n[B01] C4 Note On"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    SendSD80Midi(
        winusbHandle,
        'B',
        1,
        0x90,
        0x3C,
        0x7F);

    Sleep(1000);


    // B01 / Note Off

    SendSD80Midi(
        winusbHandle,
        'B',
        1,
        0x80,
        0x3C,
        0x00);


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SEND SD-80 MIDI TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}

// ============================================================
// Test SD-80 PART A / PART B
//
// A01 -> Program 0  (Piano)
// B01 -> Program 9  (Glockenspiel)
//
// Both use MIDI Channel 1.
// ============================================================

static void TestSD80PartAB(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 PART A / PART B TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // ========================================================
    // A01
    // ========================================================

    std::wcout
        << L"\n[A01]"
        << std::endl;

    std::wcout
        << L"Program 0 = Piano"
        << std::endl;

    std::wcout
        << L"Press ENTER to set A01..."
        << std::endl;

    std::cin.get();


    if (!SetSD80PartTone(
        winusbHandle,
        'A',
        1,
        0,
        0,
        0))
    {
        std::wcout
            << L"[ERROR] Failed to set A01."
            << std::endl;

        return;
    }


    Sleep(500);


    // ========================================================
    // B01
    // ========================================================

    std::wcout
        << L"\n[B01]"
        << std::endl;

    std::wcout
        << L"Program 9 = Glockenspiel"
        << std::endl;

    std::wcout
        << L"Press ENTER to set B01..."
        << std::endl;

    std::cin.get();


    if (!SetSD80PartTone(
        winusbHandle,
        'B',
        1,
        0,
        0,
        9))
    {
        std::wcout
            << L"[ERROR] Failed to set B01."
            << std::endl;

        return;
    }


    Sleep(500);


    // ========================================================
    // Play A01
    // ========================================================

    std::wcout
        << L"\n[A01] Play C4"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    BYTE aNoteOn[4] =
    {
        0x09,
        0x90,
        0x3C,
        0x7F
    };


    BYTE aNoteOff[4] =
    {
        0x08,
        0x80,
        0x3C,
        0x00
    };


    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        aNoteOn);

    Sleep(1000);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        aNoteOff);


    Sleep(500);


    // ========================================================
    // Play B01
    // ========================================================

    std::wcout
        << L"\n[B01] Play C4"
        << std::endl;

    std::wcout
        << L"Press ENTER..."
        << std::endl;

    std::cin.get();


    BYTE bNoteOn[4] =
    {
        0x19,
        0x90,
        0x3C,
        0x7F
    };


    BYTE bNoteOff[4] =
    {
        0x18,
        0x80,
        0x3C,
        0x00
    };


    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bNoteOn);

    Sleep(1000);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bNoteOff);


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 PART A / PART B TEST COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}

// ============================================================
// Pure USB-MIDI Note Test
//
// Program Change / Bank Select は一切送らない。
// Cable 0～3 / Channel 1 に C4 を送るだけ。
// ============================================================

static void TestPureCableNote(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    const int cables[4] =
    {
        0, 1, 2, 3
    };

    const int channel = 1;

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"PURE CABLE NOTE TEST"
        << std::endl;

    std::wcout
        << L"Program Change is NOT sent."
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const int cable = cables[i];

        const BYTE midiChannel =
            static_cast<BYTE>(channel - 1);


        BYTE noteOn[4] =
        {
            static_cast<BYTE>(
                (cable << 4) | 0x09),

            static_cast<BYTE>(
                0x90 | midiChannel),

            0x3C,
            0x7F
        };


        BYTE noteOff[4] =
        {
            static_cast<BYTE>(
                (cable << 4) | 0x08),

            static_cast<BYTE>(
                0x80 | midiChannel),

            0x3C,
            0x00
        };


        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Cable   = "
            << cable
            << std::endl;

        std::wcout
            << L"Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"C4 Note On packet only"
            << std::endl;

        std::wcout
            << L"Press ENTER to play C4..."
            << std::endl;

        std::cin.get();


        SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOn);


        Sleep(1000);


        SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOff);


        Sleep(500);
    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"Pure Cable Note Test completed."
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}

// ============================================================
// Test four USB-MIDI Cables with different GS tones
//
// Cable 0 -> Program 0
// Cable 1 -> Program 9
// Cable 2 -> Program 20
// Cable 3 -> Program 30
//
// All use:
//   MIDI Channel 1
//   Bank MSB = 0
//   Bank LSB = 0
// ============================================================

// ============================================================
// Test MIDI Cable -> Program Change routing
//
// Cable 0 / Channel 1 -> Program 0
// Cable 1 / Channel 1 -> Program 9
// Cable 2 / Channel 1 -> Program 10
// Cable 3 / Channel 1 -> Program 11
//
// Each Cable uses MIDI Channel 1.
// ============================================================

static void TestChannelProgramRouting(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    const int cables[4] =
    {
        1,
        1,
        1,
        1
    };

    const int channels[4] =
    {
        1,
        2,
        3,
        4
    };

    const int programs[4] =
    {
        0,
        9,
        10,
        11
    };


    // ========================================================
    // Step 1
    // Set different programs for each Cable
    // ========================================================

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE -> PROGRAM ROUTING TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const int cable = cables[i];
        const int channel = channels[i];
        const int program = programs[i];


        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Cable   = "
            << cable
            << std::endl;

        std::wcout
            << L"Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"Program = "
            << program
            << std::endl;

        std::wcout
            << L"Press ENTER to send tone selection..."
            << std::endl;

        std::cin.get();


        if (!SetSD80GSTone(
            winusbHandle,
            cable,
            channel,
            0,
            0,
            program))
        {
            std::wcout
                << L"[ERROR] Failed to set tone."
                << std::endl;

            return;
        }


        Sleep(500);
    }


    // ========================================================
    // Step 2
    // Play C4 on each Cable / Channel
    // ========================================================

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"CABLE PLAYBACK TEST"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    for (int i = 0; i < 4; ++i)
    {
        const int cable = cables[i];
        const int channel = channels[i];
        const int program = programs[i];


        const BYTE midiChannel =
            static_cast<BYTE>(
                channel - 1);


        BYTE noteOn[4] =
        {
            static_cast<BYTE>(
                (cable << 4) | 0x09),

            static_cast<BYTE>(
                0x90 | midiChannel),

            0x3C,
            0x7F
        };


        BYTE noteOff[4] =
        {
            static_cast<BYTE>(
                (cable << 4) | 0x08),

            static_cast<BYTE>(
                0x80 | midiChannel),

            0x3C,
            0x00
        };


        std::wcout
            << L"\n----------------------------------------"
            << std::endl;

        std::wcout
            << L"Cable   = "
            << cable
            << std::endl;

        std::wcout
            << L"Channel = "
            << channel
            << std::endl;

        std::wcout
            << L"Program = "
            << program
            << std::endl;

        std::wcout
            << L"Press ENTER to play C4..."
            << std::endl;

        std::cin.get();


        SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOn);


        Sleep(1000);


        SendMidiPacket(
            winusbHandle,
            SD80_EP_OUT,
            noteOff);


        Sleep(500);
    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"Cable / Program routing test completed."
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}





// ============================================================
// Test SetSD80Tone()
// ============================================================
//
// Cable 0 / MIDI Channel 1
//
// Classical Set
//   Bank MSB = 96
//   Bank LSB = 0
//
// Program 0
// Program 9
// ============================================================

static void TestProgramChange(
    WINUSB_INTERFACE_HANDLE winusbHandle)
{
    const BYTE noteOn[4] =
    {
        0x09,
        0x90,
        0x3C,
        0x7F
    };

    const BYTE noteOff[4] =
    {
        0x08,
        0x80,
        0x3C,
        0x00
    };


    // ========================================================
    // GS Mode
    //
    // Bank Select MSB = 0
    // Bank Select LSB = 0
    // Program Change
    // ========================================================

    const BYTE bankMSB[4] =
    {
        0x0B,
        0xB0,
        0x00,
        0x00
    };

    const BYTE bankLSB[4] =
    {
        0x0B,
        0xB0,
        0x20,
        0x00
    };


    // ========================================================
    // Program 0
    // ========================================================

    const BYTE program0[4] =
    {
        0x0C,
        0xC0,
        0x00,
        0x00
    };


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 GS MODE TONE TEST"
        << std::endl;

    std::wcout
        << L"Bank MSB = 0 / LSB = 0"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // Select GS Bank 0
    // --------------------------------------------------------

    std::wcout
        << L"\n[INFO ] Selecting GS Bank 0 / Program 0..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankMSB);

    Sleep(50);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankLSB);

    Sleep(50);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        program0);

    Sleep(500);


    // --------------------------------------------------------
    // Play C4
    // --------------------------------------------------------

    std::wcout
        << L"[INFO ] Playing Program 0..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOn);

    Sleep(1000);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOff);

    Sleep(1000);


    // ========================================================
    // Program 9
    // ========================================================

    const BYTE program9[4] =
    {
        0x0C,
        0xC0,
        0x09,
        0x00
    };


    std::wcout
        << L"\n[INFO ] Selecting GS Bank 0 / Program 9..."
        << std::endl;


    // Re-send Bank Select before Program Change.

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankMSB);

    Sleep(50);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        bankLSB);

    Sleep(50);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        program9);

    Sleep(500);


    // --------------------------------------------------------
    // Play C4
    // --------------------------------------------------------

    std::wcout
        << L"[INFO ] Playing Program 9..."
        << std::endl;

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOn);

    Sleep(1000);

    SendMidiPacket(
        winusbHandle,
        SD80_EP_OUT,
        noteOff);

    Sleep(500);


    std::wcout
        << L"\n[INFO ] GS mode tone test completed."
        << std::endl;
}


// ============================================================
// Read multiple packets
// ============================================================

static void ReadPackets(
    HANDLE deviceHandle,
    WINUSB_INTERFACE_HANDLE winusbHandle,
    const wchar_t* modeName)
{
    constexpr int READ_COUNT = 10;


    std::wcout
        << L"\n----------------------------------------"
        << std::endl;


    std::wcout
        << modeName
        << std::endl;


    std::wcout
        << L"Read count : "
        << READ_COUNT
        << std::endl;


    std::wcout
        << L"----------------------------------------"
        << std::endl;


    for (int i = 1;
        i <= READ_COUNT;
        ++i)
    {
        std::wcout
            << L"\n[READ TEST] "
            << i
            << L" / "
            << READ_COUNT
            << std::endl;


        BYTE buffer[
            SD80_PACKET_SIZE]{};


            ULONG bytesRead = 0;


            OVERLAPPED overlapped{};


            overlapped.hEvent =
                CreateEventW(
                    nullptr,
                    TRUE,
                    FALSE,
                    nullptr);


            if (!overlapped.hEvent)
            {
                PrintLastError(
                    L"CreateEventW");

                break;
            }


            std::wcout
                << L"[READ] Waiting on EP 0x81..."
                << std::endl;


            BOOL result =
                WinUsb_ReadPipe(
                    winusbHandle,
                    SD80_EP_IN,
                    buffer,
                    sizeof(buffer),
                    &bytesRead,
                    &overlapped);


            if (result)
            {
                std::wcout
                    << L"[READ] Completed immediately: "
                    << bytesRead
                    << L" bytes"
                    << std::endl;


                if (bytesRead > 0)
                {
                    DumpHex(
                        buffer,
                        bytesRead);
                }
                else
                {
                    std::wcout
                        << L"[READ] Zero bytes."
                        << std::endl;
                }


                CloseHandle(
                    overlapped.hEvent);


                continue;
            }


            DWORD error =
                GetLastError();


            if (error != ERROR_IO_PENDING)
            {
                std::wcout
                    << L"[ERROR] WinUsb_ReadPipe failed: "
                    << error
                    << L" (0x"
                    << std::hex
                    << error
                    << std::dec
                    << L")"
                    << std::endl;


                CloseHandle(
                    overlapped.hEvent);


                break;
            }


            std::wcout
                << L"[READ] Pending..."
                << std::endl;


            DWORD waitResult =
                WaitForSingleObject(
                    overlapped.hEvent,
                    SD80_READ_TIMEOUT_MS);


            if (waitResult == WAIT_OBJECT_0)
            {
                if (!WinUsb_GetOverlappedResult(
                    winusbHandle,
                    &overlapped,
                    &bytesRead,
                    FALSE))
                {
                    DWORD completionError =
                        GetLastError();


                    std::wcout
                        << L"[ERROR] "
                        L"WinUsb_GetOverlappedResult failed: "
                        << completionError
                        << std::endl;


                    CloseHandle(
                        overlapped.hEvent);


                    break;
                }


                std::wcout
                    << L"[READ] Completed: "
                    << bytesRead
                    << L" bytes"
                    << std::endl;


                if (bytesRead > 0)
                {
                    DumpHex(
                        buffer,
                        bytesRead);
                }
                else
                {
                    std::wcout
                        << L"[READ] Zero bytes."
                        << std::endl;
                }


                CloseHandle(
                    overlapped.hEvent);


                continue;
            }


            if (waitResult == WAIT_TIMEOUT)
            {
                std::wcout
                    << L"[READ] Timeout after "
                    << SD80_READ_TIMEOUT_MS
                    << L" ms"
                    << std::endl;


                if (!CancelIoEx(
                    deviceHandle,
                    &overlapped))
                {
                    DWORD cancelError =
                        GetLastError();


                    if (cancelError !=
                        ERROR_NOT_FOUND)
                    {
                        std::wcout
                            << L"[WARNING] CancelIoEx failed: "
                            << cancelError
                            << std::endl;
                    }
                }


                WaitForSingleObject(
                    overlapped.hEvent,
                    1000);


                CloseHandle(
                    overlapped.hEvent);


                continue;
            }


            std::wcout
                << L"[ERROR] WaitForSingleObject failed: "
                << GetLastError()
                << std::endl;


            CancelIoEx(
                deviceHandle,
                &overlapped);


            WaitForSingleObject(
                overlapped.hEvent,
                1000);


            CloseHandle(
                overlapped.hEvent);


            break;
    }


    std::wcout
        << L"\n----------------------------------------"
        << std::endl;


    std::wcout
        << L"[INFO ] Read test completed."
        << std::endl;


    std::wcout
        << L"----------------------------------------"
        << std::endl;
}

// ============================================================
// Enumerate Windows MIDI IN devices
// ============================================================

static void ListMidiInputDevices()
{
    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"WINDOWS MIDI IN DEVICES"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    const UINT deviceCount = midiInGetNumDevs();


    std::wcout
        << L"[INFO ] MIDI IN device count = "
        << deviceCount
        << std::endl;


    if (deviceCount == 0)
    {
        std::wcout
            << L"[INFO ] No MIDI IN devices found."
            << std::endl;

        return;
    }


    for (UINT i = 0; i < deviceCount; ++i)
    {
        MIDIINCAPSW caps{};


        MMRESULT result =
            midiInGetDevCapsW(
                static_cast<UINT_PTR>(i),
                &caps,
                sizeof(caps));


        if (result != MMSYSERR_NOERROR)
        {
            std::wcout
                << L"\n[MIDI IN "
                << i
                << L"] Failed to get device information."
                << std::endl;

            continue;
        }


        std::wcout
            << L"\n[MIDI IN "
            << i
            << L"]"
            << std::endl;

        std::wcout
            << L"  Name      : "
            << caps.szPname
            << std::endl;

        std::wcout
            << L"  Device ID : "
            << i
            << std::endl;

        std::wcout
            << L"  Manufacturer ID : 0x"
            << std::hex
            << caps.wMid
            << std::dec
            << std::endl;

        std::wcout
            << L"  Product ID      : 0x"
            << std::hex
            << caps.wPid
            << std::dec
            << std::endl;

    }


    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"MIDI IN ENUMERATION COMPLETED"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}





// ============================================================
#endif // SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS


// Main probe
// ============================================================

bool ProbeSD80WinUSB()
{
    std::wcout
        << L"\n========================================"
        << std::endl;


    std::wcout
        << L"SD80Bridge MIDI Runtime v10.14"
        << std::endl;


    std::wcout
        << L"========================================"
        << std::endl;


    // --------------------------------------------------------
    // Find device
    // --------------------------------------------------------

    std::wstring devicePath =
        FindSD80DevicePath();


    if (devicePath.empty())
    {
        std::wcout
            << L"[ERROR] SD-80 WinUSB interface not found."
            << std::endl;

        return false;
    }


    std::wcout
        << L"[INFO ] WinUSB device interface found:"
        << std::endl;


    std::wcout
        << L"        "
        << devicePath
        << std::endl;


    // --------------------------------------------------------
    // Open
    // --------------------------------------------------------

    HANDLE deviceHandle =
        CreateFileW(
            devicePath.c_str(),
            GENERIC_READ |
            GENERIC_WRITE,
            FILE_SHARE_READ |
            FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OVERLAPPED,
            nullptr);


    if (deviceHandle ==
        INVALID_HANDLE_VALUE)
    {
        PrintLastError(
            L"CreateFileW");

        return false;
    }


    std::wcout
        << L"[INFO ] CreateFile : OK"
        << std::endl;


    // --------------------------------------------------------
    // WinUSB initialize
    // --------------------------------------------------------

    WINUSB_INTERFACE_HANDLE
        winusbHandle = nullptr;


    if (!WinUsb_Initialize(
        deviceHandle,
        &winusbHandle))
    {
        PrintLastError(
            L"WinUsb_Initialize");


        CloseHandle(
            deviceHandle);

        return false;
    }


    std::wcout
        << L"[INFO ] WinUsb_Initialize : OK"
        << std::endl;


#if SD80BRIDGE_VERBOSE_STARTUP
    // --------------------------------------------------------
    // Optional diagnostic descriptor analysis
    // --------------------------------------------------------

    ParseSD80MidiDescriptors(
        winusbHandle);

#endif

    // --------------------------------------------------------
    // Alternate Setting 0
    // --------------------------------------------------------

    USB_INTERFACE_DESCRIPTOR alt0{};


    if (!WinUsb_QueryInterfaceSettings(
        winusbHandle,
        0,
        &alt0))
    {
        PrintLastError(
            L"WinUsb_QueryInterfaceSettings Alt 0");


        WinUsb_Free(
            winusbHandle);


        CloseHandle(
            deviceHandle);

        return false;
    }


    std::wcout
        << L"\n========================================"
        << std::endl;


    std::wcout
        << L"Alternate Setting 0"
        << std::endl;


    std::wcout
        << L"========================================"
        << std::endl;


    std::wcout
        << L"Interface Number : "
        << static_cast<unsigned int>(
            alt0.bInterfaceNumber)
        << std::endl;


    std::wcout
        << L"Endpoint Count   : "
        << static_cast<unsigned int>(
            alt0.bNumEndpoints)
        << std::endl;


    // --------------------------------------------------------
    // Explicitly select Alt 0
    // --------------------------------------------------------

    if (!WinUsb_SetCurrentAlternateSetting(
        winusbHandle,
        0))
    {
        PrintLastError(
            L"WinUsb_SetCurrentAlternateSetting Alt 0");


        WinUsb_Free(
            winusbHandle);


        CloseHandle(
            deviceHandle);

        return false;
    }


    std::wcout
        << L"[INFO ] Current Alternate Setting = 0"
        << std::endl;


    // --------------------------------------------------------
    // MIDI OUT -> USB IN test
    // --------------------------------------------------------

    //TestMidiOutToUsbIn(
    //    deviceHandle,
    //    winusbHandle);


    // --------------------------------------------------------
    // Manual cable/output tests are diagnostic-only and are not
    // part of the normal runtime path.
    // --------------------------------------------------------

    // NORMAL USB-MIDI INPUT RUNTIME
    //
    // This is the only MIDI processing path enabled for normal
    // operation. Diagnostic/test functions remain compiled below,
    // but they are not invoked here.
    // ========================================================

#if SD80BRIDGE_TONE_TEST
    if (!RunOneShotToneTest(
        winusbHandle))
    {
        std::wcout
            << L"[ERROR] One-shot tone test failed."
            << std::endl;
    }
#endif

#if SD80BRIDGE_TONE_COMPARE_TEST
    if (!RunProgramCompareTest(
        winusbHandle))
    {
        std::wcout
            << L"[ERROR] Program comparison test failed."
            << std::endl;
    }
#endif

#if SD80BRIDGE_APPLY_PART_TONES_ON_STARTUP
    std::wcout
        << L"[INFO ] Applying configured Part A/B tones at startup."
        << std::endl;

    if (!ApplyConfiguredPartTonesOnStartup(
        winusbHandle))
    {
        std::wcout
            << L"[ERROR] Part tone startup configuration failed."
            << std::endl;
    }
#endif

    std::wcout
        << L"[INFO ] Part A initial tone: Bank 0/0 Program 8."
        << std::endl;

    std::wcout
        << L"[INFO ] Part B initial tone: Bank 0/0 Program 9."
        << std::endl;

#if SD80BRIDGE_APPLY_PART_TONES_ON_STARTUP
    std::wcout
        << L"[INFO ] Automatic Part tone application is enabled."
        << std::endl;
#else
    std::wcout
        << L"[INFO ] Automatic Part tone application is disabled."
        << std::endl;
#endif

#if SD80BRIDGE_VERBOSE_MIDI
    std::wcout
        << L"[INFO ] Detailed MIDI logging enabled."
        << std::endl;
#else
    std::wcout
        << L"[INFO ] Concise MIDI logging enabled."
        << std::endl;
#endif

    std::wcout
        << L"[INFO ] Normal MIDI bridge runtime."
        << std::endl;

    RunContinuousUsbMidiIn(
        deviceHandle,
        winusbHandle);


    // --------------------------------------------------------
    // Diagnostic tests are compiled out by default.
    // Define SD80BRIDGE_ENABLE_DIAGNOSTIC_TESTS=1 when needed.
    // --------------------------------------------------------

    WinUsb_Free(
        winusbHandle);

    CloseHandle(
        deviceHandle);

    return true;







    // --------------------------------------------------------
    // Read Alt 0
    // --------------------------------------------------------

    // Intentionally disabled for now.
    //
    // ReadPackets(
    //     deviceHandle,
    //     winusbHandle,
    //     L"ALT 0 / EP 0x81 BULK IN");


    // --------------------------------------------------------
    // Alternate Setting 1
    // --------------------------------------------------------

    //USB_INTERFACE_DESCRIPTOR alt1{};


    //if (!WinUsb_QueryInterfaceSettings(
    //    winusbHandle,
    //    1,
    //    &alt1))
    //{
    //    std::wcout
    //        << L"\n[INFO ] Alternate Setting 1 "
    //        L"is not available."
    //        << std::endl;


    //    WinUsb_Free(
    //        winusbHandle);


    //    CloseHandle(
    //        deviceHandle);

    //    return true;
    //}


    //std::wcout
    //    << L"\n========================================"
    //    << std::endl;


    //std::wcout
    //    << L"Alternate Setting 1"
    //    << std::endl;


    //std::wcout
    //    << L"========================================"
    //    << std::endl;


    //std::wcout
    //    << L"Interface Number : "
    //    << static_cast<unsigned int>(
    //        alt1.bInterfaceNumber)
    //    << std::endl;


    //std::wcout
    //    << L"Endpoint Count   : "
    //    << static_cast<unsigned int>(
    //        alt1.bNumEndpoints)
    //    << std::endl;


    //// --------------------------------------------------------
    //// Switch to Alt 1
    //// --------------------------------------------------------

    //if (!WinUsb_SetCurrentAlternateSetting(
    //    winusbHandle,
    //    1))
    //{
    //    PrintLastError(
    //        L"WinUsb_SetCurrentAlternateSetting Alt 1");


    //    WinUsb_Free(
    //        winusbHandle);


    //    CloseHandle(
    //        deviceHandle);

    //    return false;
    //}


    //std::wcout
    //    << L"[INFO ] Current Alternate Setting = 1"
    //    << std::endl;


    //// --------------------------------------------------------
    //// MIDI IN monitor
    //// --------------------------------------------------------

    //WaitForMidiInput(
    //    deviceHandle,
    //    winusbHandle,
    //    10000);


    //// --------------------------------------------------------
    //// Read Alt 1
    //// --------------------------------------------------------

    //ReadPackets(
    //    deviceHandle,
    //    winusbHandle,
    //    L"ALT 1 / EP 0x81 INTERRUPT IN");


    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    WinUsb_Free(
        winusbHandle);


    CloseHandle(
        deviceHandle);


    std::wcout
        << L"\n========================================"
        << std::endl;


    std::wcout
        << L"[INFO ] USB-MIDI descriptor test completed."
        << std::endl;


    std::wcout
        << L"========================================"
        << std::endl;


    return true;
}