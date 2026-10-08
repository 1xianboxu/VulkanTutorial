#include "renderer/PlatformDialogs.h"

#include <filesystem>
#include <iostream>
#include <vector>

int wmain(int argc, wchar_t **argv) {
  if (argc != 2)
    return 2;
  constexpr int width = 32;
  constexpr int height = 24;
  std::vector<unsigned char> rgba(width * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      unsigned char *p = rgba.data() + (y * width + x) * 4;
      p[0] = static_cast<unsigned char>(x * 255 / (width - 1));
      p[1] = static_cast<unsigned char>(y * 255 / (height - 1));
      p[2] = 180;
      p[3] = static_cast<unsigned char>((x + y) * 255 /
                                       (width + height - 2));
    }
  }
  if (!saveRgbaPng(argv[1], rgba.data(), width, height)) {
    std::wcerr << L"save failed: " << argv[1] << L'\n';
    return 1;
  }
  std::wcout << L"saved: " << argv[1] << L'\n';
  return 0;
}
