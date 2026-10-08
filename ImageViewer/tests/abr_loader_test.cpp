#include "renderer/AbrBrushLoader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
void be16(std::vector<unsigned char> &v, unsigned value) {
  v.push_back((value >> 8) & 0xff);
  v.push_back(value & 0xff);
}
void be32(std::vector<unsigned char> &v, unsigned value) {
  v.push_back((value >> 24) & 0xff);
  v.push_back((value >> 16) & 0xff);
  v.push_back((value >> 8) & 0xff);
  v.push_back(value & 0xff);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  // 合成 ABR v6/subversion 1，包含未压缩与 PackBits 压缩笔尖。
  std::vector<unsigned char> brush(47, 0);
  be32(brush, 0); // top
  be32(brush, 0); // left
  be32(brush, 2); // bottom
  be32(brush, 3); // right
  be16(brush, 8); // depth
  brush.push_back(0); // raw
  const unsigned char expected[] = {0, 32, 255, 64, 128, 224};
  brush.insert(brush.end(), std::begin(expected), std::end(expected));

  std::vector<unsigned char> compressedBrush(47, 0);
  be32(compressedBrush, 0);
  be32(compressedBrush, 0);
  be32(compressedBrush, 2);
  be32(compressedBrush, 4);
  be16(compressedBrush, 8);
  compressedBrush.push_back(1); // PackBits
  be16(compressedBrush, 5);     // 第一行：4 字节 literal
  be16(compressedBrush, 2);     // 第二行：4 个相同字节
  compressedBrush.insert(compressedBrush.end(), {3, 1, 2, 3, 4});
  compressedBrush.insert(compressedBrush.end(), {0xfd, 200});
  const unsigned char compressedExpected[] = {1, 2, 3, 4,
                                               200, 200, 200, 200};

  std::vector<unsigned char> sampleSection;
  be32(sampleSection, static_cast<unsigned>(brush.size()));
  sampleSection.insert(sampleSection.end(), brush.begin(), brush.end());
  while ((sampleSection.size() - 4) % 4)
    sampleSection.push_back(0);
  be32(sampleSection, static_cast<unsigned>(compressedBrush.size()));
  sampleSection.insert(sampleSection.end(), compressedBrush.begin(),
                       compressedBrush.end());
  while ((sampleSection.size() - 4 - ((brush.size() + 3) & ~size_t(3))) % 4)
    sampleSection.push_back(0);

  std::vector<unsigned char> file;
  be16(file, 6);
  be16(file, 1);
  const char tag[] = "8BIMsamp";
  file.insert(file.end(), tag, tag + 8);
  be32(file, static_cast<unsigned>(sampleSection.size()));
  file.insert(file.end(), sampleSection.begin(), sampleSection.end());

  const auto path = argc > 1 ? std::filesystem::path(argv[1])
                             : std::filesystem::temp_directory_path() /
                                   L"测试画笔汇总.abr";
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), file.size());
  }

  const AbrLoadResult result = loadAbrBrushFile(path.wstring());
  if (argc <= 1) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  if (!result.error.empty() || result.version != 6 ||
      result.brushes.size() != 2 || result.brushes[0].width != 3 ||
      result.brushes[0].height != 2 ||
      result.brushes[0].name != u8"测试画笔汇总 #1" ||
      result.brushes[0].mask !=
          std::vector<unsigned char>(std::begin(expected), std::end(expected)) ||
      result.brushes[1].width != 4 || result.brushes[1].height != 2 ||
      result.brushes[1].mask != std::vector<unsigned char>(
                                    std::begin(compressedExpected),
                                    std::end(compressedExpected))) {
    std::cerr << "ABR loader test failed: " << result.error << '\n';
    return 1;
  }
  std::cout << "ABR loader test passed\n";
  return 0;
}
