#include "PocketNodeSerial.h"

PocketNodeSerial pocketnode_serial;

#ifdef POCKETNODE_HAS_USB_SERIAL
  static bool usb_started = false;
#endif

namespace pocketnode {
  Stream* uartStream() {
    return &Serial;
  }

  Stream* usbStream() {
    #ifdef POCKETNODE_HAS_USB_SERIAL
      return usb_started ? static_cast<Stream*>(&USBSerial) : nullptr;
    #else
      return nullptr;
    #endif
  }

  bool usbConnected() {
    #ifdef POCKETNODE_HAS_USB_SERIAL
      return usb_started && static_cast<bool>(USBSerial);
    #else
      return false;
    #endif
  }

  void beginUsb(unsigned long baud) {
    #ifdef POCKETNODE_HAS_USB_SERIAL
      if (usb_started) return;
      USBSerial.begin(baud);
      // Zero timeout keeps a scan from blocking when the host stops reading.
      USBSerial.setTxTimeoutMs(0);
      usb_started = true;
    #else
      (void)baud;
    #endif
  }
}

void PocketNodeSerial::begin(unsigned long baud) {
  Serial.begin(baud);
  pocketnode::beginUsb(baud);
}

int PocketNodeSerial::available() {
  return Serial.available();
}

int PocketNodeSerial::read() {
  return Serial.read();
}

int PocketNodeSerial::peek() {
  return Serial.peek();
}

void PocketNodeSerial::flush() {
  Serial.flush();
}

size_t PocketNodeSerial::write(uint8_t c) {
  return write(&c, 1);
}

size_t PocketNodeSerial::write(const uint8_t* buffer, size_t size) {
  if (!buffer || size == 0) return 0;

  const size_t written = Serial.write(buffer, size);

  #ifdef POCKETNODE_HAS_USB_SERIAL
    if (usb_started) USBSerial.write(buffer, size);
  #endif

  return written;
}

void PocketNodeSerial::setDebugOutput(bool enable) {
  (void)enable;
}

uint32_t PocketNodeSerial::baudRate() {
  return 115200;
}

PocketNodeSerial::operator bool() const {
  return true;
}
