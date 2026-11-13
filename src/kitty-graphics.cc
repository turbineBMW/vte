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

#include "config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <iterator>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "kitty-graphics.hh"
#include "debug.hh"
#include "glib-glue.hh"
#include "libc-glue.hh"
#include "refptr.hh"

namespace vte {

namespace kitty {

/* Control data parsing ***************************************************/

namespace {

template<typename T>
bool
parse_int(std::string_view value,
          T& out) noexcept
{
        if (value.empty())
                return false;

        auto const begin = value.data();
        auto const end = begin + value.size();
        auto v = T{};
        auto const [ptr, ec] = std::from_chars(begin, end, v, 10);
        if (ec != std::errc{} || ptr != end)
                return false;

        out = v;
        return true;
}

bool
parse_char(std::string_view value,
           char& out) noexcept
{
        if (value.size() != 1)
                return false;
        out = value[0];
        return true;
}

} // anonymous namespace

bool
Command::parse(std::string_view control,
               std::string& error) noexcept
{
        *this = Command{};

        auto fail = [&](char const* msg) noexcept {
                error = msg;
                return false;
        };

        while (!control.empty()) {
                auto const comma = control.find(',');
                auto const item = control.substr(0, comma);
                control = (comma == std::string_view::npos) ? std::string_view{} : control.substr(comma + 1);

                if (item.empty())
                        continue; // tolerate stray commas

                auto const eq = item.find('=');
                if (eq != 1)
                        return fail("EINVAL:Malformed control data");

                auto const key = item[0];
                auto const value = item.substr(2);

                auto ok = true;
                switch (key) {
                case 'a': ok = parse_char(value, action); break;
                case 'q': ok = parse_int(value, quiet); break;
                case 'f': ok = parse_int(value, format); break;
                case 't': ok = parse_char(value, medium); break;
                case 's': ok = parse_int(value, width); break;
                case 'v': ok = parse_int(value, height); break;
                case 'S': ok = parse_int(value, data_size); break;
                case 'O': ok = parse_int(value, data_offset); break;
                case 'i': ok = parse_int(value, image_id); has_image_id = true; break;
                case 'I': ok = parse_int(value, image_number); has_image_number = true; break;
                case 'p': ok = parse_int(value, placement_id); has_placement_id = true; break;
                case 'o': ok = parse_char(value, compression); break;
                case 'm': ok = parse_int(value, more); break;
                case 'N': ok = parse_int(value, hints); break;
                case 'x': ok = parse_int(value, src_x); has_x = true; break;
                case 'y': ok = parse_int(value, src_y); has_y = true; break;
                case 'w': ok = parse_int(value, src_width); break;
                case 'h': ok = parse_int(value, src_height); break;
                case 'X': ok = parse_int(value, cell_x_offset); break;
                case 'Y': ok = parse_int(value, cell_y_offset); break;
                case 'c': ok = parse_int(value, columns); break;
                case 'r': ok = parse_int(value, rows); break;
                case 'C': ok = parse_int(value, cursor_policy); break;
                case 'U': ok = parse_int(value, unicode_placeholder); break;
                case 'z': ok = parse_int(value, z); has_z = true; break;
                case 'P': ok = parse_int(value, parent_id); break;
                case 'Q': ok = parse_int(value, parent_placement); break;
                case 'H': ok = parse_int(value, h_offset); break;
                case 'V': ok = parse_int(value, v_offset); break;
                case 'd': ok = parse_char(value, delete_what); break;
                default:
                        /* Unknown keys are ignored for forward compatibility */
                        break;
                }

                if (!ok)
                        return fail("EINVAL:Malformed control data value");
        }

        /* Semantic checks */
        if (has_image_id && has_image_number)
                return fail("EINVAL:Cannot specify both image id and image number");

        switch (action) {
        case 'a': case 'c': case 'd': case 'f': case 'p': case 'q': case 't': case 'T':
                break;
        default:
                return fail("EINVAL:Unknown action");
        }

        switch (format) {
        case 24: case 32: case 100:
                break;
        default:
                return fail("EINVAL:Unknown image format");
        }

        switch (medium) {
        case 'd': case 'f': case 't': case 's':
                break;
        default:
                return fail("EINVAL:Unknown transmission medium");
        }

        if (compression != 0 && compression != 'z')
                return fail("EINVAL:Unknown compression");

        if (more != 0 && more != 1)
                return fail("EINVAL:Invalid value for m key");

        if (has_image_id && image_id == 0)
                has_image_id = false;
        if (has_image_number && image_number == 0)
                has_image_number = false;

        return true;
}

/* Base64 ****************************************************************/

bool
base64_decode(std::string_view input,
              std::string& output) noexcept
{
        static constexpr auto k_invalid = uint8_t{0xff};
        static constexpr auto table = []() constexpr {
                std::array<uint8_t, 256> t{};
                t.fill(k_invalid);
                for (auto i = 0; i < 26; ++i) {
                        t['A' + i] = i;
                        t['a' + i] = 26 + i;
                }
                for (auto i = 0; i < 10; ++i)
                        t['0' + i] = 52 + i;
                t['+'] = 62;
                t['/'] = 63;
                /* Also accept the URL-safe alphabet */
                t['-'] = 62;
                t['_'] = 63;
                return t;
        }();

        output.clear();
        output.reserve(input.size() / 4 * 3 + 3);

        auto acc = uint32_t{0};
        auto bits = 0;
        auto padding = 0;
        for (auto const ch : input) {
                auto const c = uint8_t(ch);
                if (c == '=') {
                        ++padding;
                        continue;
                }
                if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
                        continue; // be lenient about whitespace
                if (padding)
                        return false; // data after padding

                auto const v = table[c];
                if (v == k_invalid)
                        return false;

                acc = (acc << 6) | v;
                bits += 6;
                if (bits >= 8) {
                        bits -= 8;
                        output.push_back(char((acc >> bits) & 0xff));
                }
        }

        /* Leftover bits must be zero padding of less than one byte */
        return bits < 8;
}

/* Transmission media *****************************************************/

namespace {

constexpr auto k_read_error = "EBADF:Failed to read image file";

bool
read_fd(int fd,
        size_t offset,
        size_t size, /* 0 = to end of file */
        std::string& data) noexcept
{
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
                return false;

        auto const file_size = size_t(st.st_size);
        if (offset > file_size)
                return false;

        auto const available = file_size - offset;
        auto const to_read = size == 0 ? available : std::min(size, available);
        if (size != 0 && available < size)
                return false; // smaller than the client claimed
        if (to_read > k_max_file_size)
                return false;

        try {
                data.resize(to_read);
        } catch (...) {
                return false;
        }

        auto done = size_t{0};
        while (done < to_read) {
                auto const n = pread(fd, data.data() + done, to_read - done, off_t(offset + done));
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return false;
                }
                if (n == 0)
                        return false; // truncated
                done += size_t(n);
        }

        return true;
}

bool
path_is_sensitive(std::string const& path) noexcept
{
        static constexpr std::string_view k_sensitive[] = {
                "/proc/", "/sys/", "/dev/",
        };

        for (auto const prefix : k_sensitive) {
                if (path.compare(0, prefix.size(), prefix) == 0) {
                        /* /dev/shm is explicitly allowed as a temporary directory */
                        if (prefix == "/dev/" && path.compare(0, 9, "/dev/shm/") == 0)
                                return false;
                        return true;
                }
        }
        return false;
}

bool
path_is_in_temp_dir(std::string const& path) noexcept
{
        auto in_dir = [&](char const* dir) noexcept {
                if (!dir || !*dir)
                        return false;
                auto const len = strlen(dir);
                return path.size() > len + 1 &&
                        path.compare(0, len, dir) == 0 &&
                        path[len] == '/';
        };

        return in_dir("/tmp") ||
                in_dir("/var/tmp") ||
                in_dir("/dev/shm") ||
                in_dir(g_get_tmp_dir()) ||
                in_dir(g_getenv("TMPDIR"));
}

bool
load_file(Command const& cmd,
          std::string const& path_arg,
          std::string& data) noexcept
{
        if (path_arg.empty() || path_arg.find('\0') != std::string::npos)
                return false;

        /* Resolve symlinks before deciding whether the location is
         * sensitive, so that a symlink cannot be used to bypass the check. */
        auto resolved = vte::glib::take_free_ptr(realpath(path_arg.c_str(), nullptr));
        if (!resolved)
                return false;
        auto const path = std::string{resolved.get()};

        if (path_is_sensitive(path))
                return false;

        auto fd = vte::libc::FD{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK)};
        if (!fd)
                return false;

        if (!read_fd(fd.get(), cmd.data_offset, cmd.data_size, data))
                return false;

        if (cmd.medium == 't') {
                /* Only delete files that are clearly meant for us */
                if (path.find("tty-graphics-protocol") != std::string::npos &&
                    path_is_in_temp_dir(path))
                        unlink(path.c_str());
        }

        return true;
}

bool
load_shm(Command const& cmd,
         std::string const& name,
         std::string& data) noexcept
{
        if (name.size() < 2 || name[0] != '/' ||
            name.find('/', 1) != std::string::npos ||
            name.find('\0') != std::string::npos ||
            name.size() > 255)
                return false;

        auto fd = vte::libc::FD{shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0)};
        /* Per the protocol, the object is unlinked whether or not the read succeeds */
        shm_unlink(name.c_str());
        if (!fd)
                return false;

        struct stat st;
        if (fstat(fd.get(), &st) != 0)
                return false;

        auto const size = size_t(st.st_size);
        if (cmd.data_offset > size)
                return false;
        auto const available = size - cmd.data_offset;
        auto const to_read = cmd.data_size == 0 ? available : std::min(size_t(cmd.data_size), available);
        if (cmd.data_size != 0 && available < cmd.data_size)
                return false;
        if (to_read > k_max_file_size)
                return false;

        try {
                data.resize(to_read);
        } catch (...) {
                return false;
        }

        auto done = size_t{0};
        while (done < to_read) {
                auto const n = pread(fd.get(), data.data() + done, to_read - done, off_t(cmd.data_offset + done));
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return false;
                }
                if (n == 0)
                        return false;
                done += size_t(n);
        }

        return true;
}

} // anonymous namespace

bool
load_data(Command const& cmd,
          std::string&& payload,
          std::string& data,
          std::string& error) noexcept
{
        switch (cmd.medium) {
        case 'd':
                data = std::move(payload);
                return true;

        case 'f':
        case 't':
                if (!load_file(cmd, payload, data)) {
                        error = k_read_error;
                        return false;
                }
                return true;

        case 's':
                if (!load_shm(cmd, payload, data)) {
                        error = k_read_error;
                        return false;
                }
                return true;

        default:
                error = "EINVAL:Unknown transmission medium";
                return false;
        }
}

/* Decoding ***************************************************************/

namespace {

bool
zlib_inflate(std::string const& input,
             size_t max_output,
             std::string& output) noexcept
try
{
        auto decompressor = vte::glib::take_ref(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB));
        auto converter = G_CONVERTER(decompressor.get());

        output.clear();

        auto in_pos = size_t{0};
        auto buffer = std::string{};
        buffer.resize(64 * 1024);

        while (true) {
                auto bytes_read = gsize{0};
                auto bytes_written = gsize{0};
                auto error = vte::glib::Error{};
                auto const flags = GConverterFlags(G_CONVERTER_INPUT_AT_END);
                auto const result = g_converter_convert(converter,
                                                        input.data() + in_pos,
                                                        input.size() - in_pos,
                                                        buffer.data(),
                                                        buffer.size(),
                                                        flags,
                                                        &bytes_read,
                                                        &bytes_written,
                                                        error);

                in_pos += bytes_read;
                if (bytes_written) {
                        if (output.size() + bytes_written > max_output)
                                return false;
                        output.append(buffer.data(), bytes_written);
                }

                switch (result) {
                case G_CONVERTER_CONVERTED:
                        continue;
                case G_CONVERTER_FINISHED:
                        return true;
                case G_CONVERTER_FLUSHED:
                        continue;
                case G_CONVERTER_ERROR:
                default:
                        return false;
                }
        }
}
catch (...)
{
        return false;
}

bool pixbuf_to_bitmap(GdkPixbuf* pixbuf,
                      Bitmap& bitmap,
                      std::string& error) noexcept;

/* Reads the dimensions from the PNG IHDR chunk, so that the size can be
 * checked before decoding. */
bool
png_dimensions(std::string const& data,
               uint32_t& width,
               uint32_t& height) noexcept
{
        static constexpr uint8_t k_signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

        if (data.size() < 8 + 8 + 13 ||
            memcmp(data.data(), k_signature, sizeof(k_signature)) != 0 ||
            memcmp(data.data() + 12, "IHDR", 4) != 0)
                return false;

        auto const p = reinterpret_cast<uint8_t const*>(data.data()) + 16;
        width = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        height = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | uint32_t(p[7]);
        return true;
}

bool
decode_png(std::string const& data,
           Bitmap& bitmap,
           std::string& error) noexcept
try
{
        auto width = uint32_t{0}, height = uint32_t{0};
        if (!png_dimensions(data, width, height)) {
                error = "EINVAL:Not a PNG image";
                return false;
        }
        if (width == 0 || height == 0 ||
            width > k_max_image_dimension || height > k_max_image_dimension) {
                error = "EINVAL:Image dimensions too large";
                return false;
        }

        auto err = vte::glib::Error{};
        auto loader = vte::glib::take_ref(gdk_pixbuf_loader_new_with_type("png", err));
        if (!loader) {
                error = "EINVAL:PNG decoding unavailable";
                return false;
        }

        auto ok = gdk_pixbuf_loader_write(loader.get(),
                                          reinterpret_cast<guchar const*>(data.data()),
                                          data.size(),
                                          err) != FALSE;
        if (ok)
                ok = gdk_pixbuf_loader_close(loader.get(), err) != FALSE;
        else
                gdk_pixbuf_loader_close(loader.get(), nullptr);
        if (!ok) {
                error = "EINVAL:Failed to decode PNG";
                return false;
        }

        auto const pixbuf = gdk_pixbuf_loader_get_pixbuf(loader.get()); // unowned
        if (!pixbuf) {
                error = "EINVAL:Failed to decode PNG";
                return false;
        }

        return pixbuf_to_bitmap(pixbuf, bitmap, error);
}
catch (...)
{
        error = "ENOMEM:Failed to decode PNG";
        return false;
}

/* Rejects oversized images before gdk-pixbuf allocates them */
void
pixbuf_size_prepared(GdkPixbufLoader* loader,
                     int width,
                     int height,
                     void* user_data) noexcept
{
        auto const too_big = static_cast<bool*>(user_data);
        if (width <= 0 || height <= 0 ||
            size_t(width) > k_max_image_dimension || size_t(height) > k_max_image_dimension) {
                *too_big = true;
                gdk_pixbuf_loader_set_size(loader, 1, 1);
        }
}

bool
pixbuf_to_bitmap(GdkPixbuf* pixbuf,
                 Bitmap& bitmap,
                 std::string& error) noexcept
{
        auto const pw = gdk_pixbuf_get_width(pixbuf);
        auto const ph = gdk_pixbuf_get_height(pixbuf);
        auto const channels = gdk_pixbuf_get_n_channels(pixbuf);
        if (pw <= 0 || ph <= 0 ||
            gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
            gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
            (channels != 3 && channels != 4)) {
                error = "EINVAL:Unsupported pixel format";
                return false;
        }

        bitmap.surface = surface_from_rgba(gdk_pixbuf_get_pixels(pixbuf),
                                           pw, ph, channels,
                                           size_t(gdk_pixbuf_get_rowstride(pixbuf)));
        if (!bitmap.surface) {
                error = "ENOMEM:Failed to allocate image";
                return false;
        }

        bitmap.width = pw;
        bitmap.height = ph;
        return true;
}

} // anonymous namespace

bool
decode_pixbuf_image(std::string const& data,
                    Bitmap& bitmap,
                    std::string& error) noexcept
try
{
        if (data.empty()) {
                error = "ENODATA:No image data";
                return false;
        }

        auto loader = vte::glib::take_ref(gdk_pixbuf_loader_new());
        auto too_big = false;
        g_signal_connect(loader.get(), "size-prepared", G_CALLBACK(pixbuf_size_prepared), &too_big);

        auto err = vte::glib::Error{};
        auto ok = gdk_pixbuf_loader_write(loader.get(),
                                          reinterpret_cast<guchar const*>(data.data()),
                                          data.size(),
                                          err) != FALSE;
        if (ok)
                ok = gdk_pixbuf_loader_close(loader.get(), err) != FALSE;
        else
                gdk_pixbuf_loader_close(loader.get(), nullptr);

        if (too_big) {
                error = "EINVAL:Image dimensions too large";
                return false;
        }
        if (!ok) {
                error = "EINVAL:Failed to decode image";
                return false;
        }

        auto const pixbuf = gdk_pixbuf_loader_get_pixbuf(loader.get()); // unowned
        if (!pixbuf) {
                error = "EINVAL:Failed to decode image";
                return false;
        }

        return pixbuf_to_bitmap(pixbuf, bitmap, error);
}
catch (...)
{
        error = "ENOMEM:Failed to decode image";
        return false;
}

vte::Freeable<cairo_surface_t>
surface_from_rgba(uint8_t const* pixels,
                  int width,
                  int height,
                  int channels,
                  size_t rowstride) noexcept
{
        if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4))
                return nullptr;

        auto surface = vte::take_freeable(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
                return nullptr;

        cairo_surface_flush(surface.get());
        auto const stride = cairo_image_surface_get_stride(surface.get());
        auto const dst = cairo_image_surface_get_data(surface.get());

        for (auto y = 0; y < height; ++y) {
                auto const src_row = pixels + size_t(y) * rowstride;
                auto const dst_row = reinterpret_cast<uint32_t*>(dst + size_t(y) * size_t(stride));
                for (auto x = 0; x < width; ++x) {
                        auto const p = src_row + size_t(x) * size_t(channels);
                        auto const r = uint32_t(p[0]);
                        auto const g = uint32_t(p[1]);
                        auto const b = uint32_t(p[2]);
                        if (channels == 4) {
                                auto const a = uint32_t(p[3]);
                                /* Premultiply */
                                auto const pr = (r * a + 127) / 255;
                                auto const pg = (g * a + 127) / 255;
                                auto const pb = (b * a + 127) / 255;
                                dst_row[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
                        } else {
                                dst_row[x] = (0xffu << 24) | (r << 16) | (g << 8) | b;
                        }
                }
        }

        cairo_surface_mark_dirty(surface.get());
        return surface;
}

bool
decode_image(Command const& cmd,
             std::string const& data_in,
             Bitmap& bitmap,
             std::string& error) noexcept
try
{
        auto const bpp = cmd.format == 24 ? 3 : 4;

        /* Size limits, checked before decompressing so that a small
         * compressed payload cannot expand without bound. */
        auto max_size = size_t{0};
        if (cmd.format == 100) {
                max_size = k_max_image_dimension * k_max_image_dimension * 4;
        } else {
                if (cmd.width == 0 || cmd.height == 0) {
                        error = "EINVAL:Image dimensions not specified";
                        return false;
                }
                if (cmd.width > k_max_image_dimension || cmd.height > k_max_image_dimension) {
                        error = "EINVAL:Image dimensions too large";
                        return false;
                }
                max_size = size_t(cmd.width) * size_t(cmd.height) * size_t(bpp);
        }

        auto inflated = std::string{};
        auto const* data = &data_in;
        if (cmd.compression == 'z') {
                if (!zlib_inflate(data_in, max_size, inflated)) {
                        error = "EINVAL:Failed to decompress image data";
                        return false;
                }
                data = &inflated;
        }

        if (cmd.format == 100)
                return decode_png(*data, bitmap, error);

        auto const expected = size_t(cmd.width) * size_t(cmd.height) * size_t(bpp);
        if (data->size() < expected) {
                error = "ENODATA:Insufficient image data";
                return false;
        }

        bitmap.surface = surface_from_rgba(reinterpret_cast<uint8_t const*>(data->data()),
                                           int(cmd.width), int(cmd.height), bpp,
                                           size_t(cmd.width) * size_t(bpp));
        if (!bitmap.surface) {
                error = "ENOMEM:Failed to allocate image";
                return false;
        }

        bitmap.width = int(cmd.width);
        bitmap.height = int(cmd.height);
        return true;
}
catch (...)
{
        error = "ENOMEM:Failed to decode image";
        return false;
}

/* Store *****************************************************************/

Image*
Store::find(uint32_t id) const noexcept
{
        auto const it = m_images.find(id);
        return it != m_images.end() ? it->second.get() : nullptr;
}

Image*
Store::find_by_number(uint32_t number) const noexcept
{
        if (number == 0)
                return nullptr;

        auto best = static_cast<Image*>(nullptr);
        for (auto const& [id, image] : m_images) {
                if (image->number() != number)
                        continue;
                if (!best || image->sequence() > best->sequence())
                        best = image.get();
        }
        return best;
}

Image*
Store::add(uint32_t id,
           uint32_t number,
           Bitmap&& bitmap,
           bool transient)
{
        remove(id);

        auto image = std::make_unique<Image>(id, number, std::move(bitmap), m_next_sequence++, transient);
        m_bytes_used += image->bytes();

        auto [it, inserted] = m_images.try_emplace(id, std::move(image));
        return it->second.get();
}

void
Store::remove(uint32_t id) noexcept
{
        auto const it = m_images.find(id);
        if (it == m_images.end())
                return;

        m_bytes_used -= it->second->bytes();
        m_images.erase(it);
}

void
Store::clear() noexcept
{
        m_images.clear();
        m_bytes_used = 0;
        m_upload.reset();
}

uint32_t
Store::allocate_id() noexcept
{
        /* Ids handed out by the terminal come from the upper half of the
         * id range, which clients choosing their own ids are unlikely to use. */
        for (auto i = 0; i < 1024; ++i) {
                auto const id = m_next_auto_id++;
                if (m_next_auto_id == 0)
                        m_next_auto_id = 0x40000000u;
                if (id != 0 && !m_images.contains(id))
                        return id;
        }
        return 0;
}

bool
Store::evict_for(size_t bytes,
                 std::function<bool(uint32_t)> const& has_placements,
                 std::vector<uint32_t>& evicted) const noexcept
try
{
        evicted.clear();
        if (bytes > m_quota)
                return false;

        auto used = m_bytes_used;
        if (used + bytes <= m_quota)
                return true;

        /* Order candidates: unreferenced transient, unreferenced, referenced;
         * within each group oldest first. */
        struct Candidate {
                uint32_t id;
                int group;
                uint64_t sequence;
                size_t bytes;
        };
        auto candidates = std::vector<Candidate>{};
        candidates.reserve(m_images.size());
        for (auto const& [id, image] : m_images) {
                auto const referenced = has_placements(id);
                auto const group = referenced ? 2 : (image->transient() ? 0 : 1);
                candidates.push_back({id, group, image->sequence(), image->bytes()});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](auto const& a, auto const& b) noexcept {
                          return a.group != b.group ? a.group < b.group : a.sequence < b.sequence;
                  });

        for (auto const& c : candidates) {
                if (used + bytes <= m_quota)
                        break;
                used -= c.bytes;
                evicted.push_back(c.id);
        }

        return used + bytes <= m_quota;
}
catch (...)
{
        return false;
}

/* The row/column diacritics, in ascending order; the value a diacritic
 * encodes is its index. From kitty's gen/rowcolumn-diacritics.txt. */
static constexpr char32_t k_diacritics[] = {
        0x0305, 0x030d, 0x030e, 0x0310, 0x0312, 0x033d, 0x033e, 0x033f,
        0x0346, 0x034a, 0x034b, 0x034c, 0x0350, 0x0351, 0x0352, 0x0357,
        0x035b, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
        0x036a, 0x036b, 0x036c, 0x036d, 0x036e, 0x036f, 0x0483, 0x0484,
        0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
        0x0598, 0x0599, 0x059c, 0x059d, 0x059e, 0x059f, 0x05a0, 0x05a1,
        0x05a8, 0x05a9, 0x05ab, 0x05ac, 0x05af, 0x05c4, 0x0610, 0x0611,
        0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
        0x0659, 0x065a, 0x065b, 0x065d, 0x065e, 0x06d6, 0x06d7, 0x06d8,
        0x06d9, 0x06da, 0x06db, 0x06dc, 0x06df, 0x06e0, 0x06e1, 0x06e2,
        0x06e4, 0x06e7, 0x06e8, 0x06eb, 0x06ec, 0x0730, 0x0732, 0x0733,
        0x0735, 0x0736, 0x073a, 0x073d, 0x073f, 0x0740, 0x0741, 0x0743,
        0x0745, 0x0747, 0x0749, 0x074a, 0x07eb, 0x07ec, 0x07ed, 0x07ee,
        0x07ef, 0x07f0, 0x07f1, 0x07f3, 0x0816, 0x0817, 0x0818, 0x0819,
        0x081b, 0x081c, 0x081d, 0x081e, 0x081f, 0x0820, 0x0821, 0x0822,
        0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082a, 0x082b, 0x082c,
        0x082d, 0x0951, 0x0953, 0x0954, 0x0f82, 0x0f83, 0x0f86, 0x0f87,
        0x135d, 0x135e, 0x135f, 0x17dd, 0x193a, 0x1a17, 0x1a75, 0x1a76,
        0x1a77, 0x1a78, 0x1a79, 0x1a7a, 0x1a7b, 0x1a7c, 0x1b6b, 0x1b6d,
        0x1b6e, 0x1b6f, 0x1b70, 0x1b71, 0x1b72, 0x1b73, 0x1cd0, 0x1cd1,
        0x1cd2, 0x1cda, 0x1cdb, 0x1ce0, 0x1dc0, 0x1dc1, 0x1dc3, 0x1dc4,
        0x1dc5, 0x1dc6, 0x1dc7, 0x1dc8, 0x1dc9, 0x1dcb, 0x1dcc, 0x1dd1,
        0x1dd2, 0x1dd3, 0x1dd4, 0x1dd5, 0x1dd6, 0x1dd7, 0x1dd8, 0x1dd9,
        0x1dda, 0x1ddb, 0x1ddc, 0x1ddd, 0x1dde, 0x1ddf, 0x1de0, 0x1de1,
        0x1de2, 0x1de3, 0x1de4, 0x1de5, 0x1de6, 0x1dfe, 0x20d0, 0x20d1,
        0x20d4, 0x20d5, 0x20d6, 0x20d7, 0x20db, 0x20dc, 0x20e1, 0x20e7,
        0x20e9, 0x20f0, 0x2cef, 0x2cf0, 0x2cf1, 0x2de0, 0x2de1, 0x2de2,
        0x2de3, 0x2de4, 0x2de5, 0x2de6, 0x2de7, 0x2de8, 0x2de9, 0x2dea,
        0x2deb, 0x2dec, 0x2ded, 0x2dee, 0x2def, 0x2df0, 0x2df1, 0x2df2,
        0x2df3, 0x2df4, 0x2df5, 0x2df6, 0x2df7, 0x2df8, 0x2df9, 0x2dfa,
        0x2dfb, 0x2dfc, 0x2dfd, 0x2dfe, 0x2dff, 0xa66f, 0xa67c, 0xa67d,
        0xa6f0, 0xa6f1, 0xa8e0, 0xa8e1, 0xa8e2, 0xa8e3, 0xa8e4, 0xa8e5,
        0xa8e6, 0xa8e7, 0xa8e8, 0xa8e9, 0xa8ea, 0xa8eb, 0xa8ec, 0xa8ed,
        0xa8ee, 0xa8ef, 0xa8f0, 0xa8f1, 0xaab0, 0xaab2, 0xaab3, 0xaab7,
        0xaab8, 0xaabe, 0xaabf, 0xaac1, 0xfe20, 0xfe21, 0xfe22, 0xfe23,
        0xfe24, 0xfe25, 0xfe26, 0x10a0f, 0x10a38, 0x1d185, 0x1d186, 0x1d187,
        0x1d188, 0x1d189, 0x1d1aa, 0x1d1ab, 0x1d1ac, 0x1d1ad, 0x1d242, 0x1d243,
        0x1d244,
};
static_assert(std::size(k_diacritics) == k_placeholder_diacritics);

int
placeholder_diacritic_value(char32_t c) noexcept
{
        auto const end = std::end(k_diacritics);
        auto const it = std::lower_bound(std::begin(k_diacritics), end, c);
        if (it == end || *it != c)
                return -1;
        return int(it - std::begin(k_diacritics));
}

bool
decode_placeholder(uint32_t id_low,
                   uint32_t placement_id,
                   char32_t const* marks,
                   size_t n_marks,
                   PlaceholderCell const* previous,
                   PlaceholderCell& cell) noexcept
{
        int values[3] = {-1, -1, -1};
        n_marks = std::min(n_marks, std::size(values));
        for (auto i = size_t{0}; i < n_marks; ++i) {
                values[i] = placeholder_diacritic_value(marks[i]);
                if (values[i] < 0)
                        return false;
        }
        /* The third diacritic is a byte */
        if (values[2] > 0xff)
                return false;

        cell = PlaceholderCell{};
        cell.id_low = id_low & 0xffffffu;
        cell.placement_id = placement_id;

        /* Continues the run of the cell to its left? */
        auto const continues = previous != nullptr &&
                previous->id_low == cell.id_low &&
                previous->placement_id == cell.placement_id &&
                (n_marks == 0 || previous->row == values[0]);

        cell.row = n_marks >= 1 ? values[0] : continues ? previous->row : 0;
        cell.column = n_marks >= 2 ? values[1] : continues ? previous->column + 1 : 0;
        cell.id_high = n_marks >= 3 ? uint32_t(values[2]) : continues ? previous->id_high : 0;

        return cell.image_id() != 0;
}

} // namespace kitty

} // namespace vte
