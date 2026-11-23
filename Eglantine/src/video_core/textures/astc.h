// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tegra::Texture::ASTC {

void Decompress(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                uint32_t block_width, uint32_t block_height, std::span<uint8_t> output);

/// What a scan of ASTC blocks found: every block decodes to alpha 1 (Opaque), a block has an
/// alpha endpoint mode (Alpha), or a block is an error/reserved encoding (Unknown). Real encoders
/// never write error blocks; zeros (data not streamed in yet) and stale bytes do.
enum class Opacity { Opaque, Alpha, Unknown };
Opacity ScanOpacity(std::span<const uint8_t> data);

/// True if no 16-byte block in `data` decodes to alpha below 1 and none is an error block.
bool IsOpaque(std::span<const uint8_t> data);

} // namespace Tegra::Texture::ASTC
