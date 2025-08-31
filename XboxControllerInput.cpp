#include 
#include 
#include 
#include
#include 
#include  // For _kbhit() and _getch()

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// Replace these with your Xbox controller's VID and PID
#define VID 0x045E
#define PID 0x02E0

// Function to find the device path of the controller
std::wstring find_device_path() {
    GUID HidGuid;
    HidD_GetHidGuid(&HidGuid);

    HDEVINFO device_info = SetupDiGetClassDevs(&HidGuid, NULL, NULL, DIGCF_PRESENT);
    if (device_info == INVALID_HANDLE_VALUE) {
        return L"";
    }

    SP_DEVICE_INTERFACE_DATA device_interface_data;
    device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(device_info, NULL, &HidGuid, i, &device_interface_data); ++i) {
        DWORD required_size = 0;
        SetupDiGetDeviceInterfaceDetail(device_info, &device_interface_data, NULL, 0, &required_size, NULL);
        std::vector detail_data_buffer(required_size);
        PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA)detail_data_buffer.data();
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(device_info, &device_interface_data, detail_data, required_size, NULL, NULL)) {
            HANDLE device_handle = CreateFile(detail_data->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (device_handle == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attributes;
            attributes.Size = sizeof(HIDD_ATTRIBUTES);
            if (HidD_GetAttributes(device_handle, &attributes)) {
                if (attributes.VendorID == VID && attributes.ProductID == PID) {
                    CloseHandle(device_handle);
                    SetupDiDestroyDeviceInfoList(device_info);
                    return std::wstring(detail_data->DevicePath);
                }
            }
            CloseHandle(device_handle);
        }
    }
    SetupDiDestroyDeviceInfoList(device_info);
    return L"";
}

int main() {
    std::wcout << L"Looking for Xbox controller..." << std::endl;
    std::wstring device_path = find_device_path();

    if (device_path.empty()) {
        std::wcout << L"Controller not found." << std::endl;
        return 1;
    }

    HANDLE device_handle = CreateFile(device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (device_handle == INVALID_HANDLE_VALUE) {
        std::wcout << L"Failed to open device." << std::endl;
        return 1;
    }

    // Set non-overlapped I/O
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    std::cout << "Starting input read loop. Press any key to exit." << std::endl;

    while (!_kbhit()) {
        BYTE buffer[64]; // Adjust size based on report size
        DWORD bytes_read;

        BOOL success = ReadFile(device_handle, buffer, sizeof(buffer), &bytes_read, &overlapped);

        if (!success) {
            if (GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(overlapped.hEvent, INFINITE);
                success = GetOverlappedResult(device_handle, &overlapped, &bytes_read, FALSE);
            }
        }

        if (success && bytes_read > 0) {
            // Parse buffer to extract button states and other values
            // Example: Print raw buffer
            std::vector values(buffer, buffer + bytes_read);
            std::cout << "\r[ ";
            for (auto v : values) {
                std::cout << v << " ";
            }
            std::cout << "]" << std::flush;
        }
        Sleep(50);
    }

    CloseHandle(device_handle);
    std::wcout << L"\nExiting." << std::endl;

    return 0;
}
