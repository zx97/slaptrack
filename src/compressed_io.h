// compressed_io.h

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

#pragma once

#include <string>

enum class CompressionType {
    NONE,
    GZIP,
    BZIP2,
    XZ
};

const char* compressionTypeName(CompressionType type);

// Detect compression by reading the first few magic bytes of the file.
// Magic bytes are preferred over filename extension because rotated logs
// may have non-standard names (e.g. slapd.log.07.bz2_2026-03-01).
CompressionType detectCompression(const std::string& path);

// Decompress srcPath into dstPath using the given algorithm.
// Returns true on success, false on any I/O or format error.
// On failure, dstPath is left empty / partially written and the caller
// is expected to delete it.
bool decompressToFile(const std::string& srcPath,
                      const std::string& dstPath,
                      CompressionType type);

// Create a unique temporary file path under /tmp using mkstemp semantics.
// Returns the path on success, empty string on failure.
// The caller is responsible for unlinking the file.
std::string createTempPath();

// Convenience: detect + decompress in one call.  Returns empty string on
// any failure (no temp file left behind).  On success, returns the path
// of the temp file which the caller must delete.
std::string decompressToTempIfCompressed(const std::string& srcPath,
                                         CompressionType* detectedType);
