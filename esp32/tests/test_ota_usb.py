"""Exercise the real USB stream with a sender that waits for each ACK."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class UsbFlowControlTests(unittest.TestCase):
    def test_ack_follows_flash_work_and_image_protocol_is_unchanged(self):
        source = (Path(__file__).resolve().parents[1] / "src/usb_console.cpp").read_text()
        stream = source.split("class ChunkedUsbStream", 1)[1].split("\n#if defined(WIGNET_BENCH_AUDIO)", 1)[0]
        stub = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
using std::min;
constexpr size_t IMAGE_CHUNK = 4096;
uint32_t millis() { static uint32_t now = 0; return ++now; }
void delay(int) {}
class Stream {
public:
 virtual ~Stream() = default;
 virtual int available() = 0;
 virtual int read() = 0;
 virtual int peek() = 0;
 virtual size_t write(uint8_t) = 0;
 virtual size_t readBytes(char *, size_t) = 0;
 void setTimeout(uint32_t n) { _timeout = n; }
protected:
 uint32_t _timeout = 0;
};
struct SerialMock {
 size_t at = 0, allowed = 4096, total = 9000;
 unsigned acknowledgements = 0;
 int available() { return allowed - at; }
 int peek() { return available() ? int(at % 251) : -1; }
 int read() { return available() ? int((at++) % 251) : -1; }
 size_t read(uint8_t *out, size_t n) {
  n = min(n, allowed - at);
  for(size_t i = 0; i < n; ++i) out[i] = (at++) % 251;
  return n;
 }
 void println(const char *s) {
  assert(strcmp(s,"[bench] +") == 0);
  ++acknowledgements;
  allowed = min(total, allowed + 4096);
 }
 void flush() {}
} Serial;
'''
        harness = r'''
int main() {
 char buffer[4096];
 ChunkedUsbStream ota(9000, true);
 assert(ota.readBytes(buffer, 4096) == 4096);
 assert(Serial.acknowledgements == 0); // safe to erase/write flash now
 assert(Serial.at == Serial.allowed);
 assert(ota.readBytes(buffer, 4096) == 4096);
 assert(Serial.acknowledgements == 1);
 assert(ota.readBytes(buffer, 808) == 808);
 assert(Serial.acknowledgements == 2);
 assert(ota.remaining() == 0);
 Serial = SerialMock();
 ChunkedUsbStream image(9000);
 assert(image.readBytes(buffer, 4096) == 4096);
 assert(Serial.acknowledgements == 1); // original image protocol
 assert(image.readBytes(buffer, 4096) == 4096);
 assert(Serial.acknowledgements == 2);
 assert(image.readBytes(buffer, 808) == 808);
 assert(image.remaining() == 0);
 assert(Serial.acknowledgements == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "flow.cpp"
            cpp.write_text(stub + "class ChunkedUsbStream" + stream + harness)
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(root / "flow")], check=True)
            subprocess.run([str(root / "flow")], check=True)


if __name__ == "__main__":
    unittest.main()
