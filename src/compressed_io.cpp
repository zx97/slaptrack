// compressed_io.cpp

/* 
    SPDX-License-Identifier: AGPL-3.0-or-later
    GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
    Copyright (c) 2026 Manuel FLURY
    All rights reserved.
    
    This file is part of slaptrack - an OpenLDAP Log Viewer.
    
    Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later).
    See the LICENSE file distributed with this work for full license text.
    
    THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
    AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "compressed_io.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>
#include <bzlib.h>
#include <lzma.h>
#include <unistd.h>

namespace {

constexpr size_t kIoBufferSize = 65536;

bool writeAll(FILE* dst, const char* data, size_t len) {
    while (len > 0) {
        size_t written = fwrite(data, 1, len, dst);
        if (written == 0) return false;
        data += written;
        len -= written;
    }
    return true;
}

bool decompressGzip(const std::string& srcPath, FILE* dst) {
    gzFile file = gzopen(srcPath.c_str(), "rb");
    if (!file) return false;
    std::vector<char> buffer(kIoBufferSize);
    int bytes = 0;
    bool ok = true;
    while ((bytes = gzread(file, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
        if (!writeAll(dst, buffer.data(), static_cast<size_t>(bytes))) {
            ok = false;
            break;
        }
    }
    if (bytes < 0) ok = false;
    gzclose(file);
    return ok;
}

bool decompressBzip2(const std::string& srcPath, FILE* dst) {
    FILE* file = fopen(srcPath.c_str(), "rb");
    if (!file) return false;
    int bzerror = BZ_OK;
    BZFILE* bzfile = BZ2_bzReadOpen(&bzerror, file, 0, 0, nullptr, 0);
    if (bzerror != BZ_OK) { fclose(file); return false; }
    std::vector<char> buffer(kIoBufferSize);
    bool ok = true;
    bool streamEnd = false;
    while (!streamEnd) {
        int bytes = BZ2_bzRead(&bzerror, bzfile, buffer.data(),
                               static_cast<int>(buffer.size()));
        if (bytes > 0) {
            if (!writeAll(dst, buffer.data(), static_cast<size_t>(bytes))) {
                ok = false;
                break;
            }
        }
        if (bzerror == BZ_STREAM_END) {
            streamEnd = true;
        } else if (bzerror != BZ_OK && bzerror != BZ_RUN_OK) {
            ok = false;
            break;
        }
        if (bytes <= 0 && !streamEnd) break;
    }
    BZ2_bzReadClose(&bzerror, bzfile);
    fclose(file);
    return ok;
}

bool decompressXz(const std::string& srcPath, FILE* dst) {
    FILE* file = fopen(srcPath.c_str(), "rb");
    if (!file) return false;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret initRet = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    if (initRet != LZMA_OK) { fclose(file); return false; }
    std::vector<char> inbuf(kIoBufferSize);
    std::vector<char> outbuf(kIoBufferSize);
    strm.next_in = nullptr;
    strm.avail_in = 0;
    strm.next_out = reinterpret_cast<uint8_t*>(outbuf.data());
    strm.avail_out = outbuf.size();
    lzma_action action = LZMA_RUN;
    bool ok = true;
    while (true) {
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            strm.next_in = reinterpret_cast<const uint8_t*>(inbuf.data());
            strm.avail_in = fread(inbuf.data(), 1, inbuf.size(), file);
            if (strm.avail_in == 0) action = LZMA_FINISH;
        }
        lzma_ret ret = lzma_code(&strm, action);
        if (strm.avail_out == 0 || ret == LZMA_STREAM_END) {
            size_t writeSize = outbuf.size() - strm.avail_out;
            if (writeSize > 0 && !writeAll(dst, outbuf.data(), writeSize)) {
                ok = false;
                break;
            }
            strm.next_out = reinterpret_cast<uint8_t*>(outbuf.data());
            strm.avail_out = outbuf.size();
        }
        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK && ret != LZMA_BUF_ERROR) {
            ok = false;
            break;
        }
    }
    lzma_end(&strm);
    fclose(file);
    return ok;
}

} // namespace

const char* compressionTypeName(CompressionType type) {
    switch (type) {
        case CompressionType::GZIP:  return "gzip";
        case CompressionType::BZIP2: return "bzip2";
        case CompressionType::XZ:    return "xz";
        case CompressionType::NONE:
        default:                     return "none";
    }
}

CompressionType detectCompression(const std::string& path) {
    unsigned char peek[6] = {};
    std::ifstream probe(path, std::ios::binary);
    if (!probe) return CompressionType::NONE;
    probe.read(reinterpret_cast<char*>(peek), sizeof(peek));
    std::streamsize got = probe.gcount();
    probe.close();
    if (got < 2) return CompressionType::NONE;

    if (peek[0] == 0x1f && peek[1] == 0x8b) {
        return CompressionType::GZIP;
    }
    if (got >= 4 &&
        peek[0] == 'B' && peek[1] == 'Z' && peek[2] == 'h' &&
        peek[3] >= '1' && peek[3] <= '9') {
        return CompressionType::BZIP2;
    }
    if (got >= 6 &&
        peek[0] == 0xfd && peek[1] == '7' && peek[2] == 'z' &&
        peek[3] == 'X' && peek[4] == 'Z' && peek[5] == 0x00) {
        return CompressionType::XZ;
    }
    return CompressionType::NONE;
}

bool decompressToFile(const std::string& srcPath,
                      const std::string& dstPath,
                      CompressionType type) {
    FILE* dst = fopen(dstPath.c_str(), "wb");
    if (!dst) return false;
    bool ok = false;
    switch (type) {
        case CompressionType::GZIP:  ok = decompressGzip(srcPath, dst);  break;
        case CompressionType::BZIP2: ok = decompressBzip2(srcPath, dst); break;
        case CompressionType::XZ:    ok = decompressXz(srcPath, dst);    break;
        case CompressionType::NONE:
        default:
            fclose(dst);
            return false;
    }
    if (fclose(dst) != 0) ok = false;
    return ok;
}

std::string createTempPath() {
    char tmpl[] = "/tmp/slaptrack.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd == -1) return std::string();
    close(fd);
    return std::string(tmpl);
}

std::string decompressToTempIfCompressed(const std::string& srcPath,
                                         CompressionType* detectedType) {
    CompressionType type = detectCompression(srcPath);
    if (detectedType) *detectedType = type;
    if (type == CompressionType::NONE) return std::string();

    std::string tmpPath = createTempPath();
    if (tmpPath.empty()) return std::string();

    if (!decompressToFile(srcPath, tmpPath, type)) {
        unlink(tmpPath.c_str());
        return std::string();
    }
    return tmpPath;
}
