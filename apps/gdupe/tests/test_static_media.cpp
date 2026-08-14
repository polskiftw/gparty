#include "mp4_decode.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> result;
  unsigned accumulator = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=')
      break;
    const auto p = alphabet.find(c);
    if (p == std::string_view::npos)
      continue;
    accumulator = (accumulator << 6U) | static_cast<unsigned>(p);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return result;
}

class TempFile {
public:
  TempFile() {
    path_ = std::filesystem::temp_directory_path() /
            ("gdupe-hevc-hvc1-" +
             std::to_string(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()) +
             ".mp4");
  }
  ~TempFile() { std::filesystem::remove(path_); }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void test_hevc_hvc1_mp4() {
  // Two-frame, one-second, 128x128 HEVC Main/yuv420p MP4 using an hvc1
  // sample entry. Generated once as a deterministic test fixture; runtime
  // tests require no encoder, FFmpeg, or external media tool.
  constexpr std::string_view fixture =
      "AAAAHGZ0eXBpc29tAAACAGlzb21pc28ybXA0MQAADI5tb292AAAAbG12aGQAAAAAAAAAAAAAAAAAAAPoAAAD6AABAAABAAAAAAAA"
      "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAALuXRy"
      "YWsAAABcdGtoZAAAAAMAAAAAAAAAAAAAAAEAAAAAAAAD6AAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
      "AAAAAAAAAEAAAAAAgAAAAIAAAAAAACRlZHRzAAAAHGVsc3QAAAAAAAAAAQAAA+gAAAAAAAEAAAAACzFtZGlhAAAAIG1kaGQAAAAA"
      "AAAAAAAAAAAAAEAAAABAAFXEAAAAAAAtaGRscgAAAAAAAAAAdmlkZQAAAAAAAAAAAAAAAFZpZGVvSGFuZGxlcgAAAArcbWluZgAA"
      "ABR2bWhkAAAAAQAAAAAAAAAAAAAAJGRpbmYAAAAcZHJlZgAAAAAAAAABAAAADHVybCAAAAABAAAKnHN0YmwAAAocc3RzZAAAAAAA"
      "AAABAAAKDGh2YzEAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAgACAAEgAAABIAAAAAAAAAAEVTGF2YzYxLjE5LjEwMSBsaWJ4MjY1"
      "AAAAAAAAAAAAAAAY//8AAAmIaHZjQwEBYAAAAJAAAAAAAB7wAPz9+PgAAA8EoAABABhAAQwB//8BYAAAAwCQAAADAAADAB6SgJCh"
      "AAEAKUIBAQFgAAADAJAAAAMAAAMAHqAQICBZZKkkyvAWgIAAAAMAgAAAAwEEogABAAdEAcFytCJAJwABCQ1OAQX///////////8I"
      "LKLeCbUXR9u7VaT+f8L8TngyNjUgKGJ1aWxkIDIxNSkgLSA0LjErMS0xZDExN2JlOltMaW51eF1bR0NDIDE0LjIuMF1bNjQgYml0"
      "XSA4Yml0KzEwYml0KzEyYml0IC0gSC4yNjUvSEVWQyBjb2RlYyAtIENvcHlyaWdodCAyMDEzLTIwMTggKGMpIE11bHRpY29yZXdh"
      "cmUsIEluYyAtIGh0dHA6Ly94MjY1Lm9yZyAtIG9wdGlvbnM6IGNwdWlkPTExMTEwMzkgZnJhbWUtdGhyZWFkcz0xIG51bWEtcG9v"
      "bHM9NSBuby13cHAgbm8tcG1vZGUgbm8tcG1lIG5vLXBzbnIgbm8tc3NpbSBsb2ctbGV2ZWw9MCBiaXRkZXB0aD04IGlucHV0LWNz"
      "cD0xIGZwcz0yLzEgaW5wdXQtcmVzPTEyOHgxMjggaW50ZXJsYWNlPTAgdG90YWwtZnJhbWVzPTAgbGV2ZWwtaWRjPTAgaGlnaC10"
      "aWVyPTEgdWhkLWJkPTAgcmVmPTMgbm8tYWxsb3ctbm9uLWNvbmZvcm1hbmNlIG5vLXJlcGVhdC1oZWFkZXJzIGFubmV4YiBuby1h"
      "dWQgbm8tZW9iIG5vLWVvcyBuby1ocmQgaW5mbyBoYXNoPTAgdGVtcG9yYWwtbGF5ZXJzPTAgb3Blbi1nb3AgbWluLWtleWludD0x"
      "IGtleWludD0zMCBnb3AtbG9va2FoZWFkPTAgYmZyYW1lcz0wIGItYWRhcHQ9MiBuby1iLXB5cmFtaWQgYmZyYW1lLWJpYXM9MCBy"
      "Yy1sb29rYWhlYWQ9MjAgbG9va2FoZWFkLXNsaWNlcz0wIHNjZW5lY3V0PTAgbm8taGlzdC1zY2VuZWN1dCByYWRsPTAgbm8tc3Bs"
      "aWNlIG5vLWludHJhLXJlZnJlc2ggY3R1PTY0IG1pbi1jdS1zaXplPTggbm8tcmVjdCBuby1hbXAgbWF4LXR1LXNpemU9MzIgdHUt"
      "aW50ZXItZGVwdGg9MSB0dS1pbnRyYS1kZXB0aD0xIGxpbWl0LXR1PTAgcmRvcS1sZXZlbD0wIGR5bmFtaWMtcmQ9MC4wMiBuby1z"
      "c2ltLXJkIHNpZ25oaWRlIG5vLXRza2lwIG5yLWludHJhPTAgbnItaW50ZXI9MCBuby1jb25zdHJhaW5lZC1pbnRyYSBzdHJvbmct"
      "aW50cmEtc21vb3RoaW5nIG1heC1tZXJnZT0zIGxpbWl0LXJlZnM9MSBuby1saW1pdC1tb2RlcyBtZT0xIHN1Ym1lPTIgbWVyYW5n"
      "ZT01NyB0ZW1wb3JhbC1tdnAgbm8tZnJhbWUtZHVwIG5vLWhtZSB3ZWlnaHRwIG5vLXdlaWdodGIgbm8tYW5hbHl6ZS1zcmMtcGlj"
      "cyBkZWJsb2NrPTA6MCBzYW8gbm8tc2FvLW5vbi1kZWJsb2NrIHJkPTMgc2VsZWN0aXZlLXNhbz00IGVhcmx5LXNraXAgcnNraXAg"
      "bm8tZmFzdC1pbnRyYSBuby10c2tpcC1mYXN0IG5vLWN1LWxvc3NsZXNzIGItaW50cmEgbm8tc3BsaXRyZC1za2lwIHJkcGVuYWx0"
      "eT0wIHBzeS1yZD0yLjAwIHBzeS1yZG9xPTAuMDAgbm8tcmQtcmVmaW5lIG5vLWxvc3NsZXNzIGNicXBvZmZzPTAgY3JxcG9mZnM9"
      "MCByYz1jcmYgY3JmPTI4LjAgcWNvbXA9MC42MCBxcHN0ZXA9NCBzdGF0cy13cml0ZT0wIHN0YXRzLXJlYWQ9MCBpcHJhdGlvPTEu"
      "NDAgYXEtbW9kZT0yIGFxLXN0cmVuZ3RoPTEuMDAgY3V0cmVlIHpvbmUtY291bnQ9MCBuby1zdHJpY3QtY2JyIHFnLXNpemU9MzIg"
      "bm8tcmMtZ3JhaW4gcXBtYXg9NjkgcXBtaW49MCBuby1jb25zdC12YnYgc2FyPTEgb3ZlcnNjYW49MCB2aWRlb2Zvcm1hdD01IHJh"
      "bmdlPTAgY29sb3JwcmltPTIgdHJhbnNmZXI9MiBjb2xvcm1hdHJpeD0yIGNocm9tYWxvYz0wIGRpc3BsYXktd2luZG93PTAgY2xs"
      "PTAsMCBtaW4tbHVtYT0wIG1heC1sdW1hPTI1NSBsb2cyLW1heC1wb2MtbHNiPTggdnVpLXRpbWluZy1pbmZvIHZ1aS1ocmQtaW5m"
      "byBzbGljZXM9MSBuby1vcHQtcXAtcHBzIG5vLW9wdC1yZWYtbGlzdC1sZW5ndGgtcHBzIG5vLW11bHRpLXBhc3Mtb3B0LXJwcyBz"
      "Y2VuZWN1dC1iaWFzPTAuMDUgbm8tb3B0LWN1LWRlbHRhLXFwIG5vLWFxLW1vdGlvbiBuby1oZHIxMCBuby1oZHIxMC1vcHQgbm8t"
      "ZGhkcjEwLW9wdCBuby1pZHItcmVjb3Zlcnktc2VpIGFuYWx5c2lzLXJldXNlLWxldmVsPTAgYW5hbHlzaXMtc2F2ZS1yZXVzZS1s"
      "ZXZlbD0wIGFuYWx5c2lzLWxvYWQtcmV1c2UtbGV2ZWw9MCBzY2FsZS1mYWN0b3I9MCByZWZpbmUtaW50cmE9MCByZWZpbmUtaW50"
      "ZXI9MCByZWZpbmUtbXY9MSByZWZpbmUtY3R1LWRpc3RvcnRpb249MCBuby1saW1pdC1zYW8gY3R1LWluZm89MCBuby1sb3dwYXNz"
      "LWRjdCByZWZpbmUtYW5hbHlzaXMtdHlwZT0wIGNvcHktcGljPTEgbWF4LWF1c2l6ZS1mYWN0b3I9MS4wIG5vLWR5bmFtaWMtcmVm"
      "aW5lIG5vLXNpbmdsZS1zZWkgbm8taGV2Yy1hcSBuby1zdnQgbm8tZmllbGQgcXAtYWRhcHRhdGlvbi1yYW5nZT0xLjAwIHNjZW5l"
      "Y3V0LWF3YXJlLXFwPTBjb25mb3JtYW5jZS13aW5kb3ctb2Zmc2V0cyByaWdodD0wIGJvdHRvbT0wIGRlY29kZXItbWF4LXJhdGU9"
      "MCBuby12YnYtbGl2ZS1tdWx0aS1wYXNzIG5vLW1jc3RmIG5vLXNicmMgbm8tZnJhbWUtcmOAAAAACmZpZWwBAAAAABBwYXNwAAAA"
      "AQAAAAEAAAAUYnRydAAAAAAAAISIAAAAAAAAABhzdHRzAAAAAAAAAAEAAAACAAAgAAAAABRzdHNzAAAAAAAAAAEAAAABAAAAHHN0"
      "c2MAAAAAAAAAAQAAAAEAAAACAAAAAQAAABxzdHN6AAAAAAAAAAAAAAACAAAJ/QAABpQAAAAUc3RjbwAAAAAAAAABAAAMugAAAGF1"
      "ZHRhAAAAWW1ldGEAAAAAAAAAIWhkbHIAAAAAAAAAAG1kaXJhcHBsAAAAAAAAAAAAAAAALGlsc3QAAAAkqXRvbwAAABxkYXRhAAAA"
      "AQAAAABMYXZmNjEuNy4xMDMAAAAIZnJlZQAAEJltZGF0AAAJ+SgBr3iYpGzK3HZtXsga3FfbTUFSiVribLqnCjx7/eMBfxEjis4v"
      "zCFPzKk0E4LjuQ1MtqQWa+n2ySbxDI5gFWKLy1e25oJ/T5e3GfzgkYqvF2LVggBAG1a/dvCQEnjvXIWe9wZcQVl/1Uanp456DLaY"
      "F847RI6m5hB21304TrAtpyM6t/DgxF80AfSwwRvIBrV8zcl4fGyqzAY5C/CxmvTyFky833lLw42pWv4H9DvYqZkYejrtdoINu7Kd"
      "MK+QI7MHek41UAusS9umt4wt9IVNlbVRegiw1kMfRSTiZ3+gI/IyMIDsDl1GXBpwwFhy6jK254TPVJDNFgIuHO2ItElheNDWpvX5"
      "bH7zjvIDZlWaK0Vrd1USHbYrsVFqRpRr6LTuCrxCVKvva5s60JDUFluuL9qnKkxCBabVxQftLvAhOiwP96DvoJOv+MHAVIeFxoGk"
      "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";

  const auto bytes = base64_decode(fixture);
  require(bytes.size() == 7499, "embedded HEVC MP4 fixture decoded incorrectly");

  TempFile temp;
  {
    std::ofstream output(temp.path(), std::ios::binary);
    require(output.good(), "could not create HEVC MP4 test fixture");
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "could not write HEVC MP4 test fixture");
  }

  const auto decoded = gdupe::decode_mp4_static(
      temp.path(), 2, std::chrono::steady_clock::now() + std::chrono::seconds(20));
  require(decoded.width == 128 && decoded.height == 128,
          "HEVC hvc1 decode returned wrong dimensions");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          "HEVC hvc1 decode returned wrong duration");
  require(decoded.frame_count == 2,
          "HEVC hvc1 MP4 sample table returned wrong frame count");
  require(decoded.sampled_frames.size() == 2,
          "HEVC hvc1 decode did not return both sampled frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 128 && frame.height == 128,
            "HEVC hvc1 sampled frame dimensions are wrong");
    require(frame.pixels.size() == 128U * 128U,
            "HEVC hvc1 grayscale frame buffer is incomplete");
  }
}

} // namespace

int main() {
  try {
    test_hevc_hvc1_mp4();
    std::cout << "gdupe static media tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe static media test failure: " << problem.what() << '\n';
    return 1;
  }
}
