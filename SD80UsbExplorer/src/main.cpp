#include <Windows.h>

#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

#include "Logger.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

bool ProbeSD80WinUSB();


// ============================================================
// USB Hubを開く
// ============================================================

static HANDLE OpenUsbHub(const wchar_t* devicePath)
{
    return CreateFileW(
        devicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
}


// ============================================================
// Descriptorを取得
// ============================================================

static bool GetUsbDescriptor(
    HANDLE hubHandle,
    ULONG portNumber,
    UCHAR descriptorType,
    UCHAR descriptorIndex,
    std::vector<BYTE>& data,
    USHORT length)
{
    const size_t requestSize =
        sizeof(USB_DESCRIPTOR_REQUEST) + length;

    std::vector<BYTE> buffer(requestSize);

    auto request =
        reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(
            buffer.data());

    ZeroMemory(
        request,
        requestSize);

    request->ConnectionIndex =
        portNumber;

    request->SetupPacket.bmRequest =
        0x80;   // Device-to-host | Standard | Device

    request->SetupPacket.bRequest =
        0x06;   // GET_DESCRIPTOR

    request->SetupPacket.wValue =
        static_cast<USHORT>(
            (static_cast<USHORT>(descriptorType) << 8) |
            descriptorIndex);

    request->SetupPacket.wIndex =
        0;

    request->SetupPacket.wLength =
        length;

    ULONG bytesReturned = 0;

    BOOL result =
        DeviceIoControl(
            hubHandle,
            IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
            request,
            static_cast<DWORD>(requestSize),
            request,
            static_cast<DWORD>(requestSize),
            &bytesReturned,
            nullptr);

    if (!result)
    {
        std::wcout
            << L"    Descriptor request failed. Error = "
            << GetLastError()
            << std::endl;

        return false;
    }

    if (bytesReturned <=
        sizeof(USB_DESCRIPTOR_REQUEST))
    {
        return false;
    }

    const size_t returnedLength =
        bytesReturned -
        sizeof(USB_DESCRIPTOR_REQUEST);

    data.assign(
        request->Data,
        request->Data + returnedLength);

    return true;
}


// ============================================================
// 16進ダンプ
// ============================================================

static void HexDump(
    const std::vector<BYTE>& data)
{
    for (size_t i = 0;
        i < data.size();
        i += 16)
    {
        std::wcout
            << std::hex
            << std::uppercase
            << std::setw(4)
            << std::setfill(L'0')
            << i
            << L"  ";

        for (size_t j = 0;
            j < 16;
            ++j)
        {
            if (i + j < data.size())
            {
                std::wcout
                    << std::setw(2)
                    << static_cast<int>(
                        data[i + j])
                    << L' ';
            }
            else
            {
                std::wcout
                    << L"   ";
            }
        }

        std::wcout << L" ";

        for (size_t j = 0;
            j < 16;
            ++j)
        {
            if (i + j < data.size())
            {
                BYTE c = data[i + j];

                if (c >= 32 && c <= 126)
                {
                    std::wcout
                        << static_cast<wchar_t>(c);
                }
                else
                {
                    std::wcout << L'.';
                }
            }
        }

        std::wcout
            << std::endl;
    }

    std::wcout
        << std::dec;
}


// ============================================================
// Device Descriptor
// ============================================================

static bool DumpDeviceDescriptor(
    HANDLE hubHandle,
    ULONG portNumber)
{
    std::vector<BYTE> data;

    if (!GetUsbDescriptor(
        hubHandle,
        portNumber,
        USB_DEVICE_DESCRIPTOR_TYPE,
        0,
        data,
        18))
    {
        return false;
    }

    if (data.size() < 18)
    {
        return false;
    }

    std::wcout
        << L"\n----------------------------------------"
        << std::endl;

    std::wcout
        << L"SD-80 Device Descriptor"
        << std::endl;

    std::wcout
        << L"----------------------------------------"
        << std::endl;

    std::wcout
        << L"bLength            : "
        << static_cast<int>(data[0])
        << std::endl;

    std::wcout
        << L"bDescriptorType    : 0x"
        << std::hex
        << std::setw(2)
        << std::setfill(L'0')
        << static_cast<int>(data[1])
        << std::endl;

    USHORT bcdUSB =
        static_cast<USHORT>(
            data[2] |
            (data[3] << 8));

    std::wcout
        << L"bcdUSB             : 0x"
        << std::setw(4)
        << bcdUSB
        << std::endl;

    std::wcout
        << L"bDeviceClass       : 0x"
        << std::setw(2)
        << static_cast<int>(data[4])
        << std::endl;

    std::wcout
        << L"bDeviceSubClass    : 0x"
        << std::setw(2)
        << static_cast<int>(data[5])
        << std::endl;

    std::wcout
        << L"bDeviceProtocol    : 0x"
        << std::setw(2)
        << static_cast<int>(data[6])
        << std::endl;

    std::wcout
        << L"bMaxPacketSize0    : "
        << std::dec
        << static_cast<int>(data[7])
        << std::endl;

    USHORT vid =
        static_cast<USHORT>(
            data[8] |
            (data[9] << 8));

    USHORT pid =
        static_cast<USHORT>(
            data[10] |
            (data[11] << 8));

    std::wcout
        << L"idVendor           : 0x"
        << std::hex
        << std::setw(4)
        << std::setfill(L'0')
        << vid
        << std::endl;

    std::wcout
        << L"idProduct          : 0x"
        << std::setw(4)
        << pid
        << std::endl;

    USHORT bcdDevice =
        static_cast<USHORT>(
            data[12] |
            (data[13] << 8));

    std::wcout
        << L"bcdDevice          : 0x"
        << std::setw(4)
        << bcdDevice
        << std::endl;

    std::wcout
        << L"iManufacturer      : "
        << std::dec
        << static_cast<int>(data[14])
        << std::endl;

    std::wcout
        << L"iProduct           : "
        << static_cast<int>(data[15])
        << std::endl;

    std::wcout
        << L"iSerialNumber      : "
        << static_cast<int>(data[16])
        << std::endl;

    std::wcout
        << L"bNumConfigurations : "
        << static_cast<int>(data[17])
        << std::endl;

    std::wcout
        << L"\nRaw Device Descriptor:"
        << std::endl;

    HexDump(data);

    return true;
}


// ============================================================
// Configuration Descriptor structure parser
// ============================================================

static const wchar_t* GetTransferTypeName(BYTE attributes)
{
    switch (attributes & 0x03)
    {
    case 0:
        return L"Control";

    case 1:
        return L"Isochronous";

    case 2:
        return L"Bulk";

    case 3:
        return L"Interrupt";

    default:
        return L"Unknown";
    }
}


static void DumpConfigurationStructure(
    const std::vector<BYTE>& data)
{
    std::wcout
        << L"\n"
        << L"========================================"
        << std::endl;

    std::wcout
        << L"SD-80 USB Structure"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    size_t offset = 0;

    int currentInterface = -1;
    int currentAlternateSetting = -1;


    while (offset + 2 <= data.size())
    {
        BYTE length = data[offset];
        BYTE type = data[offset + 1];


        // 不正なDescriptor長を防止
        if (length < 2)
        {
            std::wcout
                << L"\nInvalid descriptor length at 0x"
                << std::hex
                << offset
                << std::dec
                << std::endl;

            break;
        }

        if (offset + length > data.size())
        {
            std::wcout
                << L"\nDescriptor exceeds buffer at 0x"
                << std::hex
                << offset
                << std::dec
                << std::endl;

            break;
        }


        // ----------------------------------------------------
        // Configuration Descriptor
        // ----------------------------------------------------

        if (type == 0x02)
        {
            if (length >= 9)
            {
                USHORT totalLength =
                    static_cast<USHORT>(
                        data[offset + 2] |
                        (data[offset + 3] << 8));

                BYTE numInterfaces =
                    data[offset + 4];

                std::wcout
                    << L"\n[Configuration Descriptor]"
                    << std::endl;

                std::wcout
                    << L"  wTotalLength    : "
                    << totalLength
                    << std::endl;

                std::wcout
                    << L"  bNumInterfaces  : "
                    << static_cast<int>(
                        numInterfaces)
                    << std::endl;
            }
        }


        // ----------------------------------------------------
        // Interface Descriptor
        // ----------------------------------------------------

        else if (type == 0x04)
        {
            if (length >= 9)
            {
                currentInterface =
                    data[offset + 2];

                currentAlternateSetting =
                    data[offset + 3];

                BYTE numEndpoints =
                    data[offset + 4];

                BYTE interfaceClass =
                    data[offset + 5];

                BYTE interfaceSubClass =
                    data[offset + 6];

                BYTE interfaceProtocol =
                    data[offset + 7];

                BYTE interfaceString =
                    data[offset + 8];


                std::wcout
                    << L"\n[Interface Descriptor]"
                    << std::endl;

                std::wcout
                    << L"  Interface Number : "
                    << static_cast<int>(
                        currentInterface)
                    << std::endl;

                std::wcout
                    << L"  Alternate Setting: "
                    << static_cast<int>(
                        currentAlternateSetting)
                    << std::endl;

                std::wcout
                    << L"  Num Endpoints    : "
                    << static_cast<int>(
                        numEndpoints)
                    << std::endl;

                std::wcout
                    << L"  Class            : 0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<int>(
                        interfaceClass)
                    << std::endl;

                std::wcout
                    << L"  SubClass         : 0x"
                    << std::setw(2)
                    << static_cast<int>(
                        interfaceSubClass)
                    << std::endl;

                std::wcout
                    << L"  Protocol         : 0x"
                    << std::setw(2)
                    << static_cast<int>(
                        interfaceProtocol)
                    << std::endl;

                std::wcout
                    << L"  iInterface       : "
                    << std::dec
                    << static_cast<int>(
                        interfaceString)
                    << std::endl;
            }
        }


        // ----------------------------------------------------
        // Endpoint Descriptor
        // ----------------------------------------------------

        else if (type == 0x05)
        {
            if (length >= 7)
            {
                BYTE endpointAddress =
                    data[offset + 2];

                BYTE attributes =
                    data[offset + 3];

                USHORT maxPacketSize =
                    static_cast<USHORT>(
                        data[offset + 4] |
                        (data[offset + 5] << 8));

                BYTE interval =
                    data[offset + 6];


                bool directionIn =
                    (endpointAddress & 0x80) != 0;

                BYTE endpointNumber =
                    endpointAddress & 0x0F;


                std::wcout
                    << L"\n[Endpoint Descriptor]"
                    << std::endl;

                std::wcout
                    << L"  Interface        : "
                    << currentInterface
                    << std::endl;

                std::wcout
                    << L"  Alternate Setting: "
                    << currentAlternateSetting
                    << std::endl;

                std::wcout
                    << L"  Endpoint         : 0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<int>(
                        endpointAddress)
                    << std::endl;

                std::wcout
                    << L"  Number           : "
                    << std::dec
                    << static_cast<int>(
                        endpointNumber)
                    << std::endl;

                std::wcout
                    << L"  Direction        : "
                    << (directionIn
                        ? L"IN"
                        : L"OUT")
                    << std::endl;

                std::wcout
                    << L"  Transfer Type    : "
                    << GetTransferTypeName(
                        attributes)
                    << std::endl;

                std::wcout
                    << L"  Attributes       : 0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<int>(
                        attributes)
                    << std::endl;

                std::wcout
                    << L"  Max Packet Size  : "
                    << std::dec
                    << maxPacketSize
                    << std::endl;

                std::wcout
                    << L"  Interval         : "
                    << static_cast<int>(
                        interval)
                    << std::endl;
            }
        }


        // ----------------------------------------------------
        // Class-specific Descriptor
        // ----------------------------------------------------

        else if (type == 0x24)
        {
            std::wcout
                << L"\n[Class-Specific Descriptor]"
                << std::endl;

            std::wcout
                << L"  Offset            : 0x"
                << std::hex
                << std::uppercase
                << offset
                << std::endl;

            std::wcout
                << L"  Length            : "
                << std::dec
                << static_cast<int>(
                    length)
                << std::endl;

            std::wcout
                << L"  Descriptor Type   : 0x24"
                << std::endl;

            if (length >= 3)
            {
                std::wcout
                    << L"  Subtype           : 0x"
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill(L'0')
                    << static_cast<int>(
                        data[offset + 2])
                    << std::endl;
            }

            std::wcout
                << L"  Raw Data          :";

            for (size_t i = 0;
                i < length;
                ++i)
            {
                std::wcout
                    << L" "
                    << std::setw(2)
                    << static_cast<int>(
                        data[offset + i]);
            }

            std::wcout
                << std::dec
                << std::endl;
        }


        // ----------------------------------------------------
        // Unknown Descriptor
        // ----------------------------------------------------

        else
        {
            std::wcout
                << L"\n[Descriptor]"
                << std::endl;

            std::wcout
                << L"  Type              : 0x"
                << std::hex
                << std::uppercase
                << std::setw(2)
                << std::setfill(L'0')
                << static_cast<int>(
                    type)
                << std::endl;

            std::wcout
                << L"  Length            : "
                << std::dec
                << static_cast<int>(
                    length)
                << std::endl;
        }


        offset += length;
    }


    std::wcout
        << L"\n"
        << L"========================================"
        << std::endl;

    std::wcout
        << L"USB Structure Analysis Complete"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;
}



// ============================================================
// Configuration Descriptor
// ============================================================

static bool DumpConfigurationDescriptor(
    HANDLE hubHandle,
    ULONG portNumber)
{
    std::vector<BYTE> data;

    // 最初は十分大きめのバッファを用意
    if (!GetUsbDescriptor(
        hubHandle,
        portNumber,
        USB_CONFIGURATION_DESCRIPTOR_TYPE,
        0,
        data,
        4096))
    {
        return false;
    }

    if (data.size() < 9)
    {
        return false;
    }

    USHORT totalLength =
        static_cast<USHORT>(
            data[2] |
            (data[3] << 8));

    std::wcout
        << L"\n========================================"
        << std::endl;

    std::wcout
        << L"SD-80 Configuration Descriptor"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;

    std::wcout
        << L"Returned bytes : "
        << std::dec
        << data.size()
        << std::endl;

    std::wcout
        << L"wTotalLength   : "
        << totalLength
        << std::endl;

    std::wcout
        << L"bNumInterfaces : "
        << static_cast<int>(data[4])
        << std::endl;

    std::wcout
        << L"Raw Configuration Descriptor:"
        << std::endl;

    HexDump(data);

    DumpConfigurationStructure(data);

    return true;
}


// ============================================================
// SD-80を探索
// ============================================================

static bool ScanHubForSD80(
    const std::wstring& devicePath,
    int hubIndex)
{
    HANDLE hubHandle =
        OpenUsbHub(devicePath.c_str());

    if (hubHandle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    USB_HUB_INFORMATION_EX hubInfo{};

    ULONG bytesReturned = 0;

    BOOL result =
        DeviceIoControl(
            hubHandle,
            IOCTL_USB_GET_HUB_INFORMATION_EX,
            &hubInfo,
            sizeof(hubInfo),
            &hubInfo,
            sizeof(hubInfo),
            &bytesReturned,
            nullptr);

    if (!result)
    {
        CloseHandle(hubHandle);
        return false;
    }

    USHORT portCount =
        hubInfo.HighestPortNumber;

    for (ULONG port = 1;
        port <= portCount;
        ++port)
    {
        const size_t bufferSize =
            sizeof(
                USB_NODE_CONNECTION_INFORMATION_EX);

        std::vector<BYTE> buffer(
            bufferSize);

        auto info =
            reinterpret_cast<
            PUSB_NODE_CONNECTION_INFORMATION_EX>(
                buffer.data());

        ZeroMemory(
            info,
            bufferSize);

        info->ConnectionIndex =
            port;

        result =
            DeviceIoControl(
                hubHandle,
                IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                info,
                static_cast<DWORD>(bufferSize),
                info,
                static_cast<DWORD>(bufferSize),
                &bytesReturned,
                nullptr);

        if (!result)
        {
            continue;
        }

        if (info->DeviceDescriptor.idVendor !=
            0x0582 ||
            info->DeviceDescriptor.idProduct !=
            0x0029)
        {
            continue;
        }

        std::wcout
            << L"\n\n"
            << L"****************************************"
            << std::endl;

        std::wcout
            << L"*** EDIROL SD-80 FOUND ***"
            << std::endl;

        std::wcout
            << L"****************************************"
            << std::endl;

        std::wcout
            << L"Hub Index : "
            << hubIndex
            << std::endl;

        std::wcout
            << L"Port      : "
            << port
            << std::endl;

        std::wcout
            << L"VID       : 0x0582"
            << std::endl;

        std::wcout
            << L"PID       : 0x0029"
            << std::endl;

        // -----------------------------------------------
        // Device Descriptor
        // -----------------------------------------------

        DumpDeviceDescriptor(
            hubHandle,
            port);

        // -----------------------------------------------
        // Configuration Descriptor
        // -----------------------------------------------

        DumpConfigurationDescriptor(
            hubHandle,
            port);

        CloseHandle(hubHandle);

        return true;
    }

    CloseHandle(hubHandle);

    return false;
}


// ============================================================
// USB Hub列挙
// ============================================================

static bool EnumerateUsbHubs()
{
    sd80::Logger::Info(
        L"Searching for EDIROL SD-80...");

    HDEVINFO deviceInfoSet =
        SetupDiGetClassDevsW(
            &GUID_DEVINTERFACE_USB_HUB,
            nullptr,
            nullptr,
            DIGCF_PRESENT |
            DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        sd80::Logger::Error(
            L"SetupDiGetClassDevsW() failed.");

        return false;
    }

    DWORD index = 0;

    bool found = false;

    while (true)
    {
        SP_DEVICE_INTERFACE_DATA interfaceData{};

        interfaceData.cbSize =
            sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(
            deviceInfoSet,
            nullptr,
            &GUID_DEVINTERFACE_USB_HUB,
            index,
            &interfaceData))
        {
            if (GetLastError() ==
                ERROR_NO_MORE_ITEMS)
            {
                break;
            }

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
            ++index;
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

        if (SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            detailData,
            requiredSize,
            nullptr,
            nullptr))
        {
            if (ScanHubForSD80(
                detailData->DevicePath,
                static_cast<int>(index)))
            {
                found = true;
                break;
            }
        }

        ++index;
    }

    SetupDiDestroyDeviceInfoList(
        deviceInfoSet);

    return found;
}

// ============================================================
// Diagnostic Device Property Reader
// ============================================================

static void DumpDeviceProperty(
    DEVINST devInst,
    const DEVPROPKEY& key,
    const wchar_t* name)
{
    DEVPROPTYPE propertyType = 0;
    ULONG bufferSize = 0;

    // --------------------------------------------------------
    // First call:
    // Ask Windows for the required buffer size and property type
    // --------------------------------------------------------

    CONFIGRET cr =
        CM_Get_DevNode_PropertyW(
            devInst,
            &key,
            &propertyType,
            nullptr,
            &bufferSize,
            0);

    std::wcout
        << L"\n  "
        << name
        << std::endl;

    std::wcout
        << L"    First CONFIGRET : 0x"
        << std::hex
        << std::uppercase
        << static_cast<unsigned long>(cr)
        << std::endl;

    std::wcout
        << L"    Property Type   : 0x"
        << std::hex
        << std::uppercase
        << static_cast<unsigned long>(propertyType)
        << std::endl;

    std::wcout
        << L"    Required Size   : "
        << std::dec
        << bufferSize
        << L" bytes"
        << std::endl;


    // --------------------------------------------------------
    // Property does not exist
    // --------------------------------------------------------

    if (cr == CR_NO_SUCH_VALUE)
    {
        std::wcout
            << L"    Result          : PROPERTY NOT FOUND"
            << std::endl;

        return;
    }


    // --------------------------------------------------------
    // Unexpected error
    // --------------------------------------------------------

    if (cr != CR_BUFFER_SMALL &&
        cr != CR_SUCCESS)
    {
        std::wcout
            << L"    Result          : CM ERROR"
            << std::endl;

        return;
    }


    // --------------------------------------------------------
    // Empty property
    // --------------------------------------------------------

    if (bufferSize == 0)
    {
        std::wcout
            << L"    Result          : EMPTY"
            << std::endl;

        return;
    }


    // --------------------------------------------------------
    // Allocate buffer
    // --------------------------------------------------------

    std::vector<BYTE> buffer(
        bufferSize + sizeof(wchar_t),
        0);


    // --------------------------------------------------------
    // Second call:
    // Actually retrieve the property
    // --------------------------------------------------------

    ULONG actualSize = bufferSize;

    cr =
        CM_Get_DevNode_PropertyW(
            devInst,
            &key,
            &propertyType,
            buffer.data(),
            &actualSize,
            0);


    std::wcout
        << L"    Second CONFIGRET: 0x"
        << std::hex
        << std::uppercase
        << static_cast<unsigned long>(cr)
        << std::endl;

    std::wcout
        << L"    Actual Size     : "
        << std::dec
        << actualSize
        << L" bytes"
        << std::endl;


    if (cr != CR_SUCCESS)
    {
        std::wcout
            << L"    Result          : PROPERTY READ FAILED"
            << std::endl;

        return;
    }


    // ========================================================
    // Decode property
    // ========================================================

    if (propertyType == DEVPROP_TYPE_STRING ||
        propertyType == DEVPROP_TYPE_STRING_INDIRECT)
    {
        const wchar_t* text =
            reinterpret_cast<const wchar_t*>(
                buffer.data());

        std::wcout
            << L"    Value           : "
            << text
            << std::endl;

        return;
    }


    if (propertyType == DEVPROP_TYPE_STRING_LIST)
    {
        const wchar_t* p =
            reinterpret_cast<const wchar_t*>(
                buffer.data());

        std::wcout
            << L"    Value           : ";

        bool first = true;

        while (*p != L'\0')
        {
            if (!first)
            {
                std::wcout
                    << L" | ";
            }

            std::wcout
                << p;

            p += wcslen(p) + 1;

            first = false;
        }

        std::wcout
            << std::endl;

        return;
    }


    if (propertyType == DEVPROP_TYPE_UINT32 &&
        actualSize >= sizeof(UINT32))
    {
        UINT32 value = 0;

        memcpy(
            &value,
            buffer.data(),
            sizeof(value));

        std::wcout
            << L"    Value           : 0x"
            << std::hex
            << std::uppercase
            << value
            << std::dec
            << std::endl;

        return;
    }


    // --------------------------------------------------------
    // Unknown property type
    // --------------------------------------------------------

    std::wcout
        << L"    Value           : <binary / unsupported type>"
        << std::endl;

    std::wcout
        << L"    Raw Data        : ";

    for (ULONG i = 0;
        i < actualSize;
        ++i)
    {
        std::wcout
            << std::hex
            << std::uppercase
            << std::setw(2)
            << std::setfill(L'0')
            << static_cast<unsigned int>(
                buffer[i])
            << L' ';
    }

    std::wcout
        << std::dec
        << std::endl;
}


// ============================================================
// SD-80 Driver Information via Configuration Manager
// ============================================================

static void DumpSD80CMProperties(
    DEVINST devInst)
{
    std::wcout
        << L"\n"
        << L"========================================"
        << std::endl;

    std::wcout
        << L"SD-80 Driver Properties"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_Driver,
        L"Driver");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_Service,
        L"Service");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_DriverInfPath,
        L"Driver INF");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_DriverVersion,
        L"Driver Version");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_DriverProvider,
        L"Driver Provider");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_Class,
        L"Device Class");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_ClassGuid,
        L"Class GUID");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_FriendlyName,
        L"Friendly Name");

    DumpDeviceProperty(
        devInst,
        DEVPKEY_Device_LocationInfo,
        L"Location");


    std::wcout
        << L"========================================"
        << std::endl;
}






// ============================================================
// SD-80 Device Driver Information
// ============================================================

static void DumpProperty(
    HDEVINFO deviceInfoSet,
    SP_DEVINFO_DATA& deviceInfoData,
    DWORD property,
    const wchar_t* propertyName)
{
    DWORD dataType = 0;
    DWORD requiredSize = 0;

    SetupDiGetDeviceRegistryPropertyW(
        deviceInfoSet,
        &deviceInfoData,
        property,
        &dataType,
        nullptr,
        0,
        &requiredSize);

    if (requiredSize == 0)
    {
        std::wcout
            << L"  "
            << propertyName
            << L" : <not available>"
            << std::endl;

        return;
    }

    std::vector<BYTE> buffer(requiredSize + sizeof(wchar_t));

    if (!SetupDiGetDeviceRegistryPropertyW(
        deviceInfoSet,
        &deviceInfoData,
        property,
        &dataType,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr))
    {
        std::wcout
            << L"  "
            << propertyName
            << L" : <error "
            << GetLastError()
            << L">"
            << std::endl;

        return;
    }

    std::wcout
        << L"  "
        << propertyName
        << L" : ";

    if (dataType == REG_SZ ||
        dataType == REG_EXPAND_SZ)
    {
        std::wcout
            << reinterpret_cast<
            const wchar_t*>(buffer.data());
    }
    else if (dataType == REG_MULTI_SZ)
    {
        const wchar_t* p =
            reinterpret_cast<
            const wchar_t*>(buffer.data());

        bool first = true;

        while (*p != L'\0')
        {
            if (!first)
            {
                std::wcout << L" | ";
            }

            std::wcout << p;

            p += wcslen(p) + 1;
            first = false;
        }
    }
    else if (dataType == REG_DWORD &&
        buffer.size() >= sizeof(DWORD))
    {
        DWORD value =
            *reinterpret_cast<
            const DWORD*>(buffer.data());

        std::wcout
            << L"0x"
            << std::hex
            << std::uppercase
            << value
            << std::dec;
    }
    else
    {
        std::wcout
            << L"<binary data>";
    }

    std::wcout
        << std::endl;
}


// ============================================================
// Device Interface DetailからDevice Informationを取得
// ============================================================

static bool DumpSD80DriverInformation()
{
    std::wcout
        << L"\n"
        << L"========================================"
        << std::endl;

    std::wcout
        << L"SD-80 Windows Driver Information"
        << std::endl;

    std::wcout
        << L"========================================"
        << std::endl;


    HDEVINFO deviceInfoSet =
        SetupDiGetClassDevsW(
            nullptr,
            L"USB\\VID_0582&PID_0029",
            nullptr,
            DIGCF_PRESENT |
            DIGCF_ALLCLASSES);

    if (deviceInfoSet ==
        INVALID_HANDLE_VALUE)
    {
        std::wcout
            << L"SetupDiGetClassDevsW failed. Error = "
            << GetLastError()
            << std::endl;

        return false;
    }


    SP_DEVINFO_DATA deviceInfoData{};

    deviceInfoData.cbSize =
        sizeof(SP_DEVINFO_DATA);


    for (DWORD index = 0;; ++index)
    {
        ZeroMemory(
            &deviceInfoData,
            sizeof(deviceInfoData));

        deviceInfoData.cbSize =
            sizeof(SP_DEVINFO_DATA);


        if (!SetupDiEnumDeviceInfo(
            deviceInfoSet,
            index,
            &deviceInfoData))
        {
            if (GetLastError() ==
                ERROR_NO_MORE_ITEMS)
            {
                break;
            }

            continue;
        }


        DumpSD80CMProperties(deviceInfoData.DevInst);




        // ----------------------------------------------------
        // Instance ID
        // ----------------------------------------------------

        wchar_t instanceId[1024]{};

        if (CM_Get_Device_IDW(
            deviceInfoData.DevInst,
            instanceId,
            ARRAYSIZE(instanceId),
            0) == CR_SUCCESS)
        {
            std::wcout
                << L"\nInstance ID:"
                << std::endl;

            std::wcout
                << L"  "
                << instanceId
                << std::endl;
        }


        // ----------------------------------------------------
        // Device properties
        // ----------------------------------------------------

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_DEVICEDESC,
            L"Device Description");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_FRIENDLYNAME,
            L"Friendly Name");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_MFG,
            L"Manufacturer");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_CLASS,
            L"Class");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_CLASSGUID,
            L"Class GUID");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_SERVICE,
            L"Service");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_DRIVER,
            L"Driver Key");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_LOCATION_INFORMATION,
            L"Location");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_PHYSICAL_DEVICE_OBJECT_NAME,
            L"PDO Name");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_UPPERFILTERS,
            L"Upper Filters");

        DumpProperty(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_LOWERFILTERS,
            L"Lower Filters");


        // ----------------------------------------------------
        // Status
        // ----------------------------------------------------

        ULONG status = 0;
        ULONG problem = 0;

        CONFIGRET cr =
            CM_Get_DevNode_Status(
                &status,
                &problem,
                deviceInfoData.DevInst,
                0);

        if (cr == CR_SUCCESS)
        {
            std::wcout
                << L"\nStatus:"
                << std::endl;

            std::wcout
                << L"  Status  : 0x"
                << std::hex
                << std::uppercase
                << status
                << std::endl;

            std::wcout
                << L"  Problem : "
                << std::dec
                << problem
                << std::endl;
        }


        // ----------------------------------------------------
        // Registry Driver Key path
        // ----------------------------------------------------

        wchar_t driverKey[1024]{};

        DWORD propertyType = 0;
        DWORD requiredSize = 0;

        if (SetupDiGetDeviceRegistryPropertyW(
            deviceInfoSet,
            &deviceInfoData,
            SPDRP_DRIVER,
            &propertyType,
            reinterpret_cast<PBYTE>(
                driverKey),
            sizeof(driverKey),
            &requiredSize))
        {
            std::wcout
                << L"\nDriver Registry Key:"
                << std::endl;

            std::wcout
                << L"  HKLM\\SYSTEM\\CurrentControlSet"
                << L"\\Control\\Class\\"
                << driverKey
                << std::endl;
        }
    }


    SetupDiDestroyDeviceInfoList(
        deviceInfoSet);

    return true;
}



// ============================================================
// Main
// ============================================================

#ifndef SD80BRIDGE_VERBOSE_STARTUP
#define SD80BRIDGE_VERBOSE_STARTUP 0
#endif

int main()
{
    sd80::Logger::Info(
        L"SD80UsbExplorer started.");

#if SD80BRIDGE_VERBOSE_STARTUP

    // Diagnostic-only startup path.
    // Existing driver/property and USB hub/descriptor diagnostics
    // remain available when explicitly enabled.
    DumpSD80DriverInformation();

    bool found =
        EnumerateUsbHubs();

    if (!found)
    {
        sd80::Logger::Error(
            L"EDIROL SD-80 was not found.");

        return 1;
    }

    sd80::Logger::Info(
        L"SD-80 descriptor analysis completed.");

#endif

    // Normal runtime path.
    // ProbeSD80WinUSB() performs WinUSB interface discovery and
    // starts the MIDI bridge runtime.
    if (!ProbeSD80WinUSB())
    {
        sd80::Logger::Error(
            L"SD80Bridge MIDI runtime failed.");

        return 1;
    }

    return 0;
}
