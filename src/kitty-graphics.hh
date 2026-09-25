/*
 * Copyright © 2026 Vmux contributors
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Protocol-level support for the kitty terminal graphics protocol,
 * https://sw.kovidgoyal.net/kitty/graphics-protocol/
 *
 * This file contains everything that does not need the terminal:
 * control data parsing, payload decoding (base64, zlib, RGB/RGBA/PNG),
 * the transmission media (direct, file, temporary file, shared memory)
 * and the image store with its quota.
 *
 * Placement of images on the grid lives in the terminal (vte-kitty.cc).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cairo-glue.hh"

namespace vte {

namespace kitty {

/* Limits */
inline constexpr size_t k_max_image_dimension = 10000;
inline constexpr size_t k_default_quota = size_t{320} * 1024 * 1024;
inline constexpr size_t k_max_file_size = size_t{320} * 1024 * 1024;

/* Parsed control data of one graphics command. Unset keys have their
 * protocol default value; the has_* flags tell whether a key was given.
 */
struct Command {
        char action{'t'};            // a
        int quiet{0};                // q
        int format{32};              // f
        char medium{'d'};            // t
        uint32_t width{0};           // s
        uint32_t height{0};          // v
        uint32_t data_size{0};       // S
        uint32_t data_offset{0};     // O
        uint32_t image_id{0};        // i
        uint32_t image_number{0};    // I
        uint32_t placement_id{0};    // p
        char compression{0};         // o
        int more{0};                 // m
        uint32_t hints{0};           // N
        uint32_t src_x{0};           // x
        uint32_t src_y{0};           // y
        uint32_t src_width{0};       // w
        uint32_t src_height{0};      // h
        uint32_t cell_x_offset{0};   // X
        uint32_t cell_y_offset{0};   // Y
        uint32_t columns{0};         // c
        uint32_t rows{0};            // r
        int cursor_policy{0};        // C
        int unicode_placeholder{0};  // U
        int32_t z{0};                // z
        uint32_t parent_id{0};       // P
        uint32_t parent_placement{0};// Q
        int32_t h_offset{0};         // H
        int32_t v_offset{0};         // V
        char delete_what{'a'};       // d

        bool has_image_id{false};
        bool has_image_number{false};
        bool has_placement_id{false};
        bool has_z{false};
        bool has_x{false};
        bool has_y{false};

        /* Parses the comma-separated key=value control data. On failure,
         * returns false and sets @error to a protocol error message
         * (e.g. "EINVAL:..."). */
        bool parse(std::string_view control,
                   std::string& error) noexcept;

        /* Whether a response should be sent for this command */
        bool wants_response(bool ok) const noexcept
        {
                if (!has_image_id && !has_image_number)
                        return false;
                if (quiet >= 2)
                        return false;
                if (quiet == 1 && ok)
                        return false;
                return true;
        }
};

/* Decoded pixel data */
struct Bitmap {
        vte::Freeable<cairo_surface_t> surface{};
        int width{0};
        int height{0};

        size_t bytes() const noexcept
        {
                return surface ? size_t(cairo_image_surface_get_stride(surface.get())) * size_t(height) : 0;
        }
};

/* Decodes base64. Returns false on malformed input. */
bool base64_decode(std::string_view input,
                   std::string& output) noexcept;

/* Loads the raw (still compressed, if o=z) image data for @cmd.
 * @payload is the base64-decoded payload: for the direct medium it is
 * the image data itself, otherwise it is the path / shm name.
 * On failure returns false and sets @error to a protocol error message.
 */
bool load_data(Command const& cmd,
               std::string&& payload,
               std::string& data,
               std::string& error) noexcept;

/* Decompresses (if needed) and decodes @data into a bitmap.
 * On failure returns false and sets @error to a protocol error message.
 */
bool decode_image(Command const& cmd,
                  std::string const& data,
                  Bitmap& bitmap,
                  std::string& error) noexcept;

/* Decodes an image in any format gdk-pixbuf understands (used for iTerm2
 * inline images). On failure returns false and sets @error. */
bool decode_pixbuf_image(std::string const& data,
                         Bitmap& bitmap,
                         std::string& error) noexcept;

/* Converts tightly packed or strided 8-bit RGB(A) pixels (non-premultiplied,
 * sRGB) into a CAIRO_FORMAT_ARGB32 surface. */
vte::Freeable<cairo_surface_t> surface_from_rgba(uint8_t const* pixels,
                                                 int width,
                                                 int height,
                                                 int channels,
                                                 size_t rowstride) noexcept;

/* An image stored by the terminal, referenced by placements */
class Image {
public:
        Image(uint32_t id,
              uint32_t number,
              Bitmap&& bitmap,
              uint64_t sequence,
              bool transient) noexcept
                : m_id{id},
                  m_number{number},
                  m_bitmap{std::move(bitmap)},
                  m_sequence{sequence},
                  m_transient{transient}
        {
        }

        Image(Image const&) = delete;
        Image(Image&&) = delete;
        Image& operator=(Image const&) = delete;
        Image& operator=(Image&&) = delete;

        auto id() const noexcept { return m_id; }
        auto number() const noexcept { return m_number; }
        auto width() const noexcept { return m_bitmap.width; }
        auto height() const noexcept { return m_bitmap.height; }
        auto surface() const noexcept { return m_bitmap.surface.get(); }
        auto bytes() const noexcept { return m_bitmap.bytes(); }
        auto sequence() const noexcept { return m_sequence; }
        auto transient() const noexcept { return m_transient; }

private:
        uint32_t m_id;
        uint32_t m_number;
        Bitmap m_bitmap;
        uint64_t m_sequence;
        bool m_transient;
};

/* In-progress chunked transmission */
struct Upload {
        Command command{};
        std::string data{};
        bool active{false};

        void reset() noexcept
        {
                command = Command{};
                data.clear();
                data.shrink_to_fit();
                active = false;
        }
};

/* The image store: images by id, the chunked upload in progress, and
 * the storage quota. */
class Store {
public:
        Store() = default;
        ~Store() = default;

        Store(Store const&) = delete;
        Store(Store&&) = delete;
        Store& operator=(Store const&) = delete;
        Store& operator=(Store&&) = delete;

        Image* find(uint32_t id) const noexcept;

        /* The newest image with the given number */
        Image* find_by_number(uint32_t number) const noexcept;

        /* Adds an image. If an image with the same id exists, it is
         * replaced (the caller must have removed its placements). */
        Image* add(uint32_t id,
                   uint32_t number,
                   Bitmap&& bitmap,
                   bool transient);

        void remove(uint32_t id) noexcept;
        void clear() noexcept;

        auto const& images() const noexcept { return m_images; }
        auto bytes_used() const noexcept { return m_bytes_used; }
        auto quota() const noexcept { return m_quota; }

        /* Allocates an id for an image created with an image number */
        uint32_t allocate_id() noexcept;

        auto& upload() noexcept { return m_upload; }

        /* Fills @evicted with the ids of the images, that should be
         * evicted to make room for @bytes more: unreferenced images first
         * (transient ones before the others), then the oldest images.
         * @has_placements tells whether an image is referenced by
         * placements. Returns false if @bytes can never fit. */
        bool evict_for(size_t bytes,
                       std::function<bool(uint32_t)> const& has_placements,
                       std::vector<uint32_t>& evicted) const noexcept;

private:
        /* Images by id. Ordered by id, so creation order is tracked with a
         * sequence number instead. */
        std::map<uint32_t, std::unique_ptr<Image>> m_images{};
        size_t m_bytes_used{0};
        size_t m_quota{k_default_quota};
        uint64_t m_next_sequence{0};
        uint32_t m_next_auto_id{0x40000000u};
        Upload m_upload{};
};

} // namespace kitty

} // namespace vte
