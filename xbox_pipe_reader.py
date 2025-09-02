# xbox_pipe_reader.py
# Base requirements:
# - For Windows Pipe: pip install pywin32
# - For BLE (optional): pip install bleak
# - For USB Serial (optional): pip install pyserial

from abc import ABC, abstractmethod
import time
import json
from typing import Optional, Dict, Any

# Transport base class
class ControllerTransport(ABC):
    """Abstract base class defining the interface for all transport methods"""
    
    @abstractmethod
    def connect(self) -> bool:
        """Establish connection with the controller reader"""
        pass
    
    @abstractmethod
    def disconnect(self) -> None:
        """Clean up and close the connection"""
        pass
    
    @abstractmethod
    def read_state(self) -> Optional[Dict[str, Any]]:
        """Read one controller state update, returns parsed JSON or None if no data"""
        pass
    
    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """Check if transport is currently connected"""
        pass

# Windows Named Pipe Transport
class WindowsPipeTransport(ControllerTransport):
    """Windows Named Pipe implementation of the controller transport"""
    
    def __init__(self, pipe_name: str = r"\\.\pipe\XboxReaderPipe"):
        import win32file
        import win32pipe
        import pywintypes
        
        self._pipe_name = pipe_name
        self._handle = None
        self._buffer = b""
    
    def connect(self) -> bool:
        import win32file
        import pywintypes
        
        try:
            self._handle = win32file.CreateFile(
                self._pipe_name,
                win32file.GENERIC_READ,
                0,
                None,
                win32file.OPEN_EXISTING,
                0,
                None
            )
            return True
        except pywintypes.error:
            return False
        except Exception as e:
            print(f"Pipe connection error: {e}")
            return False

    def disconnect(self) -> None:
        import win32file
        
        if self._handle:
            try:
                win32file.CloseHandle(self._handle)
            except Exception as e:
                print(f"Pipe disconnect error: {e}")
            finally:
                self._handle = None
    
    def read_state(self) -> Optional[Dict[str, Any]]:
        import win32file
        import pywintypes
        
        if not self.is_connected:
            return None
            
        try:
            hr, data = win32file.ReadFile(self._handle, 4096, None)
            if not data:
                time.sleep(0.05)  # No data available, small delay
                return None
                
            self._buffer += data
            if b"\n" in self._buffer:
                line, self._buffer = self._buffer.split(b"\n", 1)
                if line:
                    return json.loads(line.decode("utf-8"))
        except pywintypes.error as e:
            print(f"Pipe read error: {e}")
            self.disconnect()  # Ensure cleanup on error
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            print(f"Data parsing error: {e}")
        return None
            
    @property
    def is_connected(self) -> bool:
        return self._handle is not None

# Bluetooth LE Transport
class BLETransport(ControllerTransport):
    """Bluetooth LE implementation of the controller transport"""
    
    def __init__(self, service_uuid: str, characteristic_uuid: str):
        self._service_uuid = service_uuid
        self._char_uuid = characteristic_uuid
        self._device = None
        self._characteristic = None
        
    def connect(self) -> bool:
        try:
            # Bleak is imported here to make it an optional dependency
            import asyncio
            from bleak import BleakClient, BleakScanner
            
            # Scan for devices advertising our service
            device = asyncio.run(BleakScanner.find_device_by_filter(
                lambda d, ad: self._service_uuid in ad.service_uuids if ad.service_uuids else False
            ))
            
            if not device:
                return False
                
            # Connect and get characteristic
            self._device = BleakClient(device.address)
            asyncio.run(self._device.connect())
            self._characteristic = self._char_uuid
            return True
        except Exception as e:
            print(f"BLE connection error: {e}")
            self._device = None
            return False
            
    def disconnect(self) -> None:
        if self._device:
            try:
                import asyncio
                asyncio.run(self._device.disconnect())
            except Exception as e:
                print(f"BLE disconnect error: {e}")
            finally:
                self._device = None
            
    def read_state(self) -> Optional[Dict[str, Any]]:
        if not self.is_connected:
            return None
            
        try:
            import asyncio
            # Read notification data from BLE characteristic
            data = asyncio.run(self._device.read_gatt_char(self._characteristic))
            return json.loads(data.decode("utf-8"))
        except Exception as e:
            print(f"BLE read error: {e}")
            self.disconnect()  # Ensure cleanup on error
            return None
            
    @property
    def is_connected(self) -> bool:
        return self._device is not None and self._device.is_connected

def main():
    # Create the appropriate transport
    transport = WindowsPipeTransport()
    
    print("Waiting for Xbox Controller connection... (will retry until connected)")
    
    # Try to connect until successful
    while not transport.connect():
        time.sleep(0.3)
    
    print(f"Connected successfully!")
    
    try:
        while True:
            state = transport.read_state()
            if state:
                print("Controller state:", state)
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        transport.disconnect()

if __name__ == "__main__":
    # Example of how to use different transports:
    
    # Windows Named Pipe (default)
    transport = WindowsPipeTransport()
    
    # Bluetooth LE (requires bleak package)
    # transport = BLETransport(
    #     service_uuid="YOUR-SERVICE-UUID",
    #     characteristic_uuid="YOUR-CHARACTERISTIC-UUID"
    # )
    
    # You can add more transport types here in the future, such as:
    # - USB Serial Transport
    # - Network Socket Transport
    # - etc.
    
    main()
