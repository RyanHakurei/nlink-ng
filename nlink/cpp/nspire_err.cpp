#include "nlink_internal.hpp"

extern "C" {
#include "error.h"
}

#ifndef __ANDROID__
#include <libusb.h>
#endif

#include <string>

namespace nlink {
namespace {
std::string android_busy() {
  return "Calculator is busy (often after a failed transfer). Unplug it, wait a few seconds, plug "
         "it back in, then connect again.";
}

const char *access_denied_message() {
#ifdef _WIN32
  return "Permission denied while accessing the calculator. On Windows, install the WinUSB driver "
         "for it with Zadig.";
#else
  return "Permission denied while accessing the calculator. On Linux, install the udev rules.";
#endif
}
}

std::string nspire_user_message(int code, bool android_text) {
  if (code == 0) return {};
  if (android_text) {
    const char *raw = nspire_strerror(code);
    std::string text = raw ? raw : ("libnspire error " + std::to_string(code));
    if (text.size() == 4 && (text[0] == 'b' || text[0] == 'B') &&
        (text[1] == 'u' || text[1] == 'U') && (text[2] == 's' || text[2] == 'S') &&
        (text[3] == 'y' || text[3] == 'Y')) {
      return android_busy();
    }
    return text;
  }
  if (code < 0) {
    auto nspire = static_cast<unsigned>(-code);
    if (nspire < NSPIRE_ERR_MAX) {
      switch (nspire) {
        case NSPIRE_ERR_TIMEOUT:
          return TIMEOUT_MESSAGE;
        case NSPIRE_ERR_BUSY:
          return "The calculator is busy. Close any open documents on the device and try again.";
        case NSPIRE_ERR_NODEVICE:
          return "The calculator was disconnected.";
        case NSPIRE_ERR_LIBUSB:
          return "LibUSB error";
        case NSPIRE_ERR_INVALPKT:
        case NSPIRE_ERR_NACK:
          return "The calculator sent an invalid response. The filesystem may be corrupted.";
        case NSPIRE_ERR_NONEXIST:
          return "The path does not exist on the calculator.";
        case NSPIRE_ERR_EXISTS:
          return "A file or directory with that name already exists.";
        case NSPIRE_ERR_NOMEM:
          return "The calculator ran out of memory while handling this request.";
        default:
          return "unknown error";
      }
    }
  }
#ifndef __ANDROID__
  switch (code) {
    case LIBUSB_ERROR_IO:
      return "Input/output error";
    case LIBUSB_ERROR_INVALID_PARAM:
      return "Invalid input";
    case LIBUSB_ERROR_ACCESS:
      return access_denied_message();
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
      return "The calculator was disconnected.";
    case LIBUSB_ERROR_BUSY:
      return "The calculator is busy. Close any open documents on the device and try again.";
    case LIBUSB_ERROR_TIMEOUT:
      return TIMEOUT_MESSAGE;
    case LIBUSB_ERROR_NO_MEM:
      return "The calculator ran out of memory while handling this request.";
    case LIBUSB_ERROR_NOT_SUPPORTED:
      return "Operation not supported or unimplemented on this platform";
    default:
      return "LibUSB error";
  }
#else
  return "LibUSB error";
#endif
}

std::string libusb_user_message(int code) {
#ifndef __ANDROID__
  switch (code) {
    case LIBUSB_ERROR_ACCESS:
      return access_denied_message();
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
      return "The calculator was disconnected.";
    case LIBUSB_ERROR_BUSY:
      return "The calculator is busy. Close any open documents on the device and try again.";
    case LIBUSB_ERROR_TIMEOUT:
      return TIMEOUT_MESSAGE;
    default:
      return "LibUSB error";
  }
#else
  (void)code;
  return "LibUSB error";
#endif
}

void nspire_check(int code, bool android_text) {
  if (code != 0) throw Error(nspire_user_message(code, android_text));
}
}  // namespace nlink
