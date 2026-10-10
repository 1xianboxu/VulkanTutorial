#include "AbrBrushLoader.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
    namespace fs = std::filesystem;

    constexpr int    kMaxDimension   = 16384;
    constexpr size_t kMaxBrushPixels = 128u * 1024u * 1024u;

    class Reader
    {
    public:
        explicit Reader(std::vector<uint8_t> bytes) : data_(std::move(bytes)) {}

        size_t size() const
        {
            return data_.size();
        }
        size_t position() const
        {
            return pos_;
        }
        const std::vector<uint8_t>& data() const
        {
            return data_;
        }

        bool seek(size_t p)
        {
            if (p > data_.size())
                return false;
            pos_ = p;
            return true;
        }
        bool skip(size_t n)
        {
            if (n > data_.size() - pos_)
                return false;
            pos_ += n;
            return true;
        }
        bool u8(uint8_t& v)
        {
            if (pos_ >= data_.size())
                return false;
            v = data_[pos_++];
            return true;
        }
        bool u16(uint16_t& v)
        {
            if (data_.size() - pos_ < 2)
                return false;
            v = (uint16_t(data_[pos_]) << 8) | uint16_t(data_[pos_ + 1]);
            pos_ += 2;
            return true;
        }
        bool s16(int16_t& v)
        {
            uint16_t u = 0;
            if (!u16(u))
                return false;
            v = static_cast<int16_t>(u);
            return true;
        }
        bool u32(uint32_t& v)
        {
            if (data_.size() - pos_ < 4)
                return false;
            v = (uint32_t(data_[pos_]) << 24) | (uint32_t(data_[pos_ + 1]) << 16) | (uint32_t(data_[pos_ + 2]) << 8) |
                uint32_t(data_[pos_ + 3]);
            pos_ += 4;
            return true;
        }
        bool s32(int32_t& v)
        {
            uint32_t u = 0;
            if (!u32(u))
                return false;
            v = static_cast<int32_t>(u);
            return true;
        }
        bool bytes(uint8_t* dst, size_t n)
        {
            if (n > data_.size() - pos_)
                return false;
            std::copy_n(data_.data() + pos_, n, dst);
            pos_ += n;
            return true;
        }

    private:
        std::vector<uint8_t> data_;
        size_t               pos_ = 0;
    };

    void appendUtf8(std::string& out, uint32_t cp)
    {
        if (cp <= 0x7f)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7ff)
        {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else if (cp <= 0xffff)
        {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool readUcs2Text(Reader& r, std::string& text)
    {
        uint32_t count = 0;
        if (!r.u32(count) || count > 1024 * 1024 || size_t(count) * 2 > r.size() - r.position())
            return false;
        text.clear();
        for (uint32_t i = 0; i < count; ++i)
        {
            uint16_t ch = 0;
            if (!r.u16(ch))
                return false;
            if (ch == 0)
                continue;
            if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < count)
            {
                uint16_t lo = 0;
                if (!r.u16(lo))
                    return false;
                ++i;
                if (lo >= 0xdc00 && lo <= 0xdfff)
                {
                    appendUtf8(text, 0x10000u + ((uint32_t(ch - 0xd800) << 10) | uint32_t(lo - 0xdc00)));
                    continue;
                }
                appendUtf8(text, ch);
                appendUtf8(text, lo);
                continue;
            }
            appendUtf8(text, ch);
        }
        return true;
    }

    bool validDimensions(int32_t width, int32_t height, int bytesPerPixel)
    {
        if (width < 1 || height < 1 || width > kMaxDimension || height > kMaxDimension || bytesPerPixel < 1 ||
            bytesPerPixel > 2)
            return false;
        const size_t pixels = size_t(width) * size_t(height);
        return pixels <= kMaxBrushPixels && pixels <= std::numeric_limits<size_t>::max() / size_t(bytesPerPixel);
    }

    // ABR 使用 Photoshop PackBits：每行前有压缩字节数，随后分别解码每行。
    bool decodePackBits(Reader& r, int rowBytes, int rows, bool wide, std::vector<uint8_t>& decoded)
    {
        if (rowBytes <= 0 || rows <= 0 || size_t(rowBytes) * size_t(rows) > kMaxBrushPixels * 2)
            return false;
        std::vector<uint32_t> lengths(static_cast<size_t>(rows), 0);
        for (int y = 0; y < rows; ++y)
        {
            if (wide)
            {
                if (!r.u32(lengths[size_t(y)]))
                    return false;
            }
            else
            {
                uint16_t length = 0;
                if (!r.u16(length))
                    return false;
                lengths[size_t(y)] = length;
            }
        }

        decoded.assign(size_t(rowBytes) * size_t(rows), 0);
        for (int y = 0; y < rows; ++y)
        {
            const size_t rowEnd = r.position() + lengths[size_t(y)];
            if (rowEnd < r.position() || rowEnd > r.size())
                return false;
            size_t   written = 0;
            uint8_t* dst     = decoded.data() + size_t(y) * size_t(rowBytes);
            while (r.position() < rowEnd && written < size_t(rowBytes))
            {
                uint8_t controlByte = 0;
                if (!r.u8(controlByte))
                    return false;
                const int control = static_cast<int8_t>(controlByte);
                if (control >= 0)
                {
                    const size_t count = size_t(control) + 1;
                    if (count > size_t(rowBytes) - written || count > rowEnd - r.position() ||
                        !r.bytes(dst + written, count))
                        return false;
                    written += count;
                }
                else if (control != -128)
                {
                    const size_t count = size_t(1 - control);
                    uint8_t      value = 0;
                    if (count > size_t(rowBytes) - written || r.position() >= rowEnd || !r.u8(value))
                        return false;
                    std::fill_n(dst + written, count, value);
                    written += count;
                }
            }
            if (written != size_t(rowBytes) || !r.seek(rowEnd))
                return false;
        }
        return true;
    }

    bool toMask8(const std::vector<uint8_t>& raw, int width, int height, int depthBytes, std::vector<uint8_t>& mask)
    {
        const size_t pixels = size_t(width) * size_t(height);
        if (depthBytes == 1 && raw.size() == pixels)
        {
            mask = raw;
            return true;
        }
        if (depthBytes == 2 && raw.size() == pixels * 2)
        {
            mask.resize(pixels);
            // Photoshop 的 16 位 ABR 采样数据按低字节在前保存。
            for (size_t i = 0; i < pixels; ++i)
            {
                const uint16_t value = uint16_t(raw[i * 2]) | (uint16_t(raw[i * 2 + 1]) << 8);
                mask[i]              = static_cast<uint8_t>((uint32_t(value) * 255u + 32767u) / 65535u);
            }
            return true;
        }
        return false;
    }

    std::string fallbackName(const fs::path& path, size_t index)
    {
        const std::wstring wideStem = path.stem().wstring();
        std::string        stem;
        for (size_t i = 0; i < wideStem.size(); ++i)
        {
            uint32_t cp = static_cast<uint32_t>(wideStem[i]);
#if WCHAR_MAX <= 0xffff
            if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < wideStem.size())
            {
                const uint32_t lo = static_cast<uint32_t>(wideStem[i + 1]);
                if (lo >= 0xdc00 && lo <= 0xdfff)
                {
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                    ++i;
                }
            }
#endif
            appendUtf8(stem, cp);
        }
        if (stem.empty())
            stem = "ABR";
        std::ostringstream name;
        name << stem << " #" << (index + 1);
        return name.str();
    }

    bool
    readMaskPayload(Reader& r, int width, int height, int depthBytes, uint8_t compression, std::vector<uint8_t>& mask)
    {
        std::vector<uint8_t> raw;
        const size_t         rawSize = size_t(width) * size_t(height) * size_t(depthBytes);
        if (compression == 0)
        {
            raw.resize(rawSize);
            if (!r.bytes(raw.data(), raw.size()))
                return false;
        }
        else if (compression == 1)
        {
            if (!decodePackBits(r, width * depthBytes, height, height > 16384, raw))
                return false;
        }
        else
        {
            return false;
        }
        return toMask8(raw, width, height, depthBytes, mask);
    }

    bool loadLegacy(Reader& r, const fs::path& path, int version, int count, AbrLoadResult& result)
    {
        if (count < 0 || count > 65535)
        {
            result.error = "ABR 画笔数量异常";
            return false;
        }
        for (int index = 0; index < count; ++index)
        {
            uint16_t type      = 0;
            uint32_t blockSize = 0;
            if (!r.u16(type) || !r.u32(blockSize))
            {
                result.error = "ABR 文件被截断";
                return false;
            }
            const size_t payloadStart = r.position();
            const size_t next         = payloadStart + blockSize;
            if (next < payloadStart || next > r.size())
            {
                result.error = "ABR 笔尖块长度损坏";
                return false;
            }
            if (type != 2)
            {
                r.seek(next); // 计算型笔尖暂不含可直接盖印的位图，安全跳过。
                continue;
            }

            uint32_t    misc           = 0;
            int16_t     spacingPercent = 25;
            std::string name;
            uint8_t     antialias = 0;
            if (!r.u32(misc) || !r.s16(spacingPercent) || (version == 2 && !readUcs2Text(r, name)) || !r.u8(antialias))
            {
                result.error = "ABR 旧版笔尖头损坏";
                return false;
            }
            for (int i = 0; i < 4; ++i)
            {
                int16_t ignored = 0;
                if (!r.s16(ignored))
                {
                    result.error = "ABR 旧版边界损坏";
                    return false;
                }
            }
            int32_t bounds[4] {};
            for (int& bound : bounds)
            {
                if (!r.s32(bound))
                {
                    result.error = "ABR 旧版边界损坏";
                    return false;
                }
            }
            int16_t depthBits   = 0;
            uint8_t compression = 0;
            if (!r.s16(depthBits) || !r.u8(compression))
            {
                result.error = "ABR 旧版像素头损坏";
                return false;
            }
            const int width      = bounds[3] - bounds[1];
            const int height     = bounds[2] - bounds[0];
            const int depthBytes = depthBits / 8;
            if (!validDimensions(width, height, depthBytes))
            {
                result.error = "ABR 笔尖尺寸或位深不受支持";
                return false;
            }

            AbrBrushTip tip;
            tip.name    = name.empty() ? fallbackName(path, result.brushes.size()) : name;
            tip.width   = width;
            tip.height  = height;
            tip.spacing = std::clamp(float(spacingPercent) / 100.0f, 0.03f, 1.0f);
            if (!readMaskPayload(r, width, height, depthBytes, compression, tip.mask))
            {
                result.error = "ABR 笔尖像素解码失败";
                return false;
            }
            result.brushes.push_back(std::move(tip));
            if (!r.seek(next))
            {
                result.error = "ABR 笔尖块结尾损坏";
                return false;
            }
        }
        return true;
    }

    bool loadModern(Reader& r, const fs::path& path, int subVersion, AbrLoadResult& result)
    {
        const auto&   bytes       = r.data();
        const uint8_t signature[] = {'8', 'B', 'I', 'M', 's', 'a', 'm', 'p'};
        auto          found       = std::search(
            bytes.begin() + std::min<size_t>(4, bytes.size()), bytes.end(), std::begin(signature), std::end(signature));
        if (found == bytes.end())
        {
            result.error = "ABR 中没有可用的 samp 笔尖区";
            return false;
        }
        if (!r.seek(size_t(found - bytes.begin()) + sizeof(signature)))
            return false;
        uint32_t sectionSize = 0;
        if (!r.u32(sectionSize) || sectionSize > r.size() - r.position())
        {
            result.error = "ABR samp 区长度损坏";
            return false;
        }
        const size_t sectionEnd = r.position() + sectionSize;
        while (r.position() + 4 <= sectionEnd)
        {
            uint32_t brushSize = 0;
            if (!r.u32(brushSize))
            {
                result.error = "ABR 笔尖记录被截断";
                return false;
            }
            if (brushSize == 0)
                break;
            const size_t payloadStart = r.position();
            const size_t paddedSize   = (size_t(brushSize) + 3u) & ~size_t(3u);
            const size_t next         = payloadStart + paddedSize;
            if (next < payloadStart || next > sectionEnd || next > r.size())
            {
                result.error = "ABR 笔尖记录长度损坏";
                return false;
            }
            const size_t unknownHeader = subVersion == 1 ? 47u : 301u;
            if (!r.skip(unknownHeader))
            {
                result.error = "ABR 笔尖头被截断";
                return false;
            }

            int32_t top = 0, left = 0, bottom = 0, right = 0;
            int16_t depthBits   = 0;
            uint8_t compression = 0;
            if (!r.s32(top) || !r.s32(left) || !r.s32(bottom) || !r.s32(right) || !r.s16(depthBits) ||
                !r.u8(compression))
            {
                result.error = "ABR 笔尖像素头损坏";
                return false;
            }
            const int width      = right - left;
            const int height     = bottom - top;
            const int depthBytes = depthBits / 8;
            if (!validDimensions(width, height, depthBytes))
            {
                result.error = "ABR 笔尖尺寸或位深不受支持";
                return false;
            }

            AbrBrushTip tip;
            tip.name    = fallbackName(path, result.brushes.size());
            tip.width   = width;
            tip.height  = height;
            tip.spacing = 0.25f; // 新版间距在 desc 描述区；无值时沿用 Photoshop 常用 25%。
            if (!readMaskPayload(r, width, height, depthBytes, compression, tip.mask))
            {
                result.error = "ABR 笔尖像素解码失败";
                return false;
            }
            result.brushes.push_back(std::move(tip));
            if (!r.seek(next))
            {
                result.error = "ABR 笔尖记录结尾损坏";
                return false;
            }
        }
        return true;
    }
} // namespace

AbrLoadResult loadAbrBrushFile(const std::wstring& pathText)
{
    AbrLoadResult  result;
    const fs::path path(pathText);
    std::ifstream  input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        result.error = "无法打开 ABR 文件";
        return result;
    }
    const std::streamoff length = input.tellg();
    if (length < 4 || uint64_t(length) > 1024ull * 1024ull * 1024ull)
    {
        result.error = "ABR 文件大小异常";
        return result;
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length))
    {
        result.error = "读取 ABR 文件失败";
        return result;
    }

    Reader   reader(std::move(bytes));
    uint16_t version = 0, countOrSubVersion = 0;
    if (!reader.u16(version) || !reader.u16(countOrSubVersion))
    {
        result.error = "ABR 文件头损坏";
        return result;
    }
    result.version = version;
    bool ok        = false;
    if (version == 1 || version == 2)
    {
        ok = loadLegacy(reader, path, version, countOrSubVersion, result);
    }
    else if (version == 6 || version == 10)
    {
        if (countOrSubVersion != 1 && countOrSubVersion != 2)
        {
            result.error = "不支持该 ABR 子版本";
            return result;
        }
        ok = loadModern(reader, path, countOrSubVersion, result);
    }
    else
    {
        result.error = "不支持的 ABR 版本：" + std::to_string(version);
        return result;
    }

    if (ok && result.brushes.empty())
        result.error = "ABR 中没有可导入的采样笔尖";
    return result;
}
