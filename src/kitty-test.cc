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

#include <cstring>
#include <string>

#include <glib.h>
#include <gio/gio.h>
#include <unistd.h>

#include "kitty-graphics.hh"
#include "glib-glue.hh"
#include "refptr.hh"

using namespace vte::kitty;

static std::string
b64(std::string const& s)
{
        auto str = vte::glib::take_free_ptr(g_base64_encode(reinterpret_cast<guchar const*>(s.data()), s.size()));
        return std::string{str.get()};
}

static void
test_parse_defaults()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("", error));
        g_assert_cmpint(cmd.action, ==, 't');
        g_assert_cmpint(cmd.format, ==, 32);
        g_assert_cmpint(cmd.medium, ==, 'd');
        g_assert_cmpint(cmd.quiet, ==, 0);
        g_assert_cmpint(cmd.delete_what, ==, 'a');
        g_assert_false(cmd.has_image_id);
        g_assert_false(cmd.wants_response(true));
}

static void
test_parse_keys()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("a=T,f=24,s=10,v=20,i=31,p=7,z=-5,x=1,y=2,w=3,h=4,X=5,Y=6,c=7,r=8,C=1,q=2,o=z,m=1,t=f,N=1,U=1,d=I", error));
        g_assert_cmpint(cmd.action, ==, 'T');
        g_assert_cmpint(cmd.format, ==, 24);
        g_assert_cmpuint(cmd.width, ==, 10);
        g_assert_cmpuint(cmd.height, ==, 20);
        g_assert_cmpuint(cmd.image_id, ==, 31);
        g_assert_true(cmd.has_image_id);
        g_assert_cmpuint(cmd.placement_id, ==, 7);
        g_assert_true(cmd.has_placement_id);
        g_assert_cmpint(cmd.z, ==, -5);
        g_assert_cmpuint(cmd.src_x, ==, 1);
        g_assert_cmpuint(cmd.src_y, ==, 2);
        g_assert_cmpuint(cmd.src_width, ==, 3);
        g_assert_cmpuint(cmd.src_height, ==, 4);
        g_assert_cmpuint(cmd.cell_x_offset, ==, 5);
        g_assert_cmpuint(cmd.cell_y_offset, ==, 6);
        g_assert_cmpuint(cmd.columns, ==, 7);
        g_assert_cmpuint(cmd.rows, ==, 8);
        g_assert_cmpint(cmd.cursor_policy, ==, 1);
        g_assert_cmpint(cmd.quiet, ==, 2);
        g_assert_cmpint(cmd.compression, ==, 'z');
        g_assert_cmpint(cmd.more, ==, 1);
        g_assert_cmpint(cmd.medium, ==, 'f');
        g_assert_cmpuint(cmd.hints, ==, 1);
        g_assert_cmpint(cmd.unicode_placeholder, ==, 1);
        g_assert_cmpint(cmd.delete_what, ==, 'I');
        g_assert_false(cmd.wants_response(true)); // q=2
        g_assert_false(cmd.wants_response(false));

        /* Large ids and the full 32-bit range */
        g_assert_true(cmd.parse("i=4294967295,z=-2147483648", error));
        g_assert_cmpuint(cmd.image_id, ==, 4294967295u);
        g_assert_cmpint(cmd.z, ==, G_MININT32);
}

static void
test_parse_responses()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("i=1", error));
        g_assert_true(cmd.wants_response(true));
        g_assert_true(cmd.wants_response(false));
        g_assert_true(cmd.parse("i=1,q=1", error));
        g_assert_false(cmd.wants_response(true));
        g_assert_true(cmd.wants_response(false));
        g_assert_true(cmd.parse("I=5", error));
        g_assert_true(cmd.has_image_number);
        g_assert_true(cmd.wants_response(true));
        /* i=0 counts as unset */
        g_assert_true(cmd.parse("i=0", error));
        g_assert_false(cmd.has_image_id);
        g_assert_false(cmd.wants_response(true));
}

static void
test_parse_errors()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_false(cmd.parse("i=1,I=2", error));
        g_assert_true(g_str_has_prefix(error.c_str(), "EINVAL:"));
        g_assert_false(cmd.parse("a=x", error));
        g_assert_false(cmd.parse("f=16", error));
        g_assert_false(cmd.parse("t=q", error));
        g_assert_false(cmd.parse("o=g", error));
        g_assert_false(cmd.parse("m=2", error));
        g_assert_false(cmd.parse("s=abc", error));
        g_assert_false(cmd.parse("s=-1", error));
        g_assert_false(cmd.parse("s=4294967296", error));
        g_assert_false(cmd.parse("garbage", error));
        g_assert_false(cmd.parse("ab=1", error));
        /* unknown keys and stray commas are tolerated */
        g_assert_true(cmd.parse(",k=1,,i=2,", error));
        g_assert_cmpuint(cmd.image_id, ==, 2);
}

static void
test_base64()
{
        auto out = std::string{};
        g_assert_true(base64_decode("", out));
        g_assert_cmpuint(out.size(), ==, 0);
        g_assert_true(base64_decode("AAAA", out));
        g_assert_cmpuint(out.size(), ==, 3);
        g_assert_true(base64_decode(b64("hello world"), out));
        g_assert_cmpstr(out.c_str(), ==, "hello world");
        g_assert_true(base64_decode("aGk=", out));
        g_assert_cmpstr(out.c_str(), ==, "hi");
        g_assert_true(base64_decode("aGk", out)); // unpadded
        g_assert_cmpstr(out.c_str(), ==, "hi");
        g_assert_true(base64_decode("aGk\n=", out)); // whitespace
        g_assert_cmpstr(out.c_str(), ==, "hi");
        g_assert_false(base64_decode("a!k=", out));
        g_assert_false(base64_decode("aGk=aa", out)); // data after padding
        /* URL-safe alphabet */
        g_assert_true(base64_decode("-_8", out));
        g_assert_cmpuint(uint8_t(out[0]), ==, 0xfb);
}

static std::string
rgb_pattern(int width,
            int height,
            int channels)
{
        auto data = std::string{};
        for (auto y = 0; y < height; ++y) {
                for (auto x = 0; x < width; ++x) {
                        data.push_back(char(x * 10));
                        data.push_back(char(y * 10));
                        data.push_back(char(200));
                        if (channels == 4)
                                data.push_back(char(128));
                }
        }
        return data;
}

static uint32_t
pixel_at(Bitmap const& bitmap,
         int x,
         int y)
{
        auto const stride = cairo_image_surface_get_stride(bitmap.surface.get());
        auto const data = cairo_image_surface_get_data(bitmap.surface.get());
        return *reinterpret_cast<uint32_t const*>(data + y * stride + x * 4);
}

static void
test_decode_rgb()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("f=24,s=4,v=3", error));

        auto bitmap = Bitmap{};
        g_assert_true(decode_image(cmd, rgb_pattern(4, 3, 3), bitmap, error));
        g_assert_cmpint(bitmap.width, ==, 4);
        g_assert_cmpint(bitmap.height, ==, 3);
        g_assert_cmpuint(pixel_at(bitmap, 2, 1), ==, 0xff140ac8u);

        /* Too little data */
        g_assert_false(decode_image(cmd, rgb_pattern(4, 2, 3), bitmap, error));
        g_assert_true(g_str_has_prefix(error.c_str(), "ENODATA:"));

        /* Missing dimensions */
        g_assert_true(cmd.parse("f=24", error));
        g_assert_false(decode_image(cmd, rgb_pattern(4, 3, 3), bitmap, error));
        g_assert_true(g_str_has_prefix(error.c_str(), "EINVAL:"));

        /* Too large */
        g_assert_true(cmd.parse("f=24,s=20000,v=1", error));
        g_assert_false(decode_image(cmd, "", bitmap, error));
        g_assert_true(g_str_has_prefix(error.c_str(), "EINVAL:"));
}

static void
test_decode_rgba()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("s=2,v=2", error)); // f defaults to 32

        auto bitmap = Bitmap{};
        g_assert_true(decode_image(cmd, rgb_pattern(2, 2, 4), bitmap, error));
        /* Premultiplied: alpha 128, blue 200 -> 100 */
        auto const p = pixel_at(bitmap, 0, 0);
        g_assert_cmpuint(p >> 24, ==, 128);
        g_assert_cmpuint(p & 0xff, ==, 100);
}

static std::string
zlib_compress(std::string const& input)
{
        auto compressor = vte::glib::take_ref(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, -1));
        auto output = std::string{};
        char buffer[4096];
        auto in_pos = size_t{0};
        while (true) {
                gsize read = 0, written = 0;
                auto const rv = g_converter_convert(G_CONVERTER(compressor.get()),
                                                    input.data() + in_pos, input.size() - in_pos,
                                                    buffer, sizeof(buffer),
                                                    G_CONVERTER_INPUT_AT_END,
                                                    &read, &written, nullptr);
                in_pos += read;
                output.append(buffer, written);
                if (rv == G_CONVERTER_FINISHED)
                        break;
                g_assert_true(rv == G_CONVERTER_CONVERTED);
        }
        return output;
}

static void
test_decode_zlib()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("f=24,s=4,v=3,o=z", error));

        auto bitmap = Bitmap{};
        g_assert_true(decode_image(cmd, zlib_compress(rgb_pattern(4, 3, 3)), bitmap, error));
        g_assert_cmpuint(pixel_at(bitmap, 2, 1), ==, 0xff140ac8u);

        g_assert_false(decode_image(cmd, "not zlib", bitmap, error));
        g_assert_true(g_str_has_prefix(error.c_str(), "EINVAL:"));
}

static cairo_status_t
append_png(void* closure,
           unsigned char const* data,
           unsigned int length)
{
        static_cast<std::string*>(closure)->append(reinterpret_cast<char const*>(data), length);
        return CAIRO_STATUS_SUCCESS;
}

static std::string
make_png(int width,
         int height)
{
        auto surface = vte::take_freeable(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        auto cr = vte::take_freeable(cairo_create(surface.get()));
        cairo_set_source_rgba(cr.get(), 1.0, 0.0, 0.0, 1.0);
        cairo_paint(cr.get());
        auto png = std::string{};
        g_assert_cmpint(cairo_surface_write_to_png_stream(surface.get(), append_png, &png), ==, CAIRO_STATUS_SUCCESS);
        return png;
}

static void
test_decode_png()
{
        auto cmd = Command{};
        auto error = std::string{};
        g_assert_true(cmd.parse("f=100", error));

        auto bitmap = Bitmap{};
        g_assert_true(decode_image(cmd, make_png(5, 7), bitmap, error));
        g_assert_cmpint(bitmap.width, ==, 5);
        g_assert_cmpint(bitmap.height, ==, 7);
        g_assert_cmpuint(pixel_at(bitmap, 1, 1), ==, 0xffff0000u);

        g_assert_false(decode_image(cmd, "not a png", bitmap, error));
        g_assert_true(g_str_has_prefix(error.c_str(), "EINVAL:"));

        /* Compressed PNG */
        g_assert_true(cmd.parse("f=100,o=z", error));
        g_assert_true(decode_image(cmd, zlib_compress(make_png(3, 3)), bitmap, error));
        g_assert_cmpint(bitmap.width, ==, 3);

        /* Any-format decoder used for iTerm2 */
        g_assert_true(decode_pixbuf_image(make_png(2, 9), bitmap, error));
        g_assert_cmpint(bitmap.height, ==, 9);
        g_assert_false(decode_pixbuf_image("", bitmap, error));
}

static void
test_load_file()
{
        auto error = std::string{};
        auto data = std::string{};
        auto cmd = Command{};

        /* A regular temporary file with the magic name gets deleted after use with t=t */
        auto path = static_cast<char*>(nullptr);
        auto fd = g_file_open_tmp("tty-graphics-protocol-XXXXXX", &path, nullptr);
        g_assert_cmpint(fd, >=, 0);
        auto const content = std::string{"0123456789"};
        g_assert_cmpint(write(fd, content.data(), content.size()), ==, ssize_t(content.size()));
        close(fd);

        g_assert_true(cmd.parse("t=f,O=2,S=3", error));
        g_assert_true(load_data(cmd, std::string{path}, data, error));
        g_assert_cmpstr(data.c_str(), ==, "234");
        g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

        /* Claimed size larger than the file */
        g_assert_true(cmd.parse("t=f,S=300", error));
        g_assert_false(load_data(cmd, std::string{path}, data, error));
        g_assert_cmpstr(error.c_str(), ==, "EBADF:Failed to read image file");

        g_assert_true(cmd.parse("t=t", error));
        g_assert_true(load_data(cmd, std::string{path}, data, error));
        g_assert_cmpstr(data.c_str(), ==, "0123456789");
        g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
        g_free(path);

        /* Sensitive locations and special files are refused with the same error */
        g_assert_true(cmd.parse("t=f", error));
        g_assert_false(load_data(cmd, "/proc/self/status", data, error));
        g_assert_cmpstr(error.c_str(), ==, "EBADF:Failed to read image file");
        g_assert_false(load_data(cmd, "/dev/zero", data, error));
        g_assert_cmpstr(error.c_str(), ==, "EBADF:Failed to read image file");
        g_assert_false(load_data(cmd, "/nonexistent/file", data, error));
        g_assert_cmpstr(error.c_str(), ==, "EBADF:Failed to read image file");

        /* Shared memory names must be well-formed */
        g_assert_true(cmd.parse("t=s", error));
        g_assert_false(load_data(cmd, "no-leading-slash", data, error));
        g_assert_false(load_data(cmd, "/a/b", data, error));
}

static void
test_store()
{
        auto store = Store{};
        g_assert_null(store.find(1));

        auto make = [](int w, int h) {
                auto bitmap = Bitmap{};
                bitmap.surface = vte::take_freeable(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h));
                bitmap.width = w;
                bitmap.height = h;
                return bitmap;
        };

        auto const* a = store.add(1, 0, make(10, 10), false);
        g_assert_nonnull(a);
        g_assert_cmpuint(store.bytes_used(), ==, 10 * 4 * 10);
        store.add(2, 42, make(10, 10), true);
        store.add(3, 42, make(10, 10), false);
        g_assert_cmpuint(store.images().size(), ==, 3);

        /* Newest image with a number wins */
        g_assert_cmpuint(store.find_by_number(42)->id(), ==, 3);
        g_assert_null(store.find_by_number(7));

        /* Replacing keeps accounting straight */
        store.add(1, 0, make(20, 10), false);
        g_assert_cmpuint(store.bytes_used(), ==, 20 * 4 * 10 + 2 * 10 * 4 * 10);

        auto const id = store.allocate_id();
        g_assert_cmpuint(id, >=, 0x40000000u);
        g_assert_cmpuint(store.allocate_id(), !=, id);

        /* Eviction: unreferenced transient first, then unreferenced, then oldest */
        auto evicted = std::vector<uint32_t>{};
        auto const quota = store.quota();
        auto const need = quota - store.bytes_used() + 1; // one byte over
        g_assert_true(store.evict_for(need, [](uint32_t) { return false; }, evicted));
        g_assert_cmpuint(evicted.size(), ==, 1);
        g_assert_cmpuint(evicted[0], ==, 2); // transient

        g_assert_true(store.evict_for(need, [](uint32_t iid) { return iid == 2; }, evicted));
        g_assert_cmpuint(evicted[0], ==, 3); // unreferenced non-transient, oldest first

        g_assert_true(store.evict_for(need, [](uint32_t) { return true; }, evicted));
        g_assert_cmpuint(evicted[0], ==, 2); // all referenced: oldest sequence (image 2 was added before 3 and the replaced 1)

        g_assert_false(store.evict_for(quota + 1, [](uint32_t) { return false; }, evicted));

        store.remove(1);
        g_assert_null(store.find(1));
        store.clear();
        g_assert_cmpuint(store.bytes_used(), ==, 0);
        g_assert_cmpuint(store.images().size(), ==, 0);
}

static void
test_placeholder_diacritics()
{
        using namespace vte::kitty;

        g_assert_cmpint(placeholder_diacritic_value(0x0305), ==, 0);
        g_assert_cmpint(placeholder_diacritic_value(0x030d), ==, 1);
        g_assert_cmpint(placeholder_diacritic_value(0x1d244), ==, k_placeholder_diacritics - 1);
        g_assert_cmpint(placeholder_diacritic_value(0x0300), ==, -1);
        g_assert_cmpint(placeholder_diacritic_value('a'), ==, -1);
        g_assert_cmpint(placeholder_diacritic_value(k_placeholder_char), ==, -1);
}

static void
test_placeholder_decode()
{
        using namespace vte::kitty;

        auto cell = PlaceholderCell{};

        /* Explicit row and column, as Claude Code writes them */
        char32_t const rc[] = {0x030d /* 1 */, 0x0310 /* 3 */};
        g_assert_true(decode_placeholder(42, 0, rc, 2, nullptr, cell));
        g_assert_cmpuint(cell.image_id(), ==, 42);
        g_assert_cmpint(cell.row, ==, 1);
        g_assert_cmpint(cell.column, ==, 3);

        /* Third diacritic: most significant byte of the image id */
        char32_t const rcm[] = {0x0305, 0x0305, 0x030e /* 2 */};
        g_assert_true(decode_placeholder(0x123456, 7, rcm, 3, nullptr, cell));
        g_assert_cmpuint(cell.image_id(), ==, 0x02123456u);
        g_assert_cmpuint(cell.placement_id, ==, 7);

        /* No diacritics: continues the cell to the left */
        auto const first = cell;
        auto next = PlaceholderCell{};
        g_assert_true(decode_placeholder(0x123456, 7, nullptr, 0, &first, next));
        g_assert_cmpint(next.row, ==, 0);
        g_assert_cmpint(next.column, ==, 1);
        g_assert_cmpuint(next.image_id(), ==, 0x02123456u);

        /* Row only: column follows the left cell if the row matches */
        char32_t const r0[] = {0x0305};
        g_assert_true(decode_placeholder(0x123456, 7, r0, 1, &next, cell));
        g_assert_cmpint(cell.column, ==, 2);
        char32_t const r1[] = {0x030d};
        g_assert_true(decode_placeholder(0x123456, 7, r1, 1, &next, cell));
        g_assert_cmpint(cell.row, ==, 1);
        g_assert_cmpint(cell.column, ==, 0);
        g_assert_cmpuint(cell.image_id(), ==, 0x123456u);

        /* A different image does not continue the run */
        g_assert_true(decode_placeholder(9, 7, nullptr, 0, &first, cell));
        g_assert_cmpint(cell.row, ==, 0);
        g_assert_cmpint(cell.column, ==, 0);

        /* Unknown diacritic, and image id 0, are not placeholders */
        char32_t const bad[] = {0x0300};
        g_assert_false(decode_placeholder(42, 0, bad, 1, nullptr, cell));
        g_assert_false(decode_placeholder(0, 0, rc, 2, nullptr, cell));

        /* High byte above 255 is invalid */
        char32_t const big[] = {0x0305, 0x0305, 0x1d244};
        g_assert_false(decode_placeholder(42, 0, big, 3, nullptr, cell));
}

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

        g_test_add_func("/vte/kitty/parse/defaults", test_parse_defaults);
        g_test_add_func("/vte/kitty/parse/keys", test_parse_keys);
        g_test_add_func("/vte/kitty/parse/responses", test_parse_responses);
        g_test_add_func("/vte/kitty/parse/errors", test_parse_errors);
        g_test_add_func("/vte/kitty/base64", test_base64);
        g_test_add_func("/vte/kitty/decode/rgb", test_decode_rgb);
        g_test_add_func("/vte/kitty/decode/rgba", test_decode_rgba);
        g_test_add_func("/vte/kitty/decode/zlib", test_decode_zlib);
        g_test_add_func("/vte/kitty/decode/png", test_decode_png);
        g_test_add_func("/vte/kitty/load/file", test_load_file);
        g_test_add_func("/vte/kitty/store", test_store);
        g_test_add_func("/vte/kitty/placeholder/diacritics", test_placeholder_diacritics);
        g_test_add_func("/vte/kitty/placeholder/decode", test_placeholder_decode);

        return g_test_run();
}
