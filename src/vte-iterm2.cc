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
 * iTerm2 inline images (OSC 1337 ; File=...), see
 * https://iterm2.com/documentation-images.html
 *
 * Also contains place_inline_image(), the placement routine shared by
 * all image protocols.
 */

#include "config.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "vteinternal.hh"
#include "kitty-graphics.hh"
#include "image.hh"

using namespace std::literals;

namespace vte {
namespace terminal {

/* Creates a placement of @surface at the cursor position, covering
 * @columns x @rows cells, and moves the cursor to the last row of the
 * placement, right of it, when @move_cursor is set. The screen scrolls
 * as needed so that the whole placement is visible.
 *
 * Returns the placement, or nullptr if it could not be created.
 */
vte::image::Image*
Terminal::place_inline_image(vte::image::Kind kind,
                             cairo_surface_t* surface,
                             int src_x,
                             int src_y,
                             int src_width,
                             int src_height,
                             int dst_width,
                             int dst_height,
                             int offset_x,
                             int offset_y,
                             int columns,
                             int rows,
                             uint32_t image_id,
                             uint32_t placement_id,
                             uint32_t image_number,
                             int32_t z,
                             bool owns_memory,
                             bool move_cursor)
{
        auto const cell_width = int(m_cell_width_unscaled);
        auto const cell_height = int(m_cell_height_unscaled);
        if (!surface || cell_width <= 0 || cell_height <= 0 ||
            src_width <= 0 || src_height <= 0 ||
            dst_width <= 0 || dst_height <= 0)
                return nullptr;

        columns = std::clamp(columns, 1, 4096);
        rows = std::clamp(rows, 1, 4096);

        /* Position: the cursor, which may be in the pending-wrap state
         * (same as maybe_retreat_cursor(), which is inline in vteseq.cc) */
        m_screen->cursor.col = get_xterm_cursor_column();
        m_screen->cursor_advanced_by_graphic_character = false;
        auto const left = m_screen->cursor.col;
        auto const top = m_screen->cursor.row;

        /* Images wider than the screen are truncated at the right edge */
        columns = std::max(1, std::min(columns, int(m_column_count - left)));

        auto* ring = m_screen->row_data;

        /* A placement with the same image id and placement id replaces the old one */
        if (kind == vte::image::Kind::KITTY && image_id != 0 && placement_id != 0) {
                ring->remove_images_if([image_id, placement_id](auto const& image) noexcept {
                        return image.kind() == vte::image::Kind::KITTY &&
                                image.image_id() == image_id &&
                                image.placement_id() == placement_id;
                });
        }

        auto image = std::make_unique<vte::image::Image>(kind,
                                                         vte::take_freeable(cairo_surface_reference(surface)),
                                                         src_x, src_y, src_width, src_height,
                                                         dst_width, dst_height,
                                                         offset_x, offset_y,
                                                         left, top,
                                                         columns, rows,
                                                         cell_width, cell_height);
        image->set_ids(image_id, image_id ? placement_id : 0, image_number);
        image->set_z(z);
        image->set_owns_memory(owns_memory);

        auto const* placed = ring->append_image(std::move(image));

        /* Move the cursor to the end of the placement, scrolling if needed
         * so that the whole image is on screen. Since the placement is
         * anchored to absolute rows, it scrolls along with the text. */
        if (move_cursor) {
                for (auto i = 1; i < rows; ++i)
                        cursor_down_with_scrolling(true);
                /* Same as move_cursor_forward(), which is inline in vteseq.cc */
                auto const right = left <= m_scrolling_region.right()
                        ? m_scrolling_region.right()
                        : m_column_count - 1;
                m_screen->cursor.col = std::min(left + columns, right);
                m_screen->cursor_advanced_by_graphic_character = false;
        }

        invalidate_rows(top, top + rows - 1);
        m_text_inserted_flag = true;

        return const_cast<vte::image::Image*>(placed);
}

namespace {

/* An iTerm2 dimension: "auto", "N" (cells), "Npx" (pixels), "N%" (percent
 * of the screen). Returns the size in pixels, or 0 for auto. */
int
iterm2_dimension(std::string_view value,
                 int cell_size,
                 int screen_size) noexcept
{
        if (value.empty() || value == "auto"sv)
                return 0;

        auto unit = std::string_view{};
        auto digits = value;
        if (value.ends_with("px"sv)) {
                unit = "px"sv;
                digits = value.substr(0, value.size() - 2);
        } else if (value.ends_with("%"sv)) {
                unit = "%"sv;
                digits = value.substr(0, value.size() - 1);
        }

        auto n = 0;
        auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n, 10);
        if (ec != std::errc{} || ptr != digits.data() + digits.size() || n <= 0)
                return 0;

        if (unit == "px"sv)
                return std::min(n, 100000);
        if (unit == "%"sv)
                return int(int64_t(std::min(n, 1000)) * screen_size / 100);
        return std::min(n, 4096) * cell_size;
}

struct Iterm2Params {
        std::string_view width{};
        std::string_view height{};
        bool preserve_aspect_ratio{true};
        bool inline_{false};
        bool move_cursor{true};
};

Iterm2Params
iterm2_parse_params(std::string_view args) noexcept
{
        auto params = Iterm2Params{};
        while (!args.empty()) {
                auto const semi = args.find(';');
                auto const item = args.substr(0, semi);
                args = semi == std::string_view::npos ? std::string_view{} : args.substr(semi + 1);

                auto const eq = item.find('=');
                if (eq == std::string_view::npos)
                        continue;
                auto const key = item.substr(0, eq);
                auto const value = item.substr(eq + 1);

                if (key == "width"sv)
                        params.width = value;
                else if (key == "height"sv)
                        params.height = value;
                else if (key == "preserveAspectRatio"sv)
                        params.preserve_aspect_ratio = value != "0"sv;
                else if (key == "inline"sv)
                        params.inline_ = value == "1"sv;
                else if (key == "doNotMoveCursor"sv)
                        params.move_cursor = value != "1"sv;
        }
        return params;
}

} // anonymous namespace

/* Decodes and places an inline image. @args are the File= parameters,
 * @base64 the (possibly whitespace-containing) base64 image data. */
void
Terminal::iterm2_place_file(std::string_view args,
                            std::string_view base64)
{
        auto const params = iterm2_parse_params(args);
        if (!params.inline_)
                return; // a download request, not supported

        auto data = std::string{};
        if (!vte::kitty::base64_decode(base64, data))
                return;

        auto bitmap = vte::kitty::Bitmap{};
        auto error = std::string{};
        if (!vte::kitty::decode_pixbuf_image(data, bitmap, error)) {
                _vte_debug_print(vte::debug::category::IMAGE,
                                 "iTerm2 image: {}", error);
                return;
        }

        auto const cell_width = int(m_cell_width_unscaled);
        auto const cell_height = int(m_cell_height_unscaled);
        if (cell_width <= 0 || cell_height <= 0)
                return;

        auto const screen_width = int(m_column_count) * cell_width;
        auto const screen_height = int(m_row_count) * cell_height;
        auto const box_width = iterm2_dimension(params.width, cell_width, screen_width);
        auto const box_height = iterm2_dimension(params.height, cell_height, screen_height);

        auto const image_width = double(bitmap.width);
        auto const image_height = double(bitmap.height);
        auto dst_width = image_width;
        auto dst_height = image_height;

        if (box_width && box_height) {
                if (params.preserve_aspect_ratio) {
                        auto const scale = std::min(box_width / image_width, box_height / image_height);
                        dst_width = image_width * scale;
                        dst_height = image_height * scale;
                } else {
                        dst_width = box_width;
                        dst_height = box_height;
                }
        } else if (box_width) {
                dst_width = box_width;
                dst_height = box_width * image_height / image_width;
        } else if (box_height) {
                dst_height = box_height;
                dst_width = box_height * image_width / image_height;
        } else if (dst_width > screen_width) {
                /* Natural size, but never wider than the screen */
                dst_height = dst_height * screen_width / dst_width;
                dst_width = screen_width;
        }

        auto const columns = int(std::ceil(dst_width / cell_width));
        auto const rows = int(std::ceil(dst_height / cell_height));

        place_inline_image(vte::image::Kind::ITERM2,
                           bitmap.surface.get(),
                           0, 0, bitmap.width, bitmap.height,
                           int(std::lround(dst_width)), int(std::lround(dst_height)),
                           0, 0,
                           columns, rows,
                           0, 0, 0, 0,
                           true /* owns memory */,
                           params.move_cursor);
}

/* OSC 1337 ; @rest, where @rest is everything after "1337;" */
void
Terminal::iterm2_command(std::string_view rest)
{
        if (!m_iterm2_enabled)
                return;

        auto const eq = rest.find('=');
        auto const key = rest.substr(0, eq);
        auto const value = eq == std::string_view::npos ? std::string_view{} : rest.substr(eq + 1);

        if (key == "File"sv) {
                m_iterm2_upload.reset();

                auto const colon = value.find(':');
                if (colon == std::string_view::npos)
                        return;
                iterm2_place_file(value.substr(0, colon), value.substr(colon + 1));
        } else if (key == "MultipartFile"sv) {
                m_iterm2_upload.reset();
                m_iterm2_upload.args = std::string{value};
                m_iterm2_upload.active = true;
        } else if (key == "FilePart"sv) {
                if (!m_iterm2_upload.active)
                        return;
                if (m_iterm2_upload.data.size() + value.size() > vte::kitty::k_max_file_size * 4 / 3) {
                        m_iterm2_upload.reset();
                        return;
                }
                m_iterm2_upload.data.append(value);
        } else if (key == "FileEnd"sv) {
                if (!m_iterm2_upload.active)
                        return;
                auto args = std::move(m_iterm2_upload.args);
                auto data = std::move(m_iterm2_upload.data);
                m_iterm2_upload.reset();
                iterm2_place_file(args, data);
        }
}

} // namespace terminal
} // namespace vte
