#pragma once

#ifndef PocketNodeSerial_h
#define PocketNodeSerial_h

#include <Arduino.h>

// The native USB-C port is a separate CDC endpoint only when the core is built
// with the hardware USB-Serial-JTAG peripheral (USBMode=hwcdc) while UART0 keeps
// the `Serial` object (CDCOnBoot=disabled).
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    defined(CONFIG_IDF_TARGET_ESP32S3) && !ARDUINO_USB_CDC_ON_BOOT
  #define POCKETNODE_HAS_USB_SERIAL 1
#endif

namespace pocketnode {
  // Real ports behind the proxy. usbStream() is NULL when the build has no
  // native USB CDC, so callers never need their own #ifdef.
  Stream* uartStream();
  Stream* usbStream();
  bool usbConnected();
  void beginUsb(unsigned long baud);
}

// Mirrors every byte written through `Serial` to UART0 and, when available, to
// the native USB CDC port. Writes never block on a host that is not reading, so
// an unplugged phone or laptop cannot stall a scan.
class PocketNodeSerial : public Stream {
  public:
    void begin(unsigned long baud);
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;
    void setDebugOutput(bool);
    uint32_t baudRate();
    operator bool() const;
};

extern PocketNodeSerial pocketnode_serial;

#endif
